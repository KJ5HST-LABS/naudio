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
using naudio::Provenance;

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

// A collecting emitter: stores copies of non-null emits, counts NULL gaps, and
// records the Provenance of every emit alongside it (issue #65). The provenance
// vectors are parallel to `packets` / `gaps` rather than folded into them, so the
// arms that predate provenance keep reading exactly what they always read.
struct DecSink {
    std::vector<AudioPacket> packets;
    std::vector<Provenance> packetProvenance;  // parallel to `packets`
    std::vector<Provenance> gapProvenance;     // one entry per NULL emit
    int gaps = 0;
    FecDecoder::Emitter emitter() {
        return [this](const AudioPacket* p, Provenance prov) {
            if (p == nullptr) {
                ++gaps;
                gapProvenance.push_back(prov);
            } else {
                packets.push_back(*p);
                packetProvenance.push_back(prov);
            }
        };
    }
    const AudioPacket* find(std::int32_t seq) const {
        for (const auto& p : packets) if (p.sequence() == seq) return &p;
        return nullptr;
    }
    // Provenance of the emit that delivered `seq`, or nullptr if it was never
    // emitted. Indexes packetProvenance by the same position as `packets`.
    const Provenance* provenanceOf(std::int32_t seq) const {
        for (std::size_t i = 0; i < packets.size(); ++i)
            if (packets[i].sequence() == seq) return &packetProvenance[i];
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
    // The parity seq is computed in UNSIGNED space, matching how the wire treats a sequence number
    // and how `s` is already derived four lines up. Signed arithmetic here is undefined on
    // overflow, and this helper is called with startSeq at INT32_MAX-4 by
    // FecDecoder.ADisplacedMemberEvictedByThePacketCapIsStillDeclined — an arm that exists to
    // exercise wrap. UBSan caught it: "signed integer overflow: 2147483643 + 5" (issue #28).
    return AudioPacket(PacketType::FecParity,
                       static_cast<std::int32_t>(s + static_cast<std::uint32_t>(payloads.size())),
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
// The seven arms below pin the retention POLICY, which is the thing #47 got wrong,
// plus the discarded-slot guard that #52's fix turned out to require. They use the
// injected clock, so they measure the policy and never the machine they run on.

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

// A slot THIS CLASS discarded must not be "recovered" — it would be a byte-exact
// duplicate of audio the application already received.
//
// process() emits an audio packet BEFORE storing a copy of it, and retention then
// erases the copy. handleParity infers membership from the parity's declared range and
// cannot tell an erased slot from one the peer never sent, so with exactly ONE erased it
// would run the XOR and emit a frame that was already delivered — while incrementing
// packets_recovered_by_fec for a repair that never happened. Measured through a real
// server-fed connection before the guard existed: 6 frames delivered for 5 sent, at ZERO
// packet loss. Paired with the neighbour below, which must still recover.
TEST(FecDecoder, ADiscardedSlotIsDeclinedRatherThanRecoveredAsADuplicate) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::int64_t now = 1000;
    d.setClock([&now]() { return now; });

    std::vector<std::vector<std::uint8_t>> pl;
    for (int i = 0; i < 5; ++i) pl.push_back({static_cast<std::uint8_t>(i + 1), 0x55});

    d.processPacket(rx(0, pl[0]));  // delivered to the application, and stored for repair
    ASSERT_NE(nullptr, s.find(0)) << "the arm's premise: seq 0 WAS delivered on arrival";

    now = 1121;                     // one past the 120 ms default idle bound
    d.checkTimeoutAt(now);
    ASSERT_EQ(1, d.pendingPacketsDiscarded()) << "premise: the stored copy of seq 0 is gone";

    for (int i = 1; i < 5; ++i) d.processPacket(rx(i, pl[i]));
    d.processPacket(buildParity(0, pl));  // range [0,5): 1-4 present, slot 0 absent

    EXPECT_EQ(0, d.packetsRecoveredByFec())
        << "the discarded slot was recovered — this emits a duplicate of delivered audio";
    EXPECT_EQ(1, d.fecBlocksUnreconciled()) << "the decline must be counted, not silent";
    EXPECT_EQ(0, d.fecBlocksComplete());

    // The decisive assertion: seq 0 appears exactly ONCE across everything emitted.
    int seq0 = 0;
    for (const AudioPacket& p : s.packets) {
        if (p.packetType() == PacketType::AudioRx && p.sequence() == 0) seq0++;
    }
    EXPECT_EQ(1, seq0) << "seq 0 was delivered " << seq0 << " times; a fabricated duplicate";
}

// Retention has TWO discard paths and the guard must record both. This arm drives the
// other one — the MAX_PENDING_PACKETS cap in capPending, not the idle timeout — because a
// mutation audit proved the timeout arm above does not cover it: deleting capPending's
// recording left every other arm green, so the cap half of the guard was unprotected by
// any test. Predicted as a pass before it was run, which is the only reason the gap was
// visible rather than comfortable.
TEST(FecDecoder, ASlotEvictedByThePacketCapIsAlsoDeclinedRatherThanDuplicated) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::int64_t now = 1000;
    d.setClock([&now]() { return now; });  // never advanced: the TIMEOUT must not fire here

    // One past the cap, so capPending evicts exactly the lowest sequence (seq 0).
    const int over = static_cast<int>(FecDecoder::MAX_PENDING_PACKETS) + 1;
    std::vector<std::vector<std::uint8_t>> pl;
    for (int i = 0; i < over; ++i) pl.push_back({static_cast<std::uint8_t>(i + 1), 0x55});
    for (int i = 0; i < over; ++i) d.processPacket(rx(i, pl[i]));

    ASSERT_EQ(1, d.pendingPacketsDiscarded()) << "premise: the cap evicted exactly one slot";
    ASSERT_NE(nullptr, s.find(0)) << "premise: seq 0 was delivered on arrival before eviction";

    // A parity whose range covers the evicted slot: 1-4 are still pending, 0 is gone.
    d.processPacket(buildParity(0, {pl[0], pl[1], pl[2], pl[3], pl[4]}));

    EXPECT_EQ(0, d.packetsRecoveredByFec())
        << "a cap-evicted slot was recovered — a duplicate of audio already delivered";
    EXPECT_EQ(1, d.fecBlocksUnreconciled());
    int seq0 = 0;
    for (const AudioPacket& p : s.packets) {
        if (p.packetType() == PacketType::AudioRx && p.sequence() == 0) seq0++;
    }
    EXPECT_EQ(1, seq0) << "seq 0 was delivered " << seq0 << " times";
}

// The healthy neighbour, byte-identical to the arm above except that slot 0 is
// GENUINELY absent — never delivered, never discarded. Recovery must still happen, or
// the guard has bought correctness by disabling the feature.
TEST(FecDecoder, AGenuinelyMissingSlotStillRecoversAfterAnUnrelatedDiscard) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::int64_t now = 1000;
    d.setClock([&now]() { return now; });

