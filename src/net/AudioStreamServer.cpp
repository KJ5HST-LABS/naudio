// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — AudioStreamServer implementation.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/net/AudioStreamServer.hpp"

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
    // Pending RX-audio bytes in outQueue_ (control messages count 0 — see enqueueControl).
    // Guarded by outMutex_.
    std::size_t outQueueBytes_ = 0;
    // The backlog cap: CONNECTION_TIMEOUT_MS worth of audio at this stream's bit rate
    // (1,920,000 B on the default preset). Issue #56.
    //
    // WHY THIS QUANTITY. It makes one deadline govern both directions of "this peer has
    // stopped making progress". The server already declares a peer dead after
    // CONNECTION_TIMEOUT_MS of not SENDING (AudioProtocolHandler::isConnectionTimedOut);
    // this applies the same window to a peer that will not DRAIN. The coupling is
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
        // already uses: an empty queue always accepts one chunk however large the host made
        // it (injectAudio forwards whatever it is given), so overshoot is bounded by exactly
        // one chunk and a healthy client can never be refused.
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
    if (!performHandshake()) {
        close();
        return;
    }

    // Send config + accept directly (awaited before any RX audio is registered).
    if (!connection_->sendControl(ControlMessage::audioConfig(sessionConfig_)) ||
        !connection_->sendControl(ControlMessage::connectAccept())) {
        close();
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
        if (connection_->shouldSendHeartbeat()) {
            connection_->sendHeartbeat();
        }
        if (connection_->isConnectionTimedOut()) {
            server_->notifyError(clientId_, "Connection timeout");
            break;
        }
        std::unique_lock<std::mutex> lock(runStopMutex_);
        runStopCv_.wait_for(lock, std::chrono::milliseconds(1000),
                            [this]() { return closed_.load() || !server_->running_.load(); });
    }

    // Cleanup.
    streaming_.store(false);
    server_->notifyStreamStopped(clientId_);
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
            connection_->sendControl(ControlMessage::txDenied(mixer->currentTxOwner()));
        }
    }
}

void AudioStreamServer::ClientSession::handleControlMessage(const AudioPacket& packet) {
    auto msg = ControlMessage::deserialize(packet.payload());
    if (!msg) return;

    switch (msg->messageType()) {
        case ControlType::LatencyProbe:
            connection_->sendControl(ControlMessage::latencyResponse(msg->parseLatencyTimestamp()));
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

void AudioStreamServer::stop() {
    if (!running_.exchange(false)) return;

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

    stopSharedAudio();

    // Close the transport to unblock the accept thread, then join it.
    std::shared_ptr<ServerTransport> transport;
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        transport = transport_;
    }
    if (transport) transport->close();
    if (acceptThread_.joinable()) acceptThread_.join();
    {
        std::lock_guard<std::mutex> lock(runMutex_);
        transport_.reset();
    }

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
        connection->close();
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
        sessions_[clientId] = session;
    }
    notifyClientConnected(clientId, address);
    session->startRunThread();
}

void AudioStreamServer::rejectClient(const std::shared_ptr<ClientConnection>& connection,
                                     RejectReason reason, const std::string& message) {
    connection->sendControl(ControlMessage::connectReject(reason, message));
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
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        --activeThreads_;
    }
    threadsCv_.notify_all();
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
