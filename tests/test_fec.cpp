// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — FEC encoder/decoder unit tests.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// The fec_encode (2) and fec_recover (1) conformance vectors gate the byte-exact
// contract; these add the structural paths the vectors don't cover (variable
// payload sizes, block boundaries, 0/1/2+-missing recovery branches, stats).
//
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "naudio/AudioPacket.hpp"
#include "naudio/FecDecoder.hpp"
#include "naudio/FecEncoder.hpp"

using naudio::AudioPacket;
using naudio::FecDecoder;
using naudio::FecEncoder;
using naudio::PacketType;

namespace {

AudioPacket rx(std::int32_t seq, std::vector<std::uint8_t> data) {
    return AudioPacket::createRxAudio(seq, std::move(data));
}

// ---- FecEncoder ----

TEST(FecEncoder, ConstructorRejectsInvalidBlockSize) {
    EXPECT_THROW(FecEncoder(1), std::invalid_argument);
    EXPECT_THROW(FecEncoder(11), std::invalid_argument);
    EXPECT_NO_THROW(FecEncoder(2));
    EXPECT_NO_THROW(FecEncoder(10));
}

TEST(FecEncoder, ReturnsNulloptUntilBlockComplete) {
    FecEncoder enc(3);
    EXPECT_FALSE(enc.recordAndMaybeEmit(rx(0, {1, 2, 3})).has_value());
    EXPECT_FALSE(enc.recordAndMaybeEmit(rx(1, {4, 5, 6})).has_value());
    EXPECT_EQ(2u, enc.currentCount());
}

TEST(FecEncoder, EmitsParityOnBlockComplete) {
    FecEncoder enc(3);
    enc.recordAndMaybeEmit(rx(0, {1, 2, 3}));
    enc.recordAndMaybeEmit(rx(1, {4, 5, 6}));
    auto parity = enc.recordAndMaybeEmit(rx(2, {7, 8, 9}));
    ASSERT_TRUE(parity.has_value());
    EXPECT_EQ(PacketType::FecParity, parity->packetType());
}

TEST(FecEncoder, ParityPayloadContainsCorrectXor) {
    FecEncoder enc(3);
    enc.recordAndMaybeEmit(rx(0, {0x10, 0x20, 0x30}));
    enc.recordAndMaybeEmit(rx(1, {0x01, 0x02, 0x03}));
    auto parity = enc.recordAndMaybeEmit(rx(2, {0xFF, 0x00, 0x0F}));
    ASSERT_TRUE(parity.has_value());
    const auto& payload = parity->payload();
    // Header: [startSeq:4][blockSize:1].
    EXPECT_EQ(0, (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3]);
    EXPECT_EQ(3, payload[4]);
    const std::uint8_t* xorData = payload.data() + FecEncoder::PARITY_HEADER_SIZE;
    EXPECT_EQ(0x10 ^ 0x01 ^ 0xFF, xorData[0]);
    EXPECT_EQ(0x20 ^ 0x02, xorData[1]);
    EXPECT_EQ(0x30 ^ 0x03 ^ 0x0F, xorData[2]);
}

TEST(FecEncoder, HandlesVariablePayloadSizes) {
    FecEncoder enc(2);
    enc.recordAndMaybeEmit(rx(0, {0x01, 0x02}));
    auto parity = enc.recordAndMaybeEmit(rx(1, {0x03, 0x04, 0x05, 0x06}));
    ASSERT_TRUE(parity.has_value());
    const auto& payload = parity->payload();
    EXPECT_EQ(4u, payload.size() - FecEncoder::PARITY_HEADER_SIZE);
    const std::uint8_t* xorData = payload.data() + FecEncoder::PARITY_HEADER_SIZE;
    EXPECT_EQ(0x01 ^ 0x03, xorData[0]);
    EXPECT_EQ(0x02 ^ 0x04, xorData[1]);
    EXPECT_EQ(0x05, xorData[2]);  // d1 zero-padded ^ 0x05
    EXPECT_EQ(0x06, xorData[3]);  // d1 zero-padded ^ 0x06
}

TEST(FecEncoder, ResetsAfterEmittingParity) {
    FecEncoder enc(2);
    enc.recordAndMaybeEmit(rx(0, {1}));
    EXPECT_TRUE(enc.recordAndMaybeEmit(rx(1, {2})).has_value());
    EXPECT_EQ(0u, enc.currentCount());
    EXPECT_FALSE(enc.recordAndMaybeEmit(rx(10, {3})).has_value());
    EXPECT_EQ(1u, enc.currentCount());
}

// ---- FecDecoder ----

// A collecting emitter: stores copies of non-null emits, counts NULL gaps.
struct DecSink {
    std::vector<AudioPacket> packets;
    int gaps = 0;
    FecDecoder::Emitter emitter() {
        return [this](const AudioPacket* p) {
            if (p == nullptr) ++gaps; else packets.push_back(*p);
        };
    }
    const AudioPacket* find(std::int32_t seq) const {
        for (const auto& p : packets) if (p.sequence() == seq) return &p;
        return nullptr;
    }
};

// Builds a parity packet from payloads (test helper):
// [startSeq:4 BE][blockSize:1][xor:maxLen]; parity seq = startSeq + N.
AudioPacket buildParity(std::int32_t startSeq, const std::vector<std::vector<std::uint8_t>>& payloads) {
    std::size_t maxLen = 0;
    for (const auto& p : payloads) maxLen = std::max(maxLen, p.size());
    std::vector<std::uint8_t> xorData(maxLen, 0);
    for (const auto& p : payloads)
        for (std::size_t j = 0; j < p.size(); ++j) xorData[j] ^= p[j];
    const std::uint32_t s = static_cast<std::uint32_t>(startSeq);
    std::vector<std::uint8_t> payload{
        static_cast<std::uint8_t>((s >> 24) & 0xFF), static_cast<std::uint8_t>((s >> 16) & 0xFF),
        static_cast<std::uint8_t>((s >> 8) & 0xFF),  static_cast<std::uint8_t>(s & 0xFF),
        static_cast<std::uint8_t>(payloads.size())};
    payload.insert(payload.end(), xorData.begin(), xorData.end());
    return AudioPacket(PacketType::FecParity, startSeq + static_cast<std::int32_t>(payloads.size()),
                       std::move(payload));
}

TEST(FecDecoder, AllPresentBlockNoRecovery) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> d0{0x10, 0x20}, d1{0x30, 0x40}, d2{0x50, 0x60};
    d.processPacket(rx(0, d0));
    d.processPacket(rx(1, d1));
    d.processPacket(rx(2, d2));
    d.processPacket(buildParity(0, {d0, d1, d2}));
    EXPECT_EQ(3u, s.packets.size());
    EXPECT_EQ(0, d.packetsRecoveredByFec());
    EXPECT_EQ(1, d.fecBlocksComplete());
    EXPECT_EQ(0, d.fecBlocksFailed());
}

