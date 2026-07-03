// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — reliability algorithms.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>

#include "naudio/AudioPacket.hpp"

namespace naudio {

// XOR-based forward error correction decoder.
//
// Collects audio packets into blocks by sequence range. When a parity packet
// arrives: if exactly one audio packet is missing it is recovered via XOR; if
// zero are missing the parity is discarded; if two or more are missing recovery
// is impossible and a NULL (silence) is emitted for each gap. Blocks that don't
// receive a parity within the timeout are flushed as-is.
//
// Recovery is byte-exact ONLY for uniform-length blocks (all packets in a block
// the same size), which holds for the fixed-size PCM audio frames this decoder
// is built for. The frozen 5-byte parity header carries no per-slot length, so a
// recovered packet is always block-max (`xorLen`) bytes; a missing packet that
// was shorter than the block max is reconstructed zero-extended to that length.
// See handleParity() in FecDecoder.cpp for the full rationale.
//
// The emitter is a std::function<void(const AudioPacket*)> with NULL = a silence
// gap. Input packets are taken by value (std::optional, owned); the emitter
// borrows. The decoder emits AFTER it finishes mutating internal state in each
// branch (no emit while iterating active blocks). The block clock is
// injectable (setClock) and checkTimeoutAt(now) is the testable form.
//
// Compiled into naudio_core (definitions in FecDecoder.cpp).
class FecDecoder {
public:
    using Emitter = std::function<void(const AudioPacket*)>;
    using Clock = std::function<std::int64_t()>;

    // Default block timeout in milliseconds.
    static constexpr std::int64_t DEFAULT_BLOCK_TIMEOUT_MS = 60;

    explicit FecDecoder(Emitter emitter, std::int64_t blockTimeoutMs = DEFAULT_BLOCK_TIMEOUT_MS);

    // Overrides the monotonic-millis clock used for block creation timestamps
    // (testability — §3.3). Production uses the default steady-clock source.
    void setClock(Clock clock);

    // Processes a packet (or a silence gap) from the reorder buffer. Audio
    // packets are emitted immediately AND stored for potential FEC recovery;
    // FEC_PARITY packets trigger a recovery attempt; nullopt records a missing
    // slot. `sequence` is required when `packet` is nullopt.
    void process(std::optional<AudioPacket> packet, std::int32_t sequence);

    // Convenience for a known non-null packet.
    void processPacket(AudioPacket packet);

    // Flushes timed-out blocks, emitting a NULL silence for each missing slot,
    // using the injected clock.
    void checkTimeout();

    // Flushes timed-out blocks with an explicit "now" (the testable form). The
    // pending (unassigned) block uses 2x the timeout. Collect-then-emit: blocks
    // are removed first, NULLs emitted afterward (no emit while iterating).
    void checkTimeoutAt(std::int64_t nowMs);

    std::int64_t packetsRecoveredByFec() const;
    std::int64_t fecBlocksComplete() const;
    std::int64_t fecBlocksFailed() const;

    void reset();

private:
    // Sentinel key for the "pending" block of unassigned packets (no parity seen
    // yet).
    static constexpr std::int32_t PENDING_KEY = std::numeric_limits<std::int32_t>::min();

    struct FecBlock {
        std::int32_t startSeq;
        std::int32_t blockSize;
        std::int64_t createdAtMs;
        std::map<std::int32_t, AudioPacket> packets;
        std::set<std::int32_t> missingSequences;

        FecBlock(std::int32_t s, std::int32_t bs, std::int64_t created)
            : startSeq(s), blockSize(bs), createdAtMs(created) {}

        // Whether `sequence` falls in [startSeq, startSeq + blockSize).
        bool covers(std::int32_t sequence) const {
            return sequence >= startSeq &&
                   static_cast<std::int64_t>(sequence) <
                       static_cast<std::int64_t>(startSeq) + blockSize;
        }
    };

    void emit(const AudioPacket* p);

    // The key of an existing block whose sequence range covers `sequence`.
    std::optional<std::int32_t> matchingBlockKey(std::int32_t sequence) const;

    // Gets the block at `key`, creating a fresh pending-shaped block (timestamped
    // from the injected clock) if absent.
    FecBlock& getOrCreateBlock(std::int32_t key);

    void storeInBlock(std::int32_t sequence, AudioPacket packet);

    void recordMissing(std::int32_t sequence);

    // Handles a parity packet: attempts to recover a single missing audio packet.
    void handleParity(const AudioPacket& parityPacket);

    static std::int64_t defaultNowMs();

    Emitter emitter_;
    std::int64_t blockTimeoutMs_;
    Clock clock_ = &FecDecoder::defaultNowMs;
    std::map<std::int32_t, FecBlock> activeBlocks_;
    std::int32_t nextEmitSeq_ = -1;  // set-only ordering hint
    std::int64_t packetsRecoveredByFec_ = 0;
    std::int64_t fecBlocksComplete_ = 0;
    std::int64_t fecBlocksFailed_ = 0;
};

}  // namespace naudio
