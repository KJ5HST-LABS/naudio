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

#include "ScriptedTransport.hpp"

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
//
// NOT a barrier for injectAudio, and it is weaker than it looks: sessions_ is populated at accept
// (AudioStreamServer.cpp:659), BEFORE the session's run thread is even started at :662. So this is
// already satisfied while the server is still waiting to read CONNECT_REQUEST, and it adds no
// ordering whatsoever over the clientHandshake() that callers put on the line above it. The session
// does not become a broadcast target until :224, fourteen lines after the CONNECT_ACCEPT the
// handshake waits for. Use waitForClientsUpdate below to order an inject. (Issue #46.)
bool waitForClientCount(const AudioStreamServer& server, int n, int budgetMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (server.clientCount() == n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return server.clientCount() == n;
}

// Drains frames until a CLIENTS_UPDATE with clientCount == expectedCount arrives. This is the
// barrier a subsequent injectAudio needs — but it is PER-CONNECTION, not per-roster.
//
// What makes it sound is the receiving end, not the sending end: ClientSession::sendControlMessage
// (AudioStreamServer.cpp:88-90) drops the roster message unless the RECEIVING session's own
// streaming_ is set, and streaming_ is set at :240, after that same session's addTarget at :224.
// So arrival on `c` proves `c` is a registered broadcast target. It proves nothing about the other
// expectedCount-1 sessions the message is merely counting — those are in sessions_ from accept
// time and may still be short of :224. Every client that must receive an injected frame therefore
// needs its own wait; waiting on one and injecting for two is issue #46's defect.
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
    // The roster count is not this arm's precondition — see waitForClientCount above. One frame is
    // injected, once, and a frame injected before addTarget fans out to nothing and is not retried.
    ASSERT_TRUE(waitForClientsUpdate(*cc, 1, 3000));

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
//
// ON THE PRE-DRAIN WINDOW (#49). The one-shot sendTxAudio below is not ordered against the
// session's drain: clientHandshake returns when the server sends CONNECT_ACCEPT
// (AudioStreamServer.cpp:210) and the receiveLoop thread is not started until :234, while
// waitForClientCount adds no ordering at all (see its comment at the top of this file). The
// window is real and it is TRANSPORT-INDEPENDENT — TCP is not what makes this arm safe, which is
// worth writing down because it is the natural assumption. On UDP a pre-drain datagram is not
// discarded either: the demux thread parks it in the same per-connection ordered queue
// (UdpServerTransport.cpp:102), which is precisely how the UDP handshake works at all — the
// CONNECT_REQUEST that CREATES the connection is enqueued before any ClientSession exists.
//
// What actually makes it safe is runLoop's ordering: mixer->registerClient (:225) and the writer
// thread (:229) both precede the receiveLoop spawn (:234), so any AUDIO_TX the drain can possibly
// see is already past registration and the grant's reply path is already live. What TCP adds is
// only that the holding buffer is lossless — a flow-controlled kernel socket buffer — where
// UDP's is a 2048-packet oldest-drop queue behind a socket given 8 datagrams of headroom, and an
// AUDIO_TX carries no control ARQ, so a lost one-shot send is never retried. At this arm's depth
// of a single 64-byte frame that difference cannot bite; at flood depth it does, which is why
// ServerQueueDropsStayZeroWhileTheDrainKeepsUp waits on TX_GRANTED instead.
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
    // BOTH waits are load-bearing: each proves only its own connection reached addTarget, and this
    // arm's bound counts the fan-out to both. Waiting on b alone left A free to still be short of
    // AudioStreamServer.cpp:224 when the 60 frames went out — measured here at 2 runs in 20 with a
    // 300 ms delay injected before addTarget, and deterministic when only A's session is delayed.
    ASSERT_TRUE(waitForClientsUpdate(*a, 2, 3000));
    ASSERT_TRUE(waitForClientsUpdate(*b, 2, 3000));
    ASSERT_TRUE(waitForClientCount(server, 2, 2000));

    // Drive traffic to BOTH clients so each connection carries a non-trivial count.
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(config.bytesPerFrame()), 0x5A);
    const int kFrames = 60;
    for (int i = 0; i < kFrames; i++) server.injectAudio(frame);
    // Wait on the COMPLETION CONDITION, not on a fixed drain count: keep receiving from both
    // clients until the server reports every injected frame fanned out to both of them. A fixed
    // count cannot establish that — it was 30 iterations for kFrames=60 per client, so it
    // guaranteed at most half the sends and left the rest to whatever the socket buffers
    // happened to absorb. That read 106 of 120 on the Windows runner (issue #44).
    //
    // The draining is load-bearing and CANNOT be replaced by a plain sleep-and-poll on stats():
    // writerLoop() calls sendAll(), which BLOCKS once the socket buffers fill, so a reader that
    // stops reading stalls this counter permanently rather than merely delaying it. Measured on
    // macOS loopback: with no draining at all, packetsSent stalls at 290 of 2000 and stays flat
    // for a full 4 s, while draining as we poll reaches 2000 in 94 ms. This arm's own 230 KB per
    // client fits under macOS's ~556 KB buffer, which is precisely why the fixed count passed
    // here and failed there — the old loop measured the runner's buffer size, not completion.
    const auto sendDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (server.stats().packetsSent < 2 * kFrames &&
           std::chrono::steady_clock::now() < sendDeadline) {
        (void)recvUntil(*a, PacketType::AudioRx, std::nullopt, 100);
        (void)recvUntil(*b, PacketType::AudioRx, std::nullopt, 100);
    }

    const ServerStats both = server.stats();
    EXPECT_TRUE(both.running);
    EXPECT_EQ(both.clientsConnected, 2);
    // LOWER bound, against a quantity the library never sees: each of the two sessions was
    // handed kFrames injected frames, and every session also sends handshake control traffic.
    EXPECT_GE(both.packetsSent, 2 * kFrames)
        << "the fan-out never completed within the deadline: " << both.packetsSent << " of "
        << 2 * kFrames << " expected sends. This is a completion failure, not a threshold miss "
        << "— every frame injected above is queued to an unbounded per-session deque, so the "
        << "count reaches the bound unless a writer thread is stuck or dead.";
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
    // Same barrier as the arms above. This arm's assertions would survive losing every injected
    // frame — packetsSent is non-zero from handshake traffic alone — so the roster count did not
    // make it FAIL, it made the EXPECT_GT below vacuous in exactly the way its comment denies, and
    // cost 5 s of timeouts doing it (measured under an injected addTarget delay).
    ASSERT_TRUE(waitForClientsUpdate(*cc, 1, 3000));

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
// not. Measured here: 20k TX packets pushed as fast as the socket accepts them leave queue_drops
// at 0, because the session's receive path is non-blocking END TO END by design (the
// writer-bridge decision, §3.2) — handleTxAudio only writes into the mixer's ring buffer, so the
// drain keeps pace with the demux thread and the 2048-packet queue never backs up.
//
// WHY THE HEADLINE IS NOT GATED ON `packetsReceived >= sent` (issue #49). That field cannot
// express "the drain kept up": the DEMUX thread increments it as the FIRST statement of
// enqueueReceived (UdpClientConnection.cpp:175), ahead of every branch that could then discard
// the packet and ahead of the queue on either path — note udpLan() gives the SERVER connection a
// reorder buffer (reorderBufferSize = 8, copied to cfg.reorderWindowSize at
// AudioStreamServer.cpp:435), so the offer that feeds the queue here is the reorder emit callback
// at UdpClientConnection.cpp:81, not the passthrough one at :205; the client's own
// reorderWindowSize = 0 below governs only the client. And offer evicts rather than blocks, so
// the drain applies no back-pressure and every drop is a packet that already counted. Measured by
// stalling receiveLoop 1 ms per packet: packets_received 20001, UNCHANGED from a healthy run,
// alongside 17859 queue drops. A dead drain therefore SATISFIES that guard rather than falsifying
// it. (The one way the drain could ever move the number is indirectly, by letting a connection be
// reaped — ServerStats sums the LIVE roster, so a departure subtracts; that needs a 10 s
// CONNECTION_TIMEOUT_MS and cannot happen inside this arm's ~1 s.) What the guard's skip branch
// really keyed on was arrival loss upstream of the queue — and that is what silently disabled
// this arm on Linux for its whole life: ubuntu-latest delivers ~14.5k of 20000 because the
// loopback receive buffer is capped at net.core.rmem_max = 212992, and the shortfall equalled the
// kernel's own UDP RcvbufErrors delta exactly, three runs of three. macOS loses none, which is
// the only reason the assertion ever ran at all.
//
// So the premise is stated over the queue's own input instead, and ASSERTED rather than skipped:
// a 0 has teeth only if more than kDefaultMaxSize packets were enqueued, since below that the
// queue could not have overflowed even with a dead drain. Measured margin 7.1x on Linux (the
// loss-heaviest platform) and 9.8x on macOS. A skip is invisible in a green summary; this arm's
// whole value is being an executable negative, so it now runs on every platform or fails loudly.
//
// That the counter WORKS was established separately and in two places: its arithmetic at the
// class level (test_udp_connection.cpp, OrderedQueueBoundedUnderFlood — 200 dropped past a
// 2048 cap), and its wiring through ServerStats by the 1 ms consumer stall above (which is also
// this arm's positive control: with it applied, the assertion below fails). So a 0 here is a real
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

    std::vector<std::uint8_t> pcm(960, 0x11);

    // BARRIER: prove the drain is EXECUTING before flooding it. waitForClientCount does not —
    // sessions_ is populated at accept, before the run thread starts (see its comment above), so
    // it is satisfied while the server has yet to read CONNECT_REQUEST. Nor would
    // waitForClientsUpdate: the roster broadcast is sent at AudioStreamServer.cpp:241, after the
    // receiveLoop thread is CONSTRUCTED at :234, and construction is not execution — nothing
    // between :237 and :241 synchronises with that thread, and receiveLoop sets no state before
    // it blocks in receivePacket. TX_GRANTED is the one observable that is only reachable THROUGH
    // the drain: receiveLoop -> handleTxAudio -> AudioMixer::submitTxAudio -> claimTxChannelLocked
    // -> onTxGranted. Its arrival is proof the loop ran, not that a thread object exists.
    ASSERT_TRUE(cc->sendTxAudio(pcm.data(), pcm.size()));
    ASSERT_TRUE(recvUntil(*cc, PacketType::Control, ControlType::TxGranted, 3000).has_value())
        << "no TX_GRANTED: nothing proves the session's receiveLoop ever consumed a packet, so "
        << "the flood below would be racing the drain into existence";

    const int kSend = 20000;
    int sent = 0;
    for (int i = 0; i < kSend; i++) {
        if (cc->sendTxAudio(pcm.data(), pcm.size())) ++sent;
    }
    ASSERT_GT(sent, kSend / 2) << "the flood never left the client; the arm proves nothing";

    // Let the backlog settle, then read once.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const ServerStats s = server.stats();

    // Holds BY CONSTRUCTION under enqueue-time counting — every drop is a packet that incremented
    // packets_received on the same connection first — so this detects nothing and is kept only as
    // an executable statement of that subset relationship, which the header does not spell out.
    EXPECT_LE(s.queueDrops, s.packetsReceived);

    // The premise, over the queue's own input rather than over what the client believes it sent.
    // packets_received counts the whole session, so it runs 2 ahead of the flood (CONNECT_REQUEST
    // and the barrier's own TX frame) — never compare it to `sent` as though they were the
    // same population; the capacity is what it is measured against.
    ASSERT_GT(s.packetsReceived, static_cast<std::int64_t>(BlockingPacketQueue::kDefaultMaxSize))
        << s.packetsReceived << " packets reached the server's queue (the client got " << sent
        << " of " << kSend << " onto the wire), fewer than its "
        << BlockingPacketQueue::kDefaultMaxSize
        << "-packet capacity — so it could not have overflowed even with a dead drain, and a 0 "
        << "below would prove nothing. This is arrival loss UPSTREAM of the queue (kernel "
        << "socket-buffer overflow, a rejected sender, a CRC failure at crc_errors="
        << s.crcErrors << "), never drain lag: packets_received is incremented at enqueue";

    EXPECT_EQ(s.queueDrops, 0)
        << "the server enqueued " << s.packetsReceived << " packets, "
        << (s.packetsReceived / static_cast<std::int64_t>(BlockingPacketQueue::kDefaultMaxSize))
        << "x the queue's capacity, and still reported " << s.queueDrops << " queue drops — the "
        << "receive path has acquired a blocking step";

    server.stop();
}

