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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
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
#include "naudio/net/CallbackDispatcher.hpp"  // fence(), exercised directly at the end of this file
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

// #20: an inject larger than MAX_PAYLOAD must not be silently truncated.
//
// serialize() clamps the payload to MAX_PAYLOAD (AudioPacket.cpp:83, in serialize()). That clamp
// is correct in
// itself — the length field is a u16 and deserialize() rejects anything longer, so the encoder
// must never emit a frame the decoder would reject. What was missing is the layer above it:
// nothing SPLIT an oversized buffer, so a single injectAudio serialized to ONE clamped packet and
// the remainder was dropped with no error, no counter and no log, while every layer reported
// success. MEASURED before the fix, over this exact path: inject(70000) -> NA_OK, 16384 bytes at
// the client, 53616 gone; inject(16385) -> NA_OK, one byte gone. The fan-out now frames an
// oversized buffer instead, so every injected byte arrives.
//
// THIS ARM IS END-TO-END ON PURPOSE. A broadcaster-level arm cannot see this defect at all: the
// whole buffer does reach the BroadcastTarget today: the loss happens later, inside serialize().
// Only a real client reading real 0xAF01 frames off a real socket observes it.
TEST(Server, OversizedInjectIsFramedNotTruncated) {
    AudioStreamServer server{0};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    TcpClientTransport client;
    auto cc = client.connect("127.0.0.1", static_cast<std::uint16_t>(server.port()), 2000, &err);
    ASSERT_TRUE(cc) << err;
    ASSERT_TRUE(clientHandshake(*cc, "tester"));
    ASSERT_TRUE(waitForClientsUpdate(*cc, 1, 3000));

    // Two full chunks plus a partial, so the arm pins the full-chunk path AND the remainder —
    // a fix that framed only whole chunks would drop the tail and still look green on a
    // round multiple.
    const std::size_t kInject = AudioPacket::MAX_PAYLOAD * 2 + 1234;
    std::vector<std::uint8_t> payload(kInject);
    for (std::size_t i = 0; i < kInject; ++i) {
        // Position-derived, not constant: a constant fill cannot distinguish "all bytes arrived"
        // from "one chunk arrived three times", and cannot see reordering at all.
        payload[i] = static_cast<std::uint8_t>((i * 7 + 13) & 0xFF);
    }
    server.injectAudio(payload);

    // Reassemble across however many AUDIO_RX frames the server chose to send. The count is
    // deliberately not asserted — the contract is "every byte, in order, each frame wire-legal",
    // not a particular chunk size.
    std::vector<std::uint8_t> got;
    while (got.size() < kInject) {
        auto rx = recvUntil(*cc, PacketType::AudioRx, std::nullopt, 2000);
        ASSERT_TRUE(rx.has_value())
            << "stalled after " << got.size() << " of " << kInject << " bytes";
        EXPECT_LE(rx->payload().size(), AudioPacket::MAX_PAYLOAD)
            << "emitted a frame the decoder would reject";
        got.insert(got.end(), rx->payload().begin(), rx->payload().end());
    }
    EXPECT_EQ(got.size(), kInject);
    EXPECT_EQ(got, payload);

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

// THE #92 GATE, server side: a UDP or DUAL config with every reliability component off is refused
// at start() — before any bind — unless explicitBareUdp made it explicit. The consent arm then
// binds an ephemeral port for real, so the gate provably passes an explicit bare server through
// rather than refusing everything.
TEST(Server, BareUdpStartRefusedWithoutConsent) {
    for (TransportType tt : {TransportType::Udp, TransportType::Dual}) {
        AudioStreamConfig bare{};
        bare.transportType = tt;
        AudioStreamServer server{0, bare};
        server.setInjectOnlyMode(true);
        std::string err;
        EXPECT_FALSE(server.start(&err)) << "transport " << static_cast<int>(tt);
        EXPECT_NE(err.find("NA_RELIABILITY_UDP_BARE"), std::string::npos) << err;
        EXPECT_NE(err.find("udpBare"), std::string::npos) << err;
    }
    AudioStreamConfig consent{};
    consent.transportType = TransportType::Udp;
    consent.explicitBareUdp = true;
    AudioStreamServer ok{0, consent};
    ok.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(ok.start(&err)) << err;
    EXPECT_GT(ok.port(), 0);
    ok.stop();

    // EVERY CONJUNCT OF THE PREDICATE, individually: the gate fires only when ALL four components
    // are off, so a UDP config with any SINGLE one on must start without consent. A dropped
    // conjunct in the gate widens it and reddens exactly the config for the dropped knob — this
    // is what makes the predicate's shape testable rather than asserted.
    for (int knob = 0; knob < 4; knob++) {
        AudioStreamConfig one{};
        one.transportType = TransportType::Udp;
        switch (knob) {
            case 0: one.fecEnabled = true; break;
            case 1: one.reorderBufferSize = 8; break;
            case 2: one.adaptiveJitterEnabled = true; break;
            case 3: one.controlReliabilityEnabled = true; break;
        }
        AudioStreamServer s{0, one};
        s.setInjectOnlyMode(true);
        err.clear();
        EXPECT_TRUE(s.start(&err)) << "knob " << knob << " alone was gated: " << err;
        s.stop();
    }
}

// THE GATE (fan-out): three raw clients — two TCP and one UDP, via the 3a/3b transports —
// complete the handshake; an injected payload fans out byte-identically to all three.
TEST(Server, GateThreeClientsByteIdenticalBroadcast) {
    AudioStreamConfig config{};
    config.transportType = TransportType::Dual;  // serve TCP + UDP on one port
    config.explicitBareUdp = true;  // bare-by-request (#92): this arm tests fan-out, not the layer
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
// ServerReceiveDrainClearsItsBacklogAfterAFlood waits on TX_GRANTED instead.
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
// not. Measured here: 20k TX packets pushed as fast as the socket accepts them leave the queue
// EMPTY at the end, because the session's receive path is non-blocking END TO END by design (the
// writer-bridge decision, §3.2) — handleTxAudio only writes into the mixer's ring buffer, so the
// drain keeps pace with the demux thread and the 2048-packet queue never stays backed up.
//
// WHY THE FLOOD'S OWN DROP COUNT IS NO LONGER THE ASSERTION (issue #75). It was, for most of this
// arm's life, and it failed twice on windows-latest inside three pushes — 218 drops in 19,866
// packets, then again on the next commit — on diffs that cannot reach the server's receive path
// at all. Flood-time drops are a RACE, not a property: offer() evicts as soon as the queue holds
// kDefaultMaxSize, so a drop needs only the consumer to fall that far behind the producer ONCE.
// MEASURED here (macOS, one run each, 2026-08-11): the flood's producer runs at 9-17 packets/ms,
// which prices 2048 packets of capacity at 120-220 ms of consumer starvation, and the failing
// windows-latest run implies ~126 ms. One lost scheduling quantum buys that. A test does not own
// the relative speed of two threads, so it cannot assert an exact zero on it (L91).
//
// So the flood is kept as the STRESS and the assertion moves onto a quantity scheduling cannot
// move. After the flood settles, a SECOND burst is sent that is strictly SMALLER THAN THE QUEUE,
// and that burst must not drop. Phase-2 packets cannot overflow the queue by themselves — there
// are fewer of them than it holds — so this stays true even if the consumer is descheduled for
// the whole of phase 2. A drop there therefore proves the queue was STILL HOLDING THE FLOOD'S
// BACKLOG when they arrived, which is exactly what "the drain kept up" denies. Passing proves a
// bound rather than a zero: the backlog had fallen to at most (capacity - the burst).
//
// MEASURED, macOS, one run per row, 2026-08-11, with a stall injected into receiveLoop and the
// backlog read from a temporary counter in that same loop:
//
//     injected consumer stall    flood drops   backlog at 500 ms   this arm
//     none                                 0                   1   pass
//     one-shot,  100 ms                    0                   1   pass
//     one-shot,  150 ms                  292                   1   pass
//     one-shot,  300 ms                 1826                   1   pass
//     one-shot, 1500 ms               12,953                   1   pass
//     one-shot, 2000 ms               12,953                2049   FAIL
//     1 ms every packet               17,028                1634   FAIL (569 phase-2 drops)
//
// The 1500 and 2000 rows carry IDENTICAL flood drops and opposite verdicts, which is the whole
// change in one line: the arm no longer keys on how far the consumer once fell behind, only on
// whether the backlog was still there when phase 2 arrived. The old assertion reddens from the
// 150 ms row down — and windows-latest reported 218 drops, between this machine's 150 ms (292)
// and 300 ms (1826) rows, so that CI failure is quantitatively an ordinary scheduling stall.
// Tolerance therefore goes from a stall of ~150 ms to one of 1500 ms, >10x on the same machine
// under the same injection, while the bottom row shows the real defect is still caught.
//
// What sets the boundary is not a budget that accumulates: a healthy consumer clears a full
// 2048-packet backlog inside one 100 ms sample once it is scheduled again (the 1500 ms row's
// backlog is 1), so the arm reddens only if starvation is STILL IN EFFECT when phase 2 runs —
// which the 2000 ms row shows directly, its backlog still 2049 at the moment of the reading.
//
// SENSITIVITY, stated because this is a trade and not a strict improvement. The settle divided by
// the burst gives the smallest per-packet blocking step the arm can still resolve: 500 ms / 1024
// packets, about 0.5 ms. A stall milder than that drains inside the settle and is no longer
// caught, and the flood-wide assertion did catch it on a platform quiet enough to run it.
// Deliberate: 0.5 ms is half the 1 ms/packet stall that is this arm's own positive control, and
// every blocking step it guards against — a mutex wait, a socket send, a condition-variable wait
// — is milliseconds or worse. A shorter settle would buy sensitivity and spend starvation
// tolerance.
//
// WHY THE PREMISE IS NOT `packetsReceived >= sent` (issue #49). That field cannot express "the
// drain kept up": the DEMUX thread increments it as the FIRST statement of enqueueReceived
// (UdpClientConnection.cpp:175), ahead of every branch that could then discard the packet and
// ahead of the queue on either path — note udpLan() gives the SERVER connection a reorder buffer
// (reorderBufferSize = 8, copied to cfg.reorderWindowSize at AudioStreamServer.cpp:435), so the
// offer that feeds the queue here is the reorder emit callback at UdpClientConnection.cpp:81, not
// the passthrough one at :205; the client's own reorderWindowSize = 0 below governs only the
// client. And offer evicts rather than blocks, so the drain applies no back-pressure and every
// drop is a packet that already counted. Re-measured 2026-08-11 by stalling receiveLoop 1 ms per
// packet: packets_received 20002, IDENTICAL to a healthy run, alongside 17,028 queue drops. A
// dead drain therefore SATISFIES that guard rather than falsifying it. So the premise is stated
// over the queue's own input instead, and ASSERTED rather than skipped — a skip is invisible in a
// green summary, and this arm's whole value is being an executable negative. Measured margin 7.1x
// on Linux (the loss-heaviest platform) and 9.8x on macOS.
//
// ONE CAVEAT THAT PHASE 2 DEPENDS ON MORE THAN THE OLD SINGLE READING DID: ServerStats is a
// roster GAUGE, not a lifetime meter — it sums the LIVE roster, so a departing client subtracts
// its history (GateServerStatsIsARosterGaugeNotALifetimeTotal). Phase 2 reads DELTAS across two
// calls, so a session reaped between them would make both deltas negative. It cannot happen
// inside this arm: reaping needs a 10 s CONNECTION_TIMEOUT_MS and the arm runs in ~2.5 s, and the
// one client is sending throughout. If it ever did, the resolvability assertion below fires on a
// negative rather than letting a zero pass silently, and its message separates the two causes by
// the sign of the count.
//
// That same asymmetry is why phase 2 SIZES ITSELF by reading packets_received rather than by
// counting sends: what matters is how many packets reached the queue, and arrival loss upstream
// of it is a platform property. ubuntu-latest delivers ~14.5k of 20000 in the flood because the
// loopback receive buffer is capped at net.core.rmem_max = 212992 (the shortfall equalled the
// kernel's own UDP RcvbufErrors delta exactly, three runs of three, when #49 measured it); macOS
// loses none. A fixed send count would therefore mean a different burst on every platform.
// Because packets_received is incremented ahead of the reorder buffer it OVER-counts arrivals at
// the queue, which ends the burst early — the safe direction, since a shorter burst can only
// produce fewer drops.
//
// That the counter WORKS was established separately and in two places: its arithmetic at the
// class level (test_udp_connection.cpp, OrderedQueueBoundedUnderFlood — 200 dropped past a
// 2048 cap), and its wiring through ServerStats by the 1 ms consumer stall above, which is also
// this arm's positive control: with it applied, the phase-2 assertion below fails.
//
// Committed as a NEGATIVE on purpose, the same way c_client_stats.c pins the client-side
// impossibility: it keeps the limitation executable. If a future change puts a blocking step
// back on the receive path, this arm is what notices.
//
// Named ServerQueueDropsStayZeroWhileTheDrainKeepsUp until 2026-08-11; renamed with the #75
// rework because the surviving assertion is about the backlog clearing, not about the flood.
TEST(Server, ServerReceiveDrainClearsItsBacklogAfterAFlood) {
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

    const std::int64_t kCapacity = static_cast<std::int64_t>(BlockingPacketQueue::kDefaultMaxSize);

    // ---- Phase 1: the STRESS. Its own drop count is a two-thread race and is deliberately NOT
    // asserted on — that assertion is issue #75. What the flood must establish is only that the
    // queue was driven hard enough for phase 2's zero to mean something.
    const int kSend = 20000;
    int sent = 0;
    for (int i = 0; i < kSend; i++) {
        if (cc->sendTxAudio(pcm.data(), pcm.size())) ++sent;
    }
    ASSERT_GT(sent, kSend / 2) << "the flood never left the client; the arm proves nothing";

    // The settle. Its length is the sensitivity knob, not a pause for tidiness — see the header:
    // it sets the smallest per-packet blocking step phase 2 can resolve (kSettleMs / the burst).
    constexpr int kSettleMs = 500;
    std::this_thread::sleep_for(std::chrono::milliseconds(kSettleMs));
    const ServerStats s1 = server.stats();

    // Holds BY CONSTRUCTION under enqueue-time counting — every drop is a packet that incremented
    // packets_received on the same connection first — so this detects nothing and is kept only as
    // an executable statement of that subset relationship, which the header does not spell out.
    EXPECT_LE(s1.queueDrops, s1.packetsReceived);

    // The premise, over the queue's own input rather than over what the client believes it sent.
    // packets_received counts the whole session, so it runs 2 ahead of the flood (CONNECT_REQUEST
    // and the barrier's own TX frame) — never compare it to `sent` as though they were the
    // same population; the capacity is what it is measured against.
    ASSERT_GT(s1.packetsReceived, kCapacity)
        << s1.packetsReceived << " packets reached the server's queue (the client got " << sent
        << " of " << kSend << " onto the wire), fewer than its " << kCapacity
        << "-packet capacity — so the flood applied no stress at all, and phase 2's zero below "
        << "would prove nothing. This is arrival loss UPSTREAM of the queue (kernel "
        << "socket-buffer overflow, a rejected sender, a CRC failure at crc_errors="
        << s1.crcErrors << "), never drain lag: packets_received is incremented at enqueue";

    // ---- Phase 2: the ASSERTION. A burst that cannot overflow the queue on its own.
    //
    // Aim for half the capacity: that splits the queue evenly between the residual backlog this
    // arm tolerates (capacity - burst) and the headroom the burst needs in order to survive a
    // consumer descheduled for the whole of phase 2. Sized by ARRIVALS rather than by sends,
    // because arrival loss upstream of the queue is a platform property (see the header).
    const std::int64_t kBurstTarget = kCapacity / 2;
    // Below this the burst can no longer resolve the 1 ms/packet stall that is this arm's
    // positive control: detection needs the burst to exceed what such a consumer drains during
    // the settle. Derived from those two numbers, not picked.
    constexpr int kPositiveControlStallMsPerPacket = 1;
    const std::int64_t kMinResolvable = kSettleMs / kPositiveControlStallMsPerPacket;
    constexpr int kChunk = 64;
    // Capping the SENDS below the queue's capacity is what makes "this burst cannot overflow the
    // queue by itself" structural rather than hoped for: arrivals can never exceed sends, however
    // the platform's loss and stats lag behave.
    //
    // MEASURED that this cap is INERT on a platform with no arrival loss: widening it to 8x the
    // target leaves the arm green on macOS, because the loop reaches its target in ~1024 sends
    // and never approaches the cap. So the cap has no detector here. That is shipped knowingly —
    // the invariant it protects is asserted directly below, and that assertion IS covered (a
    // mutation of it reddens), so widening the cap on a low-loss platform fails loudly rather
    // than silently turning phase 2 back into a thread race.
    const int kMaxRawSends = static_cast<int>(kCapacity) - 1;

    int raw = 0;
    std::int64_t enqueued2 = 0;
    while (enqueued2 < kBurstTarget && raw < kMaxRawSends) {
        for (int i = 0; i < kChunk && raw < kMaxRawSends; i++, raw++) {
            cc->sendTxAudio(pcm.data(), pcm.size());
        }
        // Let the demux thread count what was just sent before deciding whether to send more;
        // without this the loop overshoots by however far the stats lag the wire.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        enqueued2 = server.stats().packetsReceived - s1.packetsReceived;
    }
    // Let the last chunk land and be counted, so a drop it caused is inside the reading below.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const ServerStats s2 = server.stats();
    enqueued2 = s2.packetsReceived - s1.packetsReceived;
    const std::int64_t drops2 = s2.queueDrops - s1.queueDrops;

    ASSERT_LT(enqueued2, kCapacity)
        << "phase 2 put " << enqueued2 << " packets into a " << kCapacity << "-packet queue in "
        << raw << " sends, so it could have overflowed on its own and the zero below would be a "
        << "thread race again. kMaxRawSends is supposed to make that impossible — this is the "
        << "arm's construction broken, not the server";
    ASSERT_GE(enqueued2, kMinResolvable)
        << "phase 2 got only " << enqueued2 << " packets to the queue in " << raw << " sends, "
        << "below the " << kMinResolvable << " needed to resolve a "
        << kPositiveControlStallMsPerPacket << " ms/packet stall across a " << kSettleMs
        << " ms settle, so a zero below would be weaker than this arm claims. Two causes reach "
        << "this, and they are told apart by the sign: a POSITIVE count short of the floor is "
        << "arrival loss upstream of the queue, never drain lag (crc_errors=" << s2.crcErrors
        << "); a NEGATIVE one means the session left the roster between the two readings, since "
        << "ServerStats is a gauge over the live roster and these are deltas";

    EXPECT_EQ(drops2, 0)
        << "a " << enqueued2 << "-packet burst into a " << kCapacity << "-packet queue dropped "
        << drops2 << ". That burst cannot overflow the queue by itself, so the queue was still "
        << "holding at least " << (kCapacity - enqueued2 + drops2) << " packets of the flood's "
        << "backlog " << kSettleMs << " ms after the flood ended — the receive path has acquired "
        << "a blocking step. (The flood itself dropped " << s1.queueDrops << " of "
        << s1.packetsReceived << " enqueued; that number is a scheduling race and is deliberately "
        << "not asserted on — issue #75.)";

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
// (docs/audio-streaming-protocol-v1.md:502), while the treatment of locally reconstructed
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
// The probe is the barrier, and it survived #56 item 2 changing how the response is sent.
// handleControlMessage now ENQUEUES the LatencyResponse (AudioStreamServer.cpp:568) instead of
// sending it inline, so the writer thread transmits it — but the enqueue still happens on the
// receive thread, strictly after handleTxAudio returned, and receiveLoop is strictly sequential
// (AudioStreamServer.cpp:437-462). So "response observed" still implies "enqueued", which still
// implies "the frame pushed before it has been through handleTxAudio and the mixer". The
// happens-after edge is unchanged; only the thread that finally writes the bytes moved.
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

// --- issue #57.1 — stop() vs a client already inside handleNewClient -----------------------
//
// A connection that PARKS the accept thread inside handleNewClient, holding no lock, in the
// window between handleNewClient's entry running_ check and the session insert.
//
// remoteAddress() is the hook because of what it does NOT do: it is called at the top of
// handleNewClient (src/net/AudioStreamServer.cpp:753) and takes no lock. The obvious
// alternative — blocking the backend's openCaptureStream — is WRONG here: that runs under
// runMutex_ (openSharedAudioLines takes it), so stop() would block in stopSharedAudio() before
// reaching the barrier and the race would never be staged. The whole point is to let stop()
// run its ENTIRE teardown while the accept thread sits in the window.
class LatchedAddressConnection : public naudio::test::ScriptedClientConnection {
public:
    using ScriptedClientConnection::ScriptedClientConnection;

    std::string remoteAddress() const override {
        entered_.store(true);
        std::unique_lock<std::mutex> lock(latchMutex_);
        latchCv_.wait(lock, [this]() { return released_; });
        return ScriptedClientConnection::remoteAddress();
    }

    bool waitUntilEntered(int timeoutMs) const {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (entered_.load()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return entered_.load();
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(latchMutex_);
            released_ = true;
        }
        latchCv_.notify_all();
    }

private:
    mutable std::atomic<bool> entered_{false};
    mutable std::mutex latchMutex_;
    mutable std::condition_variable latchCv_;
    bool released_ = false;
};

// Counts the connect events a listener is told about.
class ConnectCountingListener : public AudioStreamListener {
public:
    void onClientConnected(const std::string&, const std::string&) override {
        connects_.fetch_add(1);
    }
    int connects() const { return connects_.load(); }

private:
    std::atomic<int> connects_{0};
};

// #57.1 — stop() must not return while a session admitted during its teardown is still live.
//
// Staged, not raced: the accept thread is held in handleNewClient's window while stop() runs its
// close-all / barrier / clear pass, so the interleaving is deterministic rather than a timing
// loop that may never hit it.
//
// PRE-FIX this reads clientCount() == 1. stop() passed the activeThreads_ == 0 barrier precisely
// BECAUSE startRunThread() had not run yet, cleared sessions_, and then blocked in
// acceptThread_.join() — which guarantees the straggler is inserted and its detached runLoop
// spawned before stop() returns. Nothing ever joined that thread: the destructor's stop() is a
// no-op once running_ is already false, so ~AudioStreamServer freed runMutex_/sessionsMutex_
// under a running thread.
//
// WHAT THIS ARM DETECTS — MEASURED, three builds, not reasoned:
//   - both fixes removed (original code): RED on connects(), which reads 1.
//   - second teardown pass removed, re-check kept: GREEN.
//   - both fixes in place: GREEN.
//
// So the detector is connects(), and it pins the running_ RE-CHECK at insert. Its observable is
// the one thing no later cleanup can undo: a listener was told a client connected to a server
// that had already stopped.
//
// clientCount() IS NOT A DETECTOR — it reads 0 in all three builds above. ClientSession::close()
// erases the session from sessions_ itself (src/net/AudioStreamServer.cpp:487-489), so the
// straggler removes its own evidence before the assertion runs. It is kept only as a plain
// post-condition (a stopped server has an empty roster); do not read a green here as coverage.
//
// THE SECOND TEARDOWN PASS HAS NO DETECTOR IN THIS ARM, and that is a known gap rather than an
// oversight. The interleaving it defends is: handleNewClient passes the re-check while running_
// is still true, releases sessionsMutex_, and is descheduled BEFORE startRunThread(); stop()
// then runs its whole first pass (the barrier passes because threadStarted() has not run) and
// blocks in the join, and the spawn happens after. Staging that needs a latch between the insert
// and startRunThread() — notifyClientConnected() at :787 sits exactly there and would be the
// hook. The pass is justified by that argument and by being idempotent, NOT by a red test.
TEST(Server, StopDoesNotStrandASessionAdmittedWhileItWasTearingDown) {
    auto transport = std::make_shared<naudio::test::ScriptedServerTransport>();

    // Declared BEFORE the server so it outlives it: the server holds a raw listener pointer and
    // there is no removeStreamListener to hand it back.
    ConnectCountingListener listener;

    AudioStreamServer server{0};
    server.setInjectOnlyMode(true);
    server.setTransportFactory([transport]() { return transport; });
    server.addStreamListener(&listener);

    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    auto conn = std::make_shared<LatchedAddressConnection>("racer");
    conn->pushConnectRequest();
    transport->offer(conn);

    // Park the accept thread in the window before touching stop().
    ASSERT_TRUE(conn->waitUntilEntered(3000))
        << "the accept thread never reached handleNewClient — the window was not staged, so a "
           "green result here would be vacuous";

    // stop() ends up blocked in acceptThread_.join() waiting for the thread the latch holds, so
    // the release has to come from a third thread or this deadlocks.
    std::thread releaser([conn]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        conn->release();
    });
    server.stop();
    releaser.join();

    EXPECT_EQ(server.clientCount(), 0)
        << "stop() returned with a session in the map it had already cleared";
    EXPECT_EQ(listener.connects(), 0)
        << "a listener was told a client connected to a server that had already stopped";
    EXPECT_FALSE(server.isRunning());
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
           "per-episode denial (docs/audio-streaming-protocol-v1.md:502)";
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
    // hard-wired NORMAL priority, docs/audio-streaming-protocol-v1.md:523).
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

// ---------------------------------------------------------------------------------------
// Issue #56 — the outbound backlog cap, and eviction of a peer that stops draining.
//
// A TCP peer that stops reading closes its receive window; the session's writer thread then
// blocks inside Socket::sendAll, and every frame the capture path fans out piles up behind
// it. These arms drive a REAL ClientSession over a supplied transport whose sendRxAudio
// parks on entry, which is that state exactly, minus the socket.
//
// WHEN THESE ARMS WERE WRITTEN THAT BLOCK HAD NO DEADLINE OF ANY KIND. It does now:
// AudioProtocolHandler's constructor arms CONNECTION_TIMEOUT_MS / 2 on every TCP connection
// (#56's send-deadline half) and sendAll spends it as a whole-call budget (#70). The cap
// these arms cover is still the load-bearing mechanism, not a redundant one — the deadline
// bounds how long ONE frame can wedge the writer, while the cap bounds how much audio piles
// up behind it and is what actually evicts the peer. The double therefore still parks
// without a deadline; see ScriptedTransport::stallRxAudio for why that divergence is
// deliberate.
//
// WHY A DOUBLE AND NOT A REAL STALLED SOCKET. Wedging a real sender means filling both the
// sender's send buffer and the receiver's receive buffer, and those are autotuned: measured
// on this machine across three consecutive runs of one binary, the absorbed volume was
// 548,546 / 586,251 / 741,696 bytes, with SO_SNDBUF growing 146,988 -> 335,404 mid-run. A
// threshold arm built on that is a coin flip, and it is a different coin on every platform.
// Parking the send makes the queue depth exactly known instead, so the boundary pair below
// can differ by ONE frame and still be deterministic.
//
// WHAT THESE ARMS DO NOT COVER, stated so nobody reads more into them. They prove the queue
// is bounded and the session is evicted. They do NOT prove the wedged writer THREAD unwinds:
// that depends on whether closing the socket interrupts a send already blocked in the
// kernel, which is a platform property (measured true on macOS: close() released a genuinely
// wedged sendAll in 0 ms; unverified on Linux/Windows, where the tree has no ::shutdown() to
// fall back on). The double releases on close() by construction, so it models the favourable
// platform and cannot speak to the other. See issue #56's follow-up for that half.
// ---------------------------------------------------------------------------------------

namespace {

// One frame at the session's own bit rate, so the arithmetic below is the shipped one.
std::vector<std::uint8_t> backlogFrame(const AudioStreamConfig& c) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(c.bytesPerFrame()), 0x33);
}

// How many whole frames the cap admits: CONNECTION_TIMEOUT_MS of audio.
//
// Derived through frameDurationMs, while production derives through bytesPerSecond
// (AudioStreamServer.cpp, outQueueMaxBytes_) — deliberately the other route to the same
// quantity, so this is not the production formula copied back to confirm itself.
int framesAdmittedByCap(const AudioStreamConfig& c) {
    return AudioProtocolHandler::CONNECTION_TIMEOUT_MS / c.frameDurationMs;
}

// Connects a scripted client and leaves its writer thread PARKED inside sendRxAudio, with
// the queue verified empty.
//
// The parked-writer barrier is what makes every count below exact. One frame is injected and
// the writer is awaited INSIDE it: at that point the item has been popped, so the backlog is
// zero and every later inject accumulates one-for-one. Without this the writer's pop races
// the test's injects and the boundary moves by a frame.
std::shared_ptr<naudio::test::ScriptedClientConnection> connectWithParkedWriter(
    ScriptedFixture& fx, const std::string& id) {
    auto conn = connectScripted(*fx.transport, id);
    if (!conn) return nullptr;
    conn->stallRxAudio();
    fx.server.injectAudio(backlogFrame(fx.config));
    for (int i = 0; i < 500 && conn->rxAudioCalls() < 1; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return conn->rxAudioCalls() == 1 ? conn : nullptr;
}

}  // namespace

// SUBJECT. One frame past the cap and the peer is gone.
TEST(Server, AStalledWriterIsEvictedWhenTheOutboundBacklogReachesItsCap) {
    ScriptedFixture fx;
    std::string err;
    ASSERT_TRUE(fx.server.start(&err)) << err;

    // GUARD "the harness ran" AND "the fault fired", in one: a null return means either the
    // session never registered or the writer never reached the parked send, and those are
    // both setup failures rather than the subject.
    auto conn = connectWithParkedWriter(fx, "stalled-a");
    ASSERT_TRUE(conn) << "writer never parked inside sendRxAudio; nothing was stalled";
    ASSERT_EQ(fx.server.clientCount(), 1) << "premise: the stalled peer is connected";
    ASSERT_EQ(fx.server.outboundBacklogEvictions(), 0) << "premise: nothing evicted yet";

    const int admitted = framesAdmittedByCap(fx.config);
    for (int i = 0; i < admitted + 1; i++) fx.server.injectAudio(backlogFrame(fx.config));

    // SUBJECT. The eviction runs synchronously on this thread — the broadcaster erases the
    // refusing target and calls the server's failure listener before injectAudio returns —
    // so this needs no wait, and a wait here would hide a regression that only evicts late.
    EXPECT_EQ(fx.server.clientCount(), 0)
        << "a peer that stopped draining was never evicted; " << conn->diagnostics();
    EXPECT_EQ(fx.server.outboundBacklogEvictions(), 1)
        << "the eviction is invisible to an operator: nothing counted it";
}

// BOUNDARY CONTROL. Same fault, one frame fewer, opposite verdict.
//
// This is what makes the subject above a statement about the CAP rather than about stalling
// at all: a regression that evicted on the first stalled frame would satisfy the subject
// perfectly and reddens here.
TEST(Server, AStalledWriterIsNotEvictedOneFrameBelowTheCap) {
    ScriptedFixture fx;
    std::string err;
    ASSERT_TRUE(fx.server.start(&err)) << err;

    auto conn = connectWithParkedWriter(fx, "stalled-b");
    ASSERT_TRUE(conn) << "writer never parked inside sendRxAudio; nothing was stalled";

    const int admitted = framesAdmittedByCap(fx.config);
    for (int i = 0; i < admitted; i++) fx.server.injectAudio(backlogFrame(fx.config));

    EXPECT_EQ(fx.server.outboundBacklogEvictions(), 0)
        << "the cap fired early: " << admitted << " frames is exactly "
        << AudioProtocolHandler::CONNECTION_TIMEOUT_MS << " ms of audio and must be admitted";
    EXPECT_EQ(fx.server.clientCount(), 1) << conn->diagnostics();
    // The writer is still parked on the very first frame — nothing drained, so the whole
    // backlog really is sitting in the queue and the count above is not an artefact of the
    // session having quietly caught up.
    EXPECT_EQ(conn->rxAudioCalls(), 1) << conn->diagnostics();
}

// HEALTHY-CLIENT CONTROL. The same volume through a peer that drains costs nothing.
//
// Without this, both arms above are satisfied by a cap that counts TOTAL frames sent rather
// than the pending backlog — which would evict every long-lived client on a live server.
TEST(Server, AClientThatDrainsIsNeverEvictedByTheSameVolume) {
    ScriptedFixture fx;
    std::string err;
    ASSERT_TRUE(fx.server.start(&err)) << err;

    auto conn = connectScripted(*fx.transport, "healthy-c");  // no stall armed
    ASSERT_TRUE(conn) << "no ClientsUpdate: a real session never registered";

    const int total = framesAdmittedByCap(fx.config) + 1;  // the count that evicted above
    for (int i = 1; i <= total; i++) {
        fx.server.injectAudio(backlogFrame(fx.config));
        // Let the writer catch up in batches, so the backlog stays shallow by construction
        // rather than by luck. A drainer that fell far enough behind SHOULD be evicted, and
        // this arm is about a client that does not.
        if (i % 50 == 0) {
            for (int w = 0; w < 500 && conn->rxAudioCalls() < i; w++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    EXPECT_EQ(fx.server.outboundBacklogEvictions(), 0)
        << "a client that drained everything was evicted anyway; " << conn->diagnostics();
    EXPECT_EQ(fx.server.clientCount(), 1) << conn->diagnostics();
    // GUARD: the frames really did reach the wire, so the zero above is a real absence and
    // not the result of the session having died before the volume arrived.
    EXPECT_GE(conn->rxAudioCalls(), total - 50) << conn->diagnostics();
}

// ---------------------------------------------------------------------------------------
// Issue #56 — THE IDLE STALL. The gap the two arms above cannot reach.
//
// Both arms above evict through the BACKLOG CAP, and the cap is a bound on VOLUME. So they
// only speak about a peer that stops draining WHILE AUDIO IS FLOWING. Take the audio away
// and every mechanism they exercise goes quiet: nothing is enqueued, outQueueBytes_ stays 0,
// the cap is never approached, and writerLoop sits on its condition variable having never
// attempted a send. The peer is just as dead and holds just as much — two detached threads
// and one of DEFAULT_MAX_CLIENTS == 4 slots — but no volume ever accumulates to prove it.
//
// In that state the session's ONLY liveness machinery is runLoop's heartbeat. And the peer
// this arm stages is the one #56 leads with: it has stopped READING while still SENDING.
// That distinction is the whole arm, because isConnectionTimedOut() keys on lastReceiveTime_
// (AudioProtocolHandler.cpp, isConnectionTimedOut) — NOT on send time. A peer that keeps
// sending refreshes that clock forever, so the timeout can never fire no matter how long its
// receive window has been shut. setTimedOut is therefore left FALSE on purpose: flipping it
// would stage a different, easier peer and the arm would pass without the fix.
//
// What is left is the heartbeat's own return value. Since #56's deadline half, a heartbeat
// into a shut window does not park forever — sendAll spends CONNECTION_TIMEOUT_MS / 2 and
// reports failure. That failure is the ONLY evidence the server gets, and failHeartbeatSends
// is that failure with the five-second wait taken out.
//
// The mirror of this on the client was issue #71, and the client's heartbeatLoop already
// checks its watchdog on both sides of the send and reports a failed one
// (AudioStreamClient.cpp, heartbeatLoop). The server's runLoop never received that fix.
// Note writerLoop in this same file already treats a failed send as fatal — so the server
// holds two send paths with opposite failure policies, and this arm pins the one that is
// missing rather than asserting a new policy invented here.
TEST(Server, APeerThatStopsReadingButKeepsSendingIsEvictedWithNoAudioFlowing) {
    ScriptedFixture fx;
    std::string err;
    ASSERT_TRUE(fx.server.start(&err)) << err;

    auto conn = connectScripted(*fx.transport, "idle-stalled");
    ASSERT_TRUE(conn) << "no ClientsUpdate: a real session never registered";
    ASSERT_EQ(fx.server.clientCount(), 1) << "premise: the peer is connected";

    // The staging, and each line is load-bearing:
    //   - heartbeats fail, because the peer's receive window is shut and the send deadline
    //     has expired against it;
    //   - the connection is NOT timed out, because the peer is still sending to us.
    conn->failHeartbeatSends();
    conn->setTimedOut(false);
    conn->setShouldSendHeartbeat(true);

    // No injectAudio anywhere in this arm. That absence IS the fixture.

    for (int i = 0; i < 600 && fx.server.clientCount() > 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // GUARD "the fault actually fired" before reading the subject (L136). If runLoop never
    // attempted a heartbeat, a green verdict below would mean nothing — and this is exactly
    // the guard that separates "the session was torn down by the fix" from "the run loop
    // stopped iterating for some reason unrelated to the subject".
    ASSERT_GT(conn->heartbeatsSent(), 0)
        << "premise failed: runLoop never attempted a heartbeat, so nothing was staged; "
        << conn->diagnostics();

    // SUBJECT.
    EXPECT_EQ(fx.server.clientCount(), 0)
        << "a peer whose every heartbeat fails was never evicted: with no audio flowing the "
           "backlog cap cannot fire and isConnectionTimedOut() keys on RECEIVE time, so the "
           "failed heartbeat is the only evidence there is and runLoop discards it; "
        << conn->diagnostics();
}

// #59: the server's END of the device-loss path. The Broadcaster/Mixer arms prove those loops
// catch and report; this arm is what proves AudioStreamServer actually INSTALLS the hooks, and
// that the loss reaches a listener's onError — the surface a C consumer sees.
//
// It exists because a mutation measured the gap: deleting the setCaptureErrorListener wiring in
// AudioStreamServer left the whole suite green. An unset std::function is silent under this
// project's zero warning flags (L125), so the wiring needed its own detector.
namespace {

class DyingDeviceBackend : public DeviceBackend {
public:
    int throwAfterReads = -1;   // capture dies after N reads  (-1 = never)
    int throwAfterWrites = -1;  // playback dies after N writes (-1 = never)

    std::vector<RawDevice> enumerate() override { return {}; }
    bool probeFormat(int, const AudioFormat&, Direction) override { return true; }
    std::unique_ptr<CaptureStream> openCaptureStream(int, const AudioFormat& fmt) override {
        auto s = std::make_unique<FakeCaptureStream>(fmt);
        s->throwAfterReads = throwAfterReads;
        return s;
    }
    std::unique_ptr<PlaybackStream> openPlaybackStream(int, const AudioFormat& fmt) override {
        auto s = std::make_unique<FakePlaybackStream>(fmt);
        s->throwAfterWrites = throwAfterWrites;
        return s;
    }
};

class ErrorRecordingListener : public AudioStreamListener {
public:
    void onError(const std::string&, const std::string& error) override {
        std::lock_guard<std::mutex> l(m_);
        errors_.push_back(error);
    }
    bool sawError(const std::string& needle) {
        std::lock_guard<std::mutex> l(m_);
        for (const auto& e : errors_) {
            if (e.find(needle) != std::string::npos) return true;
        }
        return false;
    }

private:
    std::mutex m_;
    std::vector<std::string> errors_;
};

}  // namespace

TEST(Server, CaptureDeviceLostMidStreamSurfacesOnError) {
    DyingDeviceBackend backend;
    backend.throwAfterReads = 2;
    AudioStreamConfig config{};
    config.maxClients = 4;
    // `listener` is declared BEFORE `server` so it is destroyed AFTER it. stop() only POSTS
    // onServerStopped() to the dispatcher; the drain happens in ~AudioStreamServer
    // (AudioStreamServer.cpp:637), so the callback is delivered after stop() has returned. With
    // the declaration order reversed this arm was a stack-use-after-scope, which is how ASan
    // found it (issue #28) — the listener died first and the dispatcher thread then read its
    // vtable. Same borrowed-and-must-outlive rule the `backend` above already follows.
    ErrorRecordingListener listener;
    AudioStreamServer server{0, config};
    server.setBackend(&backend);
    server.setCaptureDevice(0);

    server.addStreamListener(&listener);

    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!listener.sawError("Capture device lost") &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(listener.sawError("Capture device lost"));

    server.stop();
}

// #59: the mixer half of the same wiring proof — the shared TX-to-rig playback device dies and
// the loss must reach onError. Measured as necessary: with only the capture arm above, deleting
// the ml.onPlaybackDeviceError wiring in AudioStreamServer left the suite green.
TEST(Server, PlaybackDeviceLostMidStreamSurfacesOnError) {
    DyingDeviceBackend backend;
    backend.throwAfterWrites = 2;  // capture stays healthy (throwAfterReads = -1); playback dies
    AudioStreamConfig config{};
    config.maxClients = 4;
    // Declared before `server` so it outlives it — see the note in the capture arm above.
    ErrorRecordingListener listener;
    AudioStreamServer server{0, config};
    server.setBackend(&backend);
    // A capture device is REQUIRED to reach the playback path at all: startInternal opens the
    // shared audio lines only under `if (captureBackendId_.has_value())`, so a playback-only
    // server never starts AudioMixer's loop and this arm would pass vacuously without it.
    server.setCaptureDevice(0);
    server.setPlaybackDevice(0);

    server.addStreamListener(&listener);

    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!listener.sawError("Playback device lost") &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(listener.sawError("Playback device lost"));

    server.stop();
}

// ===========================================================================
// Roster hygiene on the accept/reject path (issue #64).
// ===========================================================================

namespace {

// Records the connect/disconnect events a listener is told about, in order and with ids, so an
// arm can assert PAIRING rather than merely counting. ConnectCountingListener above counts only
// connects, which is the detector #57 needed; this is the one #64.2 needs.
class LifecyclePairingListener : public AudioStreamListener {
public:
    void onClientConnected(const std::string& id, const std::string&) override {
        std::lock_guard<std::mutex> l(m_);
        connected_.push_back(id);
    }
    void onClientDisconnected(const std::string& id) override {
        std::lock_guard<std::mutex> l(m_);
        disconnected_.push_back(id);
    }
    std::size_t connects() const {
        std::lock_guard<std::mutex> l(m_);
        return connected_.size();
    }
    std::size_t disconnects() const {
        std::lock_guard<std::mutex> l(m_);
        return disconnected_.size();
    }
    // The unpaired set: every id that was announced as connected and never as disconnected.
    std::vector<std::string> unpaired() const {
        std::lock_guard<std::mutex> l(m_);
        std::vector<std::string> out;
        for (const auto& id : connected_) {
            if (std::find(disconnected_.begin(), disconnected_.end(), id) == disconnected_.end()) {
                out.push_back(id);
            }
        }
        return out;
    }

private:
    mutable std::mutex m_;
    std::vector<std::string> connected_;
    std::vector<std::string> disconnected_;
};

// Connects one raw client and waits for the CONNECT_REJECT the server sends at accept time.
// Returns false if no reject arrived.
//
// `sendRequest` is TRUE ONLY FOR UDP, and the asymmetry is load-bearing rather than tidiness.
// UdpServerTransport creates a connection only for a valid CONNECT_REQUEST (anti-spoof,
// UdpServerTransport.cpp:105-110), so without one the server never accepts and never rejects.
// TCP accepts the socket itself and rejects before reading anything — so a CONNECT_REQUEST sent
// there is never consumed, and closing a socket that still holds unread data makes the stack send
// an RST rather than a FIN, which discards the peer's already-delivered receive buffer. The
// CONNECT_REJECT is in that buffer.
//
// What is MEASURED is the failure, not the mechanism: an earlier version of this helper sent the
// request on both transports; it passed on macOS and ubuntu and FAILED on windows-latest (CI run
// 31660239300) at transport 0, attempt 1 — the second TCP reject, the first having won the race.
// The RST-on-unread-data rule above is the standard socket behaviour (RFC 1122 §4.2.2.13) and is
// the explanation, not a second measurement; what makes it the likely one is that the other two
// reject arms in this file (RejectsWhenNoCaptureDevice, MaxClientsRejectsBusy) send nothing before
// reading the reject, and neither has ever failed there.
//
// The RST is NOT a defect in the fix under test, and it is not this arm's subject — it is a real
// pre-existing property of the reject path that this helper stumbled into, filed separately. This
// arm is about the transport-map leak, so it stops conflating the two.
bool rejectedOnce(ClientTransport& transport, std::uint16_t port, const std::string& name,
                  bool sendRequest) {
    std::string err;
    auto c = transport.connect("127.0.0.1", port, 2000, &err);
    if (!c) return false;
    if (sendRequest &&
        !c->sendControl(ControlMessage::connectRequest(name, AudioPacket::VERSION))) {
        return false;
    }
    const bool rejected =
        recvUntil(*c, PacketType::Control, ControlType::ConnectReject, 3000).has_value();
    c->close();
    return rejected;
}

}  // namespace

// #64.1 — a client the server REJECTS must leave the transport's connection map, not merely have
// its socket closed.
//
// ServerStats is a gauge over the LIVE ROSTER (its contract in AudioStreamServer.hpp, pinned by
// GateServerStatsIsARosterGaugeNotALifetimeTotal above), and the aggregation is a sum over the
// transport's connection map — TcpServerTransport::packetsSent and friends iterate connections_
// (TcpServerTransport.cpp:96-148); the UDP mirror iterates byId_ (UdpServerTransport.cpp:216-270).
// A rejected connection that stays in that map keeps contributing its own reject message forever,
// so a server being polled by a retrying client accrues one dead entry per attempt.
//
// This arm rejects on the NO-CAPTURE-DEVICE path rather than the Busy path, which is what makes
// the assertion exact rather than approximate: with no session on the roster at all there is no
// heartbeat traffic, so the gauge's correct reading is a hard ZERO at every point. Reaching the
// same statement through the Busy path needs a resident client whose run loop is sending
// heartbeats on a 1 s pacing wait, and the storm's own duration then bounds the tolerance. All
// three reject paths (no capture device, Busy, devices unavailable) funnel through the single
// rejectClient(), so this covers the eviction; it does not separately cover the other two
// callers' reasons for getting there.
//
// MEASURED PRE-FIX, 8 attempts: packetsSent 8, bytesSent 440 — IDENTICAL on TCP and UDP, which is
// the useful part: it says the leak is in the shared rejectClient() and not in either transport.
// clientsConnected reads 0 throughout and is therefore NOT a detector here; the byte counters are.
TEST(Server, GateRejectedClientsLeaveNoTraceInTheRosterGauge) {
    for (TransportType tt : {TransportType::Tcp, TransportType::Udp}) {
        AudioStreamConfig config{};
        config.transportType = tt;
        config.explicitBareUdp = true;  // bare-by-request (#92); inert on the TCP arm
        AudioStreamServer server{0, config};  // no capture device, NOT inject-only => reject all
        std::string err;
        ASSERT_TRUE(server.start(&err)) << err;
        const auto port = static_cast<std::uint16_t>(server.port());

        const ServerStats before = server.stats();
        ASSERT_TRUE(before.running);
        ASSERT_EQ(before.clientsConnected, 0);
        ASSERT_EQ(before.packetsSent, 0);

        const int kAttempts = 8;
        for (int i = 0; i < kAttempts; i++) {
            std::unique_ptr<ClientTransport> t;
            if (tt == TransportType::Tcp) {
                t = std::make_unique<TcpClientTransport>();
            } else {
                t = std::make_unique<UdpClientTransport>();
            }
            ASSERT_TRUE(rejectedOnce(*t, port, "probe-" + std::to_string(i),
                                     /*sendRequest=*/tt == TransportType::Udp))
                << "transport " << static_cast<int>(tt) << " attempt " << i;
        }

        // The roster is still empty, so every aggregate over it must still be zero. Pre-fix each
        // rejected connection is still in the map and still counting its own reject message.
        //
        // THE GAUGE IS ALLOWED TO SETTLE, and the wait is not a flake-patch. Since #87 rejectClient
        // drains the peer's unread CONNECT_REQUEST before closing (so the close emits a FIN and the
        // reject is not lost to an RST), so the map entry is released up to the drain budget AFTER
        // the client has already read its reject. The contract this arm pins is that a rejected
        // client LEAVES the map — not that it leaves before an observer can look. Measured: without
        // this wait the arm read packetsSent == 1, the final attempt still inside its drain
        // (20 ms then; REJECT_DRAIN_BUDGET_MS now, and since #104 a silent peer pays all of it).
        //
        // It stays a real detector: a leak that never resolves still fails, because the settle
        // window is 40x the drain — derived from the budget, so it moves with it. Confirmed by
        // re-running the M-A mutation after this wait was added.
        ServerStats after = server.stats();
        const auto settleBy =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(40 * AudioStreamServer::REJECT_DRAIN_BUDGET_MS);
        while ((after.packetsSent != 0 || after.bytesSent != 0) &&
               std::chrono::steady_clock::now() < settleBy) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            after = server.stats();
        }
        EXPECT_EQ(after.clientsConnected, 0);
        EXPECT_EQ(after.packetsSent, 0) << "transport " << static_cast<int>(tt);
        EXPECT_EQ(after.bytesSent, 0) << "transport " << static_cast<int>(tt);

        server.stop();
    }
}

// #56 item 2 — a stalled writer must not wedge the session's RECEIVE thread.
//
// receiveLoop used to answer a LatencyProbe and a TX denial with a DIRECT connection_->sendControl.
// Every send funnels through AudioProtocolHandler::sendPacket, which holds one sendMutex_ across
// the whole Socket::sendAll — so while the writer thread was blocked sending to a peer whose
// receive window had closed, the receive thread blocked behind it on that same mutex. The thread
// that processes that client's TX audio, its heartbeats and its DISCONNECT stopped, because the
// client had stopped reading. Bounded since the send deadline landed (#56/#70), but bounded is not
// the same as absent, and the writer bridge exists precisely so that no other thread touches a
// socket.
//
// WHY NO ARM CAUGHT THIS BEFORE, which is the part worth keeping: ScriptedClientConnection
// deliberately did NOT serialise sendRxAudio on its sendMutex_, with a comment saying production
// does but that serialising it here would deadlock the LatencyProbe barrier the server arms use.
// That is true — and it is true BECAUSE of this defect. The double was bent to fit the bug, so the
// one place the contention lived was the one place the harness refused to model it. The double is
// faithful again now that the fix removed the reason for the exception.
//
// THE OBSERVABLE IS THE DISCONNECT, not the probe. A LatencyResponse would only prove the probe
// was handled; the Disconnect pushed BEHIND it proves receiveLoop kept going past it, which is the
// actual claim. It is also un-fakeable by later cleanup: the session leaves the roster only if
// handleControlMessage ran.
TEST(Server, ServerReceiveLoopIsNotWedgedByAStalledWriter) {
    auto transport = std::make_shared<naudio::test::ScriptedServerTransport>();
    AudioStreamServer server{0};
    server.setInjectOnlyMode(true);
    server.setTransportFactory([transport]() { return transport; });
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    auto conn = connectScripted(*transport, "stalled-reader");
    ASSERT_TRUE(conn) << "the scripted session never reached the roster";

    // Park the writer inside a send, holding sendMutex_ — a peer whose window has closed.
    conn->stallRxAudio();
    std::vector<std::uint8_t> frame(64, 0x5A);
    server.injectAudio(frame);

    // THE L136 GUARD. Asserting anything about the receive thread "while the writer is wedged" is
    // vacuous unless the writer is actually wedged at that moment. rxAudioCalls saturates at 1
    // with the latch armed, so this is the proof the fault was staged, not merely requested.
    const auto parkedBy = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (conn->rxAudioCalls() < 1 && std::chrono::steady_clock::now() < parkedBy) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(conn->rxAudioCalls(), 1) << "the writer never entered sendRxAudio, so nothing is "
                                          "holding sendMutex_ and this arm proves nothing: "
                                       << conn->diagnostics();

    // Drive the receive thread: a probe (which used to block on the held mutex) and, behind it,
    // a Disconnect whose handling is the observable.
    conn->pushLatencyProbe();
    conn->push(AudioPacket::createControl(0, ControlMessage::disconnect().serialize()),
               Provenance::Live);

    EXPECT_TRUE(waitForClientCount(server, 0, 3000))
        << "receiveLoop never got past the LatencyProbe to the Disconnect behind it — it is "
           "wedged behind the parked writer: "
        << conn->diagnostics();

    server.stop();
}

// #87 — a rejected client must actually RECEIVE its reason, when it behaves the way the real
// client does.
//
// THE POINT OF THIS ARM IS THE ORDER OF TWO LINES. Both older reject arms above
// (RejectsWhenNoCaptureDevice, MaxClientsRejectsBusy) connect and read WITHOUT SENDING ANYTHING.
// AudioStreamClient does not: it connects, sends CONNECT_REQUEST, and only then waits for
// ACCEPT/REJECT (AudioStreamClient.cpp:248 is where it handles the reject). The server decides the
// reject at ACCEPT, before reading anything, so that request is still unread when the connection
// is torn down — and closing a socket that holds unread received data makes the stack send an RST
// rather than a FIN, which discards the peer's already-delivered receive buffer, reject included.
//
// So the two existing arms could never have seen this: the coverage was shaped exactly like the
// bug's blind spot. This arm drives the path the way production does.
//
// PLATFORM NOTE, MEASURED, so nobody reads a green macOS run as proof: this arm is a detector on
// Windows and (so far) nowhere else. A scratch probe drove this exact sequence 200 times on
// macOS/arm64 loopback — 5 read-delays from 0 to 300 ms crossed with 0 and 20 unread filler
// packets — and the reject arrived 200/200. The window would not open here however hard it was
// staged. The failing evidence is CI run 31660239300 on windows-latest, where the same sequence
// failed at attempt 1 of 8 with attempt 0 having won the race. Keep the loop: one attempt is not
// a reliable detector even on the platform that fails.
//
// The Busy path is used rather than no-capture-device because "Maximum clients (N) reached" is the
// reason an operator actually needs to read: it distinguishes "retry later" from "this server will
// never take you". Losing THAT is the user-visible cost.
TEST(Server, ARejectedClientReceivesItsReasonWhenItBehavesLikeARealClient) {
    AudioStreamConfig config{};
    config.maxClients = 1;
    AudioStreamServer server{0, config};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    // Fill the single slot so every later client is rejected Busy.
    TcpClientTransport t0;
    auto resident = t0.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(resident) << err;
    ASSERT_TRUE(clientHandshake(*resident, "resident"));
    ASSERT_TRUE(waitForClientsUpdate(*resident, 1, 3000));

    // 32, not 8 (issue #104): one lost reject in eight attempts is a flake that shows one run
    // in ten on windows-latest; the same window at 32 shows almost every run. A detector is
    // worth more red than it is worth green, and the fix it now measures is the drain waiting
    // for the request (AudioProtocolHandler::discardPendingInput).
    const int kAttempts = 32;
    for (int i = 0; i < kAttempts; i++) {
        TcpClientTransport t;
        auto c = t.connect("127.0.0.1", port, 2000, &err);
        ASSERT_TRUE(c) << "attempt " << i << ": " << err;
        // The realistic order — send, THEN read. This is the line the older arms omit.
        ASSERT_TRUE(c->sendControl(
            ControlMessage::connectRequest("real-" + std::to_string(i), AudioPacket::VERSION)))
            << "attempt " << i;

        auto reject = recvUntil(*c, PacketType::Control, ControlType::ConnectReject, 3000);
        ASSERT_TRUE(reject.has_value())
            << "attempt " << i << ": no CONNECT_REJECT reached a client that had sent a "
            << "CONNECT_REQUEST first — the reason was lost in transit, not withheld";

        auto msg = ControlMessage::deserialize(reject->payload());
        ASSERT_TRUE(msg.has_value()) << "attempt " << i;
        const auto reason = msg->parseErrorMessage();
        EXPECT_TRUE(reason.has_value() &&
                    reason->find("Maximum clients") != std::string::npos)
            << "attempt " << i << " reason: " << reason.value_or("<none>");
        c->close();
    }

    server.stop();
}

// #104 — the window the arm above hit one attempt in ~fifty on windows-latest, opened on purpose.
//
// The reject is decided at accept, before the client's CONNECT_REQUEST can have arrived; the
// client sends it from a thread that has to be scheduled after connect() returns. The server's
// drain used to take its first empty 2 ms read as "nothing to drain" and close — and when the
// request then landed on the closed socket, the stack answered RST, which on Windows throws away
// the client's unread receive buffer, the reject with it. On a loaded runner a scheduling gap
// over 2 ms is ordinary. Here the gap is made deliberately: the client connects, WAITS, and only
// then sends its request — inside the drain's budget, well past the old first-read exit.
//
// MEASURED PRE-FIX on windows-latest: fails deterministically (every attempt); macOS and Linux
// deliver the reject either way, their stacks leaving queued data readable after an RST, so this
// arm is a Windows detector and a documentation of the rule everywhere else. The delay is a
// fifth of REJECT_DRAIN_BUDGET_MS so a slow runner has room before the send lands past the drain
// — the case past the budget is the "reason is best-effort" clause on
// AudioStreamServer::rejectClient, and it is not this arm's to prove. That room is smaller than
// it looks: Windows sleeps in 15.6 ms quanta, so the asked-for 10 ms is 16-31 ms in practice. So
// the arm MEASURES the gap it actually produced and prints it with the failure — a gap inside
// the budget is #104 back; a gap past it is a stalled runner, and the two must not read alike.
TEST(Server, ARejectedClientThatSendsLateStillReceivesItsReason) {
    AudioStreamConfig config{};
    config.maxClients = 1;
    AudioStreamServer server{0, config};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    TcpClientTransport t0;
    auto resident = t0.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(resident) << err;
    ASSERT_TRUE(clientHandshake(*resident, "resident"));
    ASSERT_TRUE(waitForClientsUpdate(*resident, 1, 3000));

    const int kAttempts = 8;
    for (int i = 0; i < kAttempts; i++) {
        TcpClientTransport t;
        auto c = t.connect("127.0.0.1", port, 2000, &err);
        ASSERT_TRUE(c) << "attempt " << i << ": " << err;
        // The scheduling gap, made real: longer than the old drain's first-read exit (2 ms),
        // a fifth of the budget by request — and measured, because the request is not the gap.
        const auto connectedAt = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(AudioStreamServer::REJECT_DRAIN_BUDGET_MS / 5));
        const auto sentAfterMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - connectedAt)
                                     .count();
        ASSERT_TRUE(c->sendControl(
            ControlMessage::connectRequest("late-" + std::to_string(i), AudioPacket::VERSION)))
            << "attempt " << i;

        auto reject = recvUntil(*c, PacketType::Control, ControlType::ConnectReject, 3000);
        ASSERT_TRUE(reject.has_value())
            << "attempt " << i << ": a client that sent its CONNECT_REQUEST " << sentAfterMs
            << " ms after connecting got no CONNECT_REJECT (budget "
            << AudioStreamServer::REJECT_DRAIN_BUDGET_MS << " ms) — inside the budget this is "
            << "#104 back: the server closed on the request and reset it; past the budget it is "
            << "a stalled runner, not a regression";
        auto msg = ControlMessage::deserialize(reject->payload());
        ASSERT_TRUE(msg.has_value()) << "attempt " << i;
        const auto reason = msg->parseErrorMessage();
        EXPECT_TRUE(reason.has_value() &&
                    reason->find("Maximum clients") != std::string::npos)
            << "attempt " << i << " reason: " << reason.value_or("<none>");
        c->close();
    }

    server.stop();
}

// #104's other edge — a peer that connects and NEVER sends must not pin the accept thread past the
// drain budget.
//
// The #104 fix made the drain wait for a first byte, so for a silent peer the budget's deadline
// became the ONLY way out of the loop (AudioProtocolHandler::discardPendingInput); before it, the
// first quiet read was a second exit, and the deadline never had to hold alone. Nothing in this
// file exercised it alone either: every silent peer here closes its own socket after reading its
// reject, or lets server.stop() close the transport under the drain — both return Closed/Error and
// leave before the deadline is reached. MEASURED (S180 review): with the deadline check deleted,
// all 228 tests in this binary stayed green. This arm is the one that goes red — the silent peer
// HOLDS its socket, and a second client, whose reject is decided on the same accept thread, must
// still be answered. Mutation-checked: without the deadline it reads "waited 1013 ms" and fails;
// with it, the second reject lands inside the budget plus scheduling.
//
// The bound is derived from REJECT_DRAIN_BUDGET_MS, not restated: ten budgets is generous on a
// loaded runner; the receive wait is twice that, so a pinned thread shows as a measured wait past
// the bound with the reject still missing, not as a bare timeout.
TEST(Server, ASilentPeerCannotPinTheAcceptThreadPastTheDrainBudget) {
    AudioStreamServer server{0};  // no capture device, not inject-only => reject all
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    TcpClientTransport t0;
    auto silent = t0.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(silent) << err;
    ASSERT_TRUE(recvUntil(*silent, PacketType::Control, ControlType::ConnectReject, 2000)
                    .has_value());
    // ... and HOLDS the socket, sending nothing, for the rest of the arm.

    constexpr int kBoundMs = 10 * AudioStreamServer::REJECT_DRAIN_BUDGET_MS;
    const auto t = std::chrono::steady_clock::now();
    TcpClientTransport t1;
    auto next = t1.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(next) << err;
    auto reject = recvUntil(*next, PacketType::Control, ControlType::ConnectReject, 2 * kBoundMs);
    const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t)
                              .count();
    EXPECT_TRUE(reject.has_value())
        << "second client got no CONNECT_REJECT within " << 2 * kBoundMs
        << " ms — the accept thread is pinned by the silent peer (waited " << waitedMs << " ms)";
    EXPECT_LT(waitedMs, kBoundMs)
        << "second client waited " << waitedMs << " ms for its reject; the drain budget is "
        << AudioStreamServer::REJECT_DRAIN_BUDGET_MS << " ms";

    silent->close();
    next->close();
    server.stop();
}

// #64.2 — a session that fails the handshake must not leave an unpaired connect event behind.
//
// onClientConnected fires at accept, when the session enters the roster (AudioStreamServer.cpp:858)
// — before the handshake has been read. runLoop's failure paths then call close() and return, and
// close() emits no listener event, so a peer that connects and says the wrong thing produces a
// connect with no matching disconnect. A listener that pairs the two accumulates phantom clients:
// every port scanner, every version-mismatched client, every half-open probe.
//
// The event pair BRACKETS ROSTER MEMBERSHIP: connected means the session is in sessions_ and
// counted by clientCount(), disconnected means it has left. This arm is the pin on that, and it
// deliberately reads the roster too — a fix that emitted the disconnect without the session
// actually being gone would pass on events alone.
//
// MEASURED PRE-FIX: connects 1, disconnects 0, one unpaired id, clientCount() 0. Note which of
// those is the detector — the roster ALREADY reads 0, because close() erases the session itself.
// The damage is in the listener's model, not in the server's, so only the event pair can see it.
TEST(Server, AFailedHandshakeLeavesNoUnpairedConnectEvent) {
    // Declared before `server` so it is destroyed AFTER it — the same rule the two device-loss
    // arms above follow, and for the same reason (addStreamListener's contract). This arm had the
    // order reversed and was the intermittent sanitizer failure filed as issue #97: stop() only
    // POSTS onServerStopped(), the drain happens in ~AudioStreamServer, so the trailing callback
    // landed on a listener whose frame had already gone. CI caught it on one run in many; a
    // parked-dispatcher probe reproduces it on demand.
    LifecyclePairingListener listener;
    AudioStreamServer server{0};
    server.setInjectOnlyMode(true);
    server.addStreamListener(&listener);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    TcpClientTransport t;
    auto c = t.connect("127.0.0.1", static_cast<std::uint16_t>(server.port()), 2000, &err);
    ASSERT_TRUE(c) << err;

    // A HEARTBEAT is a well-formed frame that is not a CONNECT_REQUEST, so performHandshake
    // rejects it on the first packet rather than sitting out its 10 s budget. That budget is the
    // other half of the defect — a peer that says NOTHING holds the session for ten seconds — but
    // an arm that waited it out would cost ten seconds to prove the same thing.
    ASSERT_TRUE(c->sendHeartbeat());

    // The connect event must arrive (it is not this arm's job to move it), and the disconnect
    // must follow it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (listener.disconnects() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_EQ(listener.connects(), 1u);
    EXPECT_EQ(listener.disconnects(), 1u);
    const auto orphans = listener.unpaired();
    EXPECT_TRUE(orphans.empty()) << "unpaired connect for " << (orphans.empty() ? "" : orphans[0]);
    EXPECT_EQ(server.clientCount(), 0);

    server.stop();
}

// ===========================================================================
// Issue #89 — the listener lifetime contract.
//
// Two arms, guarding the two halves of one decision. The first guards the decision NOT to drain
// the dispatcher inside stop(); the second guards the mechanism that was shipped instead.
// ===========================================================================

namespace {

// Counts the two lifecycle events across a restart. Deliberately counts rather than latches:
// the defect this guards is a SECOND event going missing, which a bool cannot see.
class RestartCountingListener : public AudioStreamListener {
public:
    void onServerStarted(int) override { ++starts_; }
    void onServerStopped() override { ++stops_; }
    int starts() const { return starts_.load(); }
    int stops() const { return stops_.load(); }

private:
    std::atomic<int> starts_{0};
    std::atomic<int> stops_{0};
};

// Parks the dispatch thread inside a callback so a removal can be staged while one is in flight,
// then touches its own members AFTER the park — the read that is a use-after-free if the object
// was destroyed during it.
class ParkedCallbackListener : public AudioStreamListener {
public:
    void onServerStarted(int) override {
        {
            std::lock_guard<std::mutex> lock(m_);
            entered_ = true;
        }
        cv_.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::lock_guard<std::mutex> lock(m_);
        completed_ = true;  // a WRITE to this object, after the park
    }

    bool waitUntilEntered(int budgetMs) {
        std::unique_lock<std::mutex> lock(m_);
        return cv_.wait_for(lock, std::chrono::milliseconds(budgetMs), [this] { return entered_; });
    }
    bool completed() {
        std::lock_guard<std::mutex> lock(m_);
        return completed_;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool completed_ = false;
};

}  // namespace

// #89, half one: the decision NOT to drain in stop(), guarded by its consequence.
//
// This arm exists because the obvious repair for #89 — have stop() drain the dispatcher before
// returning — is WRONG, and wrong in a way nothing else in this suite can see. start() after
// stop() is a supported public transition (naudio.h, na_server_stats.running). CallbackDispatcher
// is not restartable: stop() latches stop_ and joins, after which post() and start() both no-op
// forever. So a drain inside stop() leaves a restarted server running correctly and PERMANENTLY
// MUTE — with start() still returning success.
//
// MEASURED, not argued (S91): with `dispatcher_.stop()` appended to AudioStreamServer::stop(),
// this arm reads starts()==1 stops()==1 against the 2/2 below — and the other 393 tests in the
// tree stay green. It is the only detector for that regression, which is precisely why it is here.
TEST(Server, RestartStillDeliversLifecycleCallbacksAfterAStop) {
    RestartCountingListener listener;
    AudioStreamServer server{0};
    server.setInjectOnlyMode(true);
    server.addStreamListener(&listener);

    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    server.stop();
    ASSERT_TRUE(server.start(&err)) << err << " — start() after stop() is a supported transition";
    server.stop();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((listener.starts() < 2 || listener.stops() < 2) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_EQ(listener.starts(), 2)
        << "the SECOND onServerStarted never arrived: the dispatcher went mute across a restart. "
           "If stop() was just changed to drain, THIS is the cost — see issue #89.";
    EXPECT_EQ(listener.stops(), 2) << "the SECOND onServerStopped never arrived";
}

// #89, half two: removeStreamListener() must WAIT, not merely erase.
//
// The deterministic assertion is completed() — an erase-only removal returns while the parked
// callback is still running, so it reads false on every platform with no sanitizer needed. The
// destroy that follows is the second detector, and the one that speaks under ASan: without the
// fence the parked callback writes to freed memory when it wakes.
//
// The waitUntilEntered gate is what stops a green here from being vacuous (L222): if the callback
// never started, there was no in-flight window to close and the arm would pass without testing
// anything.
TEST(Server, RemoveStreamListenerWaitsForAnInFlightCallback) {
    auto listener = std::make_unique<ParkedCallbackListener>();

    AudioStreamServer server{0};
    server.setInjectOnlyMode(true);
    server.addStreamListener(listener.get());

    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    ASSERT_TRUE(listener->waitUntilEntered(3000))
        << "the dispatch thread never entered onServerStarted — the in-flight window was never "
           "staged, so a green result here would be vacuous";

    server.removeStreamListener(listener.get());

    EXPECT_TRUE(listener->completed())
        << "removeStreamListener() returned while a callback was still running on the listener — "
           "destroying it here is a use-after-free, which is the whole defect in issue #89";

    // The destroy the contract exists to make safe, and it happens BEFORE the server — the exact
    // ordering addStreamListener's comment forbids without a hand-back.
    listener.reset();

    server.stop();
}

// #89, the primitive itself. removeStreamListener() on BOTH AudioStreamServer and
// AudioStreamClient is an erase plus CallbackDispatcher::fence(), so fence()'s three no-wait
// paths are shared by both classes and are the dangerous edges: each one is a case where the
// caller CANNOT be made to wait, and getting any of them wrong is a hang or a self-deadlock
// rather than a wrong answer. None of them is reachable through the two arms above.
TEST(CallbackDispatcherFence, WaitsForQueuedWorkAndNeverHangsOnAPathThatCannotWait) {
    // 1. The wait itself: a task queued before fence() has completed by the time it returns.
    {
        CallbackDispatcher d;
        d.start();
        std::atomic<bool> ran{false};
        d.post([&ran] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            ran.store(true);
        });
        EXPECT_TRUE(d.fence()) << "fence() must report that it actually waited";
        EXPECT_TRUE(ran.load()) << "fence() returned while a queued task was still running";
        d.stop();
    }

    // 2. Never started: nothing was ever queued, so there is nothing to wait for. Must return
    //    false immediately rather than block forever on a marker no thread will ever run.
    {
        CallbackDispatcher d;
        EXPECT_FALSE(d.fence()) << "fence() on a never-started dispatcher must not wait";
    }

    // 3. Already stopped: post() no-ops from then on and stop() has drained what was queued, so
    //    again there is nothing pending — and a marker posted here would never run. This is the
    //    path that would HANG a removeStreamListener() called after teardown.
    {
        CallbackDispatcher d;
        d.start();
        d.stop();
        EXPECT_FALSE(d.fence()) << "fence() on a stopped dispatcher must not wait";
    }

    // 4. Called ON the dispatch thread — a thread cannot wait for itself. This is the path a
    //    consumer takes by calling removeStreamListener() from inside a callback; it must detach
    //    without deadlocking. Asserted from within a dispatched task, which is the only place the
    //    condition exists.
    {
        CallbackDispatcher d;
        d.start();
        std::atomic<bool> observed{false};
        std::atomic<bool> waited{true};
        d.post([&d, &observed, &waited] {
            waited.store(d.fence());  // must be false, and must return rather than self-deadlock
            observed.store(true);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!observed.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_TRUE(observed.load())
            << "the dispatched task never returned — fence() self-deadlocked on its own thread";
        EXPECT_FALSE(waited.load()) << "fence() must not claim to have waited on its own thread";
        d.stop();
    }
}
