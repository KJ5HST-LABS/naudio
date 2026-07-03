// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — AudioStreamServer over loopback.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// AudioStreamServer over loopback (FakeBackend / inject-only). Hardware-free.
// This file holds the basic lifecycle/handshake/reject coverage; the multi-client GATE
// (byte-identical fan-out, TX arbitration, roster, writer-bridge) is appended in the next
// commit.

#include "naudio/net/AudioStreamServer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "naudio/AudioPacket.hpp"
#include "naudio/AudioStreamConfig.hpp"
#include "naudio/ControlMessage.hpp"
#include "naudio/DeviceBackend.hpp"
#include "naudio/FakeBackend.hpp"
#include "naudio/Stream.hpp"
#include "naudio/Types.hpp"
#include "naudio/net/TcpClientTransport.hpp"
#include "naudio/net/Transport.hpp"
#include "naudio/net/UdpClientTransport.hpp"

using namespace naudio;
using namespace naudio::net;

namespace {

// Receives frames from a connection until one matches `type` (and, for Control, `ctype`),
// or the budget elapses. Returns nullopt on timeout or a closed connection.
std::optional<AudioPacket> recvUntil(ClientConnection& c, PacketType type,
                                     std::optional<ControlType> ctype, int budgetMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        ReceiveResult r = c.receivePacket(100);
        if (r.closed) return std::nullopt;
        if (!r.hasPacket()) continue;
        if (r.packet->packetType() != type) continue;
        if (type == PacketType::Control && ctype.has_value()) {
            auto msg = ControlMessage::deserialize(r.packet->payload());
            if (!msg || msg->messageType() != *ctype) continue;
        }
        return std::move(r.packet);
    }
    return std::nullopt;
}

// Sends CONNECT_REQUEST and waits for AUDIO_CONFIG + CONNECT_ACCEPT (the server sends config
// first, then accept). Returns true on a completed handshake.
bool clientHandshake(ClientConnection& c, const std::string& name) {
    if (!c.sendControl(ControlMessage::connectRequest(name, AudioPacket::VERSION))) return false;
    auto cfg = recvUntil(c, PacketType::Control, ControlType::AudioConfig, 3000);
    if (!cfg.has_value()) return false;
    auto acc = recvUntil(c, PacketType::Control, ControlType::ConnectAccept, 3000);
    return acc.has_value();
}

// Waits until clientCount() reaches `n` (bounded).
bool waitForClientCount(const AudioStreamServer& server, int n, int budgetMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (server.clientCount() == n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return server.clientCount() == n;
}

// Drains frames until a CLIENTS_UPDATE with clientCount == expectedCount arrives. Reaching
// this state means every session has registered as a broadcast target (the roster broadcast
// fires after addTarget + streaming), so a subsequent injectAudio fans out to all of them.
bool waitForClientsUpdate(ClientConnection& c, int expectedCount, int budgetMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        ReceiveResult r = c.receivePacket(100);
        if (r.closed) return false;
        if (!r.hasPacket() || r.packet->packetType() != PacketType::Control) continue;
        auto msg = ControlMessage::deserialize(r.packet->payload());
        if (!msg || msg->messageType() != ControlType::ClientsUpdate) continue;
        auto info = msg->parseClientsUpdate();
        if (info.has_value() && info->clientCount == expectedCount) return true;
    }
    return false;
}

// A capture stream that fills each read with a fixed byte and paces itself so the capture
// loop does not spin at 100% CPU (FakeCaptureStream returns immediately on a blocking read).
class PacedCaptureStream : public CaptureStream {
public:
    PacedCaptureStream(AudioFormat fmt, std::uint8_t fill) : fmt_(fmt), fill_(fill) {}
    IoResult read(void* buffer, int frames, int /*timeoutMs*/) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::memset(buffer, fill_, static_cast<std::size_t>(frames) * fmt_.frameSize());
        IoResult r;
        r.frames = frames;
        return r;
    }
    const AudioFormat& actualFormat() const override { return fmt_; }

private:
    AudioFormat fmt_;
    std::uint8_t fill_;
};