    std::vector<std::vector<std::uint8_t>> pl;
    for (int i = 0; i < 5; ++i) pl.push_back({static_cast<std::uint8_t>(i + 1), 0x55});

    // Nothing is delivered for seq 0 at all, so nothing about it can be discarded.
    ASSERT_EQ(nullptr, s.find(0));
    for (int i = 1; i < 5; ++i) d.processPacket(rx(i, pl[i]));
    d.processPacket(buildParity(0, pl));

    EXPECT_EQ(1, d.packetsRecoveredByFec()) << "the guard is over-declining: real loss is a repair";
    EXPECT_EQ(0, d.fecBlocksUnreconciled());
    EXPECT_EQ(1, d.fecBlocksComplete());
    const AudioPacket* rec = s.find(0);
    ASSERT_NE(nullptr, rec);
    EXPECT_EQ(pl[0], rec->payload()) << "recovered payload is not what the sender sent";
}

// Retention is bounded by PACKET COUNT as well as by time, because the time bound
// alone leaves two holes: a peer that never sends parity refreshes the idle bound on
// every arrival and so never expires, and a consumer that stops polling produces no
// tick at all in either receive mode (issue #52 made the server-fed limb tick, so the
// old form of this comment — "checkTimeout() is never called on a server-fed
// connection" — no longer holds; the cap's justification does).
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

