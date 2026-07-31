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
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "naudio/AudioPacket.hpp"
#include "naudio/AudioStreamConfig.hpp"
#include "naudio/ControlMessage.hpp"
#include "naudio/ControlReliability.hpp"
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

// ServerStats is a GAUGE OVER THE LIVE ROSTER, not a monotonic lifetime total. The aggregation
// sums over the transport's routing map, and disconnectClient erases the departed connection
// from it — so a client leaving takes its counters out of the sum and the totals go DOWN.
//
// This arm exists because that sentence is written into the ServerStats contract and into the
// na_server_stats contract in the public C header, where a consumer will build a rate on top of
// it. A counter that silently decreases is exactly the kind of claim that must be observed
// rather than reasoned about, so this measures the decrease instead of asserting a shape.
//
// It is bounded in BOTH directions against a quantity the library does not compute: `sent`,
// counted here from the frames this test itself injected and fanned out.
TEST(Server, GateServerStatsIsARosterGaugeNotALifetimeTotal) {
    AudioStreamConfig config{};
    config.maxClients = 4;
    AudioStreamServer server{0, config};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    // Before any client: running, but an empty roster sums to nothing.
    const ServerStats idle = server.stats();
    EXPECT_TRUE(idle.running);
    EXPECT_EQ(idle.clientsConnected, 0);
    EXPECT_EQ(idle.packetsSent, 0);

    TcpClientTransport ta, tb;
    auto a = ta.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(a);
    auto b = tb.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(b);
    ASSERT_TRUE(clientHandshake(*a, "A"));
    ASSERT_TRUE(clientHandshake(*b, "B"));
    ASSERT_TRUE(waitForClientsUpdate(*b, 2, 3000));
    ASSERT_TRUE(waitForClientCount(server, 2, 2000));

    // Drive traffic to BOTH clients so each connection carries a non-trivial count.
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(config.bytesPerFrame()), 0x5A);
    const int kFrames = 60;
    for (int i = 0; i < kFrames; i++) server.injectAudio(frame);
    // Drain both so the writer threads actually complete their sends before we read.
    for (int i = 0; i < 30; i++) {
        (void)recvUntil(*a, PacketType::AudioRx, std::nullopt, 500);
        (void)recvUntil(*b, PacketType::AudioRx, std::nullopt, 500);
    }

    const ServerStats both = server.stats();
    EXPECT_TRUE(both.running);
    EXPECT_EQ(both.clientsConnected, 2);
    // LOWER bound, against a quantity the library never sees: each of the two sessions was
    // handed kFrames injected frames, and every session also sends handshake control traffic.
    EXPECT_GE(both.packetsSent, 2 * kFrames);
    EXPECT_GT(both.bytesSent, both.packetsSent);  // every packet carries a header + payload

    // THE MEASUREMENT: one client leaves. Its counters leave the sum with it.
    a->close();
    ASSERT_TRUE(waitForClientCount(server, 1, 3000));
    // The routing-map erase happens in the session's close path, just after the roster erase
    // clientCount() observes, so give the transport a moment to catch up before reading.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    ServerStats one = server.stats();
    while (one.packetsSent >= both.packetsSent &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        one = server.stats();
    }

    EXPECT_EQ(one.clientsConnected, 1);
    // The whole point: STRICTLY LESS than the previous read. This is the assertion that would
    // fail if the aggregation were ever changed to accumulate departed clients — at which point
    // the contract in naudio.h must change with it.
    EXPECT_LT(one.packetsSent, both.packetsSent)
        << "ServerStats::packetsSent did not decrease when a client left: " << both.packetsSent
        << " -> " << one.packetsSent << ". The roster-gauge contract in AudioStreamServer.hpp "
        << "and na_server_stats in naudio.h both promise it does.";
    // UPPER bound, again independent of the library's own arithmetic: what remains is one
    // client's share, so it cannot still hold both clients' worth of fan-out.
    EXPECT_LT(one.packetsSent, 2 * kFrames);

    // After stop() there is no transport, so it reads as not-running defaults rather than a
    // stale final total — the same "check this before believing a zero" rule as ClientStats.
    server.stop();
    const ServerStats stopped = server.stats();
    EXPECT_FALSE(stopped.running);
    EXPECT_EQ(stopped.packetsSent, 0);
    EXPECT_EQ(stopped.clientsConnected, 0);
}

