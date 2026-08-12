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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
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

// #77's PRODUCTION half, and the reason it needs its own arm at all: the client-side arms in
// test_client_e2e.cpp drive the SCRIPTED connection, which carries its own trySendControl, so
// none of them reaches this implementation. MEASURED — with this method stubbed to a bare
// `return false`, the entire suite stayed green at 370/370. This arm is what makes that
// mutation red.
//
// It covers two of the method's three outcomes: a free lock transmits, and a closed connection
// does not. The third — a lock HELD by another sender, which is the outcome the method exists
// for — is covered by the arm below this one (#79), against a real wedged peer, on POSIX.
TEST(ProtocolHandler, TrySendControlTransmitsOnAFreeLockAndDeclinesOnceClosed) {
    Link link;
    ASSERT_TRUE(makeLink(link));
    auto& h = *link.handler;

    // Wrap the far end so the frame can be decoded rather than merely counted — a counter
    // alone would still pass if the bytes never left.
    AudioProtocolHandler peer{std::move(link.writer)};

    ASSERT_TRUE(h.trySendControl(ControlMessage::disconnect()));
    EXPECT_EQ(h.packetsSent(), 1);

    auto got = recvFrame(peer);
    ASSERT_TRUE(got.has_value()) << "trySendControl reported success but nothing reached the peer";
    EXPECT_EQ(got->packetType(), PacketType::Control);
    auto msg = ControlMessage::deserialize(got->payload());
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->messageType(), ControlType::Disconnect);

    // The other side of the contract: it is not unconditionally true. A closed connection
    // declines through the same sendPacketLocked guard sendPacket uses.
    h.close();
    EXPECT_FALSE(h.trySendControl(ControlMessage::disconnect()));
    EXPECT_EQ(h.packetsSent(), 1) << "a send was counted on a closed connection";
}

