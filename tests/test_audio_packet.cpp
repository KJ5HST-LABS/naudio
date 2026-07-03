// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — 0xAF01 wire codec (AudioPacket on-wire layout + CRC).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
//
// Locks the AudioPacket on-wire layout + CRC to the golden `frame` vectors
// (byte-for-byte) and exercises the reject paths. The conformance harness runs
// these same vectors from the INI; these unit tests pin the codec independently
// of the loader so a codec regression fails even without the conformance dir.
#include "naudio/AudioPacket.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using naudio::AudioPacket;
using naudio::PacketType;

namespace {

std::vector<std::uint8_t> hexToBytes(const std::string& s) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::string bytesToHex(const std::vector<std::uint8_t>& b) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (std::uint8_t x : b) {
        s.push_back(kHex[x >> 4]);
        s.push_back(kHex[x & 0xF]);
    }
    return s;
}

// Builds a packet with a pinned (deterministic) timestamp for byte-exact vectors.
AudioPacket pinned(PacketType type, std::int32_t seq, std::vector<std::uint8_t> payload,
                   std::uint8_t flags = 0) {
    AudioPacket p(type, seq, std::move(payload));
    p.setFlags(flags);
    p.setTimestamp(0);
    return p;
}

}  // namespace

TEST(AudioPacket, SerializeHeartbeatVectorByteExact) {
    // frame-heartbeat-seq5: type=3 seq=5 flags=0 ts=0 payload=empty.
    auto p = pinned(PacketType::Heartbeat, 5, {});
    EXPECT_EQ("af010103000000000500000000000000000000156994f7", bytesToHex(p.serialize()));
}

TEST(AudioPacket, SerializeAudioRxVectorByteExact) {
    // frame-audio-rx-seq1: type=0 seq=1 payload=deadbeef.
    auto p = pinned(PacketType::AudioRx, 1, hexToBytes("deadbeef"));
    EXPECT_EQ("af010100000000000100000000000000000004deadbeef94802704", bytesToHex(p.serialize()));
}

TEST(AudioPacket, SerializeLowBandwidthFlagVectorByteExact) {
    // frame-audio-rx-lowbw-flag: type=0 seq=2 flags=2 payload=deadbeef.
    auto p = pinned(PacketType::AudioRx, 2, hexToBytes("deadbeef"), AudioPacket::FLAG_LOW_BANDWIDTH);
    EXPECT_EQ("af010100020000000200000000000000000004deadbeeff1494b8c", bytesToHex(p.serialize()));
}

TEST(AudioPacket, RoundTripPreservesFields) {
    auto p = pinned(PacketType::Control, 12345, hexToBytes("040000bb801002001400640028012c"));
    auto bytes = p.serialize();
    auto decoded = AudioPacket::deserialize(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(PacketType::Control, decoded->packetType());
    EXPECT_EQ(12345, decoded->sequence());
    EXPECT_EQ(0, decoded->flags());
    EXPECT_EQ(0, decoded->timestamp());
    EXPECT_EQ(p.payload(), decoded->payload());
}

TEST(AudioPacket, DeserializeAcceptVectorFields) {
    auto frame = hexToBytes("af010100000000000100000000000000000004deadbeef94802704");
    auto decoded = AudioPacket::deserialize(frame);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(PacketType::AudioRx, decoded->packetType());
    EXPECT_EQ(1, decoded->sequence());
    EXPECT_EQ(hexToBytes("deadbeef"), decoded->payload());
}

TEST(AudioPacket, RejectBadMagic) {
    // frame-neg-bad-magic: 23 zero bytes.
    EXPECT_FALSE(
        AudioPacket::deserialize(hexToBytes("0000000000000000000000000000000000000000000000"))
            .has_value());
}

TEST(AudioPacket, RejectBadCrc) {
    // frame-neg-bad-crc: valid header, corrupted final CRC byte.
    EXPECT_FALSE(
        AudioPacket::deserialize(hexToBytes("af010100000000000100000000000000000004deadbeef948027fb"))
            .has_value());
}

TEST(AudioPacket, RejectTooShort) {
    // frame-neg-too-short: 10 bytes (< HEADER_SIZE + CRC_SIZE = 23).
    EXPECT_FALSE(AudioPacket::deserialize(hexToBytes("00000000000000000000")).has_value());
}

TEST(AudioPacket, RejectUnknownTypeByte) {
    // Start from a valid heartbeat, flip the type byte to an unknown value, and
    // the frame must reject (CRC would also fail, but the type guard fires).
    auto frame = hexToBytes("af010103000000000500000000000000000000156994f7");
    frame[3] = 0x09;  // unknown type
    EXPECT_FALSE(AudioPacket::deserialize(frame).has_value());
}

TEST(AudioPacket, TypeValueRoundTrip) {
    for (auto t : {PacketType::AudioRx, PacketType::AudioTx, PacketType::Control,
                   PacketType::Heartbeat, PacketType::FecParity}) {
        auto v = static_cast<std::uint8_t>(t);
        auto back = naudio::packetTypeFromValue(v);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(t, *back);
    }
    EXPECT_FALSE(naudio::packetTypeFromValue(0x99).has_value());
}

TEST(AudioPacket, Constants) {
    EXPECT_EQ(0xAF01u, AudioPacket::MAGIC);
    EXPECT_EQ(1u, AudioPacket::VERSION);
    EXPECT_EQ(19u, AudioPacket::HEADER_SIZE);
    EXPECT_EQ(16384u, AudioPacket::MAX_PAYLOAD);
    EXPECT_EQ(23u, AudioPacket::packetSize(0));
}
