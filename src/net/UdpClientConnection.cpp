// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — UDP ClientConnection (impl).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
//
// The receive pipeline order is fixed
// (control reliability -> jitter -> reorder/FEC -> ordered queue) and identical
// across the two receive modes; the only difference is where the packet comes
// from (the demux thread via enqueueReceived, or the owned socket via
// receiveFromSocket). The pipe lock is held only around subsystem calls and is
// always released before any socket send.

#include "naudio/net/UdpClientConnection.hpp"

#include <algorithm>
#include <utility>

namespace naudio::net {

UdpClientConnection::UdpClientConnection(Socket* sharedSocket, std::string remoteHost,
                                         std::uint16_t remotePort, ClientAddress address,
                                         const UdpReliabilityConfig& cfg)
    : socket_(sharedSocket),
      ownsSocket_(false),
      remoteHost_(std::move(remoteHost)),
      remotePort_(remotePort),
      address_(std::move(address)) {
    lastSendTime_.store(nowMs());
    lastReceiveTime_.store(nowMs());
    initPipeline(cfg);
}

UdpClientConnection::UdpClientConnection(Socket ownedSocket, std::string remoteHost,
                                         std::uint16_t remotePort, ClientAddress address,
                                         const UdpReliabilityConfig& cfg)
    : ownedSocket_(std::move(ownedSocket)),
      ownsSocket_(true),
      remoteHost_(std::move(remoteHost)),
      remotePort_(remotePort),
      address_(std::move(address)) {
    socket_ = &(*ownedSocket_);
    lastSendTime_.store(nowMs());
    lastReceiveTime_.store(nowMs());
    initPipeline(cfg);
}

UdpClientConnection::~UdpClientConnection() { close(); }

void UdpClientConnection::initPipeline(const UdpReliabilityConfig& cfg) {
    // Resolve the server host to a numeric IPv4 once, for datagram-source
    // validation in the client-owned receive path. Runs in the ctor before
    // any receive thread exists, so no synchronization is needed.
    expectedSenderHost_ = Socket::resolveHostV4(remoteHost_);

    // FEC decoder emits into the ordered queue (null = unrecoverable gap, dropped
    // because the queue rejects null).
    if (cfg.fecEnabled) {
        fecEncoder_.emplace(static_cast<std::size_t>(cfg.fecBlockSize));
        fecDecoder_.emplace([this](const AudioPacket* p) {
            if (p) orderedQueue_.offer(*p);
        });
    }

    // Reorder buffer emits to the FEC decoder (if FEC is on) or directly to the
    // ordered queue. When chaining to FEC, a NULL gap is a no-op (the FEC decoder's
    // convenience overload returns early on null). The gap is recorded only when an
    // explicit sequence is known, which the reorder buffer does not carry.
    if (cfg.reorderWindowSize > 0) {
        if (fecDecoder_) {
            reorderBuffer_.emplace(
                static_cast<std::size_t>(cfg.reorderWindowSize), cfg.reorderMaxHoldMs,
                [this](const AudioPacket* p) {
                    if (p) fecDecoder_->processPacket(*p);
                });
        } else {
            reorderBuffer_.emplace(
                static_cast<std::size_t>(cfg.reorderWindowSize), cfg.reorderMaxHoldMs,
                [this](const AudioPacket* p) {
                    if (p) orderedQueue_.offer(*p);
                });
        }
    }

    if (cfg.adaptiveJitterEnabled && cfg.jitterMinMs > 0 && cfg.jitterMaxMs >= cfg.jitterMinMs) {
        jitterEstimator_.emplace(cfg.jitterMinMs, cfg.jitterMaxMs, cfg.jitterMultiplier);
    }

    if (cfg.controlReliabilityEnabled) {
        controlReliability_.emplace(std::max(1, cfg.controlRetransmitMaxAttempts),
                                    ControlReliability::DEFAULT_TIMEOUT_MS);
    }
}

// --- Send methods ---

bool UdpClientConnection::sendPacket(const AudioPacket& packet) {
    if (closed_.load()) return false;  // connection is closed
    std::vector<std::uint8_t> data = packet.serialize();
    // MTU advisory guard: a datagram larger than UDP_MAX_PAYLOAD IP-fragments on a
    // standard ~1500-byte-MTU path, and losing any one fragment drops the whole
    // datagram — which defeats the FEC parity layer. We do NOT app-layer fragment
    // (that would be a 0xAF01 wire change) and we NEVER drop valid audio; we only
    // count the event so a caller can see it is oversizing UDP frames and shrink
    // them below the path MTU. The datagram is still sent, unchanged.
    if (data.size() > AudioPacket::UDP_MAX_PAYLOAD) {
        oversizedDatagrams_.fetch_add(1);
    }
    // Send to the numeric IPv4 resolved ONCE in initPipeline (expectedSenderHost_),
    // not the raw remoteHost_. A name-based remoteHost_ would otherwise force a
    // blocking getaddrinfo() inside Socket::sendTo() on EVERY datagram (hot path);
    // the cached dotted-quad takes sendTo()'s inet_pton fast path instead. Fall back
    // to remoteHost_ when resolution failed (expectedSenderHost_ == "") so a resolver
    // hiccup never disables sending. Same numeric destination either way (wire-neutral).
    const std::string& dest = expectedSenderHost_.empty() ? remoteHost_ : expectedSenderHost_;
    if (!socket_->sendTo(data.data(), data.size(), dest, remotePort_)) return false;
    lastSendTime_.store(nowMs());
    packetsSent_.fetch_add(1);
    bytesSent_.fetch_add(static_cast<std::int64_t>(data.size()));
    return true;
}

bool UdpClientConnection::sendControl(const ControlMessage& message) {
    AudioPacket packet = AudioPacket::createControl(nextSequence(), message.serialize());
    bool ok = sendPacket(packet);
    // Record for reliability tracking (only critical types — recordSent filters).
    if (controlReliability_) {
        std::lock_guard<std::mutex> lock(pipe_);
        controlReliability_->recordSent(packet);
    }
    return ok;
}

bool UdpClientConnection::sendRxAudio(const std::uint8_t* data, std::size_t offset,
                                      std::size_t length) {
    std::vector<std::uint8_t> audioData(data + offset, data + offset + length);
    AudioPacket packet = AudioPacket::createRxAudio(nextSequence(), std::move(audioData));
    bool ok = sendPacket(packet);
    maybeSendFecParity(packet);
    return ok;
}

bool UdpClientConnection::sendTxAudio(const std::uint8_t* data, std::size_t length) {
    std::vector<std::uint8_t> audioData(data, data + length);
    AudioPacket packet = AudioPacket::createTxAudio(nextSequence(), std::move(audioData));
    bool ok = sendPacket(packet);
    maybeSendFecParity(packet);
    return ok;
}

void UdpClientConnection::maybeSendFecParity(const AudioPacket& audioPacket) {
    if (!fecEncoder_) return;
    std::optional<AudioPacket> parity;
    {
        std::lock_guard<std::mutex> lock(pipe_);
        parity = fecEncoder_->recordAndMaybeEmit(audioPacket);
    }
    if (parity) {
        parity->setSequence(nextSequence());  // parity uses a fresh sequence number
        sendPacket(*parity);                   // sent after the pipe lock is released
    }
}

bool UdpClientConnection::sendHeartbeat() {
    AudioPacket packet = AudioPacket::createHeartbeat(nextSequence());
    return sendPacket(packet);
}

// --- Receive methods ---

void UdpClientConnection::enqueueReceived(const AudioPacket& packet, int rawBytes) {
    lastReceiveTime_.store(nowMs());
    consecutiveCrcErrors_.store(0);
    packetsReceived_.fetch_add(1);
    bytesReceived_.fetch_add(rawBytes);

    // Control reliability (ACK/NACK) before normal processing — collect under the
    // lock, send after releasing it.
    if (controlReliability_ && packet.packetType() == PacketType::Control) {
        ControlOutcome outcome;
        {
            std::lock_guard<std::mutex> lock(pipe_);
            outcome = handleControlReliabilityLocked(packet);
        }
        for (const AudioPacket& out : outcome.outgoing) sendPacket(out);
        if (outcome.consumed) return;  // NACK/CONTROL_ACK — don't pass to application
    }

    std::lock_guard<std::mutex> lock(pipe_);
    if (jitterEstimator_ && (packet.packetType() == PacketType::AudioRx ||
                             packet.packetType() == PacketType::AudioTx)) {
        jitterEstimator_->recordArrival(packet.timestamp());
    }

    if (reorderBuffer_) {
        reorderBuffer_->insert(packet);
        reorderBuffer_->checkTimeout();
    } else if (fecDecoder_) {
        fecDecoder_->processPacket(packet);
        fecDecoder_->checkTimeout();
        trackSequence(packet.sequence());
    } else {
        if (packet.packetType() == PacketType::FecParity) return;  // FEC off — skip parity
        orderedQueue_.offer(packet);
        trackSequence(packet.sequence());
    }
}

void UdpClientConnection::recordCrcError() {
    crcErrors_.fetch_add(1);
    consecutiveCrcErrors_.fetch_add(1);
}

ReceiveResult UdpClientConnection::receivePacket(int timeoutMs) {
    if (closed_.load()) return ReceiveResult::dead();

    if (!ownsSocket_) {
        // Server-fed: packets come from the demux -> pipeline -> ordered queue.
        std::optional<AudioPacket> p = orderedQueue_.poll(timeoutMs);
        if (p) return ReceiveResult::of(std::move(*p));
        return ReceiveResult::noData();
    }

    // Client-owned: read directly from the socket.
    return receiveFromSocket(timeoutMs);
}

ReceiveResult UdpClientConnection::receiveFromSocket(int timeoutMs) {
    // Drain anything the pipeline already made ready.
    if (std::optional<AudioPacket> queued = orderedQueue_.poll()) {
        return ReceiveResult::of(std::move(*queued));
    }

    socket_->setRecvTimeout(timeoutMs > 0 ? timeoutMs : 0);
    std::vector<std::uint8_t> buf(MAX_DATAGRAM_SIZE);
    RecvFromResult rr = socket_->recvFrom(buf.data(), buf.size());

    if (rr.status == IoStatus::TimedOut) {
        // Check timeouts before returning no-data.
        {
            std::lock_guard<std::mutex> lock(pipe_);
            if (reorderBuffer_) reorderBuffer_->checkTimeout();
            if (fecDecoder_) fecDecoder_->checkTimeout();
        }
        if (std::optional<AudioPacket> q = orderedQueue_.poll()) {
            return ReceiveResult::of(std::move(*q));
        }
        return ReceiveResult::noData();
    }
    if (rr.status != IoStatus::Ok) return ReceiveResult::dead();  // Closed / Error

    // Drop datagrams from an unexpected source (spoofed / stray) or that were
    // truncated (oversized). Neither is a corrupt-but-from-the-peer frame, so
    // neither may count toward MAX_CONSECUTIVE_CRC_ERRORS — otherwise a flood of
    // spoofed packets could tear down a healthy connection. (C3 / C8.)
    if (rr.truncated || !isExpectedSender(rr.senderHost, rr.senderPort)) {
        return ReceiveResult::noData();
    }

    std::optional<AudioPacket> packet = AudioPacket::deserialize(buf.data(), rr.bytes);
    if (!packet) {
        crcErrors_.fetch_add(1);
        if (consecutiveCrcErrors_.fetch_add(1) + 1 >= MAX_CONSECUTIVE_CRC_ERRORS) {
            return ReceiveResult::dead();  // too corrupted — tear down
        }
        return ReceiveResult::noData();
    }

    consecutiveCrcErrors_.store(0);
    lastReceiveTime_.store(nowMs());
    packetsReceived_.fetch_add(1);
    bytesReceived_.fetch_add(static_cast<std::int64_t>(rr.bytes));

    if (controlReliability_ && packet->packetType() == PacketType::Control) {
        ControlOutcome outcome;
        {
            std::lock_guard<std::mutex> lock(pipe_);
            outcome = handleControlReliabilityLocked(*packet);
        }
        for (const AudioPacket& out : outcome.outgoing) sendPacket(out);
        if (outcome.consumed) {
            if (std::optional<AudioPacket> q = orderedQueue_.poll()) {
                return ReceiveResult::of(std::move(*q));
            }
            return ReceiveResult::noData();
        }
    }

    std::lock_guard<std::mutex> lock(pipe_);
    if (jitterEstimator_ && (packet->packetType() == PacketType::AudioRx ||
                             packet->packetType() == PacketType::AudioTx)) {
        jitterEstimator_->recordArrival(packet->timestamp());
    }

    if (reorderBuffer_) {
        reorderBuffer_->insert(*packet);
        reorderBuffer_->checkTimeout();
        if (std::optional<AudioPacket> q = orderedQueue_.poll()) {
            return ReceiveResult::of(std::move(*q));
        }
        return ReceiveResult::noData();
    } else if (fecDecoder_) {
        fecDecoder_->processPacket(*packet);
        fecDecoder_->checkTimeout();
        trackSequence(packet->sequence());
        if (std::optional<AudioPacket> q = orderedQueue_.poll()) {
            return ReceiveResult::of(std::move(*q));
        }
        return ReceiveResult::noData();
    } else {
        if (packet->packetType() == PacketType::FecParity) return ReceiveResult::noData();
        trackSequence(packet->sequence());
        return ReceiveResult::of(std::move(*packet));
    }
}

bool UdpClientConnection::isExpectedSender(const std::string& host, std::uint16_t port) const {
    if (port != remotePort_) return false;
    if (expectedSenderHost_.empty()) return true;  // unresolved — fail open
    return host == expectedSenderHost_;
}

// --- Sequence gap detection ---

void UdpClientConnection::trackSequence(std::int32_t sequenceNumber) {
    const std::int64_t seq =
        static_cast<std::int64_t>(static_cast<std::uint32_t>(sequenceNumber));
    const std::int64_t highest = highestSequenceSeen_.load();

    if (highest < 0) {
        highestSequenceSeen_.store(seq);  // first packet — baseline
        return;
    }
    if (seq > highest + 1) {
        packetsLost_.fetch_add(seq - highest - 1);  // gap — lost packets
        highestSequenceSeen_.store(seq);
    } else if (seq == highest + 1) {
        highestSequenceSeen_.store(seq);  // in-order
    } else {
        packetsOutOfOrder_.fetch_add(1);  // seq <= highest
    }
}

// --- Control reliability ---

UdpClientConnection::ControlOutcome
UdpClientConnection::handleControlReliabilityLocked(const AudioPacket& packet) {
    ControlOutcome outcome;
    std::optional<ControlMessage> msg = ControlMessage::deserialize(packet.payload());
    if (!msg) return outcome;  // not consumed, nothing to send

    if (msg->messageType() == ControlType::ControlAck) {
        const std::int32_t ackedSeq = msg->parseControlAckSequence();
        if (ackedSeq >= 0) controlReliability_->onAckReceived(ackedSeq);
        outcome.consumed = true;
        return outcome;
    }

    if (msg->messageType() == ControlType::Nack) {
        const std::int32_t nackedSeq = msg->parseNackSequence();
        if (nackedSeq >= 0) {
            std::optional<AudioPacket> retransmit = controlReliability_->onNackReceived(nackedSeq);
            if (retransmit) outcome.outgoing.push_back(std::move(*retransmit));
        }
        outcome.consumed = true;
        return outcome;
    }

    // Critical control type: generate an ACK to send back (after unlock).
    std::optional<ControlMessage> ack = controlReliability_->generateAck(packet);
    if (ack) {
        outcome.outgoing.push_back(
            AudioPacket::createControl(nextSequence(), ack->serialize()));
    }
    return outcome;  // not consumed — pass to application for normal processing
}

void UdpClientConnection::checkControlRetransmits() {
    if (!controlReliability_) return;
    std::vector<AudioPacket> retransmits;
    {
        std::lock_guard<std::mutex> lock(pipe_);
        retransmits = controlReliability_->checkRetransmits();
    }
    for (const AudioPacket& packet : retransmits) sendPacket(packet);
}

// --- Heartbeat and timeout ---

bool UdpClientConnection::shouldSendHeartbeat() {
    checkControlRetransmits();  // piggyback the retransmit sweep on the heartbeat check
    return nowMs() - lastSendTime_.load() > UDP_HEARTBEAT_INTERVAL_MS;
}

// --- Statistics that read subsystem internals (guarded by pipe_) ---

double UdpClientConnection::packetLossRate() const {
    const std::int64_t received = packetsReceived_.load();
    const std::int64_t lost = packetsLost_.load();
    const std::int64_t total = received + lost;
    return total > 0 ? static_cast<double>(lost) / static_cast<double>(total) : 0.0;
}

std::int64_t UdpClientConnection::packetsReordered() const {
    std::lock_guard<std::mutex> lock(pipe_);
    return reorderBuffer_ ? reorderBuffer_->packetsReordered() : 0;
}

std::int64_t UdpClientConnection::packetsRecoveredByFec() const {
    std::lock_guard<std::mutex> lock(pipe_);
    return fecDecoder_ ? fecDecoder_->packetsRecoveredByFec() : 0;
}

double UdpClientConnection::jitterMs() const {
    std::lock_guard<std::mutex> lock(pipe_);
    return jitterEstimator_ ? jitterEstimator_->jitterMs() : 0.0;
}

int UdpClientConnection::adaptiveBufferTargetMs() const {
    std::lock_guard<std::mutex> lock(pipe_);
    return jitterEstimator_ ? jitterEstimator_->adaptiveBufferTargetMs() : -1;
}

std::int64_t UdpClientConnection::controlRetransmits() const {
    std::lock_guard<std::mutex> lock(pipe_);
    return controlReliability_ ? controlReliability_->controlRetransmits() : 0;
}

void UdpClientConnection::close() {
    if (closed_.exchange(true)) return;
    orderedQueue_.shutdown();          // wake any blocked receivePacket
    if (ownsSocket_ && ownedSocket_) ownedSocket_->close();  // shared socket left to the server
}

}  // namespace naudio::net
