// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — PacketReorderBuffer unit tests.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// The 3 reorder conformance vectors gate
// in-order / single-OOO / window-flush; these add late/duplicate discard, the
// timeout force-flush path (driven via the injected checkTimeoutAt clock, no
// sleeps), multi-OOO drain, baseline-at-nonzero-seq, and reset.
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "naudio/AudioPacket.hpp"
#include "naudio/PacketReorderBuffer.hpp"

using naudio::AudioPacket;
using naudio::PacketReorderBuffer;

namespace {

// A collecting emitter: records emitted sequence numbers, "GAP" for a NULL gap.
struct Sink {
    std::vector<std::string> items;
    PacketReorderBuffer::Emitter emitter() {
        return [this](const AudioPacket* p) {
            items.push_back(p == nullptr ? std::string("GAP") : std::to_string(p->sequence()));
        };
    }
    std::size_t size() const { return items.size(); }
};

AudioPacket pkt(std::int32_t seq) { return AudioPacket::createRxAudio(seq, {static_cast<std::uint8_t>(seq)}); }

TEST(PacketReorderBuffer, ConstructorValidation) {
    EXPECT_THROW(PacketReorderBuffer(0, 30, [](const AudioPacket*) {}), std::invalid_argument);
    EXPECT_THROW(PacketReorderBuffer(8, -1, [](const AudioPacket*) {}), std::invalid_argument);
}

TEST(PacketReorderBuffer, InOrderPassthrough) {
    Sink s;
    PacketReorderBuffer buf(8, 30, s.emitter());
    buf.insert(pkt(0));
    buf.insert(pkt(1));
    buf.insert(pkt(2));
    EXPECT_EQ((std::vector<std::string>{"0", "1", "2"}), s.items);
    EXPECT_EQ(0, buf.packetsReordered());
    EXPECT_EQ(0, buf.packetsDroppedLate());
    EXPECT_EQ(0, buf.gapsEmitted());
    EXPECT_EQ(0u, buf.bufferedCount());
}

TEST(PacketReorderBuffer, SingleOooRecovery) {
    Sink s;
    PacketReorderBuffer buf(8, 30, s.emitter());
    buf.insert(pkt(0));  // in order
    buf.insert(pkt(2));  // ahead — buffer
    buf.insert(pkt(1));  // fills gap — emit 1 then drain 2
    EXPECT_EQ((std::vector<std::string>{"0", "1", "2"}), s.items);
    EXPECT_EQ(1, buf.packetsReordered());
    EXPECT_EQ(0u, buf.bufferedCount());
}

TEST(PacketReorderBuffer, MultipleOooRecovery) {
    Sink s;
    PacketReorderBuffer buf(8, 30, s.emitter());
    buf.insert(pkt(0));
    buf.insert(pkt(3));
    buf.insert(pkt(2));
    buf.insert(pkt(1));  // fills gap — drain 1,2,3
    EXPECT_EQ((std::vector<std::string>{"0", "1", "2", "3"}), s.items);
    EXPECT_EQ(2, buf.packetsReordered());
}

TEST(PacketReorderBuffer, WindowFullForceFlush) {
    Sink s;
    PacketReorderBuffer buf(3, 5000, s.emitter());
    buf.insert(pkt(0));  // emit
    buf.insert(pkt(2));  // buffer (gap at 1)
    buf.insert(pkt(3));  // buffer
    buf.insert(pkt(4));  // buffer -> window full -> force-flush
    EXPECT_EQ((std::vector<std::string>{"0", "GAP", "2", "3", "4"}), s.items);
    EXPECT_EQ(1, buf.gapsEmitted());
}

TEST(PacketReorderBuffer, TimeoutForceFlush) {
    Sink s;
    PacketReorderBuffer buf(8, 10, s.emitter());  // 10 ms hold
    buf.insertAt(pkt(0), 1000);  // emit
    buf.insertAt(pkt(2), 1000);  // buffer (gap at 1), arrival t=1000
    buf.checkTimeoutAt(1009);    // not yet (9 < 10)
    EXPECT_EQ(1u, s.size());
    buf.checkTimeoutAt(1010);    // now (>= 10) -> flush gap@1 then 2
    EXPECT_EQ((std::vector<std::string>{"0", "GAP", "2"}), s.items);
    EXPECT_EQ(1, buf.gapsEmitted());
}

TEST(PacketReorderBuffer, LatePacketDiscarded) {
    Sink s;
    PacketReorderBuffer buf(8, 30, s.emitter());
    buf.insert(pkt(0));
    buf.insert(pkt(1));
    buf.insert(pkt(2));
    buf.insert(pkt(0));  // late/duplicate — discard
    EXPECT_EQ(3u, s.size());
    EXPECT_EQ(1, buf.packetsDroppedLate());
}

TEST(PacketReorderBuffer, FirstPacketEstablishesBaseline) {
    Sink s;
    PacketReorderBuffer buf(8, 30, s.emitter());
    buf.insert(pkt(100));
    buf.insert(pkt(101));
    EXPECT_EQ((std::vector<std::string>{"100", "101"}), s.items);
    EXPECT_EQ(102, buf.nextExpected());
}

// R1 regression: a single packet with an out-of-range sequence must NOT spin forceFlush
// for ~2³² iterations. The unbounded loop would emit ~2 billion silence gaps here (a DoS
// hang / OOM); the R1 cap bounds the silence-gap run to windowSize and still emits the far
// packet, jumping nextExpected past it. Completes instantly.
TEST(PacketReorderBuffer, HighSequenceGapDoesNotRunaway) {
    Sink s;
    const std::size_t window = 8;
    PacketReorderBuffer buf(window, 10, s.emitter());  // 10 ms hold
    buf.insertAt(pkt(0), 1000);                 // baseline — emit "0", nextExpected=1
    const std::int32_t farSeq = 2000000000;     // ~2e9, well within uint32; ~2e9-slot span
    buf.insertAt(pkt(farSeq), 1000);            // 1 < window -> buffered, no window flush
    buf.checkTimeoutAt(2000);                   // 1000 ms >= 10 ms hold -> forceFlush

    // Bounded: exactly `window` silence gaps (the cap), NOT (farSeq - 1).
    EXPECT_EQ(static_cast<std::int64_t>(window), buf.gapsEmitted());
    ASSERT_EQ(window + 2u, s.size());           // "0" + window GAPs + the far packet
    EXPECT_EQ("0", s.items.front());
    for (std::size_t i = 1; i <= window; ++i) EXPECT_EQ("GAP", s.items[i]) << "slot " << i;
    EXPECT_EQ(std::to_string(farSeq), s.items.back());
    EXPECT_EQ(static_cast<std::int64_t>(farSeq) + 1, buf.nextExpected());
    EXPECT_EQ(0u, buf.bufferedCount());
}

TEST(PacketReorderBuffer, ResetWorks) {
    Sink s;
    PacketReorderBuffer buf(8, 30, s.emitter());
    buf.insert(pkt(0));
    buf.insert(pkt(2));  // buffered
    buf.reset();
    EXPECT_EQ(0u, buf.bufferedCount());
    EXPECT_EQ(-1, buf.nextExpected());
    EXPECT_EQ(0, buf.packetsReordered());
    s.items.clear();
    buf.insert(pkt(10));
    EXPECT_EQ((std::vector<std::string>{"10"}), s.items);
}

}  // namespace