TEST(FecDecoder, SingleLossRecovery) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> d0{0x10, 0x20}, d1{0x30, 0x40}, d2{0x50, 0x60};
    d.processPacket(rx(0, d0));
    d.process(std::nullopt, 1);  // d1 lost
    d.processPacket(rx(2, d2));
    d.processPacket(buildParity(0, {d0, d1, d2}));
    EXPECT_EQ(1, d.packetsRecoveredByFec());
    EXPECT_EQ(1, d.fecBlocksComplete());
    const AudioPacket* rec = s.find(1);
    ASSERT_NE(nullptr, rec);
    EXPECT_EQ(d1, rec->payload());
}

TEST(FecDecoder, TwoMissingCannotRecover) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> d0{0x10, 0x20}, d1{0x30, 0x40}, d2{0x50, 0x60};
    d.processPacket(rx(0, d0));
    d.process(std::nullopt, 1);
    d.process(std::nullopt, 2);
    d.processPacket(buildParity(0, {d0, d1, d2}));
    EXPECT_EQ(0, d.packetsRecoveredByFec());
    EXPECT_EQ(0, d.fecBlocksComplete());
    EXPECT_EQ(1, d.fecBlocksFailed());
    EXPECT_EQ(2, s.gaps);  // silence emitted for both gaps
}

TEST(FecDecoder, EncoderDecoderRoundTrip) {
    FecEncoder enc(3);
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> d0{0x11, 0x22, 0x33}, d1{0x44, 0x55, 0x66}, d2{0x77, 0x88, 0x99};
    enc.recordAndMaybeEmit(rx(0, d0));
    enc.recordAndMaybeEmit(rx(1, d1));
    auto parity = enc.recordAndMaybeEmit(rx(2, d2));
    ASSERT_TRUE(parity.has_value());
    // p0 received, p1 lost, p2 received, parity received.
    d.processPacket(rx(0, d0));
    d.process(std::nullopt, 1);
    d.processPacket(rx(2, d2));
    d.processPacket(*parity);
    EXPECT_EQ(1, d.packetsRecoveredByFec());
    const AudioPacket* rec = s.find(1);
    ASSERT_NE(nullptr, rec);
    EXPECT_EQ(d1, rec->payload());
}

