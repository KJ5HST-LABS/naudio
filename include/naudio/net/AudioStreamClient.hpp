// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — the high-level bidirectional audio-streaming client.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "naudio/AudioRingBuffer.hpp"
#include "naudio/AudioStreamConfig.hpp"
#include "naudio/ControlMessage.hpp"  // ClientInfo / ClientsUpdateInfo
#include "naudio/DeviceBackend.hpp"
#include "naudio/Stream.hpp"
#include "naudio/net/CallbackDispatcher.hpp"
#include "naudio/net/Transport.hpp"

namespace naudio::net {

// Client-side lifecycle / event observer. A richer interface than the server's
// AudioStreamListener (AudioStreamServer.hpp) because a client also receives TX-arbitration
// outcomes, roster updates, and reconnection events. All methods default to no-ops so a
// consumer overrides only what it needs.
//
// NOTE: a periodic statistics-update path is intentionally omitted — it is a
// diagnostic value object with no role in the e2e gate. The behavioral events
// (connect / disconnect / stream / error /
// reconnect / roster / TX) are all here.
class AudioClientListener {
public:
    virtual ~AudioClientListener() = default;
    virtual void onClientConnected(const std::string& /*clientId*/, const std::string& /*addr*/) {}
    virtual void onClientDisconnected(const std::string& /*clientId*/) {}
    virtual void onStreamStarted(const std::string& /*clientId*/) {}
    virtual void onStreamStopped(const std::string& /*clientId*/) {}
    virtual void onError(const std::string& /*clientId*/, const std::string& /*error*/) {}
    virtual void onReconnecting(const std::string& /*clientId*/, int /*attempt*/, int /*max*/) {}
    virtual void onReconnected(const std::string& /*clientId*/) {}
    virtual void onClientsUpdate(int /*count*/, int /*maxClients*/, const std::string& /*txOwner*/,
                                 const std::vector<std::string>& /*clientIds*/) {}
    virtual void onTxGranted() {}
    virtual void onTxDenied(const std::string& /*holdingClientId*/) {}
    virtual void onTxPreempted(const std::string& /*preemptingClientId*/) {}
    virtual void onTxReleased() {}
};

// Client for connecting to an AudioStreamServer.
//
// Receives RX audio from the server into a local (virtual) playback device, and captures TX
// audio from a local device to send to the server. Five worker threads (receive / playback /
// capture / send / heartbeat) plus a generation-counter reconnect with exponential backoff.
//
// Device seam: like AudioStreamServer, the client is backend-agnostic — it takes a
// DeviceBackend* and opens its capture/playback streams through it, so the SAME client runs
// over FakeBackend (hardware-free CI) and PortAudioBackend (real). A playback device is
// REQUIRED (RX); a capture device is OPTIONAL (TX).
//
// Threading model: no async runtime, all std::thread (thread-per-worker). Every
// spawned thread (the 5 workers and
// the reconnect thread) is DETACHED at spawn and tracked by an activeThreads_ join barrier; the
// final teardown (disconnect()/destructor) waits the barrier rather than joining, so a worker
// that triggers connection-loss never has to join itself. Per-generation resources (connection,
// ring buffers, streams) are held by shared_ptr and captured by the workers, so a fresh
// reconnect generation never shares mutable state with a draining old one —
// closeResources() just drops the client's references and the old workers keep their objects
// alive until they exit.
class AudioStreamClient {
public:
    AudioStreamClient(std::string serverHost, std::uint16_t serverPort,
                      std::string clientName = "naudio-client");
    ~AudioStreamClient();

    AudioStreamClient(const AudioStreamClient&) = delete;
    AudioStreamClient& operator=(const AudioStreamClient&) = delete;

    // --- Device wiring (backend-agnostic). The backend is borrowed and must outlive the client. ---
    void setBackend(DeviceBackend* backend) { backend_ = backend; }
    // The playback device is REQUIRED (RX). connect() fails without it.
    void setPlaybackDevice(int backendId) { playbackDeviceId_ = backendId; }
    // The capture device is OPTIONAL (only needed for TX).
    void setCaptureDevice(int backendId) { captureDeviceId_ = backendId; }

    // --- Identification (sent to the server so other clients see who shares the radio) ---
    void setCallsign(std::string callsign) { callsign_ = std::move(callsign); }
    const std::string& callsign() const { return callsign_; }
    void setOperatorName(std::string name) { operatorName_ = std::move(name); }
    const std::string& operatorName() const { return operatorName_; }
    void setLocation(std::string location) { location_ = std::move(location); }
    const std::string& location() const { return location_; }

    // --- Config (must be set before connect; false == InvalidState while connected) ---
    AudioStreamConfig config() const;
    bool setConfig(AudioStreamConfig config);

