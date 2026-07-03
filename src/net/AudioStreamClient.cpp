// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — AudioStreamClient implementation.
//
// Copyright (C) 2025-2026 Terrell Deppe
//

#include "naudio/net/AudioStreamClient.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

#include "naudio/AudioPacket.hpp"
#include "naudio/net/TcpClientTransport.hpp"
#include "naudio/net/UdpClientConnection.hpp"  // UdpReliabilityConfig
#include "naudio/net/UdpClientTransport.hpp"

namespace naudio::net {

namespace {

constexpr int kConnectTimeoutMs = 10000;
// A reconnect attempt uses a tighter connect deadline than the initial connect so
// a disconnect() landing during a reconnect is not stalled for the full window —
// the reconnect loop retries anyway.
constexpr int kReconnectConnectTimeoutMs = 5000;
constexpr std::int64_t kMinStableConnectionMs = 5000;
constexpr std::int64_t kHeartbeatCheckIntervalMs = 3000;

std::int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t nowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

AudioStreamClient::AudioStreamClient(std::string serverHost, std::uint16_t serverPort,
                                     std::string clientName)
    : serverHost_(std::move(serverHost)),
      serverPort_(serverPort),
      clientName_(std::move(clientName)) {
    dispatcher_.start();  // event callbacks fire on this thread, never on a worker
}

AudioStreamClient::~AudioStreamClient() {
    // This destructor is implicitly noexcept: any exception escaping it (a std::system_error from
    // the dispatcher-thread join, a bad_alloc from a final event post, a CV wait failure) would
    // std::terminate the whole process (C ABI lifetime hardening). Guard each
    // teardown step independently so one failure can't skip the others, and so nothing crosses the
    // boundary into a C caller's na_client_destroy.
    try { doClose(); } catch (...) {}
    try { waitForWorkers(); } catch (...) {}
    // Drain + join the event dispatcher LAST, after the workers have stopped posting, so
    // every queued listener callback runs before this object's state is destroyed.
    try { dispatcher_.stop(); } catch (...) {}
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

AudioStreamConfig AudioStreamClient::config() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return config_;
}

bool AudioStreamClient::setConfig(AudioStreamConfig config) {
    if (connected_.load()) return false;  // InvalidState: cannot change config while connected
    std::lock_guard<std::mutex> lock(configMutex_);
    config_ = config;
    return true;
}

AudioFormat AudioStreamClient::formatFromConfig() const {
    AudioFormat f;
    std::lock_guard<std::mutex> lock(configMutex_);
    f.sampleRate = config_.sampleRate;
    f.bitsPerSample = config_.bitsPerSample;
    f.channels = config_.channels;
    f.encoding = Encoding::PcmSigned;
    f.endianness = Endianness::Little;
    return f;
}

std::shared_ptr<ClientTransport> AudioStreamClient::createTransport() {
    AudioStreamConfig cfg = config();
    switch (cfg.transportType) {
        case TransportType::Udp: {
            UdpReliabilityConfig rc;
            rc.reorderWindowSize = cfg.reorderBufferSize;
            rc.reorderMaxHoldMs = cfg.reorderMaxHoldMs;
            rc.fecEnabled = cfg.fecEnabled;
            rc.fecBlockSize = cfg.fecBlockSize;
            rc.adaptiveJitterEnabled = cfg.adaptiveJitterEnabled;
            rc.jitterMinMs = cfg.bufferMinMs;
            rc.jitterMaxMs = cfg.bufferMaxMs;
            rc.jitterMultiplier = cfg.jitterMultiplier;
            rc.controlReliabilityEnabled = cfg.controlReliabilityEnabled;
            rc.controlRetransmitMaxAttempts = cfg.controlRetransmitMaxAttempts;
            return std::make_shared<UdpClientTransport>(rc);
        }
        case TransportType::Tcp:
        case TransportType::Dual:
        default:
            // DUAL falls through to TCP for the client — the client
            // picks ONE transport; the server is the side that serves both on one port.
            return std::make_shared<TcpClientTransport>();
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool AudioStreamClient::connect(std::string* err) {
    auto setErr = [err](const std::string& m) {
        if (err) *err = m;
    };
    if (connected_.load()) {
        setErr("Already connected");
        return false;
    }
    if (!playbackDeviceId_.has_value()) {
        setErr("Playback device not configured");
        return false;
    }

    auto transport = createTransport();
    std::string cerr;
    auto connection = transport->connect(serverHost_, serverPort_, kConnectTimeoutMs, &cerr);
    if (!connection) {
        setErr(cerr.empty() ? "Connect failed" : cerr);
        return false;
    }
    connectTimeMs_.store(nowMillis());
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        transport_ = transport;
        connection_ = connection;
    }

    if (!runConnect(connection, err)) {
        doClose();
        waitForWorkers();
        return false;
    }
    return true;
}

bool AudioStreamClient::runConnect(const std::shared_ptr<ClientConnection>& connection,
                                   std::string* err) {
    if (!performHandshake(connection)) {
        if (err) *err = "Handshake failed";
        return false;
    }
    if (!openAudioLines(err)) return false;

    // Re-check closed_ under runMutex_ before going live: a disconnect()/doClose()
    // may have raced in during the handshake or line opening. Committing connected_
    // after a close would resurrect a torn-down client. closeResources() also
    // takes runMutex_, so the check + store cannot interleave with a teardown.
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        if (closed_.load()) {
            if (err) *err = "Closed during connect";
            return false;
        }
        connected_.store(true);
    }
    notifyClientConnected("local", serverHost_ + ":" + std::to_string(serverPort_));

    startWorkerThreads();

    streaming_.store(true);
    notifyStreamStarted("local");
    return true;
}

bool AudioStreamClient::performHandshake(const std::shared_ptr<ClientConnection>& connection) {
    // Build client info if any identification is provided.
    ClientInfo info{callsign_, operatorName_, location_};
    const ClientInfo* infoPtr = info.isEmpty() ? nullptr : &info;

    AudioStreamConfig cfg = config();
    if (!connection->sendControl(ControlMessage::connectRequestFull(clientName_, AudioPacket::VERSION,
                                                                    &cfg, infoPtr))) {
        notifyError("local", "Handshake send failed");
        return false;
    }

    ReceiveResult r = connection->receivePacket(kConnectTimeoutMs);
    if (!r.hasPacket()) {
        notifyError("local", "Handshake timeout");
        return false;
    }

    // Process responses until accept/reject (apply any server-pushed audio config along the way).
    while (r.hasPacket()) {
        const AudioPacket& p = *r.packet;
        if (p.packetType() == PacketType::Control) {
            if (auto msg = ControlMessage::deserialize(p.payload())) {
                switch (msg->messageType()) {
                    case ControlType::AudioConfig: {
                        // Apply ONLY the fields the message carries — replacing the whole config
                        // would wipe UDP/FEC/reorder/jitter settings.
                        std::lock_guard<std::mutex> lock(configMutex_);
                        msg->applyAudioConfigTo(config_);
                        break;
                    }
                    case ControlType::ConnectAccept:
                        return true;
                    case ControlType::ConnectReject: {
                        auto reason = msg->parseErrorMessage();
                        notifyError("local",
                                    "Connection rejected: " + reason.value_or("unknown"));
                        return false;
                    }
                    default:
                        break;
                }
            }
        }
        r = connection->receivePacket(kConnectTimeoutMs);
    }
    return false;
}

bool AudioStreamClient::openAudioLines(std::string* err) {
    auto setErr = [err, this](const std::string& m) {
        if (err) *err = m;
        notifyError("local", m);
    };
    if (backend_ == nullptr) {
        setErr("Audio line unavailable: no backend");
        return false;
    }
    const AudioFormat fmt = formatFromConfig();

    // Capture line is optional (only needed for TX).
    std::shared_ptr<CaptureStream> capture;
    bool captureIsMono = false;
    if (captureDeviceId_.has_value()) {
        try {
            capture = backend_->openCaptureStream(*captureDeviceId_, fmt);
        } catch (const DeviceUnavailable& e) {
            setErr(std::string("Audio line unavailable: ") + e.what());
            return false;
        }
        captureIsMono = capture->actualFormat().channels == 1;
    }

    // Playback line is required (already validated in connect()).
    std::shared_ptr<PlaybackStream> playback;
    try {
        playback = backend_->openPlaybackStream(*playbackDeviceId_, fmt);
    } catch (const DeviceUnavailable& e) {
        setErr(std::string("Audio line unavailable: ") + e.what());
        return false;
    }

    AudioStreamConfig cfg = config();
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        captureStream_ = std::move(capture);
        playbackStream_ = std::move(playback);
        rxBuffer_ = std::make_shared<AudioRingBuffer>(cfg);
        txBuffer_ = std::make_shared<AudioRingBuffer>(cfg);
    }
    captureIsMono_ = captureIsMono;
    return true;
}

void AudioStreamClient::startWorkerThreads() {
    const std::int64_t generation = connectionGeneration_.load();
    const AudioStreamConfig cfg = config();
    const bool mono = captureIsMono_;

    std::shared_ptr<ClientConnection> conn;
    std::shared_ptr<AudioRingBuffer> rx;
    std::shared_ptr<AudioRingBuffer> tx;
    std::shared_ptr<CaptureStream> cap;
    std::shared_ptr<PlaybackStream> pb;
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        conn = connection_;
        rx = rxBuffer_;
        tx = txBuffer_;
        cap = captureStream_;
        pb = playbackStream_;
    }

