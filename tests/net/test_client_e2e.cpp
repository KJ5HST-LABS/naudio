// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — AudioStreamClient end-to-end.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// AudioStreamClient end-to-end against a real AudioStreamServer, over loopback
// (FakeBackend / inject-only). Hardware-free. The hardware smoke (48kHz no-underrun +
// real virtual-sink bridge) is operator-gated and lives outside CI.

#include "naudio/net/AudioStreamClient.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "naudio/AudioStreamConfig.hpp"
#include "naudio/DeviceBackend.hpp"
#include "naudio/FakeBackend.hpp"  // FakePlaybackStream
#include "naudio/Stream.hpp"
#include "naudio/Types.hpp"
#include "naudio/net/AudioStreamServer.hpp"

using namespace naudio;
using namespace naudio::net;

namespace {

// Polls `pred` every 5ms until it is true or the budget elapses.
template <typename Pred>
bool waitFor(Pred pred, int budgetMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

bool waitForServerCount(const AudioStreamServer& server, int n, int budgetMs) {
    return waitFor([&]() { return server.clientCount() == n; }, budgetMs);
}

// A capture stream that fills each read with a fixed byte and paces itself (5ms/read) so the
// client's capture loop does not spin at 100% CPU (FakeCaptureStream returns immediately). Same
// device-seam stand-in test_server.cpp uses for the server's capture side.
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

// A backend whose capture stream is paced (0xA5) and whose playback discards — a hardware-free
// stand-in for the client's local virtual devices that does not spin the capture loop.
class PacedBackend : public DeviceBackend {
public:
    std::vector<RawDevice> enumerate() override { return {}; }
    bool probeFormat(int, const AudioFormat&, Direction) override { return true; }
    std::unique_ptr<CaptureStream> openCaptureStream(int, const AudioFormat& fmt) override {
        return std::make_unique<PacedCaptureStream>(fmt, 0xA5);
    }
    std::unique_ptr<PlaybackStream> openPlaybackStream(int, const AudioFormat& fmt) override {
        return std::make_unique<FakePlaybackStream>(fmt);
    }
};

// Records the client's lifecycle / roster / TX / reconnect callbacks for assertion.
class RecordingListener : public AudioClientListener {
public:
    void onClientConnected(const std::string&, const std::string&) override {
        connected.store(true);
    }
    void onClientDisconnected(const std::string&) override { ++disconnectedCount; }
    void onStreamStarted(const std::string&) override { streamStarted.store(true); }
    void onClientsUpdate(int count, int, const std::string&,
                         const std::vector<std::string>&) override {
        lastClientCount.store(count);
        ++clientsUpdateCount;
    }
    void onTxGranted() override { txGranted.store(true); }
    void onReconnecting(const std::string&, int, int) override { reconnecting.store(true); }
    void onReconnected(const std::string&) override { reconnected.store(true); }

    std::atomic<bool> connected{false};
    std::atomic<bool> streamStarted{false};
    std::atomic<bool> txGranted{false};
    std::atomic<bool> reconnecting{false};
    std::atomic<bool> reconnected{false};
    std::atomic<int> disconnectedCount{0};
    std::atomic<int> clientsUpdateCount{0};
    std::atomic<int> lastClientCount{-1};
};

AudioStreamConfig injectOnlyServerConfig() {
    AudioStreamConfig c{};
    c.maxClients = 4;
    c.txIdleTimeoutMs = 5000;  // keep TX granted through a test
    return c;
}

}  // namespace

// --- Construction / accessors (no server) ---

TEST(Client, ConstructionDefaults) {
    AudioStreamClient client{"127.0.0.1", 4533};
    EXPECT_EQ(client.config().transportType, TransportType::Tcp);
    EXPECT_FALSE(client.isConnected());
    EXPECT_FALSE(client.isStreaming());
    EXPECT_TRUE(client.isCaptureMuted());     // starts muted (RX mode)
    EXPECT_FALSE(client.isPlaybackMuted());   // starts unmuted (hear RX)
    EXPECT_TRUE(client.isAutoReconnect());
    EXPECT_EQ(client.reconnectAttempt(), 0);
    EXPECT_FALSE(client.isReconnecting());
    EXPECT_EQ(client.serverClientCount(), -1);
    EXPECT_EQ(client.serverMaxClients(), -1);
    EXPECT_TRUE(client.serverTxOwner().empty());
    EXPECT_FALSE(client.serverClientsInfo().has_value());
}

TEST(Client, SetConfigGuardBeforeConnect) {
    AudioStreamClient client{"127.0.0.1", 4533};
    AudioStreamConfig c{};
    c.transportType = TransportType::Udp;
    EXPECT_TRUE(client.setConfig(c));
    EXPECT_EQ(client.config().transportType, TransportType::Udp);
}

TEST(Client, MutePttAccessors) {
    AudioStreamClient client{"127.0.0.1", 4533};
    client.setCaptureMuted(false);
    EXPECT_FALSE(client.isCaptureMuted());
    client.setPlaybackMuted(true);
    EXPECT_TRUE(client.isPlaybackMuted());

    client.setPTT(true);  // transmit: capture unmuted, playback muted
    EXPECT_FALSE(client.isCaptureMuted());
    EXPECT_TRUE(client.isPlaybackMuted());
    client.setPTT(false);  // receive: capture muted, playback unmuted
    EXPECT_TRUE(client.isCaptureMuted());
    EXPECT_FALSE(client.isPlaybackMuted());
}

TEST(Client, ConnectWithoutPlaybackDeviceFails) {
    PacedBackend backend;
    AudioStreamServer server{0, injectOnlyServerConfig()};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    AudioStreamClient client{"127.0.0.1", static_cast<std::uint16_t>(server.port())};
    client.setBackend(&backend);
    // No playback device configured — connect must fail before any transport connect.
    EXPECT_FALSE(client.connect(&err));
    EXPECT_FALSE(client.isConnected());

    server.stop();
}

TEST(Client, DisconnectWithoutConnect) {
    AudioStreamClient client{"127.0.0.1", 4533};
    client.disconnect();  // no-op, must not crash
    EXPECT_FALSE(client.isConnected());
}

// ===========================================================================
// End-to-end client <-> server over FakeBackend.
// ===========================================================================

// THE GATE (TCP): a real AudioStreamClient connects to a real AudioStreamServer, completes the
// handshake, receives injected RX byte-identically into its audio listener, sends captured TX
// (its paced 0xA5 capture) which the server grants (TX_GRANTED -> onTxGranted), and sees the
// roster. Disconnect leaves the server with zero clients.
TEST(Client, GateE2eRxBroadcastAndTxEvents) {
    PacedBackend backend;
    AudioStreamServer server{0, injectOnlyServerConfig()};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    AudioStreamClient client{"127.0.0.1", static_cast<std::uint16_t>(server.port())};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setCaptureDevice(0);
    client.setCaptureMuted(false);  // enable TX so the capture->send path runs
    client.setAutoReconnect(false);

    RecordingListener listener;
    client.addStreamListener(&listener);

    std::mutex rxMutex;
    std::vector<std::vector<std::uint8_t>> rxFrames;
    client.addAudioListener([&](const std::uint8_t* d, std::size_t n) {
        std::lock_guard<std::mutex> l(rxMutex);
        rxFrames.emplace_back(d, d + n);
    });

    ASSERT_TRUE(client.connect(&err)) << err;
    EXPECT_TRUE(client.isConnected());
    EXPECT_TRUE(client.isStreaming());
    // Event callbacks now fire asynchronously on the dispatch thread, so poll
    // for the flag rather than asserting it synchronously right after connect().
    EXPECT_TRUE(waitFor([&]() { return listener.streamStarted.load(); }, 2000));

    // Roster: the server reports one client to us.
    ASSERT_TRUE(waitFor([&]() { return listener.lastClientCount.load() == 1; }, 3000));
    EXPECT_TRUE(listener.connected.load());
    EXPECT_EQ(client.serverClientCount(), 1);

    // RX: inject a known payload; the client's audio listener must receive it byte-identically.
    const std::vector<std::uint8_t> known = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    for (int i = 0; i < 20; i++) server.injectAudio(known);
    ASSERT_TRUE(waitFor(
        [&]() {
            std::lock_guard<std::mutex> l(rxMutex);
            for (const auto& f : rxFrames) {
                if (f == known) return true;
            }
            return false;
        },
        3000));

    // TX: the paced 0xA5 capture flows client -> server, the mixer grants TX (TX_GRANTED).
    ASSERT_TRUE(waitFor([&]() { return listener.txGranted.load(); }, 3000));
    EXPECT_FALSE(server.txOwner().empty());

    client.disconnect();
    EXPECT_FALSE(client.isConnected());
    // onClientDisconnected is dispatched asynchronously now — poll for it.
    EXPECT_TRUE(waitFor([&]() { return listener.disconnectedCount.load() >= 1; }, 2000));
    EXPECT_TRUE(waitForServerCount(server, 0, 2000));

    server.stop();
}

// Re-entrancy gate: a listener may call back INTO the client (disconnect(), setters,
// getters) from inside its event callbacks without deadlocking. Originally these callbacks
// ran synchronously on a worker thread, so a callback calling disconnect() -> waitForWorkers()
// self-deadlocked; now they run on a dedicated dispatch thread, so the worker-join cannot
// wait on the calling thread.
TEST(Client, GateCallbackReentrancyNoDeadlock) {
    PacedBackend backend;
    AudioStreamServer server{0, injectOnlyServerConfig()};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    struct ReentrantListener : public AudioClientListener {
        AudioStreamClient* client = nullptr;
        std::atomic<bool> setterFromConnected{false};
        std::atomic<bool> reentered{false};
        void onClientConnected(const std::string&, const std::string&) override {
            // Setters + getters from inside a callback must be safe (no internal-lock recursion).
            client->setCaptureMuted(true);
            client->setPlaybackMuted(false);
            (void)client->isConnected();
            (void)client->serverClientCount();
            setterFromConnected.store(true);
        }
        void onStreamStarted(const std::string&) override {
            // The deadlock case: disconnect() from inside a callback. Must return promptly.
            client->disconnect();
            reentered.store(true);
        }
    } listener;

    AudioStreamClient client{"127.0.0.1", static_cast<std::uint16_t>(server.port())};
    listener.client = &client;
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setAutoReconnect(false);
    client.addStreamListener(&listener);

    ASSERT_TRUE(client.connect(&err)) << err;
    // Both callbacks run on the dispatch thread; the disconnect() inside onStreamStarted must
    // complete (no self-deadlock) and must not resurrect the connection.
    ASSERT_TRUE(waitFor([&]() { return listener.reentered.load(); }, 3000))
        << "disconnect() from inside a callback did not return — deadlock";
    EXPECT_TRUE(listener.setterFromConnected.load());
    EXPECT_TRUE(waitFor([&]() { return !client.isConnected(); }, 3000));
    EXPECT_FALSE(client.isConnected());  // no resurrection
    server.stop();
}

// THE GATE (UDP): the same client over the UDP transport (server serves DUAL) — proves the
// client's createTransport(UDP) + the 3b reliability data path end-to-end for RX + roster.
TEST(Client, GateE2eUdpTransport) {
    PacedBackend backend;
    AudioStreamConfig scfg = injectOnlyServerConfig();
    scfg.transportType = TransportType::Dual;  // serve TCP + UDP on one port
    AudioStreamServer server{0, scfg};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    AudioStreamClient client{"127.0.0.1", static_cast<std::uint16_t>(server.port())};
    AudioStreamConfig ccfg{};
    ccfg.transportType = TransportType::Udp;  // client picks UDP
    ASSERT_TRUE(client.setConfig(ccfg));
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setAutoReconnect(false);

    RecordingListener listener;
    client.addStreamListener(&listener);

    std::mutex rxMutex;
    std::vector<std::vector<std::uint8_t>> rxFrames;
    client.addAudioListener([&](const std::uint8_t* d, std::size_t n) {
        std::lock_guard<std::mutex> l(rxMutex);
        rxFrames.emplace_back(d, d + n);
    });

    ASSERT_TRUE(client.connect(&err)) << err;
    EXPECT_TRUE(client.isConnected());
    ASSERT_TRUE(waitFor([&]() { return listener.lastClientCount.load() == 1; }, 3000));

    const std::vector<std::uint8_t> known = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    for (int i = 0; i < 30; i++) server.injectAudio(known);
    ASSERT_TRUE(waitFor(
        [&]() {
            std::lock_guard<std::mutex> l(rxMutex);
            for (const auto& f : rxFrames) {
                if (f == known) return true;
            }
            return false;
        },
        3000));

    client.disconnect();
    EXPECT_FALSE(client.isConnected());
    EXPECT_TRUE(waitForServerCount(server, 0, 2000));

    server.stop();
}

// THE GATE (reconnect): killing the server mid-stream triggers the client's backoff reconnect;
// bringing a fresh server up on the same port reconnects the client (generation-counter guard).
TEST(Client, GateReconnectAfterServerRestart) {
    PacedBackend backend;
    AudioStreamConfig scfg = injectOnlyServerConfig();

    auto server1 = std::make_unique<AudioStreamServer>(0, scfg);
    server1->setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server1->start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server1->port());

