// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — AudioStreamServer implementation.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/net/AudioStreamServer.hpp"

#include <algorithm>  // std::remove, in removeStreamListener
#include <chrono>
#include <deque>
#include <utility>

#include "naudio/ControlMessage.hpp"
// CONNECTION_TIMEOUT_MS, which the outbound backlog cap is derived from. Reached
// transitively via TcpServerTransport below, but a transitive include is not a contract.
#include "naudio/net/AudioProtocolHandler.hpp"
#include "naudio/net/DualServerTransport.hpp"
#include "naudio/net/TcpServerTransport.hpp"
#include "naudio/net/UdpClientConnection.hpp"  // UdpReliabilityConfig
#include "naudio/net/UdpServerTransport.hpp"

namespace naudio::net {

namespace {
std::int64_t nowMs() {
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

// ---------------------------------------------------------------------------
// ClientSession — one per accepted client. Both an AudioBroadcaster::BroadcastTarget
// (RX -> this client) and an AudioMixer::TxClient (this client -> TX). Owns three detached
// threads: run (handshake + heartbeat/timeout loop), receive (dispatch incoming), and the
// mandatory writer (drains the outgoing queue — §3.2). Kept alive by the threads' shared_ptr
// captures until all three exit.
// ---------------------------------------------------------------------------
class AudioStreamServer::ClientSession
    : public AudioBroadcaster::BroadcastTarget,
      public AudioMixer::TxClient,
      public std::enable_shared_from_this<ClientSession> {
public:
    ClientSession(AudioStreamServer* server, std::string clientId,
                  std::shared_ptr<ClientConnection> connection)
        : server_(server),
          clientId_(std::move(clientId)),
          connection_(std::move(connection)),
          connectTimeMs_(nowMs()),
          sessionConfig_(server->config_),
          // server_->config_, NOT sessionConfig_: a client may override the three buffer
          // timings at handshake (performHandshake below), but bytesPerSecond() reads only
          // sampleRate/bitsPerSample/channels, which are copied verbatim from the server's
          // config and are not negotiable. Using the server's copy also means the capture
          // thread never reads a field the run thread may still be assigning.
          outQueueMaxBytes_(static_cast<std::size_t>(
              static_cast<std::int64_t>(server->config_.bytesPerSecond()) *
              AudioProtocolHandler::CONNECTION_TIMEOUT_MS / 1000)) {}

    // Launches the run thread (detached; keeps `self` alive while it runs).
    void startRunThread() {
        auto self = shared_from_this();
        server_->threadStarted();
        std::thread([self]() {
            self->runLoop();
            self->server_->threadFinished();
        }).detach();
    }

    // --- BroadcastTarget (RX fan-out target) ---
    bool receiveRxAudio(const std::uint8_t* data, std::size_t offset,
                        std::size_t length) override {
        if (closed_.load()) return false;
        // Copy the borrowed bytes and enqueue for the writer thread — NEVER block on a socket
        // send here (this runs on the shared capture thread). §3.2.
        //
        // The return value is the backpressure signal, not a formality: enqueueRxAudio says
        // false when the backlog is at its cap, and false here is what AudioBroadcaster
        // documents as "remove me" (AudioBroadcaster.hpp:49-50). Issue #56 — do not restore
        // the unconditional `return true` this replaced.
        return enqueueRxAudio(std::vector<std::uint8_t>(data + offset, data + offset + length));
    }
    std::string targetId() const override { return clientId_; }

    // --- TxClient (TX arbitration callbacks; all enqueue, never block) ---
    std::string clientId() const override { return clientId_; }
    AudioMixer::TxPriority txPriority() const override { return txPriority_; }
    void onPreempted(const std::string& preemptingClientId) override {
        enqueueControl(ControlMessage::txPreempted(preemptingClientId));
    }
    void onTxGranted() override {
        enqueueControl(ControlMessage::txGranted());
        txDeniedCount_.store(0);
    }
    void onTxReleased() override { enqueueControl(ControlMessage::txReleased()); }

    // Enqueues a roster/control message (the broadcastClientsUpdate path).
    void sendControlMessage(const ControlMessage& message) {
        if (closed_.load() || !streaming_.load()) return;
        enqueueControl(message);
    }

    std::optional<ClientInfo> clientInfoSnapshot() {
        std::lock_guard<std::mutex> lock(infoMutex_);
        return clientInfo_;
    }

    // Idempotent teardown. Callable from any of this session's threads and from the
    // broadcaster failure listener (a server thread). Never joins a thread (would self-join).
    void close();

private:
    struct Outgoing {
        std::optional<ControlMessage> control;  // set => send control, else => send rx audio
        std::vector<std::uint8_t> audio;
    };

    void runLoop();
    // close() + the matching onClientDisconnected + a roster refresh, in that order. The single
    // exit from roster membership, so that the connect event handleNewClient already fired always
    // has exactly one partner (issue #64.2). Idempotent only as far as close() is — call it once
    // per session, on whichever path is leaving.
    void leaveRoster();
    void receiveLoop();
    void writerLoop();
    bool performHandshake();
    std::optional<AudioPacket> receiveOnePacket(int totalTimeoutMs);
    void handleTxAudio(const std::vector<std::uint8_t>& data, Provenance provenance);
    void handleControlMessage(const AudioPacket& packet);
    void enqueueControl(ControlMessage message);
    // False means the outbound backlog is at its cap and this session should be removed.
    bool enqueueRxAudio(std::vector<std::uint8_t> data);

    AudioStreamServer* server_;  // back-pointer; the server outlives every session
    const std::string clientId_;
    std::shared_ptr<ClientConnection> connection_;
    const std::int64_t connectTimeMs_;

    AudioStreamConfig sessionConfig_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<std::int64_t> measuredLatencyMs_{0};
    AudioMixer::TxPriority txPriority_ = AudioMixer::TxPriority::Normal;

    std::mutex infoMutex_;
    std::optional<ClientInfo> clientInfo_;

    std::atomic<std::int64_t> txBytesSubmitted_{0};
    std::atomic<std::int64_t> txBytesAccepted_{0};
    std::atomic<int> txDeniedCount_{0};

    // Writer-bridge: the per-session outgoing queue drained by the writer thread (§3.2).
    std::mutex outMutex_;
    std::condition_variable outCv_;
    std::deque<Outgoing> outQueue_;
    bool outClosed_ = false;
    // Total entries allowed in outQueue_, audio and control together. Sized to sit ABOVE any
    // depth audio can legitimately reach so it never fires on the audio path: audio is capped by
    // outQueueMaxBytes_ (CONNECTION_TIMEOUT_MS of stream, 1,920,000 B on the default preset), and
    // since #20's fan-out frames to AudioPacket::MAX_PAYLOAD the entries are ~16 KB each — order
    // 100s, not 1000s. The smallest-framed built-in preset still lands near 1000. 4096 leaves
    // room above that while bounding a control flood at a few MB. It is a BACKSTOP, not a tuning
    // knob: if this ever fires on audio, the byte cap is the thing that is wrong.
    static constexpr std::size_t kOutQueueMaxDepth = 4096;
    // One report per session, so a peer holding the queue at its cap cannot flood the operator's
    // error callback with one message per dropped control.
    bool controlDropReported_ = false;
    // Pending RX-audio bytes in outQueue_ (control messages count 0 — see enqueueControl).
    // Guarded by outMutex_.
    std::size_t outQueueBytes_ = 0;
    // The backlog cap: CONNECTION_TIMEOUT_MS worth of audio at this stream's bit rate
    // (1,920,000 B on the default preset). Issue #56.
    //
    // WHY THIS QUANTITY. It makes one deadline govern both directions of "this peer has
    // stopped making progress". The server already declares a peer dead after
    // CONNECTION_TIMEOUT_MS of not SENDING TO US — isConnectionTimedOut() keys on
    // lastReceiveTime_ (AudioProtocolHandler.cpp), i.e. on what WE last received; this
    // applies the same window to a peer that will not DRAIN. The coupling is
    // deliberate and is a real coupling: CONNECTION_TIMEOUT_MS is frozen by the wire spec
    // (docs/audio-streaming-protocol-v1.md:291-292 and the constants table at :552-553), so
    // moving it there moves this cap with it. That is the intended behaviour, not a
    // side effect — but it means this line is not free to retune locally.
    //
    // The margin over any legitimate backlog is enormous: the widest jitter buffer any
    // preset configures is bufferMaxMs = 300 (AudioStreamConfig.hpp:30), so a peer at this
    // cap is 33x past the buffer that would have had to conceal the gap. Its audio was
    // unusable long before it was evicted.
    //
    // INT64 ARITHMETIC IS MANDATORY — do NOT reach for AudioStreamConfig::msToBytes, which
    // multiplies in int32 (AudioStreamConfig.hpp:82): udpIq()'s 768000 B/s x 10000 ms is
    // 7.68e9 and overflows, and the cast to size_t would then yield an effectively infinite
    // cap that disables this guard on a shipped preset with no diagnostic. This project
    // passes no warning flags, so nothing would report it.
    const std::size_t outQueueMaxBytes_;

    // Pacing for the run loop's heartbeat/stats wait (woken immediately on close()).
    std::mutex runStopMutex_;
    std::condition_variable runStopCv_;
};

void AudioStreamServer::ClientSession::enqueueControl(ControlMessage message) {
    std::lock_guard<std::mutex> lock(outMutex_);
    if (outClosed_) return;

    // THE DEPTH CAP EXISTS FOR CONTROL MESSAGES SPECIFICALLY (issue #56, item 2). Audio is
    // already bounded by outQueueMaxBytes_, but a control contributes ZERO to that counter by
    // design — the byte counter measures the audio backlog exactly — so without a second bound
    // controls would be the one thing that can grow this deque without limit.
    //
    // That became reachable the moment receiveLoop stopped sending inline: the receive thread
    // now keeps reading while the writer is wedged, so a peer that is not draining and that
    // spams LatencyProbes gets one queued LatencyResponse per probe, forever. Blocking the
    // receive thread used to be what prevented that, which is to say the wedge was doing the
    // bounding. Trading a liveness defect for a memory defect is not a fix.
    //
    // DROP rather than evict: a control is advisory, the drop is bounded and reported once, and
    // a peer that is genuinely gone is already evicted by the heartbeat path (the #56 fix that
    // landed in 8a9e692) within CONNECTION_TIMEOUT_MS. Evicting from here would also mean
    // tearing down a session from inside a mixer callback, which is a re-entry this class
    // deliberately avoids everywhere else.
    if (outQueue_.size() >= kOutQueueMaxDepth) {
        if (!controlDropReported_) {
            controlDropReported_ = true;
            server_->notifyError(clientId_,
                                 "Outbound control queue at its depth cap (" +
                                     std::to_string(kOutQueueMaxDepth) +
                                     " messages): dropping control traffic for a client that is "
                                     "not draining");
        }
        return;
    }

    outQueue_.push_back(Outgoing{std::move(message), {}});
    outCv_.notify_one();
}

bool AudioStreamServer::ClientSession::enqueueRxAudio(std::vector<std::uint8_t> data) {
    {
        std::lock_guard<std::mutex> lock(outMutex_);
        // Teardown in progress is NOT a backlog failure: receiveRxAudio's own closed_ check
        // already refuses for that reason, and a session being torn down must not re-enter
        // the eviction path. True here means "nothing to report", not "delivered".
        if (outClosed_) return true;

        if (outQueueBytes_ >= outQueueMaxBytes_) {
            // The peer has stopped draining. Report before refusing — writerLoop's own
            // send-failure path below sets the house standard that a client is never
            // auto-removed silently, because the disconnect alone leaves no trace of WHY.
            server_->outboundBacklogEvictions_.fetch_add(1);
            server_->notifyError(clientId_,
                                 "Outbound backlog limit reached (" +
                                     std::to_string(outQueueBytes_) + " of " +
                                     std::to_string(outQueueMaxBytes_) +
                                     " bytes): client is not draining");
            // False is the documented removal request (AudioBroadcaster.hpp:49-50). The
            // broadcaster erases this target and its failure listener closes the session
            // (AudioStreamServer.cpp, initializeSharedAudio), which is what both bounds the
            // queue and frees the maxClients slot. Returning false is the whole fix; the
            // counter and the message above are only how an operator finds out.
            return false;
        }

        // Tested against the CURRENT depth before adding, the shape BlockingPacketQueue
        // already uses: an empty queue always accepts one chunk, so overshoot is bounded by
        // exactly one chunk and a healthy client can never be refused.
        //
        // That chunk used to be "however large the host made it", because injectAudio forwarded
        // whatever it was given. Since #20 the fan-out frames to AudioPacket::MAX_PAYLOAD, so the
        // overshoot bound is now a CONSTANT (<= 16384 bytes) rather than caller-controlled — a
        // tightening, not a relaxation. A host injecting a 1 MB buffer no longer parks 1 MB in
        // this queue in a single push.
        outQueueBytes_ += data.size();
        outQueue_.push_back(Outgoing{std::nullopt, std::move(data)});
    }
    outCv_.notify_one();
    return true;
}

std::optional<AudioPacket> AudioStreamServer::ClientSession::receiveOnePacket(int totalTimeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(totalTimeoutMs);
    while (!closed_.load() && server_->running_.load() &&
           std::chrono::steady_clock::now() < deadline) {
        ReceiveResult r = connection_->receivePacket(200);
        if (r.hasPacket()) return std::move(r.packet);
        if (r.closed) return std::nullopt;
    }
    return std::nullopt;
}

bool AudioStreamServer::ClientSession::performHandshake() {
    auto packet = receiveOnePacket(10000);
    if (!packet || packet->packetType() != PacketType::Control) return false;

    auto msg = ControlMessage::deserialize(packet->payload());
    if (!msg || msg->messageType() != ControlType::ConnectRequest) return false;

    // The client keeps the server's audio format but may request its own buffer timings.
    auto requested = msg->parseConnectRequestConfig();
    if (requested.has_value()) {
        AudioStreamConfig c;
        c.sampleRate = server_->config_.sampleRate;
        c.bitsPerSample = server_->config_.bitsPerSample;
        c.channels = server_->config_.channels;
        c.frameDurationMs = server_->config_.frameDurationMs;
        c.bufferTargetMs = requested->bufferTargetMs;
        c.bufferMinMs = requested->bufferMinMs;
        c.bufferMaxMs = requested->bufferMaxMs;
        sessionConfig_ = c;
    }

    auto info = msg->parseConnectRequestClientInfo();
    if (info.has_value() && !info->isEmpty()) {
        std::lock_guard<std::mutex> lock(infoMutex_);
        clientInfo_ = info;
    }
    return true;
}

void AudioStreamServer::ClientSession::runLoop() {
    // THE CONNECT/DISCONNECT EVENT PAIR BRACKETS ROSTER MEMBERSHIP (issue #64.2). This session is
    // already in sessions_ and already counted by clientCount(), and notifyClientConnected has
    // already fired for it (handleNewClient) — so every exit from here owes the matching
    // disconnect, including the ones that never get a client onto the air.
    //
    // Both paths below used to be a bare close() + return. close() erases the session and emits
    // nothing, so the SERVER's roster was correct while a LISTENER's model was not: any peer that
    // connected and said the wrong thing — a port scanner, a version-mismatched client, a
    // half-open probe — left a connect event that nothing ever closed, and an event-pairing
    // listener accumulated one phantom client per probe. Only the event pair can see that; the
    // roster reads 0 either way, which is why it took an arm that asserts on pairing to find it.
    //
    // leaveRoster() is the same close/notify/broadcast trio the success path runs at the bottom of
    // this function, minus the stream events — this session never started streaming, so it owes no
    // onStreamStopped, and those pair on their own.
    if (!performHandshake()) {
        leaveRoster();
        return;
    }

    // Send config + accept directly (awaited before any RX audio is registered).
    if (!connection_->sendControl(ControlMessage::audioConfig(sessionConfig_)) ||
        !connection_->sendControl(ControlMessage::connectAccept())) {
        leaveRoster();
        return;
    }

    // Register with broadcaster + mixer (copied out under runMutex_).
    std::shared_ptr<AudioBroadcaster> broadcaster;
    std::shared_ptr<AudioMixer> mixer;
    {
        std::lock_guard<std::mutex> lock(server_->runMutex_);
        broadcaster = server_->broadcaster_;
        mixer = server_->mixer_;
    }
    auto self = shared_from_this();
    if (broadcaster) broadcaster->addTarget(self);
    if (mixer) mixer->registerClient(self);

    // Start the writer + receive threads (detached, keepalive, barrier-counted).
    server_->threadStarted();
    std::thread([self]() {
        self->writerLoop();
        self->server_->threadFinished();
    }).detach();
    server_->threadStarted();
    std::thread([self]() {
        self->receiveLoop();
        self->server_->threadFinished();
    }).detach();

    server_->notifyStreamStarted(clientId_);
    streaming_.store(true);
    server_->broadcastClientsUpdate();

    // Heartbeat / timeout loop.
    while (!closed_.load() && server_->running_.load()) {
        // A FAILED HEARTBEAT IS FATAL, because for one class of dead peer it is the only
        // evidence this loop will ever get (issue #56, the idle stall).
        //
        // isConnectionTimedOut() below keys on lastReceiveTime_, NOT on send time. So a peer
        // that has stopped READING while it keeps SENDING refreshes that clock forever and
        // the timeout cannot fire however long its receive window has been shut. The other
        // eviction path — the outbound backlog cap — is a bound on VOLUME, so it is equally
        // silent when no audio happens to be flowing. Take both away and a session that is
        // failing every single send stays on the roster indefinitely, holding two detached
        // threads and one of DEFAULT_MAX_CLIENTS slots. MEASURED before this guard existed:
        // seven consecutive failed heartbeats across eight watchdog evaluations, and
        // clientCount() still 1 (Server.APeerThatStopsReadingButKeepsSendingIsEvicted...).
        //
        // Since #56's deadline half, this send is bounded — sendAll spends
        // CONNECTION_TIMEOUT_MS / 2 and reports failure rather than parking forever — which
        // is what makes the return value worth reading at all. Before that it never came back.
        //
        // NOT A NEW POLICY: writerLoop in this same class already closes the session on a
        // failed send, for the same reason and with the same "never silently" reporting. This
        // loop was simply the one send path that discarded its verdict. The client's mirror of
        // this loop reports a failed heartbeat too (AudioStreamClient::heartbeatLoop, #71).
        //
        // The closed_ re-check keeps a session that is ALREADY tearing down from reporting a
        // spurious error: sendPacket also returns false once the handler is closed, and that
        // is an ordinary teardown, not a peer fault.
        if (connection_->shouldSendHeartbeat() && !connection_->sendHeartbeat()) {
            if (!closed_.load()) {
                server_->notifyError(clientId_,
                                     "Heartbeat send failed: client is not draining");
                break;
            }
        }
        if (connection_->isConnectionTimedOut()) {
            server_->notifyError(clientId_, "Connection timeout");
            break;
        }
        std::unique_lock<std::mutex> lock(runStopMutex_);
        runStopCv_.wait_for(lock, std::chrono::milliseconds(1000),
                            [this]() { return closed_.load() || !server_->running_.load(); });
    }

    // Cleanup. The stream events pair around the streaming window; leaveRoster() closes the
    // outer connect/disconnect pair, and is the same call both failure paths above make.
    streaming_.store(false);
    server_->notifyStreamStopped(clientId_);
    leaveRoster();
}

void AudioStreamServer::ClientSession::leaveRoster() {
    close();
    server_->notifyClientDisconnected(clientId_);
    server_->broadcastClientsUpdate();
}

void AudioStreamServer::ClientSession::receiveLoop() {
    while (!closed_.load() && server_->running_.load()) {
        ReceiveResult r = connection_->receivePacket(100);
        if (r.closed) {
            if (!closed_.load()) {
                server_->notifyError(clientId_, "Receive error");
                close();
            }
            break;
        }
        if (!r.hasPacket()) continue;

        switch (r.packet->packetType()) {
            case PacketType::AudioTx:
                handleTxAudio(r.packet->payload(), r.provenance);
                break;
            case PacketType::Control:
                handleControlMessage(*r.packet);
                break;
            case PacketType::Heartbeat:
                break;  // alive
            default:
                break;
        }
    }
}

void AudioStreamServer::ClientSession::writerLoop() {
    while (true) {
        Outgoing item;
        {
            std::unique_lock<std::mutex> lock(outMutex_);
            outCv_.wait(lock, [this]() { return !outQueue_.empty() || outClosed_; });
            if (outQueue_.empty()) return;  // closed and drained
            item = std::move(outQueue_.front());
            outQueue_.pop_front();
            // Still under outMutex_. Controls contribute 0 in both directions, so the
            // counter measures the audio backlog exactly and cannot drift below zero.
            if (!item.control.has_value()) outQueueBytes_ -= item.audio.size();
        }
        const bool isControl = item.control.has_value();
        const std::size_t audioBytes = isControl ? 0 : item.audio.size();
        const bool ok = isControl
                            ? connection_->sendControl(*item.control)
                            : connection_->sendRxAudio(item.audio.data(), 0, audioBytes);
        if (!ok) {
            // Auto-remove the dead client — but never silently. This close() drops the session,
            // which unregisters it from the mixer and so releases any TX channel it held: an
            // operator mid-transmission goes off the air here. Reporting only the disconnect
            // (below, via notifyClientDisconnected) leaves no trace of WHY, and the cause is
            // often a send the kernel refused rather than a client that went away — e.g. an
            // RX frame larger than the socket's send buffer (EMSGSIZE). Name the direction and
            // the size so an oversized-frame kill is distinguishable from a real disconnect.
            if (isControl) {
                server_->notifyError(clientId_, "Send error: control message");
            } else {
                server_->notifyError(clientId_, "Send error: RX audio frame (" +
                                                    std::to_string(audioBytes) + " bytes)");
            }
            close();
            return;
        }
    }
}

void AudioStreamServer::ClientSession::handleTxAudio(const std::vector<std::uint8_t>& data,
                                                     Provenance provenance) {
    txBytesSubmitted_.fetch_add(static_cast<std::int64_t>(data.size()));

    std::shared_ptr<AudioMixer> mixer;
    {
        std::lock_guard<std::mutex> lock(server_->runMutex_);
        mixer = server_->mixer_;
    }
    // KEEP THIS, AND DO NOT FILE A DETECTOR FOR IT AGAIN — it is undetectable BY CONSTRUCTION,
    // and the reason is worth more than the branch. Issue #68 (folded into #88 as item 1's
    // neighbour, item 4) asked for an arm that reddens when this line becomes `if (false)`, on
    // the premise that it protects "a TX frame arriving after stopSharedAudio() releases mixer_
    // but before the session's receive thread winds down". That premise no longer describes this
    // tree: the window is closed twice over, by two independent gates.
    //
    //   1. receiveLoop is the ONLY caller of this function (:452) and re-tests
    //      server_->running_.load() every iteration (:439).
    //   2. stop() clears running_ FIRST (:772), then runs teardownSessions() — whose join
    //      barrier waits for activeThreads_ == 0, i.e. for every runLoop/writerLoop/receiveLoop
    //      thread to have EXITED (:760-763) — and only then calls stopSharedAudio() (:775) to
    //      null mixer_. So no receive thread is alive when mixer_ is released.
    //
    // The issue #57 straggler does not reach here either: admitted between the two teardown
    // passes, its runLoop -> performHandshake -> receiveOnePacket loop is gated on the same
    // already-false running_ (:288), so the handshake fails at once and it leaves via
    // leaveRoster(). And start() refuses re-entry while running_ (:720), so initializeSharedAudio's
    // own failure-path stopSharedAudio() (:859) cannot fire with sessions alive.
    //
    // MEASURED at d253f55, not reasoned: an instrumented full suite entered this function 23,038
    // times with ZERO null-mixer arrivals, and the whole suite is 438/438 GREEN with this line
    // mutated to `if (false)`. Both numbers are the SAME fact — there is no test to write here,
    // because there is no supported call sequence that reaches the branch.
    //
    // What it still covers is CALLER MISUSE: a start() racing an in-flight stop() can leave
    // running_ true with mixer_ null (running_ goes true at :736, mixer_ is not built until :829),
    // and that path can std::terminate on the acceptThread_ reassignment before it ever gets here.
    // So this is defence-in-depth against a caller error, not against the teardown race.
    //
    // It stays because it is one branch and it becomes load-bearing the instant either gate above
    // moves — and NOTHING would catch that, which is exactly why deleting an unreachable guard is
    // the wrong trade. If you reorder stop(), or drop receiveLoop's running_ test, this line is
    // what stands between that change and a null dereference on the receive thread.
    if (!mixer) return;

    // Issue #65, closed here: the provenance carried on the ReceiveResult reaches the
    // mixer, so a frame the FEC layer reconstructed can no longer claim the TX channel,
    // preempt a talker, or spend this client's one TX_DENIED. The mixer owns that
    // contract (the table at AudioMixer.hpp:29-31); this call site only has to stop
    // lying about where the frame came from.
    //
    // This hop DOES have a detector now (issue #67, which was filed because for a while it
    // did not): Server.ARecoveredTxFrameDoesNotClaimAnUnownedTxChannel drives a real session
    // over a supplied transport (setTransportFactory) and reddens if the argument below stops
    // carrying what the receive path handed it. Forcing either this argument or receiveLoop's
    // to the live value goes red there; forcing it to Recovered goes red in a larger set of
    // end-to-end arms. Do not write that set's SIZE down here — the previous version of this
    // comment did, and the number was wrong within one release because two of those arms are
    // registered only in a bridge-enabled build. Measure it, do not transcribe it.
    //
    // TxResult::DeclinedRecovered is deliberately not handled below, and it CAN now
    // occur: falling through both branches is exactly right for it — a repair the mixer
    // declined must count no accepted bytes and must NOT consume the client's single
    // per-episode TX_DENIED, which is the whole point of it being distinct from Rejected.
    // That fall-through is a contract enforced by ABSENT code, so it has its own arm rather
    // than relying on this comment: Server.ARecoveredTxFrameOnAnUnownedChannelSendsNoTxDenied
    // reddens if the branch below is widened to admit anything that is merely not Accepted.
    const AudioMixer::TxResult result =
        mixer->submitTxAudio(clientId_, data, provenance);
    if (result == AudioMixer::TxResult::Accepted) {
        txBytesAccepted_.fetch_add(static_cast<std::int64_t>(data.size()));
    } else if (result == AudioMixer::TxResult::Rejected) {
        const int denied = txDeniedCount_.fetch_add(1) + 1;
        if (denied == 1) {  // first denial only — avoid spam
            // ENQUEUED, not sent inline (issue #56, item 2). This runs on the RECEIVE thread, and
            // a direct sendControl takes the same sendMutex_ the writer holds across its whole
            // send — so a peer that has stopped draining used to wedge the one thread that
            // processes its TX audio, its heartbeats and its DISCONNECT. Bounded since the send
            // deadline landed, but bounded-and-wrong is still wrong: the writer bridge exists
            // precisely so no other thread ever blocks on a socket.
            enqueueControl(ControlMessage::txDenied(mixer->currentTxOwner()));
        }
    }
}

void AudioStreamServer::ClientSession::handleControlMessage(const AudioPacket& packet) {
    auto msg = ControlMessage::deserialize(packet.payload());
    if (!msg) return;

    switch (msg->messageType()) {
        case ControlType::LatencyProbe:
            // Enqueued for the same reason as txDenied above (issue #56, item 2): a LatencyProbe
            // from a peer that has stopped draining must not wedge the receive thread.
            //
            // THE TRADE IS REAL AND IS THE RIGHT WAY ROUND. Queuing puts the response behind
            // whatever audio is already pending, so a backlogged client measures a LARGER
            // round-trip than it would have. That is not an error in the measurement — a client
            // whose queue is deep genuinely is that far behind, and reporting it is the honest
            // answer. Blocking the receive thread to make one number prettier costs that client
            // its TX audio and its disconnect handling.
            enqueueControl(ControlMessage::latencyResponse(msg->parseLatencyTimestamp()));
            break;
        case ControlType::LatencyResponse: {
            const std::int64_t sent = msg->parseLatencyTimestamp();
            measuredLatencyMs_.store((nowNanos() - sent) / 1'000'000 / 2);
            break;
        }
        case ControlType::Disconnect:
            close();
            break;
        default:
            break;
    }
}

void AudioStreamServer::ClientSession::close() {
    if (closed_.exchange(true)) return;

    // Copy the shared resources out under runMutex_ and call into them AFTER unlock
    // (lock-drop-before-callback, §3.3): unregisterClient re-enters the server via the mixer
    // listener -> broadcastClientsUpdate.
    std::shared_ptr<AudioBroadcaster> broadcaster;
    std::shared_ptr<AudioMixer> mixer;
    std::shared_ptr<ServerTransport> transport;
    {
        std::lock_guard<std::mutex> lock(server_->runMutex_);
        broadcaster = server_->broadcaster_;
        mixer = server_->mixer_;
        transport = server_->transport_;
    }
    if (broadcaster) broadcaster->removeTarget(clientId_);
    if (mixer) mixer->unregisterClient(clientId_);

    {
        std::lock_guard<std::mutex> lock(server_->sessionsMutex_);
        server_->sessions_.erase(clientId_);
    }

    // Wake the writer + run-loop pacing wait. The receive thread polls closed_ on its 100 ms
    // receive timeout, so it exits on its own.
    {
        std::lock_guard<std::mutex> lock(outMutex_);
        outClosed_ = true;
        // outQueueBytes_ is deliberately NOT reset here. Its invariant is "exactly the RX
        // audio bytes currently in outQueue_", and close() does not empty the queue (the
        // writer may still be inside a send holding the front item). Zeroing it while items
        // remain would make writerLoop's next `-=` underflow a size_t into a huge value —
        // which nothing reads today, and which would silently disable the cap the moment
        // anyone added a depth accessor.
    }
    outCv_.notify_all();
    runStopCv_.notify_all();

    connection_->close();
    if (transport) transport->disconnectClient(connection_);
}

// ---------------------------------------------------------------------------
// AudioStreamServer
// ---------------------------------------------------------------------------

AudioStreamServer::AudioStreamServer(std::uint16_t port, AudioStreamConfig config,
                                     std::string bindHost)
    : port_(port), config_(config), bindHost_(std::move(bindHost)) {
    dispatcher_.start();  // listener callbacks fire on this thread, never on a worker
}

AudioStreamServer::~AudioStreamServer() {
    stop();
    dispatcher_.stop();  // drain + join AFTER the workers stop posting
}

AudioFormat AudioStreamServer::formatFromConfig() const {
    AudioFormat f;
    f.sampleRate = config_.sampleRate;
    f.bitsPerSample = config_.bitsPerSample;
    f.channels = config_.channels;
    f.encoding = Encoding::PcmSigned;
    f.endianness = Endianness::Little;
    return f;
}

std::shared_ptr<ServerTransport> AudioStreamServer::createTransport() {
    // A supplied factory replaces construction outright — deliberately no fallback to the
    // config, because a caller that names a transport means it. start() null-checks the
    // result, so a factory that returns nothing fails the start rather than reaching bind().
    if (transportFactory_) return transportFactory_();

    switch (config_.transportType) {
        case TransportType::Udp: {
            auto udp = std::make_shared<UdpServerTransport>(bindHost_);
            UdpReliabilityConfig cfg;
            cfg.reorderWindowSize = config_.reorderBufferSize;
            cfg.reorderMaxHoldMs = config_.reorderMaxHoldMs;
            cfg.fecEnabled = config_.fecEnabled;
            cfg.fecBlockSize = config_.fecBlockSize;
            cfg.frameDurationMs = config_.frameDurationMs;
            cfg.maxAudioPayloadBytes = config_.udpMaxAudioPayload();  // #86
            cfg.adaptiveJitterEnabled = config_.adaptiveJitterEnabled;
            cfg.jitterMinMs = config_.bufferMinMs;
            cfg.jitterMaxMs = config_.bufferMaxMs;
            cfg.jitterMultiplier = config_.jitterMultiplier;
            cfg.controlReliabilityEnabled = config_.controlReliabilityEnabled;
            cfg.controlRetransmitMaxAttempts = config_.controlRetransmitMaxAttempts;
            udp->setReliabilityConfig(cfg);
            return udp;
        }
        case TransportType::Dual:
            return std::make_shared<DualServerTransport>(config_, bindHost_);
        case TransportType::Tcp:
        default:
            return std::make_shared<TcpServerTransport>(bindHost_);
    }
}

bool AudioStreamServer::start(std::string* err) {
    if (running_.load()) {
        if (err) *err = "Server already running";
        return false;
    }

    auto transport = createTransport();
    // Only reachable through setTransportFactory — the config switch always returns one —
    // but it is reachable from public API, and the next line would dereference it.
    if (!transport) {
        if (err) *err = "Transport factory returned no transport";
        return false;
    }
    if (!transport->bind(port_, err)) return false;
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        transport_ = transport;
    }
    running_.store(true);

    if (!initializeSharedAudio(err)) {
        running_.store(false);
        std::lock_guard<std::mutex> lock(runMutex_);
        if (transport_) transport_->close();
        transport_.reset();
        return false;
    }

    acceptThread_ = std::thread(&AudioStreamServer::acceptLoop, this);
    notifyServerStarted(transport->port());
    return true;
}

void AudioStreamServer::teardownSessions() {
    // Close all sessions (each signals its threads to exit).
    std::vector<std::shared_ptr<ClientSession>> snapshot;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (auto& [id, s] : sessions_) snapshot.push_back(s);
    }
    for (auto& s : snapshot) s->close();