// ---------------------------------------------------------------------------------------
// Issue #67 — the server's TX provenance hop, and the DeclinedRecovered fall-through.
//
// These arms drive a REAL ClientSession (real handshake, real receive/writer threads, real
// AudioMixer arbitration) over a supplied transport, and hand its receive path a frame
// marked as reconstructed. That mark is not a wire field, so no real peer can produce it and
// no induced-loss relay can put one in front of an UNOWNED mixer — see the reasoning in
// tests/net/ScriptedTransport.hpp, which also names the arm covering the other half of the
// chain (that a real connection marks a repaired frame at all).
//
// Where the behaviour they pin is written down, stated precisely because the two halves have
// different standing: "TX_DENIED is sent once per denial episode" is NORMATIVE
// (docs/audio-streaming-protocol-v1.md:323), while the treatment of locally reconstructed
// frames is the non-normative implementation note in the same section — the wire cannot see
// a repair, so it cannot mandate anything about one. The binding promise for a consumer is
// the one in the frozen public C header (include/naudio.h, the provenance paragraph), and
// that is what these arms hold the server to.
//
// EVERY SUBJECT HERE IS A NON-EVENT — nothing claimed, nothing sent. A passing run is
// therefore not evidence the arms discriminate; only the mutation sweep is. Each arm
// carries three separately-named guards for that reason: one proving the harness RAN, one
// proving the FAULT was actually injected, and (for the absence subjects) one proving the
// recorder could have seen the thing being asserted absent.
// ---------------------------------------------------------------------------------------

