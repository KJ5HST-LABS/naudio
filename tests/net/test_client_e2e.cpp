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
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "naudio/AudioStreamConfig.hpp"
#include "naudio/ControlReliability.hpp"  // DEFAULT_TIMEOUT_MS — the handshake-ARQ arms' bound
#include "naudio/DeviceBackend.hpp"
#include "naudio/FakeBackend.hpp"  // FakePlaybackStream
#include "naudio/Stream.hpp"
#include "naudio/Types.hpp"
#include "naudio/net/AudioStreamServer.hpp"

#include "ScriptedTransport.hpp"  // #71: the client-side seam's doubles

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

// #59: a backend whose device streams die MID-STREAM, the way PortAudio reports a codec that
// was unplugged (applyReadStatus / applyWriteStatus throw DeviceUnavailable on any PaError).
// FakeBackend and PacedBackend only ever failed at OPEN time, which is why the four device-IO
// worker loops had no detector at all — the suite was structurally blind to this path.
class DyingBackend : public DeviceBackend {
public:
    int throwAfterReads = -1;   // capture dies after this many reads  (-1 = never)
    int throwAfterWrites = -1;  // playback dies after this many writes (-1 = never)

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
    // #59's observable. Before this, NOTHING in tests/ read onError at all — so every
    // error the client reports across the C ABI was untested in both directions (L145).
    void onError(const std::string&, const std::string& error) override {
        std::lock_guard<std::mutex> l(errorMutex);
        errors.push_back(error);
    }

