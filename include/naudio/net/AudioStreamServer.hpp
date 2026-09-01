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
#include <functional>
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
#include "naudio/net/Discovery.hpp"
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

// A snapshot of the server's aggregate counters, summed across the LIVE ROSTER.
//
// The mirror of ClientStats, and it differs from it in one way that matters more than the
// field list: ClientStats is a cumulative total for one connection, whereas every number here
// is a sum over the connections registered RIGHT NOW. A client that disconnects takes its
// counters out of the sum, so these can DECREASE between two reads and a churning roster loses
// the departed clients' history entirely (ServerTransport, where the aggregation lives).
// Measured, not inferred: with two clients being fanned 60 frames each, packetsSent read 127;
// after one of them disconnected it read 64. Treat this as a gauge, never as a monotonic meter.
// (tests/net/test_server.cpp, GateServerStatsIsARosterGaugeNotALifetimeTotal.)
//
// The two counters that carry no information on a client (ControlReliability's retransmits and
// the ordered queue's drops — see ClientStats) are the ones this struct exists for: both are
// reachable here, because the server sends critical control messages that can go unacked and
// because a demux thread fills each connection's ordered queue while the session's application
// thread drains it.
struct ServerStats {
    // True only when a bound transport supplied these numbers. False leaves every field at its
    // default below — a default, not a reading. False before start() and after stop().
    bool running = false;

    // Roster size at the moment of the read — the divisor for any per-client average, and the
    // context every other field needs (a 0 with clientsConnected == 0 says nothing happened
    // because nobody is here, not that nothing happened).
    int clientsConnected = 0;

    std::int64_t packetsSent = 0;
    std::int64_t packetsReceived = 0;
    std::int64_t bytesSent = 0;
    std::int64_t bytesReceived = 0;
    int crcErrors = 0;

    // Control-ARQ resends. Non-zero only on a UDP/DUAL profile with control reliability
    // enabled; a structural 0 on TCP, which has no ARQ layer.
    std::int64_t controlRetransmits = 0;
    // Packets discarded from a connection's ordered queue at capacity — audio lost locally,
    // after the network delivered it. Reachable here and not on a client.
    std::int64_t queueDrops = 0;
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
// config/accept, the run loop's heartbeat, tx_denied, latency_response, and a rejected
// client's reject message) are sent directly on the connection, whose impls serialize writes.
//
// That queue is BOUNDED by bytes of pending RX audio (issue #56). A TCP peer that stops
// reading its socket closes its receive window, the writer thread blocks in send() with no
// deadline, and the queue would otherwise grow without bound at the capture rate. At the cap
// the session is EVICTED rather than trimmed: receiveRxAudio returns false, which is the
// removal request AudioBroadcaster::BroadcastTarget already documents, and the broadcaster's
// failure listener closes the session. See outboundBacklogEvictions() for the observable and
// ClientSession::outQueueMaxBytes_ for how the cap is derived.
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

    // Transport wiring. Supplies the ServerTransport that start() will use, replacing the
    // one config.transportType would have selected. The transport layer is already an
    // interface with three implementations (TCP / UDP / DUAL); this completes it by letting
    // the choice come from the caller rather than only from the enum.
    //
    // A FACTORY rather than an instance, because the server's transport is per-run: start()
    // binds it and stop() closes it and drops the reference, so a second start() needs a
    // second transport. The factory is invoked once per start().
    //
    // Contract: set it before start() — start() reads it once and ignores later changes for
    // the run in progress. start() calls bind() on what it returns, and stop() calls close()
    // and releases the server's reference. Returning null fails start() with an error rather
    // than crashing.
    //
    // It is also the seam that makes ClientSession's receive path testable: a supplied
    // transport can hand the session a frame the FEC layer reconstructed, which no real peer
    // can spell because provenance is not a wire field (issue #67).
    void setTransportFactory(std::function<std::shared_ptr<ServerTransport>()> factory) {
        transportFactory_ = std::move(factory);
    }

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