namespace {

// Long enough that no arm here can outlive a TX lease. An M1-style regression claims the
// channel and the idle thread would release it again within a second — which would let an
// "owner is still empty" subject pass on a tree where the claim demonstrably happened.
constexpr std::int64_t kNoIdleRelease = 3'600'000;

// Frame payload; contents are irrelevant to arbitration, only provenance is.
const std::vector<std::uint8_t> kTxFrame(320, 0x5A);

// Stands a server up on a scripted transport with the TX lease pinned open.
struct ScriptedFixture {
    std::shared_ptr<naudio::test::ScriptedServerTransport> transport =
        std::make_shared<naudio::test::ScriptedServerTransport>();
    AudioStreamConfig config = [] {
        AudioStreamConfig c;
        c.txIdleTimeoutMs = kNoIdleRelease;
        return c;
    }();
    AudioStreamServer server{0, config};

    ScriptedFixture() {
        server.setInjectOnlyMode(true);
        auto t = transport;
        server.setTransportFactory([t]() { return t; });
    }
    ~ScriptedFixture() { server.stop(); }
};

// Connects one scripted client and returns it once it is REGISTERED WITH THE MIXER.
//
// The barrier is deliberately ClientsUpdate and not ConnectAccept: ConnectAccept is sent at
// AudioStreamServer.cpp:209-210, BEFORE mixer->registerClient (:225), and a TX frame that
// arrives in that window takes the mixer's unknown-client early return
// (src/net/AudioMixer.cpp:72-79), where a live frame is Rejected rather than Accepted. That
// inverts the signature the ownership subject below is reading, so an arm racing this window
// could report green on a mutated tree. broadcastClientsUpdate (:241) runs after
// registration, so its message is proof the window is closed.
std::shared_ptr<naudio::test::ScriptedClientConnection> connectScripted(
    naudio::test::ScriptedServerTransport& transport, const std::string& id) {
    auto conn = std::make_shared<naudio::test::ScriptedClientConnection>(id);
    conn->pushConnectRequest();
    transport.offer(conn);
    if (!conn->waitForControl(ControlType::ClientsUpdate, 1, 5000)) return nullptr;
    return conn;
}

// Delivers one TX frame and returns when the session has FINISHED handling it.
//
// The probe is the barrier. handleControlMessage answers a LatencyProbe with a direct
// connection_->sendControl on the receive thread (AudioStreamServer.cpp:378), and
// receiveLoop is strictly sequential (:266-289) — so the response cannot be recorded until
// the frame pushed before it has been through handleTxAudio and the mixer. This is a
// happens-after edge in production code, not an observation of the double's own counters.
bool deliverTx(naudio::test::ScriptedClientConnection& conn, Provenance provenance,
               int probeOrdinal) {
    conn.push(AudioPacket::createTxAudio(100 + probeOrdinal, kTxFrame), provenance);
    conn.pushLatencyProbe();
    return conn.waitForControl(ControlType::LatencyResponse, probeOrdinal, 5000);
}

}  // namespace