// A backend whose capture stream is paced (0xA5) — a hardware-free stand-in for a real radio
// RX device that does not spin the capture loop.
class PacedBackend : public DeviceBackend {
public:
    std::vector<RawDevice> enumerate() override { return {}; }
    bool probeFormat(int, const AudioFormat&, Direction) override { return true; }
    std::unique_ptr<CaptureStream> openCaptureStream(int, const AudioFormat& fmt) override {
        return std::make_unique<PacedCaptureStream>(fmt, 0xA5);
    }
    std::unique_ptr<PlaybackStream> openPlaybackStream(int, const AudioFormat& fmt) override {
        return std::make_unique<FakePlaybackStream>(fmt);  // unused (no playback device set)
    }
};

}  // namespace

TEST(Server, StartsAndStopsInjectOnly) {
    AudioStreamServer server{0};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    EXPECT_TRUE(server.isRunning());
    EXPECT_GT(server.port(), 0);
    EXPECT_FALSE(server.hasClient());

    server.stop();
    EXPECT_FALSE(server.isRunning());
    EXPECT_EQ(server.port(), -1);
}

// No capture device and not inject-only: the server rejects every client immediately.
TEST(Server, RejectsWhenNoCaptureDevice) {
    AudioStreamServer server{0};  // no capture device, not inject-only
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    TcpClientTransport client;
    auto cc = client.connect("127.0.0.1", static_cast<std::uint16_t>(server.port()), 2000, &err);
    ASSERT_TRUE(cc) << err;

    // The reject is sent right after accept (before any handshake).
    auto reject = recvUntil(*cc, PacketType::Control, ControlType::ConnectReject, 2000);
    EXPECT_TRUE(reject.has_value());
    EXPECT_EQ(server.clientCount(), 0);

    server.stop();
}

// Inject-only single client: handshake completes and an injected payload arrives as AUDIO_RX.
TEST(Server, SingleClientHandshakeAndInjectedRx) {
    AudioStreamServer server{0};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    TcpClientTransport client;
    auto cc = client.connect("127.0.0.1", static_cast<std::uint16_t>(server.port()), 2000, &err);
    ASSERT_TRUE(cc) << err;
    ASSERT_TRUE(clientHandshake(*cc, "tester"));
    ASSERT_TRUE(waitForClientCount(server, 1, 2000));

    std::vector<std::uint8_t> payload = {0x10, 0x20, 0x30, 0x40};
    server.injectAudio(payload);

    auto rx = recvUntil(*cc, PacketType::AudioRx, std::nullopt, 2000);
    ASSERT_TRUE(rx.has_value());
    EXPECT_EQ(rx->payload(), payload);

    server.stop();
}

// maxClients enforced: the third client is rejected BUSY while two are connected.
TEST(Server, MaxClientsRejectsBusy) {
    AudioStreamConfig config{};
    config.maxClients = 2;
    AudioStreamServer server{0, config};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    TcpClientTransport c1, c2, c3;
    auto cc1 = c1.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(cc1);
    ASSERT_TRUE(clientHandshake(*cc1, "one"));
    ASSERT_TRUE(waitForClientCount(server, 1, 2000));

    auto cc2 = c2.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(cc2);
    ASSERT_TRUE(clientHandshake(*cc2, "two"));
    ASSERT_TRUE(waitForClientCount(server, 2, 2000));

    auto cc3 = c3.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(cc3);
    auto reject = recvUntil(*cc3, PacketType::Control, ControlType::ConnectReject, 2000);
    EXPECT_TRUE(reject.has_value());
    EXPECT_EQ(server.clientCount(), 2);

    server.stop();
}

// ===========================================================================
// Multi-client loopback over the server.
// ===========================================================================