// --- Issue #55: an ABSENT in-range stranger, and the block's own audio type -------------
//
// #23 (above) guards the stranger that ARRIVES. Its premise — "it stays invisible without
// loss, the stranger fills the slot so nothing looks missing" — assumes the stranger reaches
// the decoder. Three production paths falsify that, the deterministic one being a consumed
// CONTROL_ACK: it draws a sequence from the shared counter and is returned before the
// reorder/FEC pipeline, so its slot reads exactly like a lost audio packet. With one such
// slot the XOR remainder IS the block's displaced last member, and "recovery" emits a
// byte-exact DUPLICATE of audio the application already received — at ZERO packet loss.
//
// What these arms assert is therefore DELIVERY MULTIPLICITY, not a counter: an equality
// between receive modes would be useless because both modes fabricate, and a counter alone
// cannot tell a repair from a duplicate. Each arm counts how many times each payload
// reaches the sink.

AudioPacket txp(std::int32_t seq, std::vector<std::uint8_t> data) {
    return AudioPacket::createTxAudio(seq, std::move(data));
}

// How many emitted audio packets carry exactly `payload`.
int deliveries(const DecSink& s, const std::vector<std::uint8_t>& payload) {
    int n = 0;
    for (const AudioPacket& p : s.packets) {
        if (p.packetType() != PacketType::FecParity && p.payload() == payload) ++n;
    }
    return n;
}

// The canonical shape: one consumed ACK takes slot 4, displacing the block's last member to
// seq 5 — exactly at the range end. Before the fix this fabricated a duplicate of a4.
TEST(FecDecoder, AnAbsentStrangerDoesNotFabricateADuplicateAtZeroLoss) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> a0{0x10, 0x20}, a1{0x30, 0x40}, a2{0x50, 0x60}, a3{0x70, 0x01},
        a4{0x02, 0x03};
    // Sender drew: a0=0 a1=1 a2=2 a3=3 [CONTROL_ACK=4] a4=5, parity=6.
    AudioPacket parity = buildParity(0, {a0, a1, a2, a3, a4});
    parity.setSequence(6);

    d.processPacket(rx(0, a0));
    d.processPacket(rx(1, a1));
    d.processPacket(rx(2, a2));
    d.processPacket(rx(3, a3));
    // seq 4 is NEVER presented — the ACK was consumed by control reliability.
    d.processPacket(rx(5, a4));
    d.processPacket(parity);

    EXPECT_EQ(0, d.packetsRecoveredByFec()) << "nothing was lost — there is nothing to recover";
    EXPECT_EQ(1, d.fecBlocksUnreconciled());
    EXPECT_EQ(nullptr, s.find(4)) << "slot 4 held a consumed control ACK, not audio";
    EXPECT_EQ(5u, s.packets.size()) << "5 sent, 5 delivered";
    EXPECT_EQ(1, deliveries(s, a4)) << "the displaced member must not be delivered twice";
}

// THE ARM THAT SEPARATES THIS GUARD FROM THE ISSUE'S LITERAL WORDING. #55 proposes declining
// when an audio packet sits at exactly `startSeq + blockSize`. Two back-to-back ACKs put the
// displaced member one slot FURTHER out, and a `== blockEnd` lookup finds nothing there.
// This is the natural interleaving, not a corner case: CLIENTS_UPDATE and TX_GRANTED are both
// critical types, so two arriving together generate two ACKs, each drawing a sequence.
TEST(FecDecoder, TwoAbsentStrangersStillDeclineWithTheMemberPastTheRangeEnd) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> a0{0x10, 0x20}, a1{0x30, 0x40}, a2{0x50, 0x60}, a3{0x70, 0x01},
        a4{0x02, 0x03};
    // Sender drew: a0=0 a1=1 a2=2 a3=3 [ACK=4] [ACK=5] a4=6, parity=7.
    AudioPacket parity = buildParity(0, {a0, a1, a2, a3, a4});
    parity.setSequence(7);

    d.processPacket(rx(0, a0));
    d.processPacket(rx(1, a1));
    d.processPacket(rx(2, a2));
    d.processPacket(rx(3, a3));
    d.processPacket(rx(6, a4));  // displaced to blockEnd + 1, NOT blockEnd
    d.processPacket(parity);

    EXPECT_EQ(0, d.packetsRecoveredByFec());
    EXPECT_EQ(1, d.fecBlocksUnreconciled());
    EXPECT_EQ(1, deliveries(s, a4)) << "a witness one slot past blockEnd is still a witness";
    EXPECT_EQ(5u, s.packets.size());
}