    // Wait for every detached session thread to finish (the join barrier).
    {
        std::unique_lock<std::mutex> lock(threadsMutex_);
        threadsCv_.wait(lock, [this]() { return activeThreads_ == 0; });
    }
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_.clear();
    }
}

void AudioStreamServer::stop() {
    if (!running_.exchange(false)) return;

    teardownSessions();
    stopSharedAudio();

    // Close the transport to unblock the accept thread, then join it.
    //
    // This is the call stack issue #90 quoted, and it is still the right shape — but the close no
    // longer unblocks the accept by yanking the descriptor out from under it. Socket::close() takes
    // the handle, and acceptTcp's poll notices that between slices and returns; only once it has
    // left is the descriptor freed. So this line now RETURNS with the accept thread already out of
    // the kernel, and the join below reaps a thread that is on its way out rather than one that
    // still has to be woken.
    std::shared_ptr<ServerTransport> transport;
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        transport = transport_;
    }
    if (transport) transport->close();
    if (acceptThread_.joinable()) acceptThread_.join();

    // SECOND PASS — issue #57. The first pass above is NOT final: the accept thread can be part
    // way through handleNewClient when it runs. handleNewClient samples running_ at entry, so a
    // connection popped just before stop() proceeds to insert a session and call startRunThread()
    // — and the barrier above passes precisely because threadStarted() has not run yet. Worse,
    // the join we just did GUARANTEES it: acceptLoop cannot return until handleNewClient does, so
    // stop() waits for the very thread that is creating the straggler. The old code then returned
    // with activeThreads_ == 1 and a live detached runLoop; the destructor's stop() is a no-op
    // (running_.exchange(false) already returned false), so nothing ever waited for that thread
    // and ~AudioStreamServer destroyed runMutex_/sessionsMutex_/dispatcher_ underneath it.
    //
    // The same window lets handleNewClient's openSharedAudioLines() reopen captureStream_ after
    // stopSharedAudio() moved it out, leaving a capture device open on a stopped server.
    //
    // Now that acceptThread_ is joined, no new client can be admitted, so this pass IS final —
    // that ordering is the whole argument, which is why the repeat lives after the join and not
    // before it. Both calls are idempotent: with no straggler this is a mutex acquire, an
    // already-satisfied barrier predicate, and a set of null resets.
    teardownSessions();
    stopSharedAudio();

    {
        std::lock_guard<std::mutex> lock(runMutex_);
        transport_.reset();
    }

    // POSTS onServerStopped; it does not drain. Issue #89 asked whether it should, and the answer
    // is recorded in full on AudioStreamServer::removeStreamListener() — the short version is that
    // CallbackDispatcher is not restartable, so a drain here would leave a restarted server mute.
    // A consumer that needs its listener destroyed before the server calls removeStreamListener().
    notifyServerStopped();
}