// TCP contributes a STRUCTURAL zero to both of the counters this struct exists for — it has no
// control-ARQ layer and no ordered queue, so the events cannot occur rather than occurring
// uncounted. Pinning it here means the UDP arms that provoke them are measuring something TCP
// could never have supplied, and a future defaulted-to-0 override on the TCP side stays honest.
TEST(Server, ServerStatsControlAndQueueCountersAreZeroOnTcp) {
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
    for (int i = 0; i < 20; i++) server.injectAudio(payload);
    for (int i = 0; i < 10; i++) (void)recvUntil(*cc, PacketType::AudioRx, std::nullopt, 500);

    const ServerStats s = server.stats();
    EXPECT_TRUE(s.running);
    EXPECT_EQ(s.clientsConnected, 1);
    EXPECT_GT(s.packetsSent, 0);  // the connection is live, so the zeros below are not vacuous
    EXPECT_EQ(s.controlRetransmits, 0);
    EXPECT_EQ(s.queueDrops, 0);

    server.stop();
}

// controlRetransmits OBSERVED NON-ZERO ON A SERVER CONNECTION — the acceptance item #29 was
// filed with, and the reason ServerStats exists at all.
//
// The counter has been live on this class since it was written and had never been seen to move
// on a real AudioStreamServer session: the only existing coverage drives it at the CLASS level
// by feeding a connection a NACK (tests/net/test_udp_connection.cpp), which proves the mechanism
// and says nothing about whether a server reaches it. "The code path is live" is a property of
// the class; "the role under test can reach it" is a property of the role.
//
// The provocation needs no lossy relay. UdpReliabilityConfig::controlReliabilityEnabled is what
// builds the ControlReliability object, and it builds BOTH the sender-side pending ring and the
// receiver-side ACK generator. So a client configured with it OFF never emits a CONTROL_ACK,
// while the server — on udpLan(), which turns it ON — tracks every critical control it sends and
// finds them all still unacked when the retransmit sweep runs. The sweep is pumped from
// shouldSendHeartbeat() in the session run loop, which ticks once a second against a 500 ms RTO,
// so the arm must outlast several ticks.
//
// Bounded in BOTH directions against a quantity the server never computes: the client's own
// count of critical control datagrams, split into distinct sequences (D) and total arrivals (T).
// Every retransmit is the same packet resent with the same sequence, so T - D is the number of
// resends that reached the wire, measured entirely on the far side of the socket.
TEST(Server, GateServerControlRetransmitsObservedNonZero) {
    AudioStreamConfig config = AudioStreamConfig::udpLan();  // controlReliabilityEnabled = true
    config.maxClients = 4;
    ASSERT_TRUE(config.controlReliabilityEnabled) << "the provocation depends on the server ARQ";
    AudioStreamServer server{0, config};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    // The client: passthrough (so duplicate sequences are not swallowed by a reorder buffer)
    // and control reliability OFF, which is what makes it silent on ACKs.
    UdpReliabilityConfig ccfg;
    ccfg.reorderWindowSize = 0;
    ccfg.controlReliabilityEnabled = false;
    UdpClientTransport client{ccfg};
    auto cc = client.connect("127.0.0.1", static_cast<std::uint16_t>(server.port()), 2000, &err);
    ASSERT_TRUE(cc) << err;
    ASSERT_TRUE(cc->sendControl(ControlMessage::connectRequest("noack", AudioPacket::VERSION)));
    ASSERT_TRUE(waitForClientCount(server, 1, 3000));

    // Collect every critical control the server sends for long enough to outlast the sweep.
    std::set<std::int32_t> distinctCritical;
    long long totalCritical = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(6000);
    while (std::chrono::steady_clock::now() < deadline) {
        ReceiveResult r = cc->receivePacket(200);
        if (r.closed) break;
        if (!r.hasPacket() || r.packet->packetType() != PacketType::Control) continue;
        auto msg = ControlMessage::deserialize(r.packet->payload());
        if (!msg || !ControlReliability::isCriticalType(msg->messageType())) continue;
        ++totalCritical;
        distinctCritical.insert(r.packet->sequence());
    }

    const ServerStats s = server.stats();
    const long long D = static_cast<long long>(distinctCritical.size());
    const long long T = totalCritical;
    const long long observedResends = T - D;

    ASSERT_GT(D, 0) << "the server sent no critical control at all — the arm proves nothing";
    // The headline: it moves on a server, where it is structurally pinned to 0 on a client.
    EXPECT_GT(s.controlRetransmits, 0)
        << "control_retransmits stayed 0 on a SERVER connection. distinct=" << D
        << " total=" << T;

    // LOWER bound: every duplicate the client saw was a resend the server performed. The server
    // increments as it queues the packet, so its count can only lead what reached the far side.
    EXPECT_GE(s.controlRetransmits, observedResends)
        << "server counted fewer resends than the client actually received: "
        << s.controlRetransmits << " < " << observedResends;

    // UPPER bound: ControlReliability erases a pending entry once attempts reach maxAttempts, so
    // no single critical control can be resent more than that many times.
    EXPECT_LE(s.controlRetransmits, D * config.controlRetransmitMaxAttempts)
        << "server counted more resends than " << D << " criticals x "
        << config.controlRetransmitMaxAttempts << " attempts allows";

    server.stop();
}