// The displaced member can be GONE by the time the parity arrives: capPending evicts
// lowest-first over a SIGNED map, so at the int32 wrap the highest sequence sorts lowest and
// is evicted first. discardedSequences_ is then the only surviving evidence — this is the
// arm for that limb of the witness, which no other arm reaches.
TEST(FecDecoder, ADisplacedMemberEvictedByThePacketCapIsStillDeclined) {
    DecSink s;
    FecDecoder d(s.emitter());
    const std::int32_t S = 2147483643;  // S+5 wraps to INT32_MIN
    std::vector<std::uint8_t> a0{0x10, 0x20}, a1{0x30, 0x40}, a2{0x50, 0x60}, a3{0x70, 0x01},
        a4{0x02, 0x03};

    // Fill the repair cache so every later insert evicts.
    for (std::size_t i = 0; i < FecDecoder::MAX_PENDING_PACKETS; ++i) {
        d.processPacket(rx(2147483600 + static_cast<std::int32_t>(i),
                           {static_cast<std::uint8_t>(i), 0xEE}));
    }
    d.processPacket(rx(S, a0));
    d.processPacket(rx(S + 1, a1));
    d.processPacket(rx(S + 2, a2));
    // S + 3 is the absent stranger.
    d.processPacket(rx(S + 4, a3));
    d.processPacket(rx(static_cast<std::int32_t>(static_cast<std::uint32_t>(S) + 5), a4));

    AudioPacket parity = buildParity(S, {a0, a1, a2, a3, a4});
    d.processPacket(parity);

    EXPECT_EQ(0, d.packetsRecoveredByFec());
    EXPECT_EQ(1, d.fecBlocksUnreconciled());
    EXPECT_EQ(1, deliveries(s, a4)) << "the evicted member was already delivered on arrival";
}

// startSeq == PENDING_KEY makes handleParity's activeBlocks_.find(startSeq) hit the pending
// block and move it wholesale into the local `block`, leaving no PENDING_KEY entry. A witness
// search that reads only the pending block finds an empty map. Reachable without a hostile
// peer: §3.4 specifies two's-complement overflow, so the shared counter reaches INT32_MIN.
TEST(FecDecoder, AnAbsentStrangerAtTheCounterWrapIsStillDeclined) {
    DecSink s;
    FecDecoder d(s.emitter());
    const std::int32_t S = -2147483648;  // INT32_MIN == the pending block's sentinel key
    std::vector<std::uint8_t> a0{0x10, 0x20}, a1{0x30, 0x40}, a2{0x50, 0x60}, a3{0x70, 0x01},
        a4{0x02, 0x03};

    d.processPacket(rx(S, a0));
    d.processPacket(rx(S + 1, a1));
    d.processPacket(rx(S + 2, a2));
    d.processPacket(rx(S + 3, a3));
    // S + 4 is the absent stranger; the member it displaced is at S + 5.
    d.processPacket(rx(S + 5, a4));
    d.processPacket(buildParity(S, {a0, a1, a2, a3, a4}));

    EXPECT_EQ(0, d.packetsRecoveredByFec());
    EXPECT_EQ(1, d.fecBlocksUnreconciled());
    EXPECT_EQ(1, deliveries(s, a4));
}

