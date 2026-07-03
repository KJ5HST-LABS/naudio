// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — reliability algorithms.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/PacketReorderBuffer.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace naudio {

PacketReorderBuffer::PacketReorderBuffer(std::size_t windowSize, std::int64_t maxHoldMs,
                                         Emitter emitter)
    : windowSize_(windowSize), maxHoldMs_(maxHoldMs), emitter_(std::move(emitter)) {
    if (windowSize == 0) throw std::invalid_argument("windowSize must be >= 1");
    if (maxHoldMs < 0) throw std::invalid_argument("maxHoldMs must be >= 0");
}

void PacketReorderBuffer::insert(AudioPacket packet) {
    insertAt(std::move(packet), nowMillis());
}

void PacketReorderBuffer::insertAt(AudioPacket packet, std::int64_t nowMs) {
    const std::int64_t seq =
        static_cast<std::int64_t>(static_cast<std::uint32_t>(packet.sequence()));

    if (nextExpected_ < 0) nextExpected_ = seq;  // First packet — baseline.

    if (seq < nextExpected_) {
        ++packetsDroppedLate_;  // Late or duplicate — discard.
        return;
    }

    if (seq == nextExpected_) {
        // In-order — emit immediately, then drain consecutive buffered packets.
        emit(&packet);
        nextExpected_ = seq + 1;
        drainConsecutive();
    } else {
        // Ahead of expected — buffer it.
        buffer_.insert_or_assign(seq, std::move(packet));
        arrivalTimes_[seq] = nowMs;
        if (buffer_.size() >= windowSize_) forceFlush();
    }
}

void PacketReorderBuffer::checkTimeout() { checkTimeoutAt(nowMillis()); }

void PacketReorderBuffer::checkTimeoutAt(std::int64_t nowMs) {
    if (buffer_.empty()) return;
    if (!arrivalTimes_.empty()) {
        // The arrival time of the smallest buffered sequence (matches the
        // ordered-map first-element (begin()) semantics — by key, not by time).
        const std::int64_t oldestArrival = arrivalTimes_.begin()->second;
        if (nowMs - oldestArrival >= maxHoldMs_) forceFlush();
    }
}

std::int64_t PacketReorderBuffer::packetsReordered() const { return packetsReordered_; }

std::int64_t PacketReorderBuffer::packetsDroppedLate() const { return packetsDroppedLate_; }

std::int64_t PacketReorderBuffer::gapsEmitted() const { return gapsEmitted_; }

std::size_t PacketReorderBuffer::bufferedCount() const { return buffer_.size(); }

std::int64_t PacketReorderBuffer::nextExpected() const { return nextExpected_; }

void PacketReorderBuffer::reset() {
    buffer_.clear();
    arrivalTimes_.clear();
    nextExpected_ = -1;
    packetsReordered_ = 0;
    packetsDroppedLate_ = 0;
    gapsEmitted_ = 0;
}

void PacketReorderBuffer::emit(const AudioPacket* p) {
    if (emitter_) emitter_(p);
}

void PacketReorderBuffer::drainConsecutive() {
    while (!buffer_.empty()) {
        auto it = buffer_.find(nextExpected_);
        if (it == buffer_.end()) break;
        AudioPacket next = std::move(it->second);
        buffer_.erase(it);
        arrivalTimes_.erase(nextExpected_);
        ++packetsReordered_;
        emit(&next);
        ++nextExpected_;
    }
}

void PacketReorderBuffer::forceFlush() {
    if (buffer_.empty()) return;
    const std::int64_t highestBuffered = buffer_.rbegin()->first;
    const std::int64_t start = nextExpected_;

    // R1 — bounded gap fill. The buffer holds at most windowSize_ packets, but the
    // SPAN [start, highestBuffered] is corruption-/peer-controlled: a single packet
    // with an out-of-range sequence (or R3's no-wrap horizon) makes that span up to
    // ~2³², and emitting one silence gap per missing sequence would spin the thread
    // for billions of iterations (a DoS hang). So walk the BUFFERED packets in order
    // (the sorted map — never the raw integer span) and cap the total silence-gap run
    // to windowSize_: beyond the cap we treat the hole as a stream discontinuity and
    // jump forward without inventing more silence. Within the cap, every conformance
    // vector and every realistic loss burst is far below windowSize_ gaps; the cap only
    // bounds the pathological out-of-range case (otherwise unbounded). See
    // docs/protocols.md (R1, R3).
    std::int64_t gapBudget = static_cast<std::int64_t>(windowSize_);
    std::int64_t cursor = start;
    for (auto it = buffer_.begin(); it != buffer_.end();) {
        const std::int64_t seq = it->first;
        std::int64_t holes = seq - cursor;       // missing slots before this packet
        if (holes > gapBudget) holes = gapBudget;  // R1 cap on the silence-gap run
        for (std::int64_t g = 0; g < holes; ++g) {
            ++gapsEmitted_;
            emit(nullptr);
        }
        gapBudget -= holes;
        AudioPacket p = std::move(it->second);
        if (seq != start) ++packetsReordered_;
        emit(&p);
        cursor = seq + 1;
        it = buffer_.erase(it);
    }
    arrivalTimes_.clear();  // every buffered packet's arrival time is now stale
    nextExpected_ = highestBuffered + 1;
}

std::int64_t PacketReorderBuffer::nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace naudio