TEST(FecDecoder, VariablePayloadSizeRecovery) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> d0{0x01, 0x02}, d1{0x03, 0x04, 0x05, 0x06};
    d.processPacket(rx(0, d0));
    d.process(std::nullopt, 1);  // d1 (longer) lost
    d.processPacket(buildParity(0, {d0, d1}));
    EXPECT_EQ(1, d.packetsRecoveredByFec());
    const AudioPacket* rec = s.find(1);
    ASSERT_NE(nullptr, rec);
    EXPECT_EQ(d1, rec->payload());
}

TEST(FecDecoder, StatsTracking) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> dd{0x01};
    // Complete block (all present).
    d.processPacket(rx(0, dd));
    d.processPacket(rx(1, dd));
    d.processPacket(buildParity(0, {dd, dd}));
    EXPECT_EQ(0, d.packetsRecoveredByFec());
    EXPECT_EQ(1, d.fecBlocksComplete());
    // Recovery block.
    d.processPacket(rx(10, dd));
    d.process(std::nullopt, 11);
    d.processPacket(buildParity(10, {dd, {0x02}}));
    EXPECT_EQ(1, d.packetsRecoveredByFec());
    EXPECT_EQ(2, d.fecBlocksComplete());
    // Failed block (2+ missing).
    d.process(std::nullopt, 20);
    d.process(std::nullopt, 21);
    d.processPacket(rx(22, dd));
    d.processPacket(buildParity(20, {dd, dd, dd}));
    EXPECT_EQ(1, d.fecBlocksFailed());
}

TEST(FecDecoder, ResetClearsState) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> dd{0x01};
    d.processPacket(rx(0, dd));
    d.process(std::nullopt, 1);
    d.processPacket(buildParity(0, {dd, {0x02}}));
    EXPECT_EQ(1, d.packetsRecoveredByFec());
    d.reset();
    EXPECT_EQ(0, d.packetsRecoveredByFec());
    EXPECT_EQ(0, d.fecBlocksComplete());
    EXPECT_EQ(0, d.fecBlocksFailed());
}

// --- Issue #23: a non-audio packet inside the block's sequence range ---------
//
// The parity header carries a COUNT of audio packets, but the connection's
// sequence counter is shared with control and heartbeat traffic. A control
// message sent between two audio packets of the same block therefore takes a
// sequence number inside [startSeq, startSeq + blockSize) and pushes the block's
// last audio packet just past the end of that range. The decoder cannot read the
// range as the block any more: one slot looks missing, and XORing across the
// range would emit `lost ^ displaced` — a full frame of wrong samples presented
// as recovered audio.

TEST(FecDecoder, ControlInsideBlockRangeDeclinesRecoveryInsteadOfCorrupting) {
    DecSink s;
    FecDecoder d(s.emitter());
    // Server: audio 0,1,2,3 -> control 4 -> audio 5 -> parity 6 (startSeq 0, 5 audio).
    std::vector<std::uint8_t> a0{0x10, 0x20}, a1{0x30, 0x40}, a2{0x50, 0x60}, a3{0x70, 0x01},
        a5{0x02, 0x03};
    AudioPacket parity = buildParity(0, {a0, a1, a2, a3, a5});
    parity.setSequence(6);  // the parity follows the LAST audio packet, at seq 5

    d.processPacket(rx(0, a0));
    d.processPacket(rx(1, a1));
    d.process(std::nullopt, 2);  // a2 lost on the wire
    d.processPacket(rx(3, a3));
    d.processPacket(AudioPacket(PacketType::Control, 4, {'C', 'U'}));  // takes slot 4
    d.processPacket(rx(5, a5));
    d.processPacket(parity);

    // Declined, not recovered: nothing is emitted for the lost sequence.
    EXPECT_EQ(1, d.fecBlocksUnreconciled());
    EXPECT_EQ(0, d.packetsRecoveredByFec());
    EXPECT_EQ(0, d.fecBlocksComplete());
    EXPECT_EQ(nullptr, s.find(2));

    // And nothing bogus was emitted in its place: every audio packet the sink saw
    // is byte-identical to what the sender sent. Before the fix this failed with a
    // packet at seq 2 carrying a2 ^ a5.
    for (const AudioPacket& p : s.packets) {
        if (p.packetType() != PacketType::AudioRx) continue;
        switch (p.sequence()) {
            case 0: EXPECT_EQ(a0, p.payload()); break;
            case 1: EXPECT_EQ(a1, p.payload()); break;
            case 3: EXPECT_EQ(a3, p.payload()); break;
            case 5: EXPECT_EQ(a5, p.payload()); break;
            default: ADD_FAILURE() << "unexpected audio at seq " << p.sequence();
        }
    }
}