    // Borrowed, and must outlive the SERVER — not merely the last stop() — UNLESS it is handed
    // back with removeStreamListener() below. Callbacks are delivered on the dispatcher thread,
    // and stop() only POSTS onServerStopped(); the drain and join happen in ~AudioStreamServer. So
    // a listener destroyed between a returned stop() and the server's destruction is still
    // reachable, and reading it there is a use-after-free.
    //
    // Stated because it was NOT: the `backend` above carries the same borrowed-must-outlive rule
    // and says so, this one said nothing, and two of this project's own arms in
    // tests/net/test_server.cpp consequently declared their listener AFTER the server — destroying
    // it first. ASan found both (issue #28). Declare listeners before the server they attach to.
    void addStreamListener(AudioStreamListener* listener);

    // Detaches a listener and WAITS for any callback already in flight to finish. After this
    // returns, no callback for `listener` is running or will ever fire again, so destroying it is
    // safe — including before the server. Idempotent; a listener never added is a no-op.
    //
    // The wait is the point, and it is why this is not a one-line erase. Each notify* snapshots
    // the roster and captures the listener POINTERS by value, so a task queued before the erase
    // still holds this one; erasing alone would return while that task was pending and hand the
    // caller a detach that does not make destruction safe. See CallbackDispatcher::fence().
    //
    // Calling this from INSIDE a callback detaches, but cannot wait (a thread cannot wait for
    // itself) — that is sound, because the only task that could hold the pointer is the one on
    // your own stack, and you are not destroying yourself from within your own method.
    //
    // ISSUE #89 — WHY stop() DOES NOT DRAIN, AND WHY THIS EXISTS INSTEAD. The natural consumer
    // sequence `server.stop();` followed by destroying the listener is unsafe above, and the
    // obvious repair is to have stop() drain the dispatcher before returning. That was measured
    // and rejected: start() after stop() is a supported, publicly documented state transition
    // (naudio.h, na_server_stats.running — "0 means ... after na_server_stop"), and
    // CallbackDispatcher is deliberately NOT restartable — stop() latches stop_ and joins, after
    // which post() and start() both silently no-op forever. A drain inside stop() therefore leaves
    // a restarted server RUNNING CORRECTLY AND PERMANENTLY MUTE, with start() still returning
    // success: it trades a loud, sanitizer-detectable use-after-free for a silent event loss that
    // no existing arm detects. The drain would also be conditional in a way nothing states — it is
    // skipped whenever stop() is called from within a callback, which naudio.h explicitly permits
    // ("You MAY call any server method from inside one, INCLUDING na_server_stop"). The strong
    // "no callback can be in flight once it returns" guarantee is kept where it costs nothing and
    // is already documented: na_server_destroy / ~AudioStreamServer.
    void removeStreamListener(AudioStreamListener* listener);

    const AudioStreamConfig& config() const { return config_; }
    // The bound port (-1 if not bound).
    int port() const;

    // A snapshot of the aggregate counters. Safe to call from any thread at any time,
    // including before start() and after stop() (which yield running == false and defaults).
    // Read the ServerStats contract above before comparing two snapshots: it is a gauge over
    // the live roster, not a monotonic lifetime total.
    ServerStats stats() const;

    // Sessions evicted because their outbound RX-audio backlog reached the per-session cap —
    // a peer that stopped draining its socket (issue #56). Non-zero means a client was
    // dropped for not READING, which an operator would otherwise see only as an unexplained
    // disconnect; the paired notifyError names the cause.
    //
    // A LIFETIME TOTAL, and deliberately NOT a ServerStats field. Every ServerStats counter
    // is a gauge over the live roster (read its contract above), and an evicted session
    // leaves that roster — so a ServerStats counter for this event would reset itself at the
    // instant it fired.
    std::int64_t outboundBacklogEvictions() const { return outboundBacklogEvictions_.load(); }

private:
    class ClientSession;  // defined in the .cpp
    friend class ClientSession;