bool AudioStreamServer::initializeSharedAudio(std::string* err) {
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        broadcaster_ = std::make_shared<AudioBroadcaster>(config_);
        mixer_ = std::make_shared<AudioMixer>(config_);
    }

    // A failed broadcast target closes its session.
    broadcaster_->setBroadcastListener([this](const std::string& targetId, const std::string&) {
        std::shared_ptr<ClientSession> session;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            auto it = sessions_.find(targetId);
            if (it != sessions_.end()) session = it->second;
        }
        if (session) session->close();
    });

    // #59: mid-stream loss of the shared capture device. No clientId — the SOURCE went away, so
    // every client is affected equally; "" is the same id the accept-error path uses.
    broadcaster_->setCaptureErrorListener([this](const std::string& reason) {
        notifyError("", "Capture device lost: " + reason);
    });

    AudioMixer::MixerListener ml;
    ml.onTxConflict = [](const std::string&, const std::string&) {};
    ml.onTxOwnerChanged = [this](const std::string&) { broadcastClientsUpdate(); };
    ml.onPlaybackDeviceError = [this](const std::string& reason) {
        notifyError("", "Playback device lost: " + reason);
    };
    mixer_->setMixerListener(ml);

    if (captureBackendId_.has_value()) {
        if (!openSharedAudioLines(err)) {
            stopSharedAudio();
            return false;
        }
    }
    return true;
}