// THE GATE (fan-out): three raw clients — two TCP and one UDP, via the 3a/3b transports —
// complete the handshake; an injected payload fans out byte-identically to all three.
TEST(Server, GateThreeClientsByteIdenticalBroadcast) {
    AudioStreamConfig config{};
    config.transportType = TransportType::Dual;  // serve TCP + UDP on one port
    config.maxClients = 8;
    AudioStreamServer server{0, config};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    TcpClientTransport t1, t2;
    UdpClientTransport u3;
    auto c1 = t1.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(c1) << err;
    auto c2 = t2.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(c2) << err;
    auto c3 = u3.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(c3) << err;
    ASSERT_TRUE(clientHandshake(*c1, "one"));
    ASSERT_TRUE(clientHandshake(*c2, "two"));
    ASSERT_TRUE(clientHandshake(*c3, "three"));

    // Each client waits until the roster reaches 3 — proving all three are registered
    // broadcast targets before we inject.
    ASSERT_TRUE(waitForClientsUpdate(*c1, 3, 3000));
    ASSERT_TRUE(waitForClientsUpdate(*c2, 3, 3000));
    ASSERT_TRUE(waitForClientsUpdate(*c3, 3, 3000));

    std::vector<std::uint8_t> p1 = {0xAA, 0xBB, 0xCC, 0xDD};
    std::vector<std::uint8_t> p2 = {0x01, 0x02, 0x03, 0x04, 0x05};
    server.injectAudio(p1);
    server.injectAudio(p2);

    for (ClientConnection* c : {c1.get(), c2.get(), c3.get()}) {
        auto rx1 = recvUntil(*c, PacketType::AudioRx, std::nullopt, 3000);
        ASSERT_TRUE(rx1.has_value());
        EXPECT_EQ(rx1->payload(), p1);
        auto rx2 = recvUntil(*c, PacketType::AudioRx, std::nullopt, 3000);
        ASSERT_TRUE(rx2.has_value());
        EXPECT_EQ(rx2->payload(), p2);
    }

    server.stop();
}

// THE GATE (device seam): a real capture source (PacedBackend, 0xA5) drives the broadcaster's
// capture thread; the bytes fan out byte-identically to two clients.
TEST(Server, GateCaptureDeviceFansOutByteIdentical) {
    PacedBackend backend;
    AudioStreamConfig config{};
    config.maxClients = 4;
    AudioStreamServer server{0, config};
    server.setBackend(&backend);
    server.setCaptureDevice(0);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    TcpClientTransport t1, t2;
    auto c1 = t1.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(c1) << err;
    auto c2 = t2.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(c2) << err;
    ASSERT_TRUE(clientHandshake(*c1, "one"));
    ASSERT_TRUE(clientHandshake(*c2, "two"));
    ASSERT_TRUE(waitForClientsUpdate(*c1, 2, 3000));
    ASSERT_TRUE(waitForClientsUpdate(*c2, 2, 3000));

    auto rx1 = recvUntil(*c1, PacketType::AudioRx, std::nullopt, 3000);
    auto rx2 = recvUntil(*c2, PacketType::AudioRx, std::nullopt, 3000);
    ASSERT_TRUE(rx1.has_value());
    ASSERT_TRUE(rx2.has_value());
    EXPECT_EQ(rx1->payload().size(), static_cast<std::size_t>(config.bytesPerFrame()));
    for (auto byte : rx1->payload()) EXPECT_EQ(byte, 0xA5);
    EXPECT_EQ(rx1->payload(), rx2->payload());  // byte-identical fan-out

    server.stop();
}

// THE GATE (TX arbitration): A claims TX (TX_GRANTED); B (equal priority) is denied exactly
// once per episode (no spam).
TEST(Server, GateTxArbitrationGrantAndDeniedOnce) {
    AudioStreamConfig config{};
    config.maxClients = 4;
    config.txIdleTimeoutMs = 5000;  // A keeps ownership through the test
    AudioStreamServer server{0, config};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    TcpClientTransport ta, tb;
    auto a = ta.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(a);
    auto b = tb.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(b);
    ASSERT_TRUE(clientHandshake(*a, "A"));
    ASSERT_TRUE(clientHandshake(*b, "B"));
    ASSERT_TRUE(waitForClientsUpdate(*a, 2, 3000));
    ASSERT_TRUE(waitForClientsUpdate(*b, 2, 3000));

    std::vector<std::uint8_t> tx(64, 0x55);
    ASSERT_TRUE(a->sendTxAudio(tx.data(), tx.size()));
    auto granted = recvUntil(*a, PacketType::Control, ControlType::TxGranted, 3000);
    EXPECT_TRUE(granted.has_value());
    EXPECT_FALSE(server.txOwner().empty());

    // B cannot preempt an equal-priority owner -> TX_DENIED (once).
    ASSERT_TRUE(b->sendTxAudio(tx.data(), tx.size()));
    auto denied = recvUntil(*b, PacketType::Control, ControlType::TxDenied, 3000);
    EXPECT_TRUE(denied.has_value());

    // A second submit from B does NOT produce a second TX_DENIED (denied-once per episode).
    ASSERT_TRUE(b->sendTxAudio(tx.data(), tx.size()));
    auto deniedAgain = recvUntil(*b, PacketType::Control, ControlType::TxDenied, 600);
    EXPECT_FALSE(deniedAgain.has_value());

    server.stop();
}