// The healthy neighbour: the same control message, one slot earlier, so the block
// keeps a contiguous audio range. Recovery must still happen — the guard has to
// fire on the broken layout and stay silent on the correct one.
TEST(FecDecoder, ControlOutsideBlockRangeStillRecovers) {
    DecSink s;
    FecDecoder d(s.emitter());
    // Server: control 0 -> audio 1,2,3,4,5 -> parity 6 (startSeq 1, 5 audio).
    std::vector<std::uint8_t> a1{0x10, 0x20}, a2{0x30, 0x40}, a3{0x50, 0x60}, a4{0x70, 0x01},
        a5{0x02, 0x03};
    AudioPacket parity = buildParity(1, {a1, a2, a3, a4, a5});
    parity.setSequence(6);

    d.processPacket(AudioPacket(PacketType::Control, 0, {'C', 'U'}));  // before the block
    d.processPacket(rx(1, a1));
    d.processPacket(rx(2, a2));
    d.process(std::nullopt, 3);  // a3 lost
    d.processPacket(rx(4, a4));
    d.processPacket(rx(5, a5));
    d.processPacket(parity);

    EXPECT_EQ(0, d.fecBlocksUnreconciled());
    EXPECT_EQ(1, d.packetsRecoveredByFec());
    const AudioPacket* rec = s.find(3);
    ASSERT_NE(nullptr, rec);
    EXPECT_EQ(a3, rec->payload());  // byte-exact, not a XOR of two frames
}

TEST(FecDecoder, MissingParityTimesOut) {
    DecSink s;
    FecDecoder d(s.emitter(), 10);  // 10 ms timeout; pending uses 2x = 20 ms
    std::int64_t fakeNow = 1000;
    d.setClock([&fakeNow]() { return fakeNow; });  // deterministic, no sleeps
    std::vector<std::uint8_t> dd{0x01};
    d.processPacket(rx(0, dd));      // pending block created at t=1000
    d.process(std::nullopt, 1);      // missing, awaiting parity
    d.checkTimeoutAt(1020);          // 20 not > 20 -> not yet
    EXPECT_EQ(0, s.gaps);
    d.checkTimeoutAt(1021);          // 21 > 20 -> flush pending with a silence gap
    EXPECT_EQ(1, s.gaps);
}

// ---- Pending-block retention (issue #47) ----
//
// The four arms below pin the retention POLICY, which is the thing #47 got wrong.
// They use the injected clock, so they measure the policy and never the machine
// they run on.

// THE #47 REGRESSION ARM. A block whose own packets are still arriving must not be
// discarded, however long the block takes in total.
//
// Shape mirrors udpWan: a 5-packet block, one packet lost, one parity. The packets
// are spaced 40 ms apart, so the block spans 200 ms end to end — well past the
// pre-fix 120 ms bound — while no single ARRIVAL GAP exceeds 40 ms. checkTimeoutAt
// is driven between arrivals because that is what the client does (its decoder tick
// hangs off the socket recv timeout).
//
// Pre-fix this recovered 0: the bound was measured from block CREATION, so the
// block was erased at t=1160 (160 > 120) and the parity then found every slot
// missing. Post-fix the bound is measured from the last insert, so a 40 ms gap
// never trips a 120 ms idle bound and the single loss is repaired.
TEST(FecDecoder, ArrivingPacketsKeepTheirBlockAliveRegardlessOfBlockSpan) {
    DecSink s;
    FecDecoder d(s.emitter());  // default: 120 ms pending idle bound
    std::int64_t now = 1000;
    d.setClock([&now]() { return now; });

    std::vector<std::vector<std::uint8_t>> pl;
    for (int i = 0; i < 5; ++i) pl.push_back({static_cast<std::uint8_t>(i + 1), 0x55});

    for (int i = 0; i < 5; ++i) {
        d.checkTimeoutAt(now);         // the client ticks the decoder on socket idle
        if (i != 2) d.processPacket(rx(i, pl[i]));  // seq 2 is lost on the wire
        now += 40;                     // 40 ms between arrivals; 200 ms across the block
    }
    d.checkTimeoutAt(now);
    d.processPacket(buildParity(0, pl));

    EXPECT_EQ(1, d.packetsRecoveredByFec()) << "a block whose packets kept arriving was discarded";
    EXPECT_EQ(0, d.pendingPacketsDiscarded());
    const AudioPacket* rec = s.find(2);
    ASSERT_NE(nullptr, rec);
    EXPECT_EQ(pl[2], rec->payload());  // byte-exact repair
}