bool AudioStreamServer::openSharedAudioLines(std::string* err) {
    std::lock_guard<std::mutex> lock(runMutex_);
    if (!captureStream_ && captureBackendId_.has_value() && backend_ != nullptr) {
        try {
            captureStream_ = backend_->openCaptureStream(*captureBackendId_, formatFromConfig());
        } catch (const DeviceUnavailable& e) {
            if (err) *err = e.what();
            return false;
        }
        if (broadcaster_) broadcaster_->start(captureStream_.get());
    }
    if (!playbackStream_ && playbackBackendId_.has_value() && backend_ != nullptr) {
        try {
            playbackStream_ = backend_->openPlaybackStream(*playbackBackendId_, formatFromConfig());
        } catch (const DeviceUnavailable& e) {
            if (err) *err = e.what();
            return false;
        }
        if (mixer_) mixer_->start(playbackStream_.get());
    }
    return true;
}

void AudioStreamServer::stopSharedAudio() {
    // Reset the members under the lock, but stop the threads OUTSIDE it (b->stop()/m->shutdown()
    // join their worker threads — never hold runMutex_ across a join). The streams are destroyed
    // only after their reader/writer threads have stopped (order is load-bearing).
    std::shared_ptr<AudioBroadcaster> broadcaster;
    std::shared_ptr<AudioMixer> mixer;
    std::unique_ptr<CaptureStream> capture;
    std::unique_ptr<PlaybackStream> playback;
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        broadcaster = broadcaster_;
        broadcaster_.reset();
        mixer = mixer_;
        mixer_.reset();
        capture = std::move(captureStream_);
        playback = std::move(playbackStream_);
    }
    if (broadcaster) broadcaster->stop();
    if (mixer) mixer->shutdown();
    // capture / playback destructors close the device streams here, after the threads stopped.
}