// THE THIRD OUTCOME (#79), and the one the method exists for: a lock already HELD by another
// sender is declined rather than queued behind it.
//
// #79 filed this as impractical to stage, reasoning that a real wedge needs a peer with its
// receive window shut and that the send budget (CONNECTION_TIMEOUT_MS / 2) makes that
// expensive. MEASURED on macOS/arm64, both halves of that are wrong. A peer that simply never
// reads shuts its own window after 32 maximum-payload frames (~525 KB) in about 2 ms; and the
// budget costs nothing, because the arm never waits it out — close() breaks a parked send in
// 0 ms. The whole arm runs in ~200 ms, nearly all of it the deliberate park dwell below.
//
// MEASURED against the mutation this exists to catch — try_to_lock replaced by a plain
// lock_guard, i.e. #77 undone in the handler with the send left intact: the call returns TRUE,
// after ~4959 ms rather than 0-1 ms, having burned a sequence number and counted a send.
//
// NOT every assertion below flips, and the one that does not is the one that reads as though it
// must. See the note on premiseHeldAtCall: sendPacket releases the lock INSIDE sendAll, before
// the writer thread returns and clears its flag, so "the writer is still flagged as sending"
// remains true even when the call sat waiting for the lock. The discriminator is the settled
// writerDone check below it, which is a mechanism rather than a clock, corroborated by a
// duration bound whose threshold is derived from the send budget rather than tuned.
//
// POSIX ONLY, for the reason #74 records about the #70 budget arm: Winsock's loopback
// absorption means the fill phase does not wedge the socket there, so the premise cannot be
// staged.
//
// THE SEAM #74 PROPOSED FOR THIS NOW EXISTS — Socket::setSendBufferSize/setRecvBufferSize —
// AND IT IS NOT ENOUGH ON ITS OWN, so do not read its arrival as a green light to delete the
// gate below. MEASURED on windows-latest while un-gating the #70 arm: bounding the receive
// window does NOT wedge a Winsock loopback connection, in either arrangement (pre-handshake on
// the listener, post-accept on the accepted socket) — both absorbed an 8 MB send in ~30 ms with
// the peer reading nothing. What does stage the wedge there is SO_SNDBUF = 0, which disables
// Winsock's dynamic send buffering outright; see tests/net/test_socket.cpp. Applying that here
// is unfinished work tracked by #74, and it needs its own mutation proof against THIS arm's M1
// (try_to_lock replaced by a blocking lock_guard) rather than inheriting the #70 arm's.
#ifndef _WIN32
TEST(ProtocolHandler, TrySendControlDeclinesWhileAnotherSenderHoldsTheSendLock) {
    Link link;
    ASSERT_TRUE(makeLink(link));
    auto& h = *link.handler;

    // link.writer is deliberately NEVER read from. That is the whole wedge: its receive window
    // shuts, our send buffer fills, and the writer thread below parks inside Socket::sendAll
    // holding sendMutex_ — exactly the state a real stalled TCP peer produces.
    std::atomic<bool> inSend{false};
    std::atomic<bool> writerDone{false};
    std::vector<std::uint8_t> payload(AudioPacket::MAX_PAYLOAD, 0xA5);

    std::thread writer([&] {
        for (;;) {
            inSend.store(true);
            bool ok = h.sendRxAudio(payload.data(), 0, payload.size());
            inSend.store(false);
            if (!ok) break;
        }
        writerDone.store(true);
    });

    // close() breaks the parked send, which is what lets the writer exit. Declared before the
    // first ASSERT below on purpose: an ASSERT returns from the function, and a joinable
    // std::thread destroyed on that path calls std::terminate and takes the suite with it.
    struct Releaser {
        AudioProtocolHandler& handler;
        std::thread& thread;
        ~Releaser() {
            handler.close();
            if (thread.joinable()) thread.join();
        }
    } releaser{h, writer};

    // A send still in flight after kParkDwellMs is parked in the kernel: with room in the
    // buffer, a loopback send of one frame completes in microseconds. This samples the flag,
    // not the clock, so what it detects is the WRITER HOLDING THE LOCK rather than a slow machine.
    constexpr int kParkDwellMs = 150;
    // 30 s, and the margin is the point rather than the number. How much a peer must ignore
    // before its window shuts is a PLATFORM property, not a property of this arm: MEASURED at
    // ~2 ms on macOS/arm64 but ~4.9 s on ubuntu-latest CI (whole arm 154 ms vs 5.08 s), where
    // loopback buffers autotune far larger. A deadline sized from the macOS figure would sit
    // ~2x above the slowest platform that actually runs this, and the failure mode of being
    // wrong is a RED main via the premise guard below — so this is deliberately ~6x the worst
    // measurement rather than a round number near the best one. It is bounded well under the
    // suite's ctest --timeout 300 either way.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool parked = false;
    while (!parked && !writerDone.load() && std::chrono::steady_clock::now() < deadline) {
        if (!inSend.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const auto dwellUntil =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kParkDwellMs);
        parked = true;
        while (std::chrono::steady_clock::now() < dwellUntil) {
            if (!inSend.load()) {
                parked = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // THE PREMISE GUARD, same role as the L136 guard in test_client_e2e.cpp's #77 arm.
    // Everything below is a claim about what trySendControl does WHILE another sender holds the
    // lock; with no wedge it would win a free lock and pass having tested nothing at all.
    ASSERT_TRUE(parked) << "the writer never parked, so the arm's premise never held — the "
                           "peer's receive window did not shut within the deadline";

    const std::int64_t packetsBefore = h.packetsSent();
    const std::int32_t sequenceBefore = h.currentSequence();
    const auto start = std::chrono::steady_clock::now();
    const bool transmitted = h.trySendControl(ControlMessage::disconnect());
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    // The premise, re-checked at the moment of the call rather than only at the guard above:
    // a wedge that evaporated in between would leave the call winning a FREE lock and passing
    // without ever testing contention.
    //
    // IT IS NOT THE DISCRIMINATOR, though it reads like one. MEASURED under the M1 mutation
    // (blocking lock_guard): this still sampled TRUE, because sendPacket releases the lock
    // inside sendAll, before the writer thread returns and clears the flag — so a waiting
    // acquire can wake, send and return inside that window.
    const bool premiseHeldAtCall = inSend.load() && !writerDone.load();

    // THE DISCRIMINATOR, and it is a mechanism rather than a clock. A blocking acquire cannot
    // return until the holder releases, and the only thing that releases this holder is its
    // send budget expiring — which also ends the writer's loop. So a writer that is still
    // inside the same parked send, after a settle long enough to close the race above, proves
    // the call returned WITHOUT waiting for the lock. The settle is 50 ms against a budget of
    // CONNECTION_TIMEOUT_MS / 2 = 5000 ms, of which ~4850 remains at this point.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool writerStillSending = !writerDone.load();

    EXPECT_FALSE(transmitted) << "transmitted while another sender held sendMutex_";
    EXPECT_TRUE(premiseHeldAtCall) << "the wedge did not survive to the call, so contention "
                                      "was never actually exercised";
    EXPECT_TRUE(writerStillSending)
        << "the writer's send had already ended, so the call waited for the lock to be "
           "released rather than declining it (elapsed " << elapsedMs << " ms)";
    // Corroboration, with a threshold DERIVED rather than tuned: a waiting acquire is bounded
    // below by the holder's remaining send budget (~4850 ms here), and declining a try_lock is
    // microseconds. MEASURED: 0-1 ms shipped, 4959 ms under M1.
    EXPECT_LT(elapsedMs, 1000) << "declining took " << elapsedMs << " ms";
    EXPECT_EQ(h.packetsSent(), packetsBefore) << "a send was counted on a declined call";
    // The decline must not burn a sequence number, or it leaves a gap in a sequence space
    // nothing ever transmitted (AudioProtocolHandler.cpp allocates only after the lock is won).
    EXPECT_EQ(h.currentSequence(), sequenceBefore) << "a declined call consumed a sequence number";
}
#endif  // !_WIN32 — see the Winsock note above