    // Receive (RX -> ring buffer + audio listeners; control; heartbeat ack).
    threadStarted();
    std::thread([this, conn, rx, generation, cfg]() {
        receiveLoop(conn, rx, generation, cfg);
        threadFinished();
    }).detach();

    // Playback (RX ring buffer -> playback stream).
    threadStarted();
    std::thread([this, rx, pb, generation, cfg]() {
        playbackLoop(rx, pb, generation, cfg);
        threadFinished();
    }).detach();

    // Capture + send (only if a capture device is configured — TX is optional).
    if (cap) {
        threadStarted();
        std::thread([this, tx, cap, mono, cfg]() {
            captureLoop(tx, cap, mono, cfg);
            threadFinished();
        }).detach();

        threadStarted();
        std::thread([this, conn, tx, generation, cfg]() {
            sendLoop(conn, tx, generation, cfg);
            threadFinished();
        }).detach();
    }

    // Heartbeat.
    threadStarted();
    std::thread([this, conn, generation]() {
        heartbeatLoop(conn, generation);
        threadFinished();
    }).detach();
}

// ---------------------------------------------------------------------------
// Worker loops
// ---------------------------------------------------------------------------

void AudioStreamClient::receiveLoop(std::shared_ptr<ClientConnection> connection,
                                    std::shared_ptr<AudioRingBuffer> rxBuffer,
                                    std::int64_t generation, AudioStreamConfig cfg) {
    std::int64_t rxPackets = 0;
    while (!closed_.load() && connected_.load()) {
        ReceiveResult r = connection->receivePacket(100);
        if (r.closed) {
            if (!closed_.load()) {
                notifyError("local", "Receive error: connection closed");
                handleConnectionLost(generation);
            }
            break;
        }
        if (!r.hasPacket()) continue;

        const AudioPacket& p = *r.packet;
        switch (p.packetType()) {
            case PacketType::AudioRx: {
                const std::vector<std::uint8_t>& payload = p.payload();
                rxBuffer->write(payload.data(), 0, payload.size());
                ++rxPackets;

                // Notify raw audio listeners (FFT / waterfall).
                for (auto& l : audioListenersSnapshot()) {
                    l(payload.data(), payload.size());
                }

                // Adaptive jitter buffer: refresh the target every 100 packets.
                if (cfg.adaptiveJitterEnabled && rxPackets % 100 == 0) {
                    int adaptiveTarget = connection->adaptiveBufferTargetMs();
                    if (adaptiveTarget > 0 && adaptiveTarget != cfg.bufferTargetMs) {
                        AudioStreamConfig adapted = cfg;
                        adapted.bufferTargetMs = adaptiveTarget;
                        rxBuffer->updateConfig(adapted);
                    }
                }
                break;
            }
            case PacketType::Control:
                handleControlMessage(p, generation);
                break;
            case PacketType::Heartbeat:
                if (!connection->sendControl(ControlMessage::heartbeatAck())) {
                    if (!closed_.load()) {
                        notifyError("local", "Receive error: heartbeat ack failed");
                        handleConnectionLost(generation);
                    }
                    return;
                }
                break;
            default:
                break;
        }
    }
}

