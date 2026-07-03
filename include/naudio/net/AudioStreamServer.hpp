// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — the multi-client bidirectional audio-streaming server.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "naudio/AudioStreamConfig.hpp"
#include "naudio/ControlMessage.hpp"  // RejectReason
#include "naudio/DeviceBackend.hpp"
#include "naudio/Stream.hpp"
#include "naudio/net/AudioBroadcaster.hpp"
#include "naudio/net/AudioMixer.hpp"
#include "naudio/net/CallbackDispatcher.hpp"
#include "naudio/net/Transport.hpp"

namespace naudio::net {

// Lifecycle observer. All methods default to no-ops so a test overrides only what it
// needs. NOTE: a periodic statistics-update path is intentionally omitted — it is a
// diagnostic value object with no role in the transport / fan-out / arbitration /
// roster gate. The behavioral lifecycle events (connect / disconnect / error / stream
// start-stop) are
// here; the periodic stats stream is deferred.
class AudioStreamListener {
public:
    virtual ~AudioStreamListener() = default;
    virtual void onServerStarted(int /*port*/) {}
    virtual void onServerStopped() {}
    virtual void onClientConnected(const std::string& /*clientId*/, const std::string& /*addr*/) {}
    virtual void onClientDisconnected(const std::string& /*clientId*/) {}
    virtual void onStreamStarted(const std::string& /*clientId*/) {}
    virtual void onStreamStopped(const std::string& /*clientId*/) {}
    virtual void onError(const std::string& /*clientId*/, const std::string& /*error*/) {}
};

// Server for bidirectional audio streaming with multi-client support.
//
// It captures radio RX audio and broadcasts it to all connected clients (via
// AudioBroadcaster), and
// receives TX audio from clients and plays it to the radio under priority-based arbitration
// (via AudioMixer). Transport is pluggable (TCP / UDP / DUAL) per config.
//
// Backend-agnostic: the server takes a DeviceBackend* and opens its capture/playback
// streams through it, so the SAME server runs over FakeBackend (hardware-free CI) and
// PortAudioBackend (real). It never touches PortAudio directly.
//
// The load-bearing design decision (the #1 correctness risk): the broadcaster/mixer
// callbacks run on the SHARED capture/playback thread. If a session sent to its socket from
// those callbacks, one slow client would stall audio for EVERY client. So each session owns a
// per-session outgoing queue drained by a dedicated WRITER THREAD; the sync callbacks only
// enqueue (never block on I/O). A direct send would block on a slow client, so the
// per-session queue is the non-blocking path. Async-context sends (handshake
// config/accept, tx_denied,
// latency_response) are sent directly on the connection, whose impls serialize writes.
//
// Lifecycle / teardown: broadcaster_/mixer_/transport_ are held by shared_ptr behind
// runMutex_. A session copies them out under the lock and calls into them OUTSIDE it
// (lock-drop-before-callback) — because
// unregistering re-enters via the mixer listener -> broadcastClientsUpdate. Sessions hold only
// a raw back-pointer to the server, which outlives every session (stop() joins all session
// threads via the activeThreads_ barrier before the server is destroyed).
class AudioStreamServer {
public:
    explicit AudioStreamServer(std::uint16_t port, AudioStreamConfig config = AudioStreamConfig{},
                               std::string bindHost = "");
    ~AudioStreamServer();

    AudioStreamServer(const AudioStreamServer&) = delete;
    AudioStreamServer& operator=(const AudioStreamServer&) = delete;

    // Device wiring (backend-agnostic). The backend is borrowed and must outlive the server.
    void setBackend(DeviceBackend* backend) { backend_ = backend; }
    void setCaptureDevice(int backendId) { captureBackendId_ = backendId; }
    void setPlaybackDevice(int backendId) { playbackBackendId_ = backendId; }
    // Inject-only mode: audio comes from injectAudio() instead of a capture device.
    void setInjectOnlyMode(bool injectOnly) { injectOnlyMode_.store(injectOnly); }
    bool isInjectOnlyMode() const { return injectOnlyMode_.load(); }

    // Starts the server (bind + audio init + accept thread). Returns false and fills err.
    bool start(std::string* err);
    // Stops the server: closes all sessions, joins their threads, tears down audio + transport.
    void stop();
    bool isRunning() const { return running_.load(); }

    bool hasClient() const;
    int clientCount() const;
    std::vector<std::string> connectedClientIds() const;
    // The current TX channel owner ("" if none).
    std::string txOwner() const;

    // Injects PCM to broadcast to all clients (recordings / engine audio / tests).
    void injectAudio(const std::vector<std::uint8_t>& data);

    void addStreamListener(AudioStreamListener* listener);

    const AudioStreamConfig& config() const { return config_; }
    // The bound port (-1 if not bound).
    int port() const;

private:
    class ClientSession;  // defined in the .cpp
    friend class ClientSession;

    std::shared_ptr<ServerTransport> createTransport();
    bool initializeSharedAudio(std::string* err);
    bool openSharedAudioLines(std::string* err);
    void stopSharedAudio();
    void acceptLoop();
    void handleNewClient(const std::shared_ptr<ClientConnection>& connection);
    void rejectClient(const std::shared_ptr<ClientConnection>& connection, RejectReason reason,
                      const std::string& message);
    void broadcastClientsUpdate();

    AudioFormat formatFromConfig() const;

    // Session-thread join barrier (the detached run/receive/writer threads).
    void threadStarted();
    void threadFinished();

    // Listener fan-out.
    void notifyServerStarted(int port);
    void notifyServerStopped();
    void notifyClientConnected(const std::string& id, const std::string& addr);
    void notifyClientDisconnected(const std::string& id);
    void notifyStreamStarted(const std::string& id);
    void notifyStreamStopped(const std::string& id);
    void notifyError(const std::string& id, const std::string& error);

    const std::uint16_t port_;
    AudioStreamConfig config_;
    std::string bindHost_;
    DeviceBackend* backend_ = nullptr;  // borrowed
    std::optional<int> captureBackendId_;
    std::optional<int> playbackBackendId_;
    std::atomic<bool> injectOnlyMode_{false};

    // runMutex_ guards the shared audio/transport resources. They are
    // shared_ptr so a session can copy one out under the lock and use it after unlock.
    mutable std::mutex runMutex_;
    std::shared_ptr<AudioBroadcaster> broadcaster_;
    std::shared_ptr<AudioMixer> mixer_;
    std::shared_ptr<ServerTransport> transport_;
    std::unique_ptr<CaptureStream> captureStream_;
    std::unique_ptr<PlaybackStream> playbackStream_;

    mutable std::mutex sessionsMutex_;
    std::map<std::string, std::shared_ptr<ClientSession>> sessions_;
    std::atomic<int> clientIdCounter_{1};

    std::atomic<bool> running_{false};
    std::thread acceptThread_;

    std::mutex threadsMutex_;
    std::condition_variable threadsCv_;
    int activeThreads_ = 0;

    mutable std::mutex listenersMutex_;
    std::vector<AudioStreamListener*> listeners_;

    // Dispatch thread for the AudioStreamListener callbacks. notify* snapshots the
    // listeners then POSTS the fan-out here, so no listener callback runs on a session/accept
    // worker or under a lock — a listener may re-enter the server (stop()) without deadlock.
    // Started in the ctor, drained + joined in the dtor after the workers stop posting.
    CallbackDispatcher dispatcher_;
};

}  // namespace naudio::net