// The bound must still BITE — a widened timeout that never expires is an unbounded
// buffer, not a fix. A genuine idle gap past the bound discards the block, and the
// discard is COUNTED, which is what stops #47's failure mode from being silent
// again: fecBlocksFailed cannot move here (its increment is guarded on a non-empty
// missingSequences, and the client's reorder->FEC chain drops gap markers).
TEST(FecDecoder, AnIdleGapPastTheBoundDiscardsThePendingBlockAndCountsIt) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::int64_t now = 1000;
    d.setClock([&now]() { return now; });

    std::vector<std::vector<std::uint8_t>> pl;
    for (int i = 0; i < 5; ++i) pl.push_back({static_cast<std::uint8_t>(i + 1), 0x55});
    d.processPacket(rx(0, pl[0]));  // pending created and touched at t=1000

    d.checkTimeoutAt(1120);  // exactly at the bound: 120 is not > 120
    EXPECT_EQ(0, d.pendingPacketsDiscarded()) << "the bound is strict >, so 120 must survive";

    d.checkTimeoutAt(1121);  // one past it
    EXPECT_EQ(1, d.pendingPacketsDiscarded()) << "the erase path is unreachable — unbounded buffer";
    EXPECT_EQ(0, d.fecBlocksFailed()) << "fecBlocksFailed cannot see this; that is why #47 was silent";
    EXPECT_EQ(0, s.gaps);  // nothing is emitted: the packets were already delivered on arrival

    // The parity now finds nothing and cannot repair.
    d.processPacket(buildParity(0, pl));
    EXPECT_EQ(0, d.packetsRecoveredByFec());
}

// Retention is bounded by PACKET COUNT as well as by time, because the time bound
// alone leaves two holes: a client whose peer never sends parity refreshes the idle
// bound on every arrival and so never expires, and checkTimeout() is never called
// at all on a server-fed connection.
TEST(FecDecoder, PendingRetentionIsCappedWhenNoParityEverArrives) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::int64_t now = 1000;
    d.setClock([&now]() { return now; });

    const int over = 10;
    const int total = static_cast<int>(FecDecoder::MAX_PENDING_PACKETS) + over;
    for (int i = 0; i < total; ++i) {
        d.processPacket(rx(i, {static_cast<std::uint8_t>(i), 0x11}));
        now += 1;  // arrivals keep refreshing the idle bound — time never saves us here
    }

    EXPECT_EQ(over, d.pendingPacketsDiscarded()) << "pending retention grew without bound";
    EXPECT_EQ(total, static_cast<int>(s.packets.size()))
        << "capping the repair cache must not affect delivery — every packet is emitted on arrival";
}

// The derived bound is clamped by the decoder, not merely by its caller, so the
// clamp holds against every present and future caller.
TEST(FecDecoder, PendingIdleTimeoutIsClampedToItsDocumentedRange) {
    DecSink s;
    FecDecoder d(s.emitter());
    EXPECT_EQ(FecDecoder::MIN_PENDING_IDLE_MS, d.pendingIdleTimeoutMs());  // 2x the default

    d.setPendingIdleTimeoutMs(1);
    EXPECT_EQ(FecDecoder::MIN_PENDING_IDLE_MS, d.pendingIdleTimeoutMs());

    d.setPendingIdleTimeoutMs(1000 * 1000);
    EXPECT_EQ(FecDecoder::MAX_PENDING_IDLE_MS, d.pendingIdleTimeoutMs());

    d.setPendingIdleTimeoutMs(300);
    EXPECT_EQ(300, d.pendingIdleTimeoutMs());  // in range, taken verbatim
}

}  // namespace