// THE NEGATIVE CONTROL. A genuine single loss with no stranger anywhere must still recover —
// a guard that declines everything would pass every arm above and destroy the feature.
TEST(FecDecoder, AGenuineLossStillRecoversWhenNothingIsDisplaced) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> a0{0x10, 0x20}, a1{0x30, 0x40}, a2{0x50, 0x60}, a3{0x70, 0x01},
        a4{0x02, 0x03};
    d.processPacket(rx(0, a0));
    d.processPacket(rx(1, a1));
    // a2 genuinely lost on the wire.
    d.processPacket(rx(3, a3));
    d.processPacket(rx(4, a4));
    d.processPacket(buildParity(0, {a0, a1, a2, a3, a4}));

    EXPECT_EQ(1, d.packetsRecoveredByFec());
    EXPECT_EQ(0, d.fecBlocksUnreconciled());
    const AudioPacket* rec = s.find(2);
    ASSERT_NE(nullptr, rec);
    EXPECT_EQ(a2, rec->payload()) << "byte-exact repair of the genuinely lost frame";
}

// The guard must never read the parity frame's OWN sequence number. §3.4 constrains it only
// to be monotonic and shared across types, so a conforming peer may number it any way it
// likes — and this repo ships such a peer: the conformance harness numbers parity at
// startSeq. A guard keyed to that field would decline every block from a legal sender, i.e.
// silently disable FEC. This arm is the executable form of that deliberate constraint.
TEST(FecDecoder, TheAbsentStrangerGuardIgnoresTheParityFramesOwnSequence) {
    std::vector<std::uint8_t> a0{0x10, 0x20}, a1{0x30, 0x40}, a2{0x50, 0x60}, a3{0x70, 0x01},
        a4{0x02, 0x03};
    for (std::int32_t paritySeq : {0, 5, 6, 7, 999}) {
        DecSink s;
        FecDecoder d(s.emitter());
        AudioPacket parity = buildParity(0, {a0, a1, a2, a3, a4});
        parity.setSequence(paritySeq);
        d.processPacket(rx(0, a0));
        d.processPacket(rx(1, a1));
        d.processPacket(rx(2, a2));
        d.processPacket(rx(3, a3));
        d.processPacket(rx(5, a4));  // slot 4 absent — the consumed ACK
        d.processPacket(parity);

        EXPECT_EQ(1, d.fecBlocksUnreconciled()) << "parity sequence " << paritySeq;
        EXPECT_EQ(0, d.packetsRecoveredByFec()) << "parity sequence " << paritySeq;
        EXPECT_EQ(1, deliveries(s, a4)) << "parity sequence " << paritySeq;
    }
}

// THE ANTI-MASKING ARM FOR ISSUE #23, and it is not optional.
//
// #55's displacement witness fires on an audio packet at or past blockEnd — which is present
// in the ORDINARY #23 layout too, because the stranger displaced a member out there. So the
// witness silently absorbs #23's detector: MEASURED, deleting `rangeIsAudioOnly` from the
// decline leaves the whole suite green, and only deleting the witness as well turns
// ControlInsideBlockRangeDeclinesRecoveryInsteadOfCorrupting red. Without the arm below,
// #23's guard could be removed by a future session with no test saying otherwise (L45/L54).
//
// The isolating shape denies the witness its evidence: the stranger is PRESENT in range, and
// the member it displaced never arrives. Two frames of the block are then unaccounted for —
// which is exactly issue #23's corruption case, where the XOR remainder would be
// `lost ^ displaced` rather than either frame.
TEST(FecDecoder, APresentStrangerIsDeclinedEvenWithNothingPastTheRangeEnd) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> a0{0x10, 0x20}, a1{0x30, 0x40}, a2{0x50, 0x60}, a3{0x70, 0x01},
        a4{0x02, 0x03};
    // Sender drew: a0=0 a1=1 a2=2 [CONTROL=3] a3=4 a4=5, parity=6.
    // Received: 0,1,2 and the control at 3. Both a3 (seq 4) and a4 (seq 5) are lost.
    AudioPacket parity = buildParity(0, {a0, a1, a2, a3, a4});
    parity.setSequence(6);

    d.processPacket(rx(0, a0));
    d.processPacket(rx(1, a1));
    d.processPacket(rx(2, a2));
    d.processPacket(AudioPacket(PacketType::Control, 3, {'C', 'U'}));
    d.processPacket(parity);

    EXPECT_EQ(1, d.fecBlocksUnreconciled()) << "the present stranger alone must decline this";
    EXPECT_EQ(0, d.packetsRecoveredByFec());
    EXPECT_EQ(nullptr, s.find(4)) << "emitting here would be a3 ^ a4 — a frame of wrong samples";
    int audio = 0;
    for (const AudioPacket& p : s.packets) {
        if (p.packetType() == PacketType::AudioRx) ++audio;
    }
    EXPECT_EQ(3, audio) << "only the three real audio frames were delivered (the sink also "
                           "sees the control packet, which the decoder passes through)";
}