void AudioStreamClient::playbackLoop(std::shared_ptr<AudioRingBuffer> rxBuffer,
                                     std::shared_ptr<PlaybackStream> playback,
                                     std::int64_t /*generation*/, AudioStreamConfig cfg) {
    const int frameBytes = cfg.bytesPerFrame();
    const int frameSize = std::max(1, playback->actualFormat().frameSize());
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(frameBytes), 0);
    std::vector<std::uint8_t> silence(static_cast<std::size_t>(frameBytes), 0);

    // Initial buffering, bounded by MAX_INITIAL_BUFFERING_MS (critical for FT8 timing — start
    // with whatever we have if the target is not reached).
    const auto bufferingStart = std::chrono::steady_clock::now();
    const auto maxBuffering = std::chrono::milliseconds(AudioStreamConfig::MAX_INITIAL_BUFFERING_MS);
    while (!closed_.load() && connected_.load() && !rxBuffer->hasReachedTargetLevel()) {
        if (std::chrono::steady_clock::now() - bufferingStart >= maxBuffering) break;
        interruptibleSleepMs(10);
    }

    const std::int64_t readTimeout = static_cast<std::int64_t>(cfg.frameDurationMs) * 2;
    while (!closed_.load() && connected_.load()) {
        int n = rxBuffer->read(buffer.data(), 0, static_cast<std::size_t>(frameBytes), readTimeout);
        if (n > 0) {
            const std::uint8_t* src = playbackMuted_.load() ? silence.data() : buffer.data();
            playback->write(src, n / frameSize, kBlockForever);
        } else if (rxBuffer->available() == 0) {
            // Underrun — insert a frame of silence.
            playback->write(silence.data(), frameBytes / frameSize, kBlockForever);
        }
    }
}