    AudioStreamClient client{"127.0.0.1", port};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setAutoReconnect(true);
    client.setReconnectDelayMs(100);
    client.setMaxReconnectDelayMs(500);
    client.setMaxReconnectAttempts(50);

    RecordingListener listener;
    client.addStreamListener(&listener);

    ASSERT_TRUE(client.connect(&err)) << err;
    ASSERT_TRUE(waitFor([&]() { return client.isConnected(); }, 2000));

    // Kill the server — the receive worker detects the closed connection and begins reconnecting.
    server1->stop();
    server1.reset();
    ASSERT_TRUE(
        waitFor([&]() { return listener.reconnecting.load() || client.isReconnecting(); }, 4000));

    // Fresh server on the SAME port (SO_REUSEADDR) — the client must reconnect.
    AudioStreamServer server2{port, scfg};
    server2.setInjectOnlyMode(true);
    ASSERT_TRUE(server2.start(&err)) << err;

    ASSERT_TRUE(
        waitFor([&]() { return listener.reconnected.load() && client.isConnected(); }, 10000));
    EXPECT_FALSE(client.isReconnecting());

    client.disconnect();
    server2.stop();
}

// C1: disconnecting WHILE a reconnect is in flight (server gone) must return promptly
// (the reconnect connect deadline is bounded + interruptible) and must NOT be
// resurrected by the in-flight reconnect attempt. This drives handleConnectionLost
// during the reconnecting_ window (also exercising the C7 path) and the closed_
// re-checks in reconnectInternal/runConnect.
TEST(Client, DisconnectDuringReconnectNoResurrection) {
    PacedBackend backend;
    AudioStreamConfig scfg = injectOnlyServerConfig();
    auto server = std::make_unique<AudioStreamServer>(0, scfg);
    server->setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server->start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server->port());

    AudioStreamClient client{"127.0.0.1", port};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setAutoReconnect(true);
    client.setReconnectDelayMs(100);
    client.setMaxReconnectDelayMs(200);
    client.setMaxReconnectAttempts(100);

    RecordingListener listener;
    client.addStreamListener(&listener);
    ASSERT_TRUE(client.connect(&err)) << err;
    ASSERT_TRUE(waitFor([&]() { return client.isConnected(); }, 2000));

    // Kill the server so the client enters its reconnect loop (every attempt fails).
    server->stop();
    server.reset();
    ASSERT_TRUE(
        waitFor([&]() { return client.isReconnecting() || listener.reconnecting.load(); }, 4000));

    // Disconnect mid-reconnect: must return promptly and leave the client closed.
    const auto t0 = std::chrono::steady_clock::now();
    client.disconnect();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
    // Budget = kReconnectConnectTimeoutMs (5000 — the one uninterruptible window:
    // transport->connect() inside a reconnect attempt is not woken by doClose) plus
    // CI-scheduler margin. The regression being guarded is a disconnect stalled for
    // the whole retry ladder (tens of seconds), so the margin costs no strength.
    EXPECT_LT(elapsedMs, 8000) << "disconnect during reconnect was stalled (C1 interruptibility)";
    EXPECT_FALSE(client.isConnected());

    // Prove the negative: no in-flight reconnect resurrects the connection.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_FALSE(client.isConnected());
    EXPECT_FALSE(client.isReconnecting());
}

// THE GATE (generation/closed guard): an explicit disconnect with auto-reconnect ENABLED must
// NOT spawn a reconnect, and the workers tearing down must fire exactly one disconnect (no
// storm). This exercises the closed-guard in handleConnectionLost.
TEST(Client, DisconnectStopsReconnect) {
    PacedBackend backend;
    AudioStreamServer server{0, injectOnlyServerConfig()};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    AudioStreamClient client{"127.0.0.1", static_cast<std::uint16_t>(server.port())};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setAutoReconnect(true);  // even with auto-reconnect ON...

    RecordingListener listener;
    client.addStreamListener(&listener);
    ASSERT_TRUE(client.connect(&err)) << err;
    ASSERT_TRUE(waitFor([&]() { return client.isConnected(); }, 2000));

    client.disconnect();  // ...an explicit disconnect must not reconnect (closed-guard).
    EXPECT_FALSE(client.isConnected());

    // Give any errant reconnect a chance to (wrongly) fire — a bounded prove-a-negative wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(client.isReconnecting());
    EXPECT_FALSE(listener.reconnecting.load());
    EXPECT_EQ(listener.disconnectedCount.load(), 1);  // exactly one, no storm

    server.stop();
}