    // --- Lifecycle ---
    // Connects, handshakes, opens lines, starts streaming. Returns false + fills err on failure.
    bool connect(std::string* err);
    // Disconnects (best-effort DISCONNECT control) and stops any reconnection.
    void disconnect();
    bool isConnected() const { return connected_.load() && !closed_.load(); }
    bool isStreaming() const { return streaming_.load(); }

    // --- Server client info (from CLIENTS_UPDATE) ---
    int serverClientCount() const;
    int serverMaxClients() const;
    std::string serverTxOwner() const;          // "" if none
    std::vector<std::string> serverClientIds() const;
    std::optional<ClientsUpdateInfo> serverClientsInfo() const;

    // --- Auto-reconnect ---
    void setAutoReconnect(bool v) { autoReconnect_.store(v); }
    bool isAutoReconnect() const { return autoReconnect_.load(); }
    void setMaxReconnectAttempts(int n) { maxReconnectAttempts_.store(n); }
    void setReconnectDelayMs(int ms) { reconnectDelayMs_.store(ms); }
    void setMaxReconnectDelayMs(int ms) { maxReconnectDelayMs_.store(ms); }
    bool isReconnecting() const { return reconnecting_.load(); }
    int reconnectAttempt() const { return reconnectAttempt_.load(); }

    // --- Mute / PTT ---
    void setCaptureMuted(bool m) { captureMuted_.store(m); }
    bool isCaptureMuted() const { return captureMuted_.load(); }
    void setPlaybackMuted(bool m) { playbackMuted_.store(m); }
    bool isPlaybackMuted() const { return playbackMuted_.load(); }
    // PTT active -> capture unmuted (send voice) + playback muted (no feedback); inactive -> reverse.
    void setPTT(bool pttActive) {
        captureMuted_.store(!pttActive);
        playbackMuted_.store(pttActive);
    }

    // --- Latency ---
    void measureLatency();
    std::int64_t measuredLatencyMs() const { return measuredLatencyMs_.load(); }

    // --- Listeners ---
    void addStreamListener(AudioClientListener* listener);
    void removeStreamListener(AudioClientListener* listener);
    // Raw PCM RX listener (FFT / waterfall). Returns a token for removal (closures have no identity).
    using AudioListener = std::function<void(const std::uint8_t*, std::size_t)>;
    int addAudioListener(AudioListener listener);
    void removeAudioListener(int token);

private:
    std::shared_ptr<ClientTransport> createTransport();
    bool runConnect(const std::shared_ptr<ClientConnection>& connection, std::string* err);
    bool performHandshake(const std::shared_ptr<ClientConnection>& connection);
    bool openAudioLines(std::string* err);
    void startWorkerThreads();
    AudioFormat formatFromConfig() const;
    std::shared_ptr<ClientConnection> currentConnection() const;

    // Worker loops (each takes its per-generation resources by shared_ptr capture).
    void receiveLoop(std::shared_ptr<ClientConnection> connection,
                     std::shared_ptr<AudioRingBuffer> rxBuffer, std::int64_t generation,
                     AudioStreamConfig cfg);
    void playbackLoop(std::shared_ptr<AudioRingBuffer> rxBuffer,
                      std::shared_ptr<PlaybackStream> playback, std::int64_t generation,
                      AudioStreamConfig cfg);
    void captureLoop(std::shared_ptr<AudioRingBuffer> txBuffer,
                     std::shared_ptr<CaptureStream> capture, bool captureIsMono,
                     AudioStreamConfig cfg);
    void sendLoop(std::shared_ptr<ClientConnection> connection,
                  std::shared_ptr<AudioRingBuffer> txBuffer, std::int64_t generation,
                  AudioStreamConfig cfg);
    void heartbeatLoop(std::shared_ptr<ClientConnection> connection, std::int64_t generation);
    void handleControlMessage(const AudioPacket& packet, std::int64_t generation);

    // Reconnection.
    void handleConnectionLost(std::int64_t generation);
    void startReconnection();
    void reconnectLoop();
    bool reconnectInternal(std::string* err);

    // Teardown.
    void doClose();
    void closeResources();
    void waitForWorkers();

    // Join barrier for the detached worker / reconnect threads (mirroring
    // AudioStreamServer's activeThreads_). threadStarted() is called on the
    // spawning thread BEFORE detach; threadFinished() at the end of each thread body.
    void threadStarted();
    void threadFinished();

    // Interruptible sleep — returns early when closed_ is set (woken via shutdownCv_).
    void interruptibleSleepMs(std::int64_t ms);

