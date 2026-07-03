// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — framing FSM (AudioProtocolHandler).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// The framing FSM (AudioProtocolHandler) driven over a real loopback
// socket pair. Crafted byte streams exercise the resilience the FSM exists for:
// clean round-trips, partial-read resumption, magic resync (mid-buffer + split
// across the read boundary), oversized-payload skip, and the 5-consecutive-error
// abort (and its reset on a good frame). Hardware-free.

#include "naudio/net/AudioProtocolHandler.hpp"
#include "naudio/net/Socket.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using namespace naudio;       // AudioPacket, PacketType
using namespace naudio::net;  // Socket, AudioProtocolHandler

namespace {

// A loopback link: a raw `writer` socket whose bytes feed an AudioProtocolHandler
// wrapping the server-accepted end.
struct Link {
    Socket listener;
    Socket writer;
    std::unique_ptr<AudioProtocolHandler> handler;
};

bool makeLink(Link& link) {
    std::string err;
    link.listener = Socket::listenTcp("", 0, true, &err);
    if (!link.listener.valid()) return false;
    std::uint16_t port = link.listener.localPort();
    link.writer = Socket::connectTcp("127.0.0.1", port, 2000, &err);
    if (!link.writer.valid()) return false;
    Socket accepted;
    if (link.listener.acceptTcp(2000, accepted, &err) != IoStatus::Ok) return false;
    link.handler = std::make_unique<AudioProtocolHandler>(std::move(accepted));
    return true;
}

// Drives the FSM until a frame decodes (returns it), the connection dies
// (nullopt + handler closed), or maxCalls is exhausted (nullopt).
std::optional<AudioPacket> recvFrame(AudioProtocolHandler& h, int maxCalls = 64) {
    for (int i = 0; i < maxCalls; i++) {
        ReceiveResult r = h.receivePacket(2000);
        if (r.hasPacket()) return std::move(r.packet);
        if (r.closed) return std::nullopt;
    }
    return std::nullopt;
}

// A frame with valid magic but a caller-chosen payloadLen header field — used to
// forge an oversized-payload header without allocating a real packet.
std::vector<std::uint8_t> forgeHeader(std::uint16_t payloadLen) {
    std::vector<std::uint8_t> h(AudioPacket::HEADER_SIZE, 0);
    h[0] = 0xAF;
    h[1] = 0x01;
    h[2] = AudioPacket::VERSION;
    h[3] = 0x00;  // type AudioRx (value irrelevant — the payload is skipped)
    h[17] = static_cast<std::uint8_t>((payloadLen >> 8) & 0xFF);
    h[18] = static_cast<std::uint8_t>(payloadLen & 0xFF);
    return h;
}

}  // namespace

TEST(ProtocolHandler, RoundTripsEachPacketType) {
    Link link;
    ASSERT_TRUE(makeLink(link));
    auto& h = *link.handler;

    // AUDIO_RX with a payload.
    std::vector<std::uint8_t> audio = {10, 20, 30, 40};
    auto rx = AudioPacket::createRxAudio(5, audio).serialize();
    ASSERT_TRUE(link.writer.sendAll(rx.data(), rx.size()));
    auto got = recvFrame(h);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->packetType(), PacketType::AudioRx);
    EXPECT_EQ(got->sequence(), 5);
    EXPECT_EQ(got->payload(), audio);

    // CONTROL.
    auto ctrl = AudioPacket::createControl(6, ControlMessage::connectAccept().serialize()).serialize();
    ASSERT_TRUE(link.writer.sendAll(ctrl.data(), ctrl.size()));
    got = recvFrame(h);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->packetType(), PacketType::Control);

    // HEARTBEAT (zero payload).
    auto hb = AudioPacket::createHeartbeat(7).serialize();
    ASSERT_TRUE(link.writer.sendAll(hb.data(), hb.size()));
    got = recvFrame(h);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->packetType(), PacketType::Heartbeat);

    EXPECT_EQ(h.packetsReceived(), 3);
    EXPECT_EQ(h.crcErrors(), 0);
}

TEST(ProtocolHandler, ResumesAcrossPartialReadTimeout) {
    Link link;
    ASSERT_TRUE(makeLink(link));
    auto& h = *link.handler;

    auto frame = AudioPacket::createRxAudio(1, {1, 2, 3, 4, 5, 6}).serialize();
    std::size_t split = 9;  // mid-header

    // Send the first part only; a short receive deadline must time out with the
    // partial bytes preserved (noData, not dead).
    ASSERT_TRUE(link.writer.sendAll(frame.data(), split));
    ReceiveResult r = h.receivePacket(150);
    EXPECT_FALSE(r.hasPacket());
    EXPECT_FALSE(r.closed);

    // Send the remainder; the FSM resumes byte-exact and decodes the frame.
    ASSERT_TRUE(link.writer.sendAll(frame.data() + split, frame.size() - split));
    auto got = recvFrame(h);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->sequence(), 1);
    EXPECT_EQ(got->payload(), (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6}));
}

