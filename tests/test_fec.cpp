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

}  // namespace