    // Listener fan-out (snapshot-then-callback; never under a lock).
    std::vector<AudioClientListener*> listenersSnapshot() const;
    std::vector<AudioListener> audioListenersSnapshot() const;
    void notifyClientConnected(const std::string& id, const std::string& addr);
    void notifyClientDisconnected(const std::string& id);
    void notifyStreamStarted(const std::string& id);
    void notifyStreamStopped(const std::string& id);
    void notifyError(const std::string& id, const std::string& error);
    void notifyReconnecting(const std::string& id, int attempt, int max);
    void notifyReconnected(const std::string& id);
    void notifyClientsUpdate(const ClientsUpdateInfo& info);
    void notifyTxGranted();
    void notifyTxDenied(const std::string& holdingClientId);
    void notifyTxPreempted(const std::string& preemptingClientId);
    void notifyTxReleased();

    // --- Immutable identity ---
    const std::string serverHost_;
    const std::uint16_t serverPort_;
    const std::string clientName_;

    // --- Device wiring ---
    DeviceBackend* backend_ = nullptr;  // borrowed
    std::optional<int> playbackDeviceId_;
    std::optional<int> captureDeviceId_;
    bool captureIsMono_ = false;

    // --- Identification ---
    std::string callsign_;
    std::string operatorName_;
    std::string location_;

    // --- Config (guarded by configMutex_) ---
    mutable std::mutex configMutex_;
    AudioStreamConfig config_;

    // --- Per-generation run resources (guarded by runMutex_; shared_ptr so workers keep alive) ---
    mutable std::mutex runMutex_;
    std::shared_ptr<ClientTransport> transport_;
    std::shared_ptr<ClientConnection> connection_;
    std::shared_ptr<AudioRingBuffer> rxBuffer_;
    std::shared_ptr<AudioRingBuffer> txBuffer_;
    std::shared_ptr<CaptureStream> captureStream_;
    std::shared_ptr<PlaybackStream> playbackStream_;

    // --- State flags ---
    std::atomic<bool> connected_{false};
    std::atomic<bool> streaming_{false};
    // Monotonic false→true, never reset (the client is one-shot). Every emitter of
    // the terminal disconnected event claims the transition with closed_.exchange(true)
    // so the event is exactly-once; that discipline breaks if a future refactor makes
    // the client reconnectable-after-disconnect by clearing this flag.
    std::atomic<bool> closed_{false};
    std::atomic<bool> captureMuted_{true};    // start muted (RX mode)
    std::atomic<bool> playbackMuted_{false};  // start unmuted (hear RX)
    std::atomic<std::int64_t> measuredLatencyMs_{0};
    std::atomic<std::int64_t> connectTimeMs_{0};

    // --- Reconnection ---
    std::atomic<bool> autoReconnect_{true};
    std::atomic<int> maxReconnectAttempts_{10};
    std::atomic<int> reconnectDelayMs_{1000};
    std::atomic<int> maxReconnectDelayMs_{30000};
    std::atomic<bool> reconnecting_{false};
    // Bumped by closeResources(); a worker captures it at spawn so a stale worker's late
    // connection-lost error cannot tear down a freshly reconnected session.
    std::atomic<std::int64_t> connectionGeneration_{0};
    std::atomic<int> reconnectAttempt_{0};
    std::mutex reconnectGuard_;  // serializes handleConnectionLost

    // --- Server client info ---
    mutable std::mutex serverInfoMutex_;
    std::optional<ClientsUpdateInfo> serverClientsInfo_;

    // --- Listeners ---
    mutable std::mutex listenersMutex_;
    std::vector<AudioClientListener*> listeners_;
    mutable std::mutex audioListenersMutex_;
    std::vector<std::pair<int, AudioListener>> audioListeners_;
    int audioListenerSeq_ = 1;

    // Dispatch thread for the AudioClientListener event callbacks. Every notify*
    // below snapshots the listeners then POSTS the fan-out here, so no event callback ever
    // runs on a worker thread or under reconnectGuard_ — a listener may re-enter the client
    // (disconnect(), setters) without deadlocking. Started in the ctor, drained + joined in
    // the dtor (after the workers stop posting). NOTE: the raw RX audio listeners
    // (addAudioListener, FFT/waterfall) stay on the receive worker — data-plane, must be
    // fast and must NOT call client lifecycle methods (see the contract in naudio.h).
    CallbackDispatcher dispatcher_;

    // --- Thread join barrier ---
    std::mutex threadsMutex_;
    std::condition_variable threadsCv_;
    int activeThreads_ = 0;

    // --- Interruptible-sleep signaling ---
    std::mutex shutdownMutex_;
    std::condition_variable shutdownCv_;
};

}  // namespace naudio::net