void AudioStreamServer::acceptLoop() {
    std::shared_ptr<ServerTransport> transport;
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        transport = transport_;
    }
    if (!transport) return;

    while (running_.load()) {
        std::shared_ptr<ClientConnection> connection;
        std::string err;
        const IoStatus st = transport->acceptClient(1000, connection, &err);
        if (st == IoStatus::Ok && connection) {
            handleNewClient(connection);
        } else if (st == IoStatus::Error) {
            if (running_.load()) notifyError("", "Accept error: " + err);
        }
        // TimedOut -> loop and re-check running_.
    }
}

void AudioStreamServer::handleNewClient(const std::shared_ptr<ClientConnection>& connection) {
    if (!running_.load()) {
        evictConnection(connection);
        return;
    }

    const std::string clientId = "audio-" + std::to_string(clientIdCounter_.fetch_add(1));
    const std::string address = connection->remoteAddress();

    // A capture device is required unless inject-only (playback is optional).
    if (!captureBackendId_.has_value() && !injectOnlyMode_.load()) {
        rejectClient(connection, RejectReason::Rejected, "Capture device not configured");
        return;
    }

    // Max-clients limit (the accept thread is single, so size-check + insert are sequential).
    bool roomAvailable;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        roomAvailable = static_cast<int>(sessions_.size()) < config_.maxClients;
    }
    if (!roomAvailable) {
        rejectClient(connection, RejectReason::Busy,
                     "Maximum clients (" + std::to_string(config_.maxClients) + ") reached");
        return;
    }

    // Ensure the shared audio lines are open.
    {
        std::string err;
        if (!openSharedAudioLines(&err)) {
            rejectClient(connection, RejectReason::Rejected, "Audio devices unavailable: " + err);
            return;
        }
    }

    auto session = std::make_shared<ClientSession>(this, clientId, connection);
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        // Re-check under the lock (issue #57). The running_ sample at entry is stale by now — the
        // handshake-stage work above (device check, roster check, openSharedAudioLines) all takes
        // time a stop() can land in. This does not by itself close the race (stop() can still pass
        // its first barrier between this insert and startRunThread() below), which is why stop()
        // repeats the whole teardown after joining the accept thread. What it buys is that the
        // COMMON case never admits a client onto a stopping server: no session in a map that is
        // about to be cleared, and no notifyClientConnected for a client that never really
        // connected — an event a listener would otherwise have to un-see.
        if (!running_.load()) {
            // Under sessionsMutex_, so this must not reach for runMutex_ — evictConnection would
            // invert the two locks. stop() closes the transport moments from now and its close()
            // drains the whole connection map, so the entry cannot outlive the transport here;
            // that is what makes the plain close() sufficient on this path and not on the
            // reject paths, which run on a server that keeps serving.
            connection->close();
            return;
        }
        sessions_[clientId] = session;
    }
    notifyClientConnected(clientId, address);
    session->startRunThread();
}