    // Builds the facts published in a DISCOVER_REPLY, read LIVE at each probe.
    // Shared by both responders: the transport answers probes aimed at the service
    // port, the rendezvous listener answers those aimed at discoveryPort.
    DiscoveryFactsProvider makeDiscoveryProvider(ServerTransport* transport);

    // The §6.8 rendezvous listener. Owned HERE and not by the transport, because it
    // is independent OF transport: a TCP-only server has no datagram path to an
    // unknown sender and cannot answer through its own transport, and TCP is the
    // default. Started best-effort in start(), stopped in stop().
    DiscoveryResponder discoveryResponder_;

    std::shared_ptr<ServerTransport> createTransport();
    bool initializeSharedAudio(std::string* err);
    bool openSharedAudioLines(std::string* err);
    void stopSharedAudio();
    // Close every live session, wait out the activeThreads_ barrier, and drop the map. Factored
    // out of stop() because it must run TWICE (issue #57): the accept thread can be inside
    // handleNewClient when the first pass runs, and only a pass made after acceptThread_ is
    // joined is final.
    void teardownSessions();
    void acceptLoop();
    void handleNewClient(const std::shared_ptr<ClientConnection>& connection);
    // Turn a client away with a reason, then drop its connection.
    //
    // DELIVERY OF THE REASON IS BEST-EFFORT, and it is worth stating because the obvious reading of
    // the code is that it is guaranteed (issue #87). The reject is decided at accept, before a byte
    // has been read, so a client that behaved normally — connect, send CONNECT_REQUEST, then read —
    // has left unread data in our receive buffer; closing on top of unread data makes the stack
    // emit an RST, and an RST makes the peer discard its own receive buffer, the reject with it.
    // discardPendingInput() below removes that data first so the close is a FIN, which is what
    // makes delivery work in practice. What it cannot promise is the general case: a peer that
    // keeps sending past the drain budget, or one whose data arrives after the close, can still
    // reset. A client must therefore treat a bare disconnect during connect as "rejected, reason
    // unknown" rather than assuming it will always be told why.
    //
    // Runs on the ACCEPT THREAD — everything here is bounded for that reason.
    void rejectClient(const std::shared_ptr<ClientConnection>& connection, RejectReason reason,
                      const std::string& message);
    // Drop a connection the server accepted but never turned into a session — all three reject
    // paths, and handleNewClient's entry check for a server that is already stopping (issue #64.1).
    // NOT handleNewClient's second stopping check: that one runs holding sessionsMutex_, and this
    // takes runMutex_, which would invert a lock order the class otherwise never nests.
    //
    // Closing the connection is NOT enough. The transport's connection map is what every
    // ServerStats aggregate sums over, and only disconnectClient() removes an entry from it, so a
    // merely-closed connection keeps contributing its counters for the life of the transport —
    // which is what makes the documented roster GAUGE (see ServerStats above) drift upward on a
    // server that is doing nothing but refusing clients. ClientSession::close() already ends this
    // way for sessions that were admitted; this is the same ending for the ones that were not.
    void evictConnection(const std::shared_ptr<ClientConnection>& connection);
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
    // Read once by start() (via createTransport) and never while running, so it needs no
    // synchronization of its own — same discipline as the device-wiring setters above.
    std::function<std::shared_ptr<ServerTransport>()> transportFactory_;
    std::unique_ptr<CaptureStream> captureStream_;
    std::unique_ptr<PlaybackStream> playbackStream_;

    mutable std::mutex sessionsMutex_;
    std::map<std::string, std::shared_ptr<ClientSession>> sessions_;
    std::atomic<int> clientIdCounter_{1};
    // Incremented by ClientSession (a friend) at the outbound-backlog cap. Monotonic for the
    // life of the server object — start()/stop() do not reset it.
    std::atomic<std::int64_t> outboundBacklogEvictions_{0};

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
