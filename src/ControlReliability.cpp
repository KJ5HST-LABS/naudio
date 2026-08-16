// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — reliability algorithms.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/ControlReliability.hpp"

#include <chrono>
#include <stdexcept>

namespace naudio {

ControlReliability::ControlReliability(std::int32_t maxAttempts, std::int64_t timeoutMs)
    : maxAttempts_(maxAttempts), timeoutMs_(timeoutMs) {
    if (maxAttempts < 1) throw std::invalid_argument("maxAttempts must be >= 1");
    if (timeoutMs < 1) throw std::invalid_argument("timeoutMs must be >= 1");
}

void ControlReliability::recordSent(const AudioPacket& packet) {
    recordSentAt(packet, nowMillis());
}

void ControlReliability::recordSentAt(const AudioPacket& packet, std::int64_t sendTimeMs) {
    if (packet.packetType() != PacketType::Control) return;
    std::optional<ControlMessage> msg = ControlMessage::deserialize(packet.payload());
    if (!msg.has_value()) return;
    if (!isCriticalType(msg->messageType())) return;

    const std::int32_t seq = packet.sequence();

    // The duplicate scan comes FIRST, because a re-record of a sequence already pending
    // replaces in place and adds no entry — so it needs no room, and evicting to make room
    // for it costs an unrelated control its remaining retransmits for a put that never
    // happens. Order matters only at capacity, which is exactly when a wrong eviction is
    // unrecoverable.
    for (auto& entry : pending_) {
        if (entry.first == seq) {
            entry.second = PendingControl(packet, sendTimeMs);  // replace in place
            return;
        }
    }

    // A genuine insert: now the buffer really does need a free slot.
    if (pending_.size() >= BUFFER_SIZE) pending_.erase(pending_.begin());
    pending_.emplace_back(seq, PendingControl(packet, sendTimeMs));
}

bool ControlReliability::onAckReceived(std::int32_t ackedSeq) {
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->first == ackedSeq) {
            pending_.erase(it);
            return true;
        }
    }
    return false;
}

std::optional<AudioPacket> ControlReliability::onNackReceived(std::int32_t nackedSeq) {
    return onNackReceivedAt(nackedSeq, nowMillis());
}

std::optional<AudioPacket> ControlReliability::onNackReceivedAt(std::int32_t nackedSeq,
                                                                std::int64_t nowMs) {
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->first != nackedSeq) continue;
        PendingControl& pc = it->second;

        // A NACK-driven resend is an attempt like any other. It used to bump only the
        // counter, which left two holes: the class promises "up to maxAttempts" with no
        // NACK carve-out, and the un-restamped lastSendTime meant the timeout sweep could
        // resend the same packet again immediately after. naudio never sends a NACK — the
        // only ControlMessage::nack() call sites are tests and the conformance harness —
        // but this handles a RECEIVED one, so the budget was a peer's to spend, and an
        // unbounded resend per inbound NACK is an amplification a peer controls.
        if (pc.attempts >= maxAttempts_) {
            pending_.erase(it);  // exhausted — the same disposal checkRetransmitsAt gives
            return std::nullopt;
        }
        ++pc.attempts;
        pc.lastSendTime = nowMs;
        ++controlRetransmits_;
        return pc.packet;
    }
    return std::nullopt;
}

std::vector<AudioPacket> ControlReliability::checkRetransmits() {
    return checkRetransmitsAt(nowMillis());
}

std::vector<AudioPacket> ControlReliability::checkRetransmitsAt(std::int64_t nowMs) {
    std::vector<AudioPacket> retransmits;
    std::size_t i = 0;
    while (i < pending_.size()) {
        PendingControl& pc = pending_[i].second;
        if (nowMs - pc.lastSendTime >= timeoutMs_) {
            if (pc.attempts >= maxAttempts_) {
                pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));  // exhausted
            } else {
                ++pc.attempts;
                pc.lastSendTime = nowMs;
                retransmits.push_back(pc.packet);
                ++controlRetransmits_;
                ++i;
            }
        } else {
            ++i;
        }
    }
    return retransmits;
}

std::optional<ControlMessage> ControlReliability::generateAck(const AudioPacket& packet) const {
    if (packet.packetType() != PacketType::Control) return std::nullopt;
    std::optional<ControlMessage> msg = ControlMessage::deserialize(packet.payload());
    if (!msg.has_value()) return std::nullopt;
    if (!isCriticalType(msg->messageType())) return std::nullopt;
    return ControlMessage::controlAck(packet.sequence());
}

bool ControlReliability::isCriticalType(ControlType type) {
    switch (type) {
        // The client's half of the handshake (issue #29 option 4). Every OTHER message in the
        // connection-establishment exchange was already critical — ConnectAccept, ConnectReject,
        // AudioConfig — so a lost server reply recovered in one retransmit interval while a lost
        // CLIENT request cost the caller the whole na_client_connect timeout and a failed connect.
        // That asymmetry is the thing being removed; it is not a frame-format change.
        //
        // BEING CRITICAL IS NECESSARY BUT NOT SUFFICIENT, and the other half is easy to lose:
        // recordSent only makes an entry PENDING. Something must run the sweep, and the sweep's
        // usual pump (shouldSendHeartbeat) belongs to the heartbeat loop, which does not start
        // until after the handshake has already returned. AudioStreamClient::performHandshake
        // therefore waits in slices and calls ClientConnection::pumpControlRetransmits between
        // them. Delete that and this line silently does nothing — which is exactly how
        // ControlType::Disconnect came to be tracked-but-never-resent, the defect #29 reported.
        case ControlType::ConnectRequest:
        case ControlType::ConnectAccept:
        case ControlType::ConnectReject:
        case ControlType::AudioConfig:
        case ControlType::StreamStart:
        case ControlType::StreamStop:
        case ControlType::StreamPause:
        case ControlType::StreamResume:
        case ControlType::TxGranted:
        case ControlType::TxDenied:
        case ControlType::TxPreempted:
        case ControlType::TxReleased:
        case ControlType::ClientsUpdate:
        case ControlType::Disconnect:
            return true;
        default:
            return false;
    }
}

std::int64_t ControlReliability::controlRetransmits() const { return controlRetransmits_; }

std::size_t ControlReliability::pendingCount() const { return pending_.size(); }

void ControlReliability::reset() {
    pending_.clear();
    controlRetransmits_ = 0;
}

std::int64_t ControlReliability::nowMillis() {
    // steady_clock, matching FecDecoder / PacketReorderBuffer / JitterEstimator. These stamps are
    // only ever compared against each other (nowMs - lastSendTime), never reported or put on the
    // wire, so a monotonic source is strictly the right one and the wall clock was strictly wrong:
    // an NTP step BACKWARD of T suppressed every pending retransmit for T, and a step FORWARD
    // timed them all out at once, burning attempts toward the silent exhaustion-drop below.
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace naudio