void AudioStreamServer::rejectClient(const std::shared_ptr<ClientConnection>& connection,
                                     RejectReason reason, const std::string& message) {
    // Send FIRST, evict second: evictConnection closes the connection, and the peer is owed the
    // reason it was turned away.
    connection->sendControl(ControlMessage::connectReject(reason, message));

    // Then DRAIN before closing, or the peer never reads what we just sent (issue #87).
    //
    // This reject was decided at accept, before a byte was read — so a client that behaved
    // normally has a CONNECT_REQUEST sitting unread in our receive buffer, and closing on top of
    // unread data makes the stack send an RST instead of a FIN. The RST makes the peer discard its
    // receive buffer, reject included. MEASURED on windows-latest: without this, a client that
    // sends before reading gets no CONNECT_REJECT at all — deterministically, from the first
    // attempt (CI run 31663121086). macOS and Linux deliver it either way; 200 loopback runs there
    // across five read-delays could not open the window, so this is not reproducible off Windows.
    //
    // A no-op on UDP, which has no reset semantics and whose server connections share one socket.
    //
    // 20 ms is a CEILING, not a cost: the ordinary call finds the request already buffered, reads
    // it, and exits on the next read's 2 ms timeout. It is bounded at all because this runs on the
    // accept thread, where time spent is time no other client is admitted.
    connection->discardPendingInput(/*budgetMs=*/20);

    evictConnection(connection);
}

