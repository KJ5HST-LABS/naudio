// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — reliability algorithms.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>

#include "naudio/AudioPacket.hpp"

namespace naudio {

// Reorder buffer for out-of-order UDP packets.
//
// Holds packets that arrive ahead of their expected sequence position and emits
// them in order once the gap is filled or a timeout/window limit is reached.
// In-order packets pass through with zero added latency. When a gap cannot be
// filled (timeout or window full), the buffer force-flushes and emits a silence
// gap (a NULL packet) for each missing sequence number.
//
// The emitter is a std::function<void(const AudioPacket*)> where a NULL pointer
// is the gap. The pointer is borrowed — valid only
// for the duration of the call; a consumer that needs to retain the packet
// copies it. Sequence numbers are unsigned (int32 reinterpreted as uint32).
// insertAt()/checkTimeoutAt()
// (explicit timestamp) are the primary, testable forms; the no-arg wrappers read
// the monotonic clock. Pure (no threads): the caller serializes access.
//
// Compiled into naudio_core (definitions in PacketReorderBuffer.cpp).
class PacketReorderBuffer {
public:
    using Emitter = std::function<void(const AudioPacket*)>;

    // Creates a reorder buffer. windowSize is the max packets to buffer before
    // force-flushing (>= 1); maxHoldMs is the max time to hold a packet before
    // force-flushing (>= 0).
    PacketReorderBuffer(std::size_t windowSize, std::int64_t maxHoldMs, Emitter emitter);

    // Inserts a packet, timestamping its arrival with the monotonic clock.
    void insert(AudioPacket packet);

    // Inserts a packet with an explicit arrival timestamp (the testable form).
    void insertAt(AudioPacket packet, std::int64_t nowMs);

    // Checks for timed-out packets and force-flushes if needed, using the
    // monotonic clock.
    void checkTimeout();

    // Checks for timed-out packets with an explicit "now" (the testable form).
    void checkTimeoutAt(std::int64_t nowMs);

    // The number of out-of-order packets successfully reordered.
    std::int64_t packetsReordered() const;
    // The number of packets discarded for arriving after their slot was emitted.
    std::int64_t packetsDroppedLate() const;
    // The number of gap slots emitted as NULL (truly lost packets).
    std::int64_t gapsEmitted() const;
    // The current number of packets held in the buffer.
    std::size_t bufferedCount() const;
    // The next expected sequence number, or -1 if not yet established.
    std::int64_t nextExpected() const;

    // Resets the buffer state. Discards any buffered packets.
    void reset();

private:
    void emit(const AudioPacket* p);

    // Drains consecutive packets starting from nextExpected_.
    void drainConsecutive();

    // Force-flushes: emits a NULL gap for each missing slot, then buffered
    // packets in order, from nextExpected_ through the highest buffered sequence.
    void forceFlush();

    static std::int64_t nowMillis();

    std::size_t windowSize_;
    std::int64_t maxHoldMs_;
    Emitter emitter_;
    std::map<std::int64_t, AudioPacket> buffer_;         // keyed by unsigned seq
    std::map<std::int64_t, std::int64_t> arrivalTimes_;  // unsigned seq -> arrival ms
    std::int64_t nextExpected_ = -1;
    std::int64_t packetsReordered_ = 0;
    std::int64_t packetsDroppedLate_ = 0;
    std::int64_t gapsEmitted_ = 0;
};

}  // namespace naudio