// THE ANTI-MASKING ARM FOR ISSUE #52's CAP LIMB, for the same reason as the arm above.
//
// ASlotEvictedByThePacketCapIsAlsoDeclinedRatherThanDuplicated fills the repair cache with
// AUDIO, so #55's witness sees packets past blockEnd and co-declines: MEASURED, deleting
// `rangeHasDiscardedSlot` leaves that arm green and reddens only its timeout twin. The
// witness needs an AUDIO packet past the range end, so filling the cache with CONTROL
// traffic instead denies it that evidence and leaves the discarded-slot guard as the only
// decliner. (The first shape I reasoned through was wrong — I concluded the cap limb could
// not be isolated at all, because I forgot the witness is type-filtered.)
TEST(FecDecoder, ACapEvictedSlotIsDeclinedEvenWithNoAudioPastTheRangeEnd) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::int64_t now = 1000;
    d.setClock([&now]() { return now; });  // never advanced: the TIMEOUT limb must not fire

    std::vector<std::vector<std::uint8_t>> pl;
    for (int i = 0; i < 5; ++i) pl.push_back({static_cast<std::uint8_t>(i + 1), 0x55});
    for (int i = 0; i < 5; ++i) d.processPacket(rx(i, pl[i]));
    // Control traffic fills the repair cache to one past the cap, evicting exactly seq 0.
    const int controls = static_cast<int>(FecDecoder::MAX_PENDING_PACKETS) - 4;
    for (int i = 0; i < controls; ++i) {
        d.processPacket(AudioPacket(PacketType::Control, 5 + i, {'C', 'U'}));
    }
    ASSERT_EQ(1, d.pendingPacketsDiscarded()) << "premise: the cap evicted exactly one slot";
    ASSERT_NE(nullptr, s.find(0)) << "premise: seq 0 was delivered on arrival before eviction";

    d.processPacket(buildParity(0, {pl[0], pl[1], pl[2], pl[3], pl[4]}));

    EXPECT_EQ(0, d.packetsRecoveredByFec())
        << "a cap-evicted slot was recovered — a duplicate of audio already delivered";
    EXPECT_EQ(1, d.fecBlocksUnreconciled());
    EXPECT_EQ(1, deliveries(s, pl[0])) << "seq 0 must be delivered exactly once";
}

// --- Issue #55 defect 2: the recovered packet carries its BLOCK's audio type -------------
//
// The encoder is driven from both audio send paths, so the client-to-server TX lane is
// FEC-protected too. A recovered frame typed AudioRx is routed to `default: break` by
// AudioStreamServer::ClientSession::receiveLoop and silently dropped, while
// packetsRecoveredByFec has already counted it — FEC that repairs nothing and says it did.
TEST(FecDecoder, ATxLaneBlockRecoversAsAudioTxNotAudioRx) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> a0{0x11, 0x21}, a1{0x31, 0x41}, a2{0x51, 0x61};
    d.processPacket(txp(0, a0));
    // a1 genuinely lost.
    d.processPacket(txp(2, a2));
    d.processPacket(buildParity(0, {a0, a1, a2}));

    EXPECT_EQ(1, d.packetsRecoveredByFec());
    const AudioPacket* rec = s.find(1);
    ASSERT_NE(nullptr, rec);
    EXPECT_EQ(PacketType::AudioTx, rec->packetType()) << "a TX block must recover as AudioTx";
    EXPECT_EQ(a1, rec->payload());
}