void AudioStreamServer::evictConnection(const std::shared_ptr<ClientConnection>& connection) {
    // The lock-drop discipline close() uses: copy the transport out under runMutex_, call into it
    // unlocked. disconnectClient takes the transport's own map lock and must not be called with
    // runMutex_ held.
    std::shared_ptr<ServerTransport> transport;
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        transport = transport_;
    }
    if (transport) transport->disconnectClient(connection);
    // Unconditional, and not redundant with the line above: disconnectClient closes only what it
    // actually found in its map, and transport_ is already null on the stop path this is also
    // called from. close() is idempotent, so the overlap costs nothing.
    connection->close();
}

void AudioStreamServer::broadcastClientsUpdate() {
    std::vector<std::pair<std::string, std::shared_ptr<ClientSession>>> snapshot;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        if (sessions_.empty()) return;
        for (auto& [id, s] : sessions_) snapshot.emplace_back(id, s);
    }

    std::string txOwnerId;
    {
        std::shared_ptr<AudioMixer> mixer;
        {
            std::lock_guard<std::mutex> lock(runMutex_);
            mixer = mixer_;
        }
        if (mixer) txOwnerId = mixer->currentTxOwner();
    }

    std::vector<std::string> clientIds;
    std::map<std::string, ClientInfo> clientInfoMap;
    for (auto& [id, session] : snapshot) {
        clientIds.push_back(id);
        auto info = session->clientInfoSnapshot();
        if (info.has_value()) clientInfoMap[id] = *info;
    }

    ControlMessage update = ControlMessage::clientsUpdateWithInfo(
        static_cast<std::int32_t>(snapshot.size()), config_.maxClients, txOwnerId, clientIds,
        &clientInfoMap);

    for (auto& [id, session] : snapshot) {
        session->sendControlMessage(update);
    }
}

bool AudioStreamServer::hasClient() const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    return !sessions_.empty();
}

int AudioStreamServer::clientCount() const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    return static_cast<int>(sessions_.size());
}

std::vector<std::string> AudioStreamServer::connectedClientIds() const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for (auto& [id, s] : sessions_) ids.push_back(id);
    return ids;
}

std::string AudioStreamServer::txOwner() const {
    std::shared_ptr<AudioMixer> mixer;
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        mixer = mixer_;
    }
    return mixer ? mixer->currentTxOwner() : "";
}

void AudioStreamServer::injectAudio(const std::vector<std::uint8_t>& data) {
    if (data.empty()) return;
    std::shared_ptr<AudioBroadcaster> broadcaster;
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        broadcaster = broadcaster_;
    }
    if (broadcaster) broadcaster->injectAudio(data);
}