void AudioStreamClient::captureLoop(std::shared_ptr<AudioRingBuffer> txBuffer,
                                    std::shared_ptr<CaptureStream> capture, bool captureIsMono,
                                    AudioStreamConfig cfg) {
    const AudioFormat capFmt = capture->actualFormat();
    const int capFrameSize = std::max(1, capFmt.frameSize());
    const int framesPerRead = std::max(1, cfg.samplesPerFrame());
    std::vector<std::uint8_t> readBuffer(static_cast<std::size_t>(framesPerRead) * capFrameSize, 0);
    // For mono capture we expand each 16-bit sample into both stereo channels.
    std::vector<std::uint8_t> stereoBuffer(
        captureIsMono ? static_cast<std::size_t>(framesPerRead) * 4 : 0, 0);

    while (!closed_.load() && connected_.load()) {
        IoResult res = capture->read(readBuffer.data(), framesPerRead, kBlockForever);
        if (res.frames <= 0 || captureMuted_.load()) continue;

        const std::size_t bytesRead = static_cast<std::size_t>(res.frames) * capFrameSize;
        if (captureIsMono) {
            std::size_t stereoBytes = 0;
            for (std::size_t i = 0; i + 1 < bytesRead && stereoBytes + 3 < stereoBuffer.size();
                 i += 2) {
                stereoBuffer[stereoBytes++] = readBuffer[i];
                stereoBuffer[stereoBytes++] = readBuffer[i + 1];
                stereoBuffer[stereoBytes++] = readBuffer[i];
                stereoBuffer[stereoBytes++] = readBuffer[i + 1];
            }
            txBuffer->write(stereoBuffer.data(), 0, stereoBytes);
        } else {
            txBuffer->write(readBuffer.data(), 0, bytesRead);
        }
    }
}

void AudioStreamClient::sendLoop(std::shared_ptr<ClientConnection> connection,
                                 std::shared_ptr<AudioRingBuffer> txBuffer,
                                 std::int64_t generation, AudioStreamConfig cfg) {
    const int frameBytes = cfg.bytesPerFrame();
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(frameBytes), 0);
    const std::int64_t readTimeout = static_cast<std::int64_t>(cfg.frameDurationMs) * 2;

    while (!closed_.load() && connected_.load()) {
        int n = txBuffer->read(buffer.data(), 0, static_cast<std::size_t>(frameBytes), readTimeout);
        if (n > 0) {
            if (!connection->sendTxAudio(buffer.data(), static_cast<std::size_t>(n))) {
                if (!closed_.load()) {
                    notifyError("local", "Send error");
                    handleConnectionLost(generation);
                }
                break;
            }
        }
    }
}

void AudioStreamClient::heartbeatLoop(std::shared_ptr<ClientConnection> connection,
                                      std::int64_t generation) {
    while (!closed_.load() && connected_.load()) {
        interruptibleSleepMs(kHeartbeatCheckIntervalMs);
        if (closed_.load() || !connected_.load()) break;

        if (connection->shouldSendHeartbeat()) {
            if (!connection->sendHeartbeat()) {
                if (!closed_.load()) notifyError("local", "Heartbeat error");
            }
        }

        if (connection->isConnectionTimedOut()) {
            notifyError("local", "Connection timeout");
            handleConnectionLost(generation);
            break;
        }

        measureLatency();
    }
}