// The no-regression twin: an RX block still recovers as AudioRx. Without this, hardcoding the
// type the other way would pass the arm above.
TEST(FecDecoder, AnRxLaneBlockStillRecoversAsAudioRx) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> a0{0x11, 0x21}, a1{0x31, 0x41}, a2{0x51, 0x61};
    d.processPacket(rx(0, a0));
    d.processPacket(rx(2, a2));
    d.processPacket(buildParity(0, {a0, a1, a2}));

    EXPECT_EQ(1, d.packetsRecoveredByFec());
    const AudioPacket* rec = s.find(1);
    ASSERT_NE(nullptr, rec);
    EXPECT_EQ(PacketType::AudioRx, rec->packetType());
}

// A range holding both audio types is not one encoder's block — the type to stamp on the
// recovered packet would be a coin flip, so it is declined rather than guessed.
TEST(FecDecoder, ATypeMixedRangeIsDeclinedRatherThanGuessed) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> a0{0x11, 0x21}, a1{0x31, 0x41}, a2{0x51, 0x61};
    d.processPacket(rx(0, a0));
    d.processPacket(txp(2, a2));  // disagrees with slot 0
    d.processPacket(buildParity(0, {a0, a1, a2}));

    EXPECT_EQ(0, d.packetsRecoveredByFec());
    EXPECT_EQ(1, d.fecBlocksUnreconciled());
    EXPECT_EQ(nullptr, s.find(1));
}

// blockSize is a wire u8: 0..255 arrives, while the encoder's validated range is 2..10 and
// nothing validates a RECEIVED header. Declining is not enough — the decline must be COUNTED,
// or a peer sending malformed parity is indistinguishable from one sending none.
TEST(FecDecoder, AnOutOfRangeBlockSizeIsDeclinedAndCounted) {
    for (std::uint8_t bad : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{11},
                             std::uint8_t{255}}) {
        DecSink s;
        FecDecoder d(s.emitter());
        std::vector<std::uint8_t> a0{0x11, 0x21};
        d.processPacket(rx(0, a0));
        // Hand-built parity: [startSeq:4 BE][blockSize:1][xor], blockSize forced out of range.
        std::vector<std::uint8_t> payload{0, 0, 0, 0, bad, 0xAA, 0xBB};
        d.processPacket(AudioPacket(PacketType::FecParity, 5, std::move(payload)));

        EXPECT_EQ(1, d.fecBlocksUnreconciled()) << "blockSize " << static_cast<int>(bad);
        EXPECT_EQ(0, d.packetsRecoveredByFec()) << "blockSize " << static_cast<int>(bad);
        EXPECT_EQ(0, d.fecBlocksComplete()) << "blockSize " << static_cast<int>(bad);
        EXPECT_EQ(1u, s.packets.size()) << "blockSize " << static_cast<int>(bad);
    }
}

// ---- Provenance (issue #65) ----
//
// This decoder is the ORIGIN of provenance: it is the only place in the project
// that can tell a repaired frame from a delivered one, because it is the only
// place that builds one. These three arms pin that origin — one per emit site that
// can be observed, covering all four sites in FecDecoder.cpp. Everything downstream
// (phases 2-4) carries what it is handed, so if the origin is wrong every later
// hop is wrong with it and no downstream test can tell.
//
// Both directions are asserted deliberately. An implementation that emitted
// Recovered for EVERYTHING would satisfy "the repair is Recovered" on its own,
// and an implementation that never set it at all — the exact failure a defaulted
// `bool` would ship silently — would satisfy "the arrivals are Live". Only the
// pair discriminates.