void AudioStreamServer::addStreamListener(AudioStreamListener* listener) {
    if (!listener) return;
    std::lock_guard<std::mutex> lock(listenersMutex_);
    for (auto* l : listeners_) {
        if (l == listener) return;
    }
    listeners_.push_back(listener);
}

void AudioStreamServer::removeStreamListener(AudioStreamListener* listener) {
    if (!listener) return;
    {
        std::lock_guard<std::mutex> lock(listenersMutex_);
        listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener),
                         listeners_.end());
    }
    // OUTSIDE listenersMutex_, and that ordering is load-bearing twice over. The erase must land
    // first, or a task posted between the two would name a listener this call is about to promise
    // is detached. And the wait must not hold the lock: the task being waited on may re-enter the
    // server from the dispatch thread, and every notify* takes listenersMutex_ to snapshot — so
    // fencing under it would deadlock the two threads against each other.
    dispatcher_.fence();
}

int AudioStreamServer::port() const {
    std::shared_ptr<ServerTransport> transport;
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        transport = transport_;
    }
    return transport ? transport->port() : -1;
}

ServerStats AudioStreamServer::stats() const {
    ServerStats s;
    // A shared_ptr copy under runMutex_, then read OUTSIDE it — the same lock-drop discipline
    // the sessions use, and it also keeps the transport alive across the reads even if stop()
    // races us and drops transport_.
    std::shared_ptr<ServerTransport> transport;
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        transport = transport_;
    }
    if (!transport) return s;  // not started (or already stopped): defaults, running == false

    s.running = true;
    // clientCount() takes sessionsMutex_, which is NOT held here and is never held with
    // runMutex_ — the two are independent and this is the only site that reads both.
    s.clientsConnected = clientCount();
    s.packetsSent = transport->packetsSent();
    s.packetsReceived = transport->packetsReceived();
    s.bytesSent = transport->bytesSent();
    s.bytesReceived = transport->bytesReceived();
    s.crcErrors = transport->crcErrors();
    s.controlRetransmits = transport->controlRetransmits();
    s.queueDrops = transport->orderedQueueDrops();
    // Deliberately NOT a consistent snapshot across fields: each aggregate takes the
    // transport's routing-map lock independently, so a client can join or leave between two of
    // them. Holding one lock across all eight would serialize the read against every accept and
    // disconnect for no benefit a diagnostic counter can use. Per-field skew is bounded by one
    // roster change, and the roster is already documented as the thing these sum over.
    return s;
}

void AudioStreamServer::threadStarted() {
    std::lock_guard<std::mutex> lock(threadsMutex_);
    ++activeThreads_;
}

void AudioStreamServer::threadFinished() {
    // notify_all() is INSIDE the lock, and that is load-bearing rather than stylistic (issue #28).
    //
    // Releasing first is the usual advice — it avoids waking a waiter that immediately blocks on
    // the mutex — but it is unsound when the waiter's next act is to DESTROY the condition
    // variable. The sequence TSan caught on ubuntu: this thread decrements to 0 and unlocks,
    // stop()'s barrier at :727 wakes and returns, ~AudioStreamServer runs to completion and
    // destroys threadsCv_, and this thread — still between the unlock and the notify — calls
    // pthread_cond_broadcast on freed memory.
    //
    //   Write of size 8 by main thread:      pthread_cond_destroy
    //                                        AudioStreamServer::~AudioStreamServer() :638
    //   Previous read of size 8 by T5:       pthread_cond_broadcast
    //                                        AudioStreamServer::threadFinished() :1120
    //
    // Holding the lock across the notify closes it: the waiter cannot return from wait() until it
    // re-acquires threadsMutex_, which cannot happen until this scope ends — after notify_all()
    // has returned. AudioStreamClient::threadFinished() has always done it this way; the two
    // siblings disagreed and only the client was right.
    std::lock_guard<std::mutex> lock(threadsMutex_);
    if (--activeThreads_ == 0) threadsCv_.notify_all();
}

// Each notify* snapshots the listeners then POSTS the fan-out to the dispatcher,
// so server-listener callbacks run on the single dispatch thread — never on a session /
// accept worker and never under a lock — and may safely re-enter the server (e.g. stop())
// without a self-join deadlock. Snapshot + args captured by value; listeners must outlive
// the server.
void AudioStreamServer::notifyServerStarted(int port) {
    std::vector<AudioStreamListener*> snapshot;
    {
        std::lock_guard<std::mutex> lock(listenersMutex_);
        snapshot = listeners_;
    }
    dispatcher_.post([snapshot, port] {
        for (auto* l : snapshot) l->onServerStarted(port);
    });
}

void AudioStreamServer::notifyServerStopped() {
    std::vector<AudioStreamListener*> snapshot;
    {
        std::lock_guard<std::mutex> lock(listenersMutex_);
        snapshot = listeners_;
    }
    dispatcher_.post([snapshot] {
        for (auto* l : snapshot) l->onServerStopped();
    });
}

void AudioStreamServer::notifyClientConnected(const std::string& id, const std::string& addr) {
    std::vector<AudioStreamListener*> snapshot;
    {
        std::lock_guard<std::mutex> lock(listenersMutex_);
        snapshot = listeners_;
    }
    dispatcher_.post([snapshot, id, addr] {
        for (auto* l : snapshot) l->onClientConnected(id, addr);
    });
}

void AudioStreamServer::notifyClientDisconnected(const std::string& id) {
    std::vector<AudioStreamListener*> snapshot;
    {
        std::lock_guard<std::mutex> lock(listenersMutex_);
        snapshot = listeners_;
    }
    dispatcher_.post([snapshot, id] {
        for (auto* l : snapshot) l->onClientDisconnected(id);
    });
}

void AudioStreamServer::notifyStreamStarted(const std::string& id) {
    std::vector<AudioStreamListener*> snapshot;
    {
        std::lock_guard<std::mutex> lock(listenersMutex_);
        snapshot = listeners_;
    }
    dispatcher_.post([snapshot, id] {
        for (auto* l : snapshot) l->onStreamStarted(id);
    });
}

void AudioStreamServer::notifyStreamStopped(const std::string& id) {
    std::vector<AudioStreamListener*> snapshot;
    {
        std::lock_guard<std::mutex> lock(listenersMutex_);
        snapshot = listeners_;
    }
    dispatcher_.post([snapshot, id] {
        for (auto* l : snapshot) l->onStreamStopped(id);
    });
}

void AudioStreamServer::notifyError(const std::string& id, const std::string& error) {
    std::vector<AudioStreamListener*> snapshot;
    {
        std::lock_guard<std::mutex> lock(listenersMutex_);
        snapshot = listeners_;
    }
    dispatcher_.post([snapshot, id, error] {
        for (auto* l : snapshot) l->onError(id, error);
    });
}

}  // namespace naudio::net