// THE GATE (idle release): a client that claims TX and goes idle is released by the
// independent idle thread (TX_RELEASED). Bounded wait.
TEST(Server, GateTxIdleReleaseEndToEnd) {
    AudioStreamConfig config{};
    config.txIdleTimeoutMs = 100;
    AudioStreamServer server{0, config};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    TcpClientTransport ta;
    auto a = ta.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(a);
    ASSERT_TRUE(clientHandshake(*a, "A"));
    ASSERT_TRUE(waitForClientCount(server, 1, 2000));

    std::vector<std::uint8_t> tx(64, 0x55);
    ASSERT_TRUE(a->sendTxAudio(tx.data(), tx.size()));
    ASSERT_TRUE(recvUntil(*a, PacketType::Control, ControlType::TxGranted, 3000).has_value());

    // Stop sending — the idle thread releases within ~(idle timeout + 500ms poll).
    // Budget: 100ms timeout + 500ms poll cadence + phase alignment + TCP delivery
    // leaves too little scheduler headroom at 2000 on a loaded CI runner; 5000
    // matches the other gates' budgets and costs nothing when green.
    auto released = recvUntil(*a, PacketType::Control, ControlType::TxReleased, 5000);
    EXPECT_TRUE(released.has_value());
    EXPECT_EQ(server.txOwner(), "");

    server.stop();
}

// THE GATE (roster): a disconnect broadcasts an updated CLIENTS_UPDATE to the survivors.
TEST(Server, GateRosterUpdatesOnDisconnect) {
    AudioStreamConfig config{};
    config.maxClients = 4;
    AudioStreamServer server{0, config};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    TcpClientTransport ta, tb;
    auto a = ta.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(a);
    auto b = tb.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(b);
    ASSERT_TRUE(clientHandshake(*a, "A"));
    ASSERT_TRUE(clientHandshake(*b, "B"));
    ASSERT_TRUE(waitForClientsUpdate(*b, 2, 3000));

    a->close();  // A disconnects
    EXPECT_TRUE(waitForClientsUpdate(*b, 1, 3000));
    EXPECT_TRUE(waitForClientCount(server, 1, 2000));

    server.stop();
}

// THE GATE (writer-bridge, the #1 correctness risk §3.2): a client that stops draining its
// socket does NOT stall the fan-out to the others. Without the per-session writer thread, the
// broadcaster's inject path would block on the wedged client and every client would starve.
TEST(Server, GateWriterBridgeSlowClientDoesNotStallOthers) {
    AudioStreamConfig config{};
    config.maxClients = 4;
    AudioStreamServer server{0, config};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    TcpClientTransport ta, tb;
    auto a = ta.connect("127.0.0.1", port, 2000, &err);  // keeps draining
    ASSERT_TRUE(a);
    auto b = tb.connect("127.0.0.1", port, 2000, &err);  // will stop draining
    ASSERT_TRUE(b);
    ASSERT_TRUE(clientHandshake(*a, "A"));
    ASSERT_TRUE(clientHandshake(*b, "B"));
    ASSERT_TRUE(waitForClientsUpdate(*a, 2, 3000));
    ASSERT_TRUE(waitForClientsUpdate(*b, 2, 3000));
    // From here B never reads again: its socket buffer fills and its writer thread blocks.

    std::vector<std::uint8_t> frame(static_cast<std::size_t>(config.bytesPerFrame()), 0x5A);
    const int kFrames = 400;
    for (int i = 0; i < kFrames; i++) server.injectAudio(frame);

    // A keeps receiving despite B being wedged — proving the broadcaster only enqueues per
    // session and never blocks on a slow client's socket.
    int received = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (received < 100 && std::chrono::steady_clock::now() < deadline) {
        auto rx = recvUntil(*a, PacketType::AudioRx, std::nullopt, 500);
        if (!rx.has_value()) break;
        ++received;
    }
    EXPECT_GE(received, 100);

    server.stop();
}
