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

    // Evict oldest if at capacity (unconditional before the put, even when seq is a
    // duplicate update).
    if (pending_.size() >= BUFFER_SIZE) pending_.erase(pending_.begin());

    for (auto& entry : pending_) {
        if (entry.first == seq) {
            entry.second = PendingControl(packet, sendTimeMs);  // replace in place
            return;
        }
    }
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
    for (auto& entry : pending_) {
        if (entry.first == nackedSeq) {
            ++controlRetransmits_;
            return entry.second.packet;
        }
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
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace naudio