TEST(ProtocolHandler, ResyncsPastLeadingGarbage) {
    Link link;
    ASSERT_TRUE(makeLink(link));
    auto& h = *link.handler;

    std::vector<std::uint8_t> stream(30, 0xFF);  // garbage, no magic
    auto frame = AudioPacket::createRxAudio(42, {7, 7, 7}).serialize();
    stream.insert(stream.end(), frame.begin(), frame.end());

    ASSERT_TRUE(link.writer.sendAll(stream.data(), stream.size()));
    auto got = recvFrame(h);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->sequence(), 42);
    EXPECT_GT(h.crcErrors(), 0);  // the garbage header(s) counted as errors
}

TEST(ProtocolHandler, ResyncsWhenMagicSplitAcrossBufferBoundary) {
    Link link;
    ASSERT_TRUE(makeLink(link));
    auto& h = *link.handler;

    // 19 garbage bytes whose LAST byte is the magic hi (0xAF) — the magic-lo
    // (0x01) only arrives in the next read, so the match straddles the boundary.
    std::vector<std::uint8_t> stream(AudioPacket::HEADER_SIZE, 0x00);
    stream.back() = 0xAF;

    // The valid frame's first byte is also 0xAF; supplying frame[1..] after the
    // kept 0xAF reconstructs the exact frame bytes (so its CRC validates).
    auto frame = AudioPacket::createRxAudio(99, {5, 5}).serialize();
    ASSERT_EQ(frame[0], 0xAF);
    stream.insert(stream.end(), frame.begin() + 1, frame.end());

    ASSERT_TRUE(link.writer.sendAll(stream.data(), stream.size()));
    auto got = recvFrame(h);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->sequence(), 99);
    EXPECT_EQ(got->payload(), (std::vector<std::uint8_t>{5, 5}));
}

TEST(ProtocolHandler, SkipsOversizedPayloadThenDecodesNextFrame) {
    Link link;
    ASSERT_TRUE(makeLink(link));
    auto& h = *link.handler;

    // A header claiming a payload just over MAX_PAYLOAD, followed by that many
    // junk bytes (payload+CRC) the FSM must skip to stay in sync.
    std::uint16_t big = static_cast<std::uint16_t>(AudioPacket::MAX_PAYLOAD + 1);
    auto hdr = forgeHeader(big);
    ASSERT_TRUE(link.writer.sendAll(hdr.data(), hdr.size()));
    std::vector<std::uint8_t> junk(big + AudioPacket::CRC_SIZE, 0x5A);
    ASSERT_TRUE(link.writer.sendAll(junk.data(), junk.size()));

    // A clean frame after the junk must decode (the skip realigned the stream).
    auto frame = AudioPacket::createHeartbeat(123).serialize();
    ASSERT_TRUE(link.writer.sendAll(frame.data(), frame.size()));

    auto got = recvFrame(h, 256);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->packetType(), PacketType::Heartbeat);
    EXPECT_EQ(got->sequence(), 123);
    EXPECT_FALSE(h.isClosed());
}

TEST(ProtocolHandler, FiveConsecutiveCrcErrorsAbortConnection) {
    Link link;
    ASSERT_TRUE(makeLink(link));
    auto& h = *link.handler;

    // Five frames with valid magic/length but a corrupted CRC byte.
    for (int i = 0; i < 5; i++) {
        auto bad = AudioPacket::createRxAudio(i, {1, 2, 3}).serialize();
        bad.back() ^= 0xFF;  // break the CRC
        ASSERT_TRUE(link.writer.sendAll(bad.data(), bad.size()));
    }

    bool died = false;
    for (int i = 0; i < 64 && !died; i++) {
        ReceiveResult r = h.receivePacket(2000);
        if (r.closed) died = true;
        EXPECT_FALSE(r.hasPacket());
    }
    EXPECT_TRUE(died);
    EXPECT_TRUE(h.isClosed());
    EXPECT_GE(h.crcErrors(), 5);
}

TEST(ProtocolHandler, GoodFrameResetsConsecutiveErrorCounter) {
    Link link;
    ASSERT_TRUE(makeLink(link));
    auto& h = *link.handler;

    // Four bad frames (one short of the abort threshold)...
    for (int i = 0; i < 4; i++) {
        auto bad = AudioPacket::createRxAudio(i, {9}).serialize();
        bad.back() ^= 0xFF;
        ASSERT_TRUE(link.writer.sendAll(bad.data(), bad.size()));
    }
    // ...then a good one, which must decode AND reset the counter.
    auto good = AudioPacket::createRxAudio(77, {4, 4}).serialize();
    ASSERT_TRUE(link.writer.sendAll(good.data(), good.size()));

    auto got = recvFrame(h, 64);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->sequence(), 77);
    EXPECT_FALSE(h.isClosed());

    // Four more bad frames still do not abort (counter was reset to 0). Drive
    // only until all four are consumed (crcErrors climbs by 4) — the frames are
    // already buffered, so this never blocks on the recv deadline.
    int targetErrs = h.crcErrors() + 4;
    for (int i = 0; i < 4; i++) {
        auto bad = AudioPacket::createRxAudio(i, {9}).serialize();
        bad.back() ^= 0xFF;
        ASSERT_TRUE(link.writer.sendAll(bad.data(), bad.size()));
    }
    bool died = false;
    for (int i = 0; i < 32 && !died && h.crcErrors() < targetErrs; i++) {
        ReceiveResult r = h.receivePacket(300);
        if (r.closed) died = true;
    }
    EXPECT_FALSE(died);
    EXPECT_FALSE(h.isClosed());
}