// The fixture's own detector. Deliberately NOT a discriminator for the provenance mutations:
// it is here so that when the doubles break, something red names THAT rather than leaving the
// provenance arms to fail for a reason they do not describe.
TEST(Server, AScriptedTransportFactoryDrivesARealClientSession) {
    ScriptedFixture fx;
    std::string err;
    ASSERT_TRUE(fx.server.start(&err)) << err;

    // The injected transport is the one in use — a sentinel port no real transport reports.
    EXPECT_EQ(fx.server.port(), naudio::test::ScriptedServerTransport::kScriptedPort);

    auto conn = connectScripted(*fx.transport, "scripted-a");
    ASSERT_TRUE(conn) << "no ClientsUpdate: a real session never registered with the mixer";

    // A real handshake ran over the double: both messages runLoop sends on acceptance.
    EXPECT_EQ(conn->countSent(ControlType::AudioConfig), 1) << conn->diagnostics();
    EXPECT_EQ(conn->countSent(ControlType::ConnectAccept), 1) << conn->diagnostics();
    EXPECT_EQ(fx.server.connectedClientIds(), std::vector<std::string>{"audio-1"});
    EXPECT_GT(fx.transport->acceptCalls(), 0);
}

// The third state the transport-factory branch creates (L131): set, and returning nothing.
// Without the guard in start() this is a null dereference reachable from public API.
TEST(Server, ATransportFactoryReturningNullFailsStartCleanly) {
    AudioStreamServer server{0};
    server.setInjectOnlyMode(true);
    server.setTransportFactory([]() { return std::shared_ptr<ServerTransport>{}; });

    std::string err;
    EXPECT_FALSE(server.start(&err));
    EXPECT_FALSE(err.empty()) << "a failed start must say why";
    EXPECT_FALSE(server.isRunning());
}