// queue_drops on a SERVER: reachable in principle, unreached in practice — and the reason is
// not the one #29 gives.
//
// The issue argues the counter moves on a server because "a demux thread fills the queue while
// the application thread drains it", i.e. that the producer/consumer split is sufficient. It is
// not. Measured here: 20k TX packets pushed as fast as the socket accepts them arrive complete
// and leave queue_drops at 0, because the session's receive path is non-blocking END TO END by
// design (the writer-bridge decision, §3.2) — handleTxAudio only writes into the mixer's ring
// buffer, so the drain keeps pace with the demux thread and the 2048-packet queue never backs up.
//
// That the counter WORKS was established separately and in two places: its arithmetic at the
// class level (test_udp_connection.cpp, OrderedQueueBoundedUnderFlood — 200 dropped past a
// 2048 cap), and its wiring through ServerStats by stalling the consumer 1 ms per packet in a
// throwaway mutation, which produced 57115 drops from 60001 received. So a 0 here is a real
// measurement of a real server, not a broken counter.
//
// Committed as a NEGATIVE on purpose, the same way c_client_stats.c pins the client-side
// impossibility: it keeps the limitation executable. If a future change puts a blocking step
// back on the receive path, this arm is what notices.
TEST(Server, ServerQueueDropsStayZeroWhileTheDrainKeepsUp) {
    AudioStreamConfig config = AudioStreamConfig::udpLan();
    config.maxClients = 4;
    AudioStreamServer server{0, config};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    UdpReliabilityConfig ccfg;
    ccfg.reorderWindowSize = 0;
    UdpClientTransport client{ccfg};
    auto cc = client.connect("127.0.0.1", static_cast<std::uint16_t>(server.port()), 2000, &err);
    ASSERT_TRUE(cc) << err;
    ASSERT_TRUE(cc->sendControl(ControlMessage::connectRequest("flood", AudioPacket::VERSION)));
    ASSERT_TRUE(waitForClientCount(server, 1, 3000));

    std::vector<std::uint8_t> pcm(960, 0x11);
    const int kSend = 20000;
    int sent = 0;
    for (int i = 0; i < kSend; i++) {
        if (cc->sendTxAudio(pcm.data(), pcm.size())) ++sent;
    }
    ASSERT_GT(sent, kSend / 2) << "the flood never left the client; the arm proves nothing";

    // Let the backlog settle, then read once.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const ServerStats s = server.stats();

    // The counter is bounded by what actually arrived, whatever the machine's speed — a
    // relationship, not a threshold, so it holds on a loaded CI runner too.
    EXPECT_LE(s.queueDrops, s.packetsReceived);

    // The headline is conditional on the premise it depends on: drops are 0 BECAUSE the drain
    // kept up. On a machine slow enough to fall behind, the premise fails and asserting 0 would
    // be asserting something this arm never established.
    if (s.packetsReceived >= sent) {
        EXPECT_EQ(s.queueDrops, 0)
            << "the server drained every one of " << sent << " flooded packets yet still "
            << "reported queue drops — the receive path has acquired a blocking step";
    } else {
        GTEST_SKIP() << "drain fell behind (" << s.packetsReceived << " of " << sent
                     << "); the zero-drop premise does not hold on this machine";
    }

    server.stop();
}
