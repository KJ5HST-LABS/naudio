// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — reliability algorithms.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "naudio/AudioPacket.hpp"
#include "naudio/ControlMessage.hpp"

namespace naudio {

// ACK-based reliability layer for critical UDP control messages.
//
// Control packets (TX_GRANTED, CONNECT_ACCEPT, ...) are critical for correct
// protocol operation but can be lost over UDP. Sender side: the last BUFFER_SIZE
// sent control packets are kept in an insertion-ordered ring keyed by global
// sequence; if no CONTROL_ACK arrives within the timeout, the packet is
// retransmitted (up to maxAttempts). Receiver side: a critical control packet
// yields a CONTROL_ACK carrying its global sequence number.
//
// An insertion-ordered vector with oldest-first eviction + an ordered timeout
// scan (BUFFER_SIZE = 16, so the linear lookups are trivial). recordSentAt
// / checkRetransmitsAt (explicit "now" ms) are the primary, testable forms; the
// no-arg wrappers read the wall clock. Pure (no threads): caller serializes.
//
// Compiled into naudio_core (definitions in ControlReliability.cpp).
class ControlReliability {
public:
    // Maximum number of sent control packets to buffer for retransmission.
    static constexpr std::size_t BUFFER_SIZE = 16;
    // Default retransmission timeout in milliseconds.
    static constexpr std::int64_t DEFAULT_TIMEOUT_MS = 500;
    // Default maximum retransmission attempts per packet.
    static constexpr std::int32_t DEFAULT_MAX_ATTEMPTS = 3;

    // Creates a reliability tracker. maxAttempts must be >= 1 (1 = no
    // retransmit); timeoutMs must be >= 1.
    explicit ControlReliability(std::int32_t maxAttempts = DEFAULT_MAX_ATTEMPTS,
                                std::int64_t timeoutMs = DEFAULT_TIMEOUT_MS);

    // --- Sender side ---

    // Records a sent control packet, using the wall clock as the send time.
    void recordSent(const AudioPacket& packet);

    // Records a sent control packet with an explicit send time (the testable
    // form). Only critical control types are tracked.
    void recordSentAt(const AudioPacket& packet, std::int64_t sendTimeMs);

    // Processes an ACK; true if it matched a pending packet.
    bool onAckReceived(std::int32_t ackedSeq);

    // Processes a NACK; returns the stored packet to retransmit (copy), or
    // nullopt if evicted/not found.
    std::optional<AudioPacket> onNackReceived(std::int32_t nackedSeq);

    // Returns packets needing retransmission, using the wall clock.
    std::vector<AudioPacket> checkRetransmits();

    // Returns packets needing retransmission with an explicit "now" (the testable
    // form). Packets past maxAttempts are removed.
    std::vector<AudioPacket> checkRetransmitsAt(std::int64_t nowMs);

    // --- Receiver side ---

    // Returns a CONTROL_ACK message if the packet carries a critical control
    // type, nullopt otherwise.
    std::optional<ControlMessage> generateAck(const AudioPacket& packet) const;

    // --- Utilities ---

    // Whether a control message type is critical and requires reliability.
    static bool isCriticalType(ControlType type);

    std::int64_t controlRetransmits() const;
    std::size_t pendingCount() const;

    void reset();

private:
    struct PendingControl {
        AudioPacket packet;
        std::int64_t lastSendTime;
        std::int32_t attempts;  // initial send counts as attempt 1
        PendingControl(const AudioPacket& p, std::int64_t sendTime)
            : packet(p), lastSendTime(sendTime), attempts(1) {}
    };

    static std::int64_t nowMillis();

    std::int32_t maxAttempts_;
    std::int64_t timeoutMs_;
    std::vector<std::pair<std::int32_t, PendingControl>> pending_;  // insertion-ordered
    std::int64_t controlRetransmits_ = 0;
};

}  // namespace naudio