// M1 — the receive path must carry provenance to the mixer.
//
// Subject: a reconstructed frame arriving at an UNOWNED channel must not claim it. Carries
// NO assertion about TX_DENIED; that observable belongs to the next arm, so that each
// mutation reddens an arm named after it (L45 — two assertions in one arm are not two
// detectors, the earlier masks the later).
TEST(Server, ARecoveredTxFrameDoesNotClaimAnUnownedTxChannel) {
    ScriptedFixture fx;
    std::string err;
    ASSERT_TRUE(fx.server.start(&err)) << err;
    auto conn = connectScripted(*fx.transport, "scripted-a");
    ASSERT_TRUE(conn) << "no ClientsUpdate: a real session never registered with the mixer";

    // PRECONDITION: the row under test is the mixer's unowned row.
    ASSERT_EQ(fx.server.txOwner(), "") << "premise: nobody owns the TX channel";

    ASSERT_TRUE(deliverTx(*conn, Provenance::Recovered, 1)) << conn->diagnostics();

    // GUARD "the fault was injected". Distinct from the guard above, which only proves the
    // harness ran. This one proves the frame under test was presented as RECONSTRUCTED — the
    // failure mode it exists for is an arm that quietly tested a live frame against a live
    // expectation and reported green (issue #67 attempt 1 shipped exactly that shape).
    // Asserted as the whole sequence so a missing, extra or mis-marked frame all name
    // themselves rather than shifting a positional index.
    ASSERT_EQ(conn->handedOut(),
              (std::vector<naudio::test::Handed>{
                  {PacketType::Control, Provenance::Live},    // ConnectRequest
                  {PacketType::AudioTx, Provenance::Recovered},  // the subject
                  {PacketType::Control, Provenance::Live}}))  // LatencyProbe
        << conn->diagnostics();

    // SUBJECT.
    EXPECT_EQ(fx.server.txOwner(), "")
        << "a reconstructed TX frame claimed the channel — the server stopped carrying "
           "provenance from the receive path to the mixer (issue #65's defect, issue #67's gap)";

    // POSITIVE CONTROL, and it must run AFTER the subject: it takes the identical path with
    // the identical call, differing only in the value under test, which is what makes the
    // subject's silence meaningful rather than merely quiet. Running it first would claim the
    // channel and destroy the unowned precondition the subject needs (L132).
    ASSERT_TRUE(deliverTx(*conn, Provenance::Live, 2)) << conn->diagnostics();
    EXPECT_EQ(fx.server.txOwner(), "audio-1")
        << "control: a LIVE frame down the same path did not claim the channel either, so the "
           "subject above proves nothing about provenance";
}