// The single-loss XOR reconstruction (FecDecoder.cpp's one Recovered emit) must
// be the ONLY emit in the block that is not Live: seq 1 never arrived, seq 0 and
// seq 2 did.
TEST(FecDecoder, TheRecoveredFrameIsTheOnlyEmitMarkedRecovered) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> d0{0x10, 0x20}, d1{0x30, 0x40}, d2{0x50, 0x60};
    d.processPacket(rx(0, d0));
    d.process(std::nullopt, 1);  // d1 lost — this is the slot FEC rebuilds
    d.processPacket(rx(2, d2));
    d.processPacket(buildParity(0, {d0, d1, d2}));

    ASSERT_EQ(1, d.packetsRecoveredByFec()) << "premise: exactly one slot was rebuilt";
    ASSERT_EQ(3u, s.packets.size()) << "premise: two arrivals plus the repair were emitted";
    ASSERT_EQ(s.packets.size(), s.packetProvenance.size())
        << "the sink's parallel vectors desynchronized";

    const Provenance* recovered = s.provenanceOf(1);
    ASSERT_NE(nullptr, recovered) << "the rebuilt slot was never emitted";
    EXPECT_EQ(Provenance::Recovered, *recovered)
        << "the XOR reconstruction must announce itself as a repair — this is the bit "
           "AudioMixer needs to refuse a TX re-claim (issue #65)";

    // The other direction: a packet that arrived on the wire is Live, and stays Live
    // even though the decoder stored a copy of it in the repair cache.
    for (std::int32_t seq : {0, 2}) {
        const Provenance* live = s.provenanceOf(seq);
        ASSERT_NE(nullptr, live) << "seq " << seq << " arrived but was not emitted";
        EXPECT_EQ(Provenance::Live, *live)
            << "seq " << seq << " arrived on the wire and must not be marked as a repair";
    }
}

// A block with 2+ missing slots cannot be rebuilt, so its NULL silence emits carry
// no packet and therefore no repair. Live is the correct reading and the safe one:
// it is what every path did before provenance existed.
TEST(FecDecoder, AnUnrecoverableGapEmitsSilenceMarkedLive) {
    DecSink s;
    FecDecoder d(s.emitter());
    std::vector<std::uint8_t> d0{0x10, 0x20}, d1{0x30, 0x40}, d2{0x50, 0x60};
    d.processPacket(rx(0, d0));
    d.process(std::nullopt, 1);
    d.process(std::nullopt, 2);
    d.processPacket(buildParity(0, {d0, d1, d2}));

    ASSERT_EQ(1, d.fecBlocksFailed()) << "premise: the block was unrecoverable";
    ASSERT_EQ(2, s.gaps) << "premise: silence was emitted for both gaps";
    ASSERT_EQ(static_cast<std::size_t>(s.gaps), s.gapProvenance.size())
        << "the sink's parallel vectors desynchronized";
    for (Provenance prov : s.gapProvenance)
        EXPECT_EQ(Provenance::Live, prov) << "a silence gap reconstructed nothing";
    EXPECT_EQ(0, d.packetsRecoveredByFec());
}

// The fourth emit site: checkTimeoutAt flushes a block that timed out waiting for a
// parity that never came, emitting silence for each gap. Nothing was reconstructed,
// so those gaps are Live.
//
// This arm exists because the site was MEASURED to be unasserted without it: flipping
// it to Recovered reddened nothing across all 330 tests, with the object hash
// confirming the mutation reached the binary. The other three sites were each caught
// by an arm above; this one had no detector at all.
TEST(FecDecoder, ATimedOutBlocksSilenceIsMarkedLive) {
    DecSink s;
    FecDecoder d(s.emitter(), 10);  // 10 ms block timeout; pending uses 2x = 20 ms
    std::int64_t fakeNow = 1000;
    d.setClock([&fakeNow]() { return fakeNow; });  // deterministic, no sleeps
    std::vector<std::uint8_t> dd{0x01};
    d.processPacket(rx(0, dd));
    d.process(std::nullopt, 1);  // missing, awaiting a parity that never arrives
    d.checkTimeoutAt(1021);      // 21 > 20 -> flush the pending block as silence

    ASSERT_EQ(1, s.gaps) << "premise: the timeout flushed exactly one silence gap";
    ASSERT_EQ(static_cast<std::size_t>(s.gaps), s.gapProvenance.size())
        << "the sink's parallel vectors desynchronized";
    EXPECT_EQ(Provenance::Live, s.gapProvenance.front())
        << "a timed-out block reconstructed nothing — its silence is not a repair";
    EXPECT_EQ(0, d.packetsRecoveredByFec());
}

}  // namespace