void AudioStreamClient::handleControlMessage(const AudioPacket& packet, std::int64_t generation) {
    auto msg = ControlMessage::deserialize(packet.payload());
    if (!msg) return;

    switch (msg->messageType()) {
        case ControlType::LatencyResponse: {
            std::int64_t sent = msg->parseLatencyTimestamp();
            measuredLatencyMs_.store((nowNanos() - sent) / 1'000'000 / 2);
            break;
        }
        case ControlType::ClientsUpdate: {
            if (auto info = msg->parseClientsUpdate()) {
                // Store BEFORE notifying so a listener that reads back the
                // client count / TX owner during its callback sees the new value.
                {
                    std::lock_guard<std::mutex> lock(serverInfoMutex_);
                    serverClientsInfo_ = *info;
                }
                notifyClientsUpdate(*info);
            }
            break;
        }
        case ControlType::TxGranted:
            notifyTxGranted();
            break;
        case ControlType::TxDenied:
            notifyTxDenied(msg->parseTxClientId().value_or(""));
            break;
        case ControlType::TxPreempted:
            notifyTxPreempted(msg->parseTxClientId().value_or(""));
            break;
        case ControlType::TxReleased:
            notifyTxReleased();
            break;
        case ControlType::Disconnect:
        case ControlType::Error: {
            if (auto error = msg->parseErrorMessage()) {
                notifyError("local", *error);
            }
            handleConnectionLost(generation);
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Reconnection
// ---------------------------------------------------------------------------

void AudioStreamClient::handleConnectionLost(std::int64_t generation) {
    std::lock_guard<std::mutex> guard(reconnectGuard_);

    if (closed_.load()) return;
    if (generation != connectionGeneration_.load()) {
        // Late error from a previous connection's worker — without this check it would tear
        // down the freshly reconnected session. The GENERATION check (not
        // reconnecting_) is the staleness gate: a CURRENT-generation failure must be
        // handled even while a reconnect is still in its success-handoff window
        // (reconnecting_ may not be cleared yet), so a brand-new connection that dies
        // immediately clears connected_/resources here instead of spinning.
        return;
    }

    const std::int64_t connectionDuration = nowMillis() - connectTimeMs_.load();
    const bool wasShortLived = connectionDuration < kMinStableConnectionMs;

    closeResources();

    if (autoReconnect_.load() && !closed_.load()) {
        if (wasShortLived) {
            // Short-lived connection — count the attempt to bound an endless immediate-fail loop.
            int attempts = reconnectAttempt_.fetch_add(1) + 1;
            if (attempts >= maxReconnectAttempts_.load()) {
                // exchange-claim, not store: only the winner of the closed_ false→true
                // transition emits events — a doClose() racing this branch may already
                // have delivered the terminal disconnected event.
                if (!closed_.exchange(true)) {
                    notifyError("local",
                                "Connection unstable - failed " + std::to_string(attempts) +
                                    " times within " + std::to_string(kMinStableConnectionMs) +
                                    "ms of connecting");
                    notifyClientDisconnected("local");
                }
                return;
            }
        } else {
            reconnectAttempt_.store(0);
        }
        startReconnection();
    } else {
        // exchange-claim: the closed-guard at the top of this function only filters
        // calls that BEGIN after close completes; a doClose() can flip closed_
        // between that check and here (it takes no reconnectGuard_), and it emits
        // the terminal disconnected event itself. The atomic claim of the
        // false→true transition makes the event exactly-once.
        if (!closed_.exchange(true)) notifyClientDisconnected("local");
    }
}

void AudioStreamClient::startReconnection() {
    if (closed_.load() || reconnecting_.load()) return;
    reconnecting_.store(true);
    threadStarted();
    std::thread([this]() {
        reconnectLoop();
        threadFinished();
    }).detach();
}

void AudioStreamClient::reconnectLoop() {
    int currentDelay = reconnectDelayMs_.load();
    bool firstIteration = true;

    while (!closed_.load() && reconnecting_.load() &&
           reconnectAttempt_.load() < maxReconnectAttempts_.load()) {
        int attempt;
        if (firstIteration) {
            firstIteration = false;
            attempt = reconnectAttempt_.load();
            if (attempt == 0) attempt = reconnectAttempt_.fetch_add(1) + 1;
        } else {
            attempt = reconnectAttempt_.fetch_add(1) + 1;
        }

        notifyReconnecting("local", attempt, maxReconnectAttempts_.load());

        interruptibleSleepMs(currentDelay);
        if (closed_.load()) break;

        std::string err;
        if (reconnectInternal(&err)) {
            // The brand-new connection may have died immediately — a current-generation
            // worker failing in the success-handoff window runs closeResources()
            // (connected_=false) rather than spinning. Declare success only if it is
            // actually still up; otherwise keep retrying inside this same loop. (Leaving
            // reconnectAttempt_ as-is; handleConnectionLost resets it once the connection
            // stays stable past MIN_STABLE_CONNECTION_MS.)
            if (!closed_.load() && connected_.load()) {
                reconnecting_.store(false);
                notifyReconnected("local");
                return;
            }
        }
        notifyError("local", "Reconnect attempt " + std::to_string(reconnectAttempt_.load()) + "/" +
                                 std::to_string(maxReconnectAttempts_.load()) +
                                 " failed: " + err);
        currentDelay = std::min(currentDelay * 2, maxReconnectDelayMs_.load());
    }

    reconnecting_.store(false);
    // exchange-claim, not load-then-store: a doClose() (e.g. the destructor during
    // reconnect exhaustion) racing this tail must not yield a second terminal
    // disconnected event.
    if (!closed_.exchange(true)) {
        notifyError("local", "Failed to reconnect after " +
                                 std::to_string(reconnectAttempt_.load()) + " attempts");
        notifyClientDisconnected("local");
    }
}

bool AudioStreamClient::reconnectInternal(std::string* err) {
    if (closed_.load()) {
        if (err) *err = "Closed";
        return false;
    }
    auto transport = createTransport();
    std::string cerr;
    // Bounded connect deadline so disconnect() during a reconnect isn't stalled.
    auto connection =
        transport->connect(serverHost_, serverPort_, kReconnectConnectTimeoutMs, &cerr);
    if (!connection) {
        if (err) *err = cerr.empty() ? "Connect failed" : cerr;
        return false;
    }
    connectTimeMs_.store(nowMillis());
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        // If disconnect()/doClose() ran while we were connecting, do NOT commit this
        // connection — close it and bail. Without this re-check the freshly-opened
        // connection would be installed and runConnect would resurrect the client.
        if (closed_.load()) {
            connection->close();
            if (err) *err = "Closed during reconnect";
            return false;
        }
        transport_ = transport;
        connection_ = connection;
    }
    return runConnect(connection, err);
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

void AudioStreamClient::disconnect() {
    // Claim the terminal transition BEFORE the courtesy DISCONNECT control: the
    // server closes its end the moment it processes it, and on a slow scheduler the
    // receive worker can observe that FIN and race handleConnectionLost against the
    // local close — with closed_ already true it never enters the lost path (and no
    // reconnect can spawn). The courtesy send still goes out: connection_ is only
    // dropped by closeResources() below.
    if (closed_.exchange(true)) {  // already closing — just wait for the workers
        waitForWorkers();
        return;
    }
    if (connected_.load()) {
        auto conn = currentConnection();
        if (conn) {
            // Courtesy salvo. On UDP the DISCONNECT is a single unprotected datagram
            // (no retransmitter can run once closed_ is set — the heartbeat loop that
            // pumps control retransmits is down) and closeResources() below closes the
            // socket microseconds after sendto — which on Winsock loopback can discard
            // the in-flight datagram, leaving the server to its 8s idle timeout. Send
            // twice with a short yield so a copy escapes the socket. Duplicates are
            // safe: the server's close() is idempotent on both transports, and a
            // post-removal duplicate is dropped by the demux gate.
            conn->sendControl(ControlMessage::disconnect());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            conn->sendControl(ControlMessage::disconnect());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // The remainder of doClose() (whose exchange we already won on its behalf).
    reconnecting_.store(false);
    {
        std::lock_guard<std::mutex> lock(shutdownMutex_);
    }
    shutdownCv_.notify_all();
    closeResources();
    notifyClientDisconnected("local");
    waitForWorkers();
}

void AudioStreamClient::doClose() {
    if (closed_.exchange(true)) return;  // already closing
    reconnecting_.store(false);
    {
        std::lock_guard<std::mutex> lock(shutdownMutex_);
    }
    shutdownCv_.notify_all();
    closeResources();
    notifyClientDisconnected("local");
}

void AudioStreamClient::closeResources() {
    // Invalidate the current generation FIRST: any in-flight error from these workers is now
    // stale by definition.
    connectionGeneration_.fetch_add(1);

    streaming_.store(false);
    connected_.store(false);
    notifyStreamStopped("local");

    // Wake any interruptible sleeps (heartbeat / reconnect backoff / initial buffering).
    {
        std::lock_guard<std::mutex> lock(shutdownMutex_);
    }
    shutdownCv_.notify_all();

    // Drop the client's references to the per-generation resources. The detached workers hold
    // their own shared_ptr copies and keep the objects alive until they exit on the cleared
    // connected_ flag (so this never joins a worker — no self-join, no deadlock). Closing the
    // connection here unblocks the receive worker's receivePacket promptly.
    std::shared_ptr<ClientConnection> conn;
    std::shared_ptr<ClientTransport> transport;
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        conn = connection_;
        transport = transport_;
        connection_.reset();
        transport_.reset();
        rxBuffer_.reset();
        txBuffer_.reset();
        captureStream_.reset();
        playbackStream_.reset();
    }
    if (conn) conn->close();
    if (transport) transport->close();
}

void AudioStreamClient::waitForWorkers() {
    std::unique_lock<std::mutex> lock(threadsMutex_);
    threadsCv_.wait(lock, [this]() { return activeThreads_ == 0; });
}

void AudioStreamClient::threadStarted() {
    std::lock_guard<std::mutex> lock(threadsMutex_);
    ++activeThreads_;
}

void AudioStreamClient::threadFinished() {
    std::lock_guard<std::mutex> lock(threadsMutex_);
    if (--activeThreads_ == 0) threadsCv_.notify_all();
}

void AudioStreamClient::interruptibleSleepMs(std::int64_t ms) {
    if (ms <= 0) return;
    std::unique_lock<std::mutex> lock(shutdownMutex_);
    shutdownCv_.wait_for(lock, std::chrono::milliseconds(ms), [this]() { return closed_.load(); });
}

std::shared_ptr<ClientConnection> AudioStreamClient::currentConnection() const {
    std::lock_guard<std::mutex> lock(runMutex_);
    return connection_;
}

// ---------------------------------------------------------------------------
// Latency
// ---------------------------------------------------------------------------

void AudioStreamClient::measureLatency() {
    if (!connected_.load()) return;
    auto conn = currentConnection();
    if (conn) conn->sendControl(ControlMessage::latencyProbe(nowNanos()));
}

// ---------------------------------------------------------------------------
// Server client info
// ---------------------------------------------------------------------------

int AudioStreamClient::serverClientCount() const {
    std::lock_guard<std::mutex> lock(serverInfoMutex_);
    return serverClientsInfo_.has_value() ? serverClientsInfo_->clientCount : -1;
}

int AudioStreamClient::serverMaxClients() const {
    std::lock_guard<std::mutex> lock(serverInfoMutex_);
    return serverClientsInfo_.has_value() ? serverClientsInfo_->maxClients : -1;
}

std::string AudioStreamClient::serverTxOwner() const {
    std::lock_guard<std::mutex> lock(serverInfoMutex_);
    if (serverClientsInfo_.has_value() && serverClientsInfo_->txOwner.has_value()) {
        return *serverClientsInfo_->txOwner;
    }
    return "";
}

std::vector<std::string> AudioStreamClient::serverClientIds() const {
    std::lock_guard<std::mutex> lock(serverInfoMutex_);
    return serverClientsInfo_.has_value() ? serverClientsInfo_->clientIds
                                          : std::vector<std::string>{};
}

std::optional<ClientsUpdateInfo> AudioStreamClient::serverClientsInfo() const {
    std::lock_guard<std::mutex> lock(serverInfoMutex_);
    return serverClientsInfo_;
}

// ---------------------------------------------------------------------------
// Listeners
// ---------------------------------------------------------------------------

void AudioStreamClient::addStreamListener(AudioClientListener* listener) {
    if (listener == nullptr) return;
    std::lock_guard<std::mutex> lock(listenersMutex_);
    for (auto* l : listeners_) {
        if (l == listener) return;  // dedup
    }
    listeners_.push_back(listener);
}

void AudioStreamClient::removeStreamListener(AudioClientListener* listener) {
    std::lock_guard<std::mutex> lock(listenersMutex_);
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
}

int AudioStreamClient::addAudioListener(AudioListener listener) {
    std::lock_guard<std::mutex> lock(audioListenersMutex_);
    int token = audioListenerSeq_++;
    audioListeners_.emplace_back(token, std::move(listener));
    return token;
}

void AudioStreamClient::removeAudioListener(int token) {
    std::lock_guard<std::mutex> lock(audioListenersMutex_);
    audioListeners_.erase(
        std::remove_if(audioListeners_.begin(), audioListeners_.end(),
                       [token](const auto& p) { return p.first == token; }),
        audioListeners_.end());
}

std::vector<AudioClientListener*> AudioStreamClient::listenersSnapshot() const {
    std::lock_guard<std::mutex> lock(listenersMutex_);
    return listeners_;
}

std::vector<AudioStreamClient::AudioListener> AudioStreamClient::audioListenersSnapshot() const {
    std::lock_guard<std::mutex> lock(audioListenersMutex_);
    std::vector<AudioListener> out;
    out.reserve(audioListeners_.size());
    for (auto& [token, l] : audioListeners_) out.push_back(l);
    return out;
}

// Each notify* snapshots the listeners on the CALLING thread (cheap, under the listener
// mutex) then POSTS the fan-out to the dispatcher, so the user callbacks run on the
// dispatch thread — never on a worker and never under reconnectGuard_ (C5/C6). The
// snapshot + args are captured by value; the listener pointers must outlive the client.
void AudioStreamClient::notifyClientConnected(const std::string& id, const std::string& addr) {
    auto listeners = listenersSnapshot();
    dispatcher_.post([listeners, id, addr] {
        for (auto* l : listeners) l->onClientConnected(id, addr);
    });
}
void AudioStreamClient::notifyClientDisconnected(const std::string& id) {
    auto listeners = listenersSnapshot();
    dispatcher_.post([listeners, id] {
        for (auto* l : listeners) l->onClientDisconnected(id);
    });
}
void AudioStreamClient::notifyStreamStarted(const std::string& id) {
    auto listeners = listenersSnapshot();
    dispatcher_.post([listeners, id] {
        for (auto* l : listeners) l->onStreamStarted(id);
    });
}
void AudioStreamClient::notifyStreamStopped(const std::string& id) {
    auto listeners = listenersSnapshot();
    dispatcher_.post([listeners, id] {
        for (auto* l : listeners) l->onStreamStopped(id);
    });
}
void AudioStreamClient::notifyError(const std::string& id, const std::string& error) {
    auto listeners = listenersSnapshot();
    dispatcher_.post([listeners, id, error] {
        for (auto* l : listeners) l->onError(id, error);
    });
}
void AudioStreamClient::notifyReconnecting(const std::string& id, int attempt, int max) {
    auto listeners = listenersSnapshot();
    dispatcher_.post([listeners, id, attempt, max] {
        for (auto* l : listeners) l->onReconnecting(id, attempt, max);
    });
}
void AudioStreamClient::notifyReconnected(const std::string& id) {
    auto listeners = listenersSnapshot();
    dispatcher_.post([listeners, id] {
        for (auto* l : listeners) l->onReconnected(id);
    });
}
void AudioStreamClient::notifyClientsUpdate(const ClientsUpdateInfo& info) {
    auto listeners = listenersSnapshot();
    const int count = info.clientCount;
    const int maxClients = info.maxClients;
    const std::string txOwner = info.txOwner.value_or("");
    const std::vector<std::string> clientIds = info.clientIds;
    dispatcher_.post([listeners, count, maxClients, txOwner, clientIds] {
        for (auto* l : listeners) l->onClientsUpdate(count, maxClients, txOwner, clientIds);
    });
}
void AudioStreamClient::notifyTxGranted() {
    auto listeners = listenersSnapshot();
    dispatcher_.post([listeners] {
        for (auto* l : listeners) l->onTxGranted();
    });
}
void AudioStreamClient::notifyTxDenied(const std::string& holdingClientId) {
    auto listeners = listenersSnapshot();
    dispatcher_.post([listeners, holdingClientId] {
        for (auto* l : listeners) l->onTxDenied(holdingClientId);
    });
}
void AudioStreamClient::notifyTxPreempted(const std::string& preemptingClientId) {
    auto listeners = listenersSnapshot();
    dispatcher_.post([listeners, preemptingClientId] {
        for (auto* l : listeners) l->onTxPreempted(preemptingClientId);
    });
}
void AudioStreamClient::notifyTxReleased() {
    auto listeners = listenersSnapshot();
    dispatcher_.post([listeners] {
        for (auto* l : listeners) l->onTxReleased();
    });
}

}  // namespace naudio::net