// G3 — DeclinedRecovered must fall through both branches of handleTxAudio.
//
// The contract is enforced by code that is ABSENT (there is no branch for it), so there is
// nothing to grep and no diff when it breaks. Subject carries NO ownership assertion.
TEST(Server, ARecoveredTxFrameOnAnUnownedChannelSendsNoTxDenied) {
    ScriptedFixture fx;
    std::string err;
    ASSERT_TRUE(fx.server.start(&err)) << err;
    auto conn = connectScripted(*fx.transport, "scripted-a");
    ASSERT_TRUE(conn) << "no ClientsUpdate: a real session never registered with the mixer";
    ASSERT_EQ(fx.server.txOwner(), "") << "premise: nobody owns the TX channel";

    ASSERT_TRUE(deliverTx(*conn, Provenance::Recovered, 1)) << conn->diagnostics();

    // GUARD "the fault was injected" — as above.
    ASSERT_EQ(conn->handedOut(),
              (std::vector<naudio::test::Handed>{
                  {PacketType::Control, Provenance::Live},
                  {PacketType::AudioTx, Provenance::Recovered},
                  {PacketType::Control, Provenance::Live}}))
        << conn->diagnostics();

    // GUARD "the recorder is live". The subject is an absence, so it must be impossible for
    // this ledger to be empty of TX_DENIED merely because it records nothing: the handshake
    // messages arrived through the very same sendControl the subject reads.
    ASSERT_GE(conn->countSent(ControlType::ConnectAccept), 1)
        << "the control recorder captured nothing at all, so an absence below is vacuous";

    // SUBJECT.
    EXPECT_EQ(conn->countSent(ControlType::TxDenied), 0)
        << "a declined repair sent TX_DENIED — the denial branch was widened to admit "
           "anything that is not Accepted, so a repair now spends the client's single "
           "per-episode denial (docs/audio-streaming-protocol-v1.md:323)";
}

// The spec's actual promise, end to end over two sessions, and the only arm here whose
// correct outcome contains a POSITIVE event — which is what makes its control load-bearing.
//
// Deliberately NOT orthogonal: it reddens under both provenance mutations, by two different
// routes. The two arms above are the discriminating pair; this one is the story.
TEST(Server, ARepairDoesNotSpendADeniedClientsOneTxDenied) {
    ScriptedFixture fx;
    std::string err;
    ASSERT_TRUE(fx.server.start(&err)) << err;

    // Connected serially so the id assignment is deterministic (clientIdCounter_ starts at 1).
    auto a = connectScripted(*fx.transport, "scripted-a");
    ASSERT_TRUE(a) << "client A never registered";
    ASSERT_TRUE(waitForClientCount(fx.server, 1, 3000));
    auto b = connectScripted(*fx.transport, "scripted-b");
    ASSERT_TRUE(b) << "client B never registered";
    ASSERT_TRUE(waitForClientCount(fx.server, 2, 3000));

    // A takes the channel, putting B in the mixer's cannot-preempt row (every session is
    // hard-wired NORMAL priority, docs/audio-streaming-protocol-v1.md:344).
    ASSERT_TRUE(deliverTx(*a, Provenance::Live, 1)) << a->diagnostics();
    ASSERT_EQ(fx.server.txOwner(), "audio-1") << "premise: A holds the channel";

    // SUBJECT: B's repair is declined silently.
    ASSERT_TRUE(deliverTx(*b, Provenance::Recovered, 1)) << b->diagnostics();

    // GUARD "the fault was injected", carried here too even though the two arms above are the
    // discriminating pair. Without it this arm passes with B's recovered frame never pushed at
    // all — measured — because its subject is an absence and the barrier only proves the probe
    // behind it was handled. Leaning on a sibling arm's guard is how a detector quietly stops
    // detecting.
    ASSERT_EQ(b->handedOut(),
              (std::vector<naudio::test::Handed>{
                  {PacketType::Control, Provenance::Live},       // ConnectRequest
                  {PacketType::AudioTx, Provenance::Recovered},  // the subject
                  {PacketType::Control, Provenance::Live}}))     // LatencyProbe
        << b->diagnostics();

    EXPECT_EQ(b->countSent(ControlType::TxDenied), 0)
        << "a reconstructed frame from a non-owner fabricated a denial, spending B's single "
           "per-episode TX_DENIED before B ever asked to transmit";

    // CONTROL: B's genuine attempt still earns exactly one. Same client, same recorder, same
    // production line — so it proves the subject's zero is a real silence, and it proves the
    // second-order promise directly: the repair did not consume the denial B is owed here.
    ASSERT_TRUE(deliverTx(*b, Provenance::Live, 2)) << b->diagnostics();
    EXPECT_EQ(b->countSent(ControlType::TxDenied), 1)
        << "control: B's genuine denial never arrived, so the subject's zero above says "
           "nothing about repairs";
    EXPECT_EQ(fx.server.txOwner(), "audio-1") << "A still holds the channel throughout";
}
