// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — wire codec primitives (CRC-32 + ByteCursor).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
//
// Pins the ISO-HDLC CRC-32 to the python-zlib reference (the `crc-kat-check`
// conformance vector) and round-trips the big-endian ByteReader/ByteWriter. The
// CRC is the unforgiving part — nail it first.
#include "naudio/ByteCursor.hpp"
#include "naudio/Crc32.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using naudio::ByteReader;
using naudio::ByteWriter;

namespace {

std::vector<std::uint8_t> bytes(std::initializer_list<int> v) {
    std::vector<std::uint8_t> out;
    for (int b : v) out.push_back(static_cast<std::uint8_t>(b));
    return out;
}

}  // namespace

TEST(Crc32, KnownAnswer123456789) {
    // The CRC-32/ISO-HDLC check value for "123456789" — exactly what
    // zlib, gzip, and any standard CRC-32/ISO-HDLC implementation produce. This is the
    // `crc-kat-check` conformance vector (inputHex 313233343536373839).
    const std::string s = "123456789";
    EXPECT_EQ(0xCBF43926u,
              naudio::crc32(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
}

TEST(Crc32, EmptyInputIsZero) {
    EXPECT_EQ(0u, naudio::crc32(nullptr, 0));
}

TEST(Crc32, HeartbeatFrameHeaderRegion) {
    // The 19-byte header of the heartbeat frame vector (af0101 03 00 seq=5 ts=0
    // plen=0); its CRC over header+payload is 0x156994f7 (S604 gotcha 4 / the
    // `frame-heartbeat-seq5` vector). Computing it here proves the codec CRC is
    // byte-aligned with the frame vectors before AudioPacket is layered on top.
    auto header = bytes({0xAF, 0x01, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_EQ(0x156994F7u, naudio::crc32(header));
}

TEST(ByteCursor, WriteThenReadBigEndian) {
    ByteWriter w;
    w.putU8(0xAB);
    w.putU16(0xBEEF);
    w.putI32(-2);                 // 0xFFFFFFFE
    w.putI64(0x0102030405060708);
    w.putBytes(bytes({0xDE, 0xAD}));

    auto buf = w.bytes();
    // 1 + 2 + 4 + 8 + 2 = 17 bytes.
    ASSERT_EQ(17u, buf.size());

    ByteReader r(buf);
    EXPECT_EQ(0xAB, r.getU8());
    EXPECT_EQ(0xBEEF, r.getU16());
    EXPECT_EQ(-2, r.getI32());
    EXPECT_EQ(0x0102030405060708, r.getI64());
    auto tail = r.readBytes(2);
    EXPECT_EQ(bytes({0xDE, 0xAD}), tail);
    EXPECT_EQ(0u, r.remaining());
}

TEST(ByteCursor, U16IsUnsigned) {
    // getU16 is the `getShort() & 0xFFFF` idiom — 0xFFFF reads as 65535, not -1.
    auto buf = bytes({0xFF, 0xFF});
    ByteReader r(buf);
    EXPECT_EQ(0xFFFF, r.getU16());
}

TEST(ByteCursor, ReadStringAndFramingAdvance) {
    ByteWriter w;
    const std::string msg = "N0CALL";
    w.putU8(static_cast<std::uint8_t>(msg.size()));
    w.putBytes(reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size());

    ByteReader r(w.bytes());
    std::size_t len = r.getU8();
    ASSERT_EQ(6u, len);
    EXPECT_EQ("N0CALL", r.readString(len));
    EXPECT_EQ(0u, r.remaining());
}

TEST(ByteCursor, OverReadClampsRatherThanFaults) {
    auto buf = bytes({0x01});
    ByteReader r(buf);
    EXPECT_EQ(0x01, r.getU8());
    // Past the end: clamps to zero-fill, remaining() stays 0.
    EXPECT_EQ(0, r.getU8());
    EXPECT_EQ(0u, r.remaining());
    EXPECT_TRUE(r.readBytes(4).empty());
}