    bool sawError(const std::string& needle) {
        std::lock_guard<std::mutex> l(errorMutex);
        for (const auto& e : errors) {
            if (e.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    std::mutex errorMutex;
    std::vector<std::string> errors;

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

// A one-hop UDP relay that DROPS the first `dropFirstN` client->server CONNECT_REQUEST datagrams
// and forwards everything else verbatim (issue #29 option 4).
//
// Deliberately not tests/client_stats_proxy.cpp: that relay exists to serve the pure-C stats arms,
// drops server->client AUDIO_RX only, and says in its own header comment that dropping control
// traffic "would break the handshake" — which was TRUE when it was written and is the very claim
// under test here. Extending it would have put a control-dropping mode into a fixture three C
// tests depend on, to be used by neither.
//
// It parses rather than counting datagrams: the client's first packet is its CONNECT_REQUEST
// today, but an arm that silently stops testing retransmission the day a client sends anything
// earlier is worse than no arm. Matching on the type means this either drops a CONNECT_REQUEST or
// drops nothing.
class ConnectRequestDroppingRelay {
public:
    // Binds an ephemeral loopback port relaying to 127.0.0.1:serverPort. Returns false if the
    // socket could not be bound.
    bool start(std::uint16_t serverPort, int dropFirstN) {
        serverPort_ = serverPort;
        dropRemaining_.store(dropFirstN);
        sock_ = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
        if (!sock_.valid()) return false;
        const int room = static_cast<int>(AudioPacket::HEADER_SIZE + AudioPacket::MAX_PAYLOAD +
                                          AudioPacket::CRC_SIZE + 64);
        sock_.setRecvBufferAtLeast(room);
        sock_.setSendBufferAtLeast(room);
        port_ = sock_.localPort();
        worker_ = std::thread([this]() { loop(); });
        return port_ != 0;
    }

    ~ConnectRequestDroppingRelay() { stop(); }

    void stop() {
        if (!worker_.joinable()) return;
        stop_.store(true);
        worker_.join();
        sock_.close();
    }

    std::uint16_t port() const { return port_; }
    long long connectRequestsSeen() const { return seen_.load(); }
    long long connectRequestsDropped() const { return dropped_.load(); }

private:
    static bool isConnectRequest(const std::uint8_t* data, std::size_t n) {
        const std::optional<AudioPacket> pkt = AudioPacket::deserialize(data, n);
        if (!pkt || pkt->packetType() != PacketType::Control) return false;
        const std::optional<ControlMessage> msg = ControlMessage::deserialize(pkt->payload());
        return msg && msg->messageType() == ControlType::ConnectRequest;
    }

    void loop() {
        std::vector<std::uint8_t> buf(AudioPacket::HEADER_SIZE + AudioPacket::MAX_PAYLOAD +
                                      AudioPacket::CRC_SIZE + 64);
        std::string clientHost;
        std::uint16_t clientPort = 0;
        sock_.setRecvTimeout(50);  // so stop_ is observed promptly at teardown
        while (!stop_.load()) {
            const RecvFromResult rr = sock_.recvFrom(buf.data(), buf.size());
            if (rr.status == IoStatus::TimedOut) continue;
            if (rr.status != IoStatus::Ok) break;

            if (rr.senderPort == serverPort_) {
                if (clientPort == 0) continue;  // no client endpoint latched yet
                sock_.sendTo(buf.data(), rr.bytes, clientHost, clientPort);
                continue;
            }
            clientHost = rr.senderHost;
            clientPort = rr.senderPort;
            if (isConnectRequest(buf.data(), rr.bytes)) {
                seen_.fetch_add(1);
                if (dropRemaining_.load() > 0) {
                    dropRemaining_.fetch_sub(1);
                    dropped_.fetch_add(1);
                    continue;  // the induced loss
                }
            }
            sock_.sendTo(buf.data(), rr.bytes, "127.0.0.1", serverPort_);
        }
    }

    Socket sock_;
    std::uint16_t serverPort_ = 0;
    std::uint16_t port_ = 0;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<int> dropRemaining_{0};
    std::atomic<long long> seen_{0};
    std::atomic<long long> dropped_{0};
};

// A client config with control-ARQ actually enabled. The existing UDP arm above uses a default
// AudioStreamConfig, where controlReliabilityEnabled is FALSE — so it exercises none of this.
AudioStreamConfig arqClientConfig() {
    AudioStreamConfig c = AudioStreamConfig::udpLan();  // Udp + controlReliabilityEnabled
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

// --- #59: mid-stream device loss must not abort the process -----------------------------
//
// Both client device loops run on DETACHED threads, so before the fix an escaping
// DeviceUnavailable was std::terminate: this binary would ABORT rather than fail, and the C
// ABI's on_error would never fire. Reaching the assertions at all is therefore half of what
// each arm proves; sawError() is the other half, and it is the part a mutation can move.

TEST(Client, PlaybackDeviceLostMidStreamIsReportedNotFatal) {
    DyingBackend backend;
    backend.throwAfterWrites = 3;  // a few good frames, then the device goes away

    AudioStreamServer server{0, injectOnlyServerConfig()};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    AudioStreamClient client{"127.0.0.1", static_cast<std::uint16_t>(server.port())};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setAutoReconnect(false);

    RecordingListener listener;
    client.addStreamListener(&listener);

    ASSERT_TRUE(client.connect(&err)) << err;
    ASSERT_TRUE(waitFor([&]() { return listener.streamStarted.load(); }, 2000));

    EXPECT_TRUE(waitFor([&]() { return listener.sawError("Playback device lost"); }, 3000));

    // Still usable afterwards: disconnect() completing rather than hanging also proves
    // threadFinished() ran, which an escaping exception would have skipped.
    client.disconnect();
    EXPECT_FALSE(client.isConnected());
    server.stop();
}

TEST(Client, CaptureDeviceLostMidStreamIsReportedNotFatal) {
    DyingBackend backend;
    backend.throwAfterReads = 3;  // playback stays healthy; only capture dies

    AudioStreamServer server{0, injectOnlyServerConfig()};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    AudioStreamClient client{"127.0.0.1", static_cast<std::uint16_t>(server.port())};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setCaptureDevice(0);
    client.setCaptureMuted(false);
    client.setAutoReconnect(false);

    RecordingListener listener;
    client.addStreamListener(&listener);

    ASSERT_TRUE(client.connect(&err)) << err;
    ASSERT_TRUE(waitFor([&]() { return listener.streamStarted.load(); }, 2000));

    EXPECT_TRUE(waitFor([&]() { return listener.sawError("Capture device lost"); }, 3000));

    client.disconnect();
    EXPECT_FALSE(client.isConnected());
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

// --- Handshake control-ARQ (issue #29 option 4) -------------------------------------------------
//
// Before this, ConnectRequest was the ONLY message in the connection-establishment exchange that
// was not retransmitted — ConnectAccept, ConnectReject and AudioConfig all were. So a single lost
// client datagram cost the caller the entire kConnectTimeoutMs (10 s) and returned a failed
// connect, while the identical loss in the server->client direction recovered in one retransmit
// interval. These two arms are the pair: one shows the loss is survived, the other shows the relay
// is capable of causing the failure it claims to.
//
// WHAT MAKES THIS A REAL ARM RATHER THAN A RESTATEMENT OF THE ENUM. Marking the type critical only
// makes it PENDING; the sweep still has to run, and its usual pump belongs to the heartbeat loop,
// which does not exist until after the handshake returns. Reverting EITHER half — the enum entry in
// ControlReliability::isCriticalType, or the sliced wait in AudioStreamClient's
// receiveWhilePumpingArq — reddens the first arm. That two-sided sensitivity is the point: a
// pending entry nobody sweeps is exactly the inert state ControlType::Disconnect is in, and it is
// the defect issue #29 was filed to report.

TEST(Client, GateHandshakeSurvivesALostConnectRequest) {
    AudioStreamConfig scfg = injectOnlyServerConfig();
    scfg.transportType = TransportType::Udp;
    AudioStreamServer server{0, scfg};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    ConnectRequestDroppingRelay relay;
    ASSERT_TRUE(relay.start(static_cast<std::uint16_t>(server.port()), 1))
        << "relay could not bind";

    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", relay.port()};
    ASSERT_TRUE(client.setConfig(arqClientConfig()));
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setAutoReconnect(false);  // the retransmit must do the work, not a reconnect

    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = client.connect(&err);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();

    EXPECT_TRUE(ok) << "connect failed after its CONNECT_REQUEST was dropped once — the request "
                       "was not retransmitted (err: "
                    << err << ")";
    EXPECT_EQ(relay.connectRequestsDropped(), 1)
        << "the relay did not drop a CONNECT_REQUEST, so this arm induced no fault at all";
    EXPECT_GE(relay.connectRequestsSeen(), 2)
        << "only " << relay.connectRequestsSeen()
        << " CONNECT_REQUEST(s) reached the relay — the client never resent one, so the connect "
           "above (if it passed) did not pass for the reason this arm claims";

    // It recovered via the ARQ timeout, not by outlasting the connect window. The lower bound is
    // the retransmit interval itself; the upper is far below kConnectTimeoutMs (10000), so a
    // regression that "passes" by simply waiting out the whole window still reddens here.
    //
    // MEASURED so nobody has to re-derive the margins before deciding whether these bounds are
    // tight: 5 of 5 runs on macOS/arm64 landed at 506-508 ms — one DEFAULT_TIMEOUT_MS (500) plus
    // about 7 ms of loopback and handshake. So the lower bound sits at ~2x below the observed
    // value and the upper at ~10x above it, and neither is near the lattice the measurement lands
    // on. The lossless control below measures 2 ms, which is what makes the lower bound a real
    // discriminator rather than a formality.
    EXPECT_GE(elapsedMs, ControlReliability::DEFAULT_TIMEOUT_MS / 2);
    EXPECT_LT(elapsedMs, 5000)
        << "connect took " << elapsedMs
        << " ms — that is the connect window elapsing, not a retransmit recovering";

    client.disconnect();
    relay.stop();
    server.stop();
}

// THE CONTROL, and it is not optional. The arm above asserts that a dropped CONNECT_REQUEST is
// survived; on its own that is satisfied just as well by a relay which never dropped anything.
// This one runs the identical path with the drop count at 0 and requires the SAME success — so the
// two together say the fault was induced and recovered, rather than never induced. It also pins the
// cost of the extra hop: a lossless relay must not push the connect past the retransmit interval,
// or the bound in the arm above would be measuring the relay instead of the ARQ.
TEST(Client, HandshakeThroughALosslessRelayIsUnaffected) {
    AudioStreamConfig scfg = injectOnlyServerConfig();
    scfg.transportType = TransportType::Udp;
    AudioStreamServer server{0, scfg};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    ConnectRequestDroppingRelay relay;
    ASSERT_TRUE(relay.start(static_cast<std::uint16_t>(server.port()), 0));

    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", relay.port()};
    ASSERT_TRUE(client.setConfig(arqClientConfig()));
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setAutoReconnect(false);

    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(client.connect(&err)) << err;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();

    EXPECT_EQ(relay.connectRequestsDropped(), 0);
    EXPECT_EQ(relay.connectRequestsSeen(), 1)
        << "the client sent more than one CONNECT_REQUEST with nothing dropped";

    // WHAT THIS ARM DOES NOT COVER, AND WHERE IT IS COVERED INSTEAD. A lossless connect completes
    // in ~2 ms, far inside the 500 ms retransmit interval, so the count above is read before any
    // retransmit could fire either way — it cannot tell an acknowledged request from a
    // tracked-but-never-ACKed one, and the latter would add two wasted datagrams to EVERY
    // connection.
    //
    // The obvious repair — sleep past the interval and re-read — was WRITTEN, MEASURED, AND
    // REJECTED. It is vacuous at any sleep under ~3 s: after connect the sweep's only pump is the
    // heartbeat loop, which ticks every kHeartbeatCheckIntervalMs (3000 ms), so a request due for
    // retransmit at 500 ms is not swept until the next tick. Proved by mutation: a receiver
    // patched to never ACK a CONNECT_REQUEST SURVIVED the 800 ms version of this check. Making it
    // real costs ~4 s of suite time to pin a two-datagram waste.
    //
    // ControlReliability.GeneratesAnAckForAConnectRequest settles the same claim at the unit level,
    // deterministically and in microseconds, and that mutation kills it. Do not re-add a sleeping
    // version here without first checking it against the 3000 ms pump interval.
    // MEASURED at 2 ms, 5 of 5 — so the extra relay hop is free and this bound carries ~250x
    // headroom. It is deliberately loose rather than tight: what it has to catch is a handshake
    // that has quietly become retransmit-driven even with nothing dropped, and that costs a whole
    // DEFAULT_TIMEOUT_MS, not a few milliseconds.
    EXPECT_LT(elapsedMs, ControlReliability::DEFAULT_TIMEOUT_MS)
        << "a lossless handshake took " << elapsedMs << " ms";

    client.disconnect();
    relay.stop();
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

// ===========================================================================
// The client transport seam (#71).
// ===========================================================================

// The seam itself, and the null-return guard it makes reachable. AudioStreamClient::connect
// dereferences createTransport()'s result on the next statement, so before this guard a
// factory returning null was a null-deref reachable from public API — the one hop that
// mirroring AudioStreamServer::setTransportFactory does not carry across on its own. The
// server has had this guard since #67 (src/net/AudioStreamServer.cpp, start()); the client
// has two call sites needing it, connect() and reconnectInternal().
TEST(Client, ConnectFailsWhenTheTransportFactoryReturnsNothing) {
    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", 4533};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setTransportFactory([]() { return std::shared_ptr<ClientTransport>{}; });

    std::string err;
    EXPECT_FALSE(client.connect(&err));
    EXPECT_NE(err.find("Transport factory"), std::string::npos) << "err was: " << err;
    EXPECT_FALSE(client.isConnected());
}

// ===========================================================================
// disconnect()'s courtesy salvo (#71).
// ===========================================================================
//
// These arms need a send that FAILS ON DEMAND, which no real socket stages cheaply: a wedged
// TCP peer costs one 5 s send budget per salvo copy (AudioProtocolHandler.cpp, the
// CONNECTION_TIMEOUT_MS / 2 arming), and getting one wedged at all needs a peer that stops
// reading. The scripted transport supplies the failure directly, so these run in
// milliseconds and do not depend on how a platform absorbs loopback writes -- which is what
// made the #70 send-budget arm unrunnable on Windows (#74).

// Builds a client on a scripted connection and takes it all the way to streaming. Returns the
// connection so the caller can arm faults and read the ledger, or null if the handshake did
// not complete -- callers ASSERT on it, so a later assertion can never pass because the setup
// silently failed (L136: the guard that separates "the fault fired" from "the setup ran").
namespace {
std::shared_ptr<naudio::test::ScriptedClientConnection> connectOverScriptedTransport(
    AudioStreamClient& client, std::string* err) {
    auto conn = std::make_shared<naudio::test::ScriptedClientConnection>("scripted-peer");
    conn->pushConnectAccept();
    auto transport = std::make_shared<naudio::test::ScriptedClientTransport>(conn);
    client.setTransportFactory([transport]() { return transport; });
    if (!client.connect(err)) return nullptr;
    return conn;
}
}  // namespace

// --- issue #57.2 — disconnect() racing a completing connect() ------------------------------
//
// A listener that signals when the connect path has passed its connected_ commit.
//
// IT DOES NOT PARK THE CONNECT THREAD, and the attempt to use it that way is worth recording
// because it is the obvious approach and it FAILS SILENTLY. runConnect commits connected_ under
// runMutex_ and then calls notifyClientConnected() (src/net/AudioStreamClient.cpp:191), which
// looks like a synchronous hook sitting exactly in the window before startWorkerThreads(). It
// is not: notifyClientConnected POSTS to dispatcher_ (:1121) rather than calling listeners
// inline, so blocking inside onClientConnected blocks the DISPATCHER thread while runConnect
// sails on. Measured, not assumed — a probe printed `startWorkerThreads ... closed=0` BEFORE
// the test thread had even called disconnect(), and the arm passed against deliberately
// unguarded code. Every client listener callback goes through the same dispatcher, so no
// listener anywhere is a synchronous seam on a client worker path.
class ConnectBlockingListener : public naudio::net::AudioClientListener {
public:
    void onClientConnected(const std::string&, const std::string&) override {
        entered_.store(true);
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return released_; });
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
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    std::atomic<bool> entered_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool released_ = false;
};

// #57.2 — disconnect() concurrent with a completing connect() must not deadlock or crash.
//
// ⚠ THIS ARM IS NOT A DETECTOR FOR THE NULL-SPAWN FIX IT SITS NEXT TO. It passes identically
// with and without the `if (!conn || !rx)` / `if (pb)` guards in startWorkerThreads —
// MEASURED, not assumed. It is named for what it actually covers so that a future reader
// cannot mistake a green here for coverage of the null spawn.
//
// The defect the guards fix is real and provable by reading: closeResources() nulls
// playbackStream_/rxBuffer_ under runMutex_ (src/net/AudioStreamClient.cpp:943-952),
// startWorkerThreads snapshots them under that same lock, and playbackLoop dereferences before
// testing anything — playback->actualFormat() is its first statement, rxBuffer-> its second. On
// a DETACHED thread that is process death with no on_error, reachable through the C ABI as
// na_client_disconnect racing a na_client_connect that is completing.
//
// WHY NO DETERMINISTIC ARM EXISTS. The window is between runConnect releasing runMutex_ after
// committing connected_ and startWorkerThreads re-taking it, and NOTHING synchronous runs in
// between: the one call that looks like a seam, notifyClientConnected, is asynchronous (see the
// listener above). Staging it would need a test-only seam in production code, which is a bigger
// change than the fix. The guards therefore rest on the read, not on a red test — recorded here
// rather than left for a reader to infer from a green suite.
TEST(Client, DisconnectConcurrentWithAConnectCommitTearsDownCleanly) {
    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", 4533};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);

    ConnectBlockingListener listener;
    client.addStreamListener(&listener);

    auto conn = std::make_shared<naudio::test::ScriptedClientConnection>("scripted-peer");
    conn->pushConnectAccept();
    auto transport = std::make_shared<naudio::test::ScriptedClientTransport>(conn);
    client.setTransportFactory([transport]() { return transport; });

    std::string err;
    std::atomic<bool> connectResult{false};
    std::thread connector([&]() { connectResult.store(client.connect(&err)); });

    // The connect thread is now parked with connected_ already true and no workers started.
    ASSERT_TRUE(listener.waitUntilEntered(3000))
        << "runConnect never reached notifyClientConnected — the window was not staged, so a "
           "green result here would be vacuous";

    // Tear the client down underneath it. This is the public call the C ABI exposes.
    client.disconnect();
    listener.release();
    connector.join();

    // MUST OUTLIVE THE SPAWN. Whatever startWorkerThreads() launched is DETACHED, so without
    // this the process can reach the end of the test — and, under a --gtest_filter that selects
    // only this arm, exit — before the playback worker is ever scheduled. That is not a
    // hypothetical: it is why the first version of this arm passed against the unguarded code.
    // A crash that happens after the last assertion is still a crash, but a test that exits
    // first cannot see it.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // connect() reports failure: runConnect returned true, but connect()'s own doClose path and
    // the disconnect have left the handle closed. What matters is that we got here at all.
    EXPECT_FALSE(client.isConnected());
}

// #58 facet 5 — na_client_stats.connected must not read 1 during a handshake that has not
// completed. naudio.h defines the field as "1 if a live connection supplied these numbers".
//
// stats() keyed on the connection OBJECT, which connect() installs BEFORE performHandshake runs.
// So for the whole up-to-10 s window of a connect that will fail, stats().connected read 1 while
// isConnected() returned 0 at the same instant — every counter beside it labelled as a reading
// from a connection that did not exist yet.
//
// THIS is the arm that pins the fix. The C-ABI arm in c_lifecycle_contract.c checks the same
// field only before and after a completed connect, and passes either way (after disconnect,
// closeResources has nulled connection_, so stats() returns early regardless) — measured, and
// labelled there as a consistency check rather than a detector. The window is only reachable
// where the handshake can be held open, which is here: a scripted connection that is never given
// a ConnectAccept parks performHandshake in receivePacket.
TEST(Client, StatsDoesNotClaimConnectedWhileTheHandshakeIsStillOutstanding) {
    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", 4533};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);

    // Deliberately NO pushConnectAccept: the handshake will block waiting for a reply.
    auto conn = std::make_shared<naudio::test::ScriptedClientConnection>("silent-peer");
    auto transport = std::make_shared<naudio::test::ScriptedClientTransport>(conn);
    client.setTransportFactory([transport]() { return transport; });

    std::string err;
    std::thread connector([&]() { client.connect(&err); });

    // Wait until the connection object is installed and the handshake is outstanding. Reading
    // stats() until it reports non-default would be the wrong probe — that is the bug itself —
    // so key on the ConnectRequest performHandshake sends immediately before it blocks.
    ASSERT_TRUE(waitFor([&]() { return conn->countSent(ControlType::ConnectRequest) > 0; }, 3000))
        << "the handshake never sent its ConnectRequest — the window was not staged, so a green "
           "result here would be vacuous";

    EXPECT_FALSE(client.isConnected()) << "precondition: the connect has not completed";
    EXPECT_FALSE(client.stats().connected)
        << "stats claimed a live connection while the handshake was still outstanding";

    client.disconnect();
    connector.join();
}

// THE POSITIVE CONTROL for the arm below, and the proof the salvo's second copy is reachable
// at all -- without it, "exactly one copy" cannot tell a working short-circuit from a salvo
// that never had two copies to begin with.
//
// It is also the UDP contract: on UDP the first sendto succeeds, so both copies must still go
// out with their 5 ms yields. That double-send is what stops a Winsock loopback discard from
// costing the server its full idle timeout (see disconnect()'s salvo comment).
TEST(Client, DisconnectSalvoSendsBothCopiesWhenTheFirstSucceeds) {
    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", 4533};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);

    std::string err;
    auto conn = connectOverScriptedTransport(client, &err);
    ASSERT_TRUE(conn) << "setup failed before the subject under test: " << err;
    ASSERT_TRUE(client.isConnected());

    client.disconnect();

    EXPECT_EQ(conn->countSent(ControlType::Disconnect), 2) << conn->diagnostics();
}

// THE SUBJECT. A first send that fails proves the socket is broken, so the second copy cannot
// arrive and can only cost a second send budget. Before the fix the salvo ignored
// sendControl's result and paid it anyway: worst case two budgets, ~10 s, on a peer already
// known to be gone.
//
// Why this discriminates: the double RECORDS failed sends (see failControlSends). The count is
// 2 before the fix and 1 after. If failures went unrecorded it would be 0 either way and the
// arm would pass vacuously.
TEST(Client, DisconnectSalvoStopsAfterAFailedFirstSend) {
    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", 4533};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);

    std::string err;
    auto conn = connectOverScriptedTransport(client, &err);
    ASSERT_TRUE(conn) << "setup failed before the subject under test: " << err;
    ASSERT_TRUE(client.isConnected());

    conn->failControlSends();
    client.disconnect();

    // Exactly one: 0 would mean the salvo never ran at all (a different defect, and why this
    // is EQ rather than LE), 2 would mean the second copy was still attempted.
    EXPECT_EQ(conn->countSent(ControlType::Disconnect), 1) << conn->diagnostics();
}

// The SECOND null-guard site. reconnectInternal() consults the factory again on every
// attempt, so a factory that starts returning null mid-run reaches this site and never
// connect()'s -- which is the whole reason mirroring the server's single guard is not enough
// here.
//
// This arm exists because the mutation sweep MEASURED that the guard had no detector:
// `if (false)` on it left the entire suite green (361/361). Without the guard the reconnect
// worker dereferences null, so the mutation crashes this arm rather than failing an
// expectation -- a crash IS the detector, because not crashing is precisely the guard's job.
TEST(Client, ReconnectAttemptSurvivesATransportFactoryThatStartsReturningNothing) {
    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", 4533};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setAutoReconnect(true);
    // TWO, and the 2 is load-bearing rather than arbitrary. The connection below dies well
    // inside kMinStableConnectionMs (5000 ms), so handleConnectionLost takes its SHORT-LIVED
    // branch and counts the attempt itself; at max=1 that branch reports "Connection
    // unstable" and returns without ever calling startReconnection, so the reconnect worker
    // -- and the guard under test -- is never reached. Measured: at max=1 this arm times out
    // with no reconnect at all.
    client.setMaxReconnectAttempts(2);
    client.setReconnectDelayMs(50);  // the attempts fail instantly; this is the only wait

    RecordingListener listener;
    client.addStreamListener(&listener);

    std::string err;
    auto conn = connectOverScriptedTransport(client, &err);
    ASSERT_TRUE(conn) << "setup failed before the subject under test: " << err;
    ASSERT_TRUE(client.isConnected());

    // From here the factory yields nothing, so the reconnect attempt below must fail cleanly
    // rather than dereference it.
    client.setTransportFactory([]() { return std::shared_ptr<ClientTransport>{}; });
    conn->close();  // peer goes away -> receiveLoop sees a dead read -> reconnect

    ASSERT_TRUE(waitFor([&]() { return listener.sawError("Failed to reconnect"); }, 5000))
        << "the reconnect worker never finished its attempt; " << conn->diagnostics();
    EXPECT_TRUE(listener.reconnecting.load());
    EXPECT_FALSE(client.isConnected());

    // Explicit teardown rather than relying on the destructor.
    //
    // THIS ARM'S DURATION USED TO BE BIMODAL — ~3 s or ~0.16 s — and #76 is why. It is now
    // uniformly fast (0.16 s locally, and the fix removed the only mechanism that made it
    // otherwise), so a slow run here is a REGRESSION SIGNAL rather than the variance it used
    // to be. It is still not the detector: assert nothing on this duration. The arm that does
    // discriminate is ReconnectExhaustionWakesAWorkerParkedInAnInterruptibleSleep below, which
    // forces the race instead of running it.
    //
    // The subject is the two reconnect attempts (50 ms + 100 ms backoff). What followed was
    // waitForWorkers(), and reconnectLoop's exhaustion tail set closed_ WITHOUT notifying
    // shutdownCv_ — so if heartbeatLoop was already parked in interruptibleSleepMs it slept out
    // its full kHeartbeatCheckIntervalMs (3000 ms, AudioStreamClient.cpp:30) before noticing.
    // If that thread had not reached its sleep yet when closed_ flipped, it returned immediately
    // and the arm was fast. Which way it went was a RACE, not a platform property — which is
    // exactly why this arm's wall clock was never a sound detector for the defect.
    //
    // MEASURED on one CI run of one pre-fix commit: macos-latest 3.07 s, ubuntu-latest 3.00 s,
    // the two lib-only jobs 0.16 s and 0.20 s; locally (macOS) four consecutive runs were all
    // ~3.00 s. Both modes PASSED — the assertion has a 5000 ms budget against a 158 ms subject,
    // a 31x margin either way, which is what let the stall sit here unnoticed.
    client.disconnect();
}

// #76's DETECTOR. `closed_` is the predicate every interruptibleSleepMs waits on, and
// reconnectLoop's exhaustion tail used to flip it WITHOUT notifying shutdownCv_ — so a worker
// already parked there was never woken. It slept out the remainder of its own interval, and
// waitForWorkers() (reached from disconnect(), from ~AudioStreamClient, and from disconnect()'s
// already-closing branch) blocked behind it for up to kHeartbeatCheckIntervalMs.
//
// WHY THIS ARM EXISTS WHEN THE ARM ABOVE ALREADY MEASURES THE STALL. #76 proposed reading that
// arm's wall clock instead — "no new detector is needed" — and its own later comment refutes
// that: the duration is BIMODAL (one CI run: macos 3.07 s, ubuntu 3.00 s, both lib-only jobs
// ~0.2 s) because the stall requires heartbeatLoop to be ALREADY PARKED when the tail runs, and
// the tail arrives ~158 ms after the loss. A fast run witnesses nothing. So that arm detects the
// regression roughly half the time, which is not a detector.
//
// THIS ARM REMOVES THE RACE RATHER THAN RUNNING IT. timeoutChecks() can only rise after
// heartbeatLoop's interruptibleSleepMs has RETURNED, so waiting for it proves the worker
// completed a full cycle and re-entered a fresh 3000 ms park — measured, not assumed. The two
// outcomes are then separated by a documented constant (kHeartbeatCheckIntervalMs,
// AudioStreamClient.cpp:30) instead of by two thread edges that have to coincide (L172).
TEST(Client, ReconnectExhaustionWakesAWorkerParkedInAnInterruptibleSleep) {
    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", 4533};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setAutoReconnect(true);
    // 2 for the same load-bearing reason as the arm above: at max=1 handleConnectionLost's
    // short-lived branch reports "Connection unstable" and returns without ever calling
    // startReconnection, so reconnectLoop — and the tail under test — is never reached.
    client.setMaxReconnectAttempts(2);
    client.setReconnectDelayMs(50);

    RecordingListener listener;
    client.addStreamListener(&listener);

    std::string err;
    auto conn = connectOverScriptedTransport(client, &err);
    ASSERT_TRUE(conn) << "setup failed before the subject under test: " << err;
    ASSERT_TRUE(client.isConnected());

    // THE PREMISE, MEASURED. heartbeatLoop's only calls to isConnectionTimedOut() sit AFTER its
    // interruptibleSleepMs returns, so timeoutChecks()>=2 proves the worker woke from its first
    // sleep, ran a whole cycle, and is on its way back into the next one. Without this the arm
    // would race the worker to its very first park and pass vacuously every time it won — which
    // is exactly the defect that makes the arm above a half-time detector.
    ASSERT_TRUE(waitFor([&]() { return conn->timeoutChecks() >= 2; }, 8000))
        << "heartbeatLoop never completed a sleep cycle, so no worker was parked to be woken "
           "and this arm would prove nothing; "
        << conn->diagnostics();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let it re-enter the sleep

    // From here the factory yields nothing, so every reconnect attempt fails and the loop runs
    // out — reaching the exhaustion tail rather than recovering.
    client.setTransportFactory([]() { return std::shared_ptr<ClientTransport>{}; });
    conn->close();

    // This error is emitted INSIDE the tail's `if (!closed_.exchange(true))`, so observing it
    // proves the tail is what claimed the terminal transition — the precise event whose missing
    // notify is the bug.
    ASSERT_TRUE(waitFor([&]() { return listener.sawError("Failed to reconnect"); }, 5000))
        << "the exhaustion tail never ran, so nothing flipped closed_ here; "
        << conn->diagnostics();

    // THE DISCRIMINATOR. closed_ is already true, so disconnect() takes its already-closing
    // branch and IS waitForWorkers() — the join barrier the parked heartbeat worker holds.
    // Unfixed, that worker sleeps out the ~2.7 s left of its interval; fixed, the tail's
    // notify_all already woke it before this line is reached. MEASURED under a pinned-artifact
    // mutation harness: 2781-2788 ms with the notify removed (whether or not the empty lock
    // scope is left behind), against 3226 ms for the WHOLE arm once fixed.
    //
    // WHAT THIS ARM DOES NOT COVER, measured rather than assumed. Moving the notify to BEFORE
    // the exchange — the ineffective shape closeResources() already has, where the predicate is
    // still false when the waiter re-evaluates it — leaves this arm GREEN. The woken thread has
    // to reacquire shutdownMutex_ before it can re-read closed_, and the exchange is two
    // instructions away, so it loses that race essentially always. This arm discriminates on
    // the PRESENCE of a notify on this path, not on its ordering, and no arm should try for the
    // latter: detecting it would mean asserting on two thread edges coinciding (L172).
    const auto t0 = std::chrono::steady_clock::now();
    client.disconnect();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();

    EXPECT_LT(elapsedMs, 1000)
        << "teardown waited out the heartbeat interval instead of being woken by the exhaustion "
           "tail: "
        << elapsedMs << " ms (#76). " << conn->diagnostics();
}

// THE SECOND AND THIRD SITES of the same omission, and the reason claimClosedAndWake() exists as
// a function rather than as three copies of two statements.
//
// #76 named ONE site — reconnectLoop's exhaustion tail. Probing the other two claims of the
// closed_ false->true transition MEASURED the identical stall on both: 2937 ms with
// auto-reconnect off and 2942 ms on the "Connection unstable" cutoff, against the tail's 2787 ms.
// So the issue's scope was a third of the defect, and the auto-reconnect-off path is the one an
// embedder that disables reconnection pays on every peer loss.
//
// What made all three the same bug is that closeResources() runs first on every one of these
// paths and DOES notify — but while closed_ is still false, so each waiter re-evaluates the
// predicate, sees false, and waits out its original deadline. An inert notify sitting a few
// statements upstream is why three separate sites could each look already-handled.
//
// Both arms below share the structure of the one above, including the unavoidable ~3 s premise:
// heartbeatLoop's park is only observable AFTER its first interruptibleSleepMs returns, so
// timeoutChecks() cannot rise sooner and there is no cheaper proof that anything is parked. Do
// not trim that wait — without it each arm races the worker to its first sleep and passes
// vacuously whenever it wins, which is precisely the flaw that made #76's proposed detector a
// half-time one.
//
// THE THREE ARMS ARE NOT REDUNDANT, measured rather than argued. Reverting each site to a raw
// closed_.exchange(true) in turn produces a clean diagonal — only that site's arm goes RED
// (2791 / 2937 / 2937 ms) while the other two stay green — and removing the notify from
// claimClosedAndWake() itself reddens all three. So each arm covers exactly one claim site, and
// the helper is the single point they share.
TEST(Client, ConnectionLossWithAutoReconnectOffWakesAWorkerParkedInAnInterruptibleSleep) {
    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", 4533};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);

    RecordingListener listener;
    client.addStreamListener(&listener);

    std::string err;
    auto conn = connectOverScriptedTransport(client, &err);
    ASSERT_TRUE(conn) << "setup failed before the subject under test: " << err;
    ASSERT_TRUE(client.isConnected());

    ASSERT_TRUE(waitFor([&]() { return conn->timeoutChecks() >= 2; }, 8000))
        << "heartbeatLoop never completed a sleep cycle, so no worker was parked to be woken "
           "and this arm would prove nothing; "
        << conn->diagnostics();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let it re-enter the sleep

    // Auto-reconnect OFF sends handleConnectionLost down its else branch, which is the claim
    // under test. Set it AFTER connecting so the premise above is established first.
    client.setAutoReconnect(false);
    conn->close();

    ASSERT_TRUE(waitFor([&]() { return listener.disconnectedCount.load() > 0; }, 5000))
        << "the loss path never claimed the terminal transition here; " << conn->diagnostics();

    const auto t0 = std::chrono::steady_clock::now();
    client.disconnect();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();

    EXPECT_LT(elapsedMs, 1000)
        << "auto-reconnect-off teardown waited out the heartbeat interval instead of being woken: "
        << elapsedMs << " ms (#76). " << conn->diagnostics();
}

TEST(Client, UnstableConnectionCutoffWakesAWorkerParkedInAnInterruptibleSleep) {
    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", 4533};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setAutoReconnect(true);
    // 1, which is what routes this to the branch under test rather than to reconnectLoop: the
    // connection dies inside kMinStableConnectionMs (5000 ms) so handleConnectionLost takes its
    // short-lived branch, counts the attempt, and at max=1 reports "Connection unstable" and
    // returns WITHOUT calling startReconnection. That early return is the third claim site.
    // (Note the premise wait below must therefore stay well under 5000 ms to keep the
    // connection short-lived; at ~3 s it does, with the cutoff at ~3.05 s.)
    client.setMaxReconnectAttempts(1);

    RecordingListener listener;
    client.addStreamListener(&listener);

    std::string err;
    auto conn = connectOverScriptedTransport(client, &err);
    ASSERT_TRUE(conn) << "setup failed before the subject under test: " << err;
    ASSERT_TRUE(client.isConnected());

    ASSERT_TRUE(waitFor([&]() { return conn->timeoutChecks() >= 2; }, 8000))
        << "heartbeatLoop never completed a sleep cycle, so no worker was parked to be woken "
           "and this arm would prove nothing; "
        << conn->diagnostics();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let it re-enter the sleep

    conn->close();

    // Emitted inside the branch's own claim, so observing it proves THIS site is the one that
    // flipped closed_ — not the tail, which is never reached at max=1.
    ASSERT_TRUE(waitFor([&]() { return listener.sawError("Connection unstable"); }, 5000))
        << "the unstable-connection cutoff never ran, so nothing flipped closed_ here; "
        << conn->diagnostics();

    const auto t0 = std::chrono::steady_clock::now();
    client.disconnect();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();

    EXPECT_LT(elapsedMs, 1000)
        << "unstable-connection teardown waited out the heartbeat interval instead of being "
           "woken: "
        << elapsedMs << " ms (#76). " << conn->diagnostics();
}

// ===========================================================================
// The heartbeat loop's watchdog under a wedged send (#71).
// ===========================================================================
//
// heartbeatLoop is the CLIENT'S ONLY WATCHDOG: nothing else notices a peer that has stopped
// sending. Its problem is that it shares a lock with the data path. Every TCP send funnels
// through AudioProtocolHandler::sendPacket, which holds one sendMutex_ across the whole
// Socket::sendAll, so a wedged peer parks the TX writer AND everything queued behind it — the
// watchdog included. A check placed after a send is therefore not evaluated for exactly as
// long as the peer stays wedged, which is the condition it exists to detect.
//
// MEASURED on the unfixed loop through this same seam: with the peer already timed out and the
// send path wedged, isConnectionTimedOut() was not reached ONCE in 12 s and no "Connection
// timeout" was ever reported.
//
// Both arms below assert on the MECHANISM (was a send attempted? was a probe sent?) rather
// than on elapsed time, so neither encodes this machine's speed as a threshold (L84).

// THE SUBJECT: a wedged TX writer must not disable the watchdog.
//
// The wedge is staged on sendTxAudio specifically, so the parked thread is sendLoop and the
// heartbeat loop is FREE — which is the production shape: the writer wedges on an audio frame
// and every other sender queues behind it on sendMutex_. Stalling all send kinds at once would
// leave it to scheduling whether the parked thread was the writer or the watchdog itself, and
// those stage opposite situations.
//
// Before the fix the loop woke, called sendHeartbeat(), blocked on the held sendMutex_, and
// never reached the check. After it, the check runs first and trips with no send attempted.
TEST(Client, HeartbeatWatchdogFiresWhileTheTxWriterHoldsTheSendLock) {
    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", 4533};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setTxInjectEnabled(true);  // must precede connect(): it is what starts sendLoop
    client.setAutoReconnect(false);   // defaults TRUE; a reconnect worker would perturb the ledger

    RecordingListener listener;
    client.addStreamListener(&listener);

    std::string err;
    auto conn = connectOverScriptedTransport(client, &err);
    ASSERT_TRUE(conn) << "setup failed before the subject under test: " << err;
    client.setPTT(true);  // captureMuted_ defaults TRUE (RX mode), and gates injectTxAudio

    // Wedge the TX writer, holding the send lock.
    conn->stallTxAudioSends();
    std::vector<std::uint8_t> pcm(1920, 0);
    for (int i = 0; i < 100 && conn->sendsParked() == 0; ++i) {
        client.injectTxAudio(pcm.data(), pcm.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // THE L136 GUARD, and it is the whole reason this arm is not vacuous: everything below is
    // a claim about what happens WHILE a send is wedged. Without a wedge the watchdog would
    // fire trivially and the arm would pass having tested nothing.
    ASSERT_GE(conn->sendsParked(), 1) << "the TX writer never wedged, so the arm's premise "
                                         "never held; " << conn->diagnostics();
    ASSERT_GE(conn->txAudioCalls(), 1) << conn->diagnostics();

    conn->setShouldSendHeartbeat(true);
    conn->setTimedOut(true);

    // Budget covers one full check interval (kHeartbeatCheckIntervalMs = 3000) plus slack.
    // Nothing releases the stall, so a pass cannot come from the wedge having ended.
    EXPECT_TRUE(waitFor([&]() { return listener.sawError("Connection timeout"); }, 8000))
        << "the watchdog never fired while the send lock was held; " << conn->diagnostics();

    // THE DISCRIMINATOR, and the reason it is a count rather than "is a send still parked".
    // Tripping the watchdog runs handleConnectionLost, which closes the connection — and
    // close() releases the stall latch by design (a latch that outlived close() would turn a
    // failed assertion into a ctest hang). So the fix's own success ENDS the wedge, and a
    // post-hoc "still parked" check is unsatisfiable: measured 0 vs 1 when written that way.
    //
    // This count is the sound form of the same guard. It cannot pass vacuously in either
    // direction: had the wedge ended on its own before the loop woke, the heartbeat send would
    // have gone through and this would be >= 1. Zero means the check ran BEFORE any send was
    // attempted, which is exactly the claim.
    EXPECT_EQ(conn->heartbeatsSent(), 0) << "the watchdog decision is still downstream of a "
                                            "send; " << conn->diagnostics();
    EXPECT_TRUE(conn->isClosed()) << "the watchdog reported a timeout without tearing the "
                                     "connection down; " << conn->diagnostics();

    // COVERAGE NOTE from the mutation sweep, recorded rather than banked. Dropping
    // sendMutex_ from the double's sendHeartbeat left the suite GREEN, which reads like this
    // arm depends on the modelled contention. It does not, and a COMBINED mutation is what
    // settles it: with the first watchdog call ALSO deleted, this arm still goes red — not
    // because the timeout goes unreported (without the lock the heartbeat completes and the
    // old check trips normally) but because heartbeatsSent() becomes 1. So the count above is
    // an independent, strictly stronger detector than "the error arrived", and the
    // serialisation's job here is fidelity to production rather than discrimination.
    // Predicted otherwise; the measurement corrected it.

    conn->releaseSends();
    client.disconnect();
}

// THE SECOND CHECK, which the arm above cannot reach: once the peer is declared dead, the
// latency probe must not spend another send budget on it. measureLatency() is a pure
// diagnostic — its answer is worthless on a connection that is already gone.
//
// Staged by letting the watchdog pass its FIRST check while the peer is still healthy, then
// declaring the peer dead while the heartbeat send is parked. With only the first check
// hoisted, the loop resumes and calls measureLatency() before it re-evaluates anything, so a
// probe goes out and the timeout is not reported until the next interval.
TEST(Client, HeartbeatWatchdogSkipsTheLatencyProbeOnceThePeerIsDeclaredDead) {
    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", 4533};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setAutoReconnect(false);

    RecordingListener listener;
    client.addStreamListener(&listener);

    std::string err;
    auto conn = connectOverScriptedTransport(client, &err);
    ASSERT_TRUE(conn) << "setup failed before the subject under test: " << err;

    // Park the watchdog inside its OWN heartbeat send, with the peer still healthy so the
    // iteration's first check passes.
    conn->stallHeartbeatSends();
    conn->setShouldSendHeartbeat(true);
    ASSERT_TRUE(conn->waitForParkedSend(8000))
        << "the heartbeat send never parked, so the arm's premise never held; "
        << conn->diagnostics();
    ASSERT_GE(conn->heartbeatsSent(), 1) << conn->diagnostics();

    // The peer dies during the send. Capture the probe count BEFORE releasing: the assertion
    // is that this number does not move again.
    const int probesBeforeRelease = conn->countSent(ControlType::LatencyProbe);
    conn->setTimedOut(true);
    conn->releaseSends();

    ASSERT_TRUE(waitFor([&]() { return listener.sawError("Connection timeout"); }, 8000))
        << "the watchdog never fired after the wedged heartbeat send returned; "
        << conn->diagnostics();
    EXPECT_EQ(conn->countSent(ControlType::LatencyProbe), probesBeforeRelease)
        << "a latency probe was spent on a peer already declared dead; " << conn->diagnostics();

    client.disconnect();
}

// ===========================================================================
// disconnect() under a held send lock (#77).
// ===========================================================================
//
// #71 fixed the salvo's SECOND copy. This is its first, and one hop earlier than #71 thought:
// the salvo blocks on ACQUIRING sendMutex_, before it can attempt a send at all.
// AudioProtocolHandler::sendPacket holds that one lock across the whole Socket::sendAll, so a
// wedged TX writer queues the courtesy DISCONNECT behind it — and closeResources(), which is
// what would break the wedge, sits AFTER the salvo in disconnect().
//
// #71's own cost breakdown blamed waitForWorkers() for this time and was wrong. MEASURED on
// macOS/arm64: a bare ::close(fd) breaks a thread already parked in ::send() in ~5 ms (505 /
// 508 / 505 ms against a close called at 500 ms, errno 9), so every path that REACHES
// waitForWorkers() is already prompt and adding shutdown() would buy nothing. The cost is
// upstream of it. Measured through this same seam on the unfixed path: disconnect() had not
// returned at 4 s and the ledger read { CONNECT_REQUEST x1 } — no DISCONNECT ever attempted.
//
// In production the wait is bounded rather than unbounded (one send budget for the lock plus
// one for the salvo's own send, CONNECTION_TIMEOUT_MS / 2 each). Bounded is not prompt.

// THE SUBJECT: disconnect() must return while another sender holds the send lock.
//
// The salvo is NOT skipped on TCP, and this arm must not be read as licensing that. #71
// established why: ClientSession::receiveLoop guards its error with if (!closed_.load()), so
// the DISCONNECT's real job is to run the server's close() before the FIN arrives — skip it
// and every clean TCP disconnect reports a receive error across the C ABI. The fix declines
// only when the lock is already held, on the reasoning that a held lock means the socket is
// backed up and the courtesy frame could not arrive promptly anyway.
//
// disconnect() runs on a WORKER THREAD, and that is not stylistic. A regression here BLOCKS,
// and a blocked disconnect() called from the test thread is a ctest timeout — a hang with no
// message — rather than a failure that names its own cause.
TEST(Client, DisconnectDeclinesTheSalvoWhileTheTxWriterHoldsTheSendLock) {
    PacedBackend backend;
    AudioStreamClient client{"127.0.0.1", 4533};
    client.setBackend(&backend);
    client.setPlaybackDevice(0);
    client.setTxInjectEnabled(true);  // must precede connect(): it is what starts sendLoop
    client.setAutoReconnect(false);   // defaults TRUE; a reconnect worker would perturb the ledger

    std::string err;
    auto conn = connectOverScriptedTransport(client, &err);
    ASSERT_TRUE(conn) << "setup failed before the subject under test: " << err;
    ASSERT_TRUE(client.isConnected());
    client.setPTT(true);  // captureMuted_ defaults TRUE (RX mode), and gates injectTxAudio

    // Wedge the TX writer so it parks HOLDING the send lock, exactly as a real writer parks
    // inside Socket::sendAll under sendPacket's mutex.
    conn->stallTxAudioSends();
    std::vector<std::uint8_t> pcm(1920, 0);
    for (int i = 0; i < 100 && conn->sendsParked() == 0; ++i) {
        client.injectTxAudio(pcm.data(), pcm.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // THE L136 GUARD. Everything below is a claim about what disconnect() does WHILE a send
    // holds the lock; with no wedge it would return promptly having tested nothing.
    ASSERT_GE(conn->sendsParked(), 1) << "the TX writer never wedged, so the arm's premise "
                                         "never held; " << conn->diagnostics();
    ASSERT_GE(conn->txAudioCalls(), 1) << conn->diagnostics();

    std::atomic<bool> returned{false};
    std::thread worker([&]() {
        client.disconnect();
        returned.store(true);
    });

    // 2 s against a subject that should complete in microseconds. Nothing releases the stall
    // before this budget elapses, so a pass cannot come from the wedge having ended early —
    // on the fixed path the wedge ends because disconnect() ITSELF reaches closeResources().
    const bool prompt = waitFor([&]() { return returned.load(); }, 2000);

    // Release BEFORE asserting. A red arm must still join and exit; asserting first would
    // strand the worker in the wedge and turn a failure into a ctest hang.
    conn->releaseSends();
    worker.join();

    EXPECT_TRUE(prompt) << "disconnect() blocked acquiring the send lock; " << conn->diagnostics();

    // THE MECHANISM, not the clock (L84). The two counts below are a PAIR, and neither alone
    // discriminates: zero DISCONNECTs is also what a salvo that never ran at all would produce
    // (connected_ false, a factory that handed back nothing), while a decline with the frame
    // still sent would mean something else declined.
    EXPECT_EQ(conn->countSent(ControlType::Disconnect), 0)
        << "the salvo queued behind the wedged writer instead of declining; "
        << conn->diagnostics();

    // EXACTLY one, which makes this a #71 regression guard as well: the second copy is gated
    // on the first succeeding, so a 2 here would mean that gate came undone.
    EXPECT_EQ(conn->controlSendsDeclined(), 1)
        << "the salvo never reached the non-blocking send; " << conn->diagnostics();
}
