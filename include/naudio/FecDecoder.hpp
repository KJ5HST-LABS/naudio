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
#include "naudio/FecEncoder.hpp"

namespace naudio {

// XOR-based forward error correction decoder.
//
// Collects audio packets into blocks by sequence range. When a parity packet
// arrives: if exactly one audio packet is missing it is recovered via XOR; if
// zero are missing the parity is discarded; if two or more are missing recovery
// is impossible and a NULL (silence) is emitted for each gap. Blocks that don't
// receive a parity within the timeout are flushed as-is.
//
// A fourth outcome guards the three above: the parity's sequence range is only
// the encoder's block while the block's audio packets are contiguous, and the
// connection's sequence counter is shared with control/heartbeat traffic. A
// non-audio packet inside the range therefore proves the range is not the block,
// and recovery is DECLINED (counted by fecBlocksUnreconciled) rather than run
// over the wrong member set — which would emit a whole frame of wrong samples as
// if it were recovered audio. See handleParity() and issue #23.
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

    // --- Pending-block retention policy (issue #47) ---
    //
    // The pending block is a REPAIR CACHE, not a delivery queue. process() emits
    // each audio packet BEFORE storing a copy of it, and the stored copies are read
    // only by handleParity's missing-count and XOR. So discarding the pending block
    // costs a repair opportunity and never costs audio or adds latency — which is
    // why the time bound below is generous and memory is bounded by a packet count
    // instead.
    //
    // The bound is IDLE, not lifetime: it is measured from the last insert into the
    // block (touchedAtMs), so it expresses a maximum in-block ARRIVAL GAP and is
    // independent of how many packets the block holds. A lifetime bound tightens as
    // blocks lengthen — at the largest legal shape (MAX_BLOCK_SIZE 10 x 20 ms
    // frames = a 200 ms block period) it is already shorter than one block period,
    // so it would discard a block whose own packets are still arriving normally.
    //
    // MIN is the historical bound (DEFAULT_BLOCK_TIMEOUT_MS * 2 = 120), which makes
    // any derived value MONOTONE-WIDENING: no configuration can regress.
    static constexpr std::int64_t MIN_PENDING_IDLE_MS = DEFAULT_BLOCK_TIMEOUT_MS * 2;
    // MAX is AudioStreamConfig::MAX_INITIAL_BUFFERING_MS. A repair that arrives
    // later than the project's declared maximum receiver buffering cannot be
    // played, so holding the block past it buys nothing. Not referenced by symbol:
    // this header is a leaf of the reliability layer and does not depend on the
    // stream-config layer. The static_assert lives at the connection, which sees
    // both (src/net/UdpClientConnection.cpp).
    static constexpr std::int64_t MAX_PENDING_IDLE_MS = 500;
    // Hard ceiling on packets retained for repair, enforced on insert rather than
    // on a tick — checkTimeout() is never called on a server-fed connection
    // (UdpClientConnection.cpp: the reorder-engaged branch ticks only the reorder
    // buffer), so a time bound alone would leave that path unbounded. Sized as four
    // maximum blocks: the block in flight, the leftovers the last parity re-filed,
    // the reorder window's front-run, and one block of slack. The 10 is derived
    // from the encoder's validated range; the 4 is a stated headroom choice.
    static constexpr std::size_t MAX_PENDING_PACKETS = 4 * FecEncoder::MAX_BLOCK_SIZE;

    explicit FecDecoder(Emitter emitter, std::int64_t blockTimeoutMs = DEFAULT_BLOCK_TIMEOUT_MS);

    // Sets the pending block's idle timeout, clamped to
    // [MIN_PENDING_IDLE_MS, MAX_PENDING_IDLE_MS]. The connection derives the value
    // from the negotiated stream shape; see UdpClientConnection::initPipeline.
    // The CONSTRUCTOR's blockTimeoutMs is deliberately left unclamped (the pending
    // bound starts at blockTimeoutMs * 2) so tests can drive short windows; every
    // production caller goes through this clamped setter.
    void setPendingIdleTimeoutMs(std::int64_t idleMs);

    // The pending block's current idle timeout, in milliseconds.
    std::int64_t pendingIdleTimeoutMs() const;

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

    // Blocks whose sequence range could not be reconciled with the parity's audio
    // count, and so were declined rather than recovered — see handleParity(). A
    // non-zero value means control/heartbeat traffic is interleaving with audio
    // inside FEC blocks, which costs those blocks their recovery; it is a loss of
    // opportunity, never a loss of correctness.
    std::int64_t fecBlocksUnreconciled() const;

    // Audio packets dropped from the pending block without ever being offered to a
    // parity — - by the idle timeout, or by the MAX_PENDING_PACKETS cap. This is the
    // ONLY observable for issue #47's failure: when the pending block is discarded,
    // fecBlocksFailed does not move (its increment is guarded on a non-empty
    // missingSequences, and the client's reorder->FEC chain drops gap markers, so
    // that set is always empty in production) and nothing is emitted. A non-zero
    // value means repair opportunities were lost to arrival stalls, never that
    // audio was lost — the packets themselves were emitted on arrival.
    std::int64_t pendingPacketsDiscarded() const;

    void reset();

private:
    // Sentinel key for the "pending" block of unassigned packets (no parity seen
    // yet).
    static constexpr std::int32_t PENDING_KEY = std::numeric_limits<std::int32_t>::min();

    struct FecBlock {
        std::int32_t startSeq;
        std::int32_t blockSize;
        // Last insert into this block, NOT its creation time — the pending bound is
        // an idle bound (see MIN_PENDING_IDLE_MS). getOrCreateBlock refreshes it on
        // every access to the pending block, which is every insert path.
        std::int64_t touchedAtMs;
        std::map<std::int32_t, AudioPacket> packets;
        std::set<std::int32_t> missingSequences;

        FecBlock(std::int32_t s, std::int32_t bs, std::int64_t touched)
            : startSeq(s), blockSize(bs), touchedAtMs(touched) {}

        // Whether `sequence` falls in [startSeq, startSeq + blockSize).
        bool covers(std::int32_t sequence) const {
            return sequence >= startSeq &&
                   static_cast<std::int64_t>(sequence) <
                       static_cast<std::int64_t>(startSeq) + blockSize;
        }
    };

    void emit(const AudioPacket* p);

    // Whether a packet type carries audio, i.e. whether the encoder would have
    // recorded it into a FEC block. Only these participate in a block.
    static bool isAudio(PacketType type);

    // The key of an existing block whose sequence range covers `sequence`.
    std::optional<std::int32_t> matchingBlockKey(std::int32_t sequence) const;

    // Gets the block at `key`, creating a fresh pending-shaped block if absent.
    // For the pending block this also REFRESHES touchedAtMs from the injected
    // clock, which is what makes the pending bound an idle bound: every insert
    // path (storeInBlock, recordMissing, and handleParity's re-file of
    // out-of-range leftovers) reaches the block through here.
    FecBlock& getOrCreateBlock(std::int32_t key);

    // Enforces MAX_PENDING_PACKETS on the pending block by evicting lowest-sequence
    // entries. Counts into pendingPacketsDiscarded_ and EMITS NOTHING — the class
    // invariant is that emission happens only after internal state is settled, and
    // this runs mid-insert, including from inside handleParity's re-file loop.
    void capPending(FecBlock& block);

    void storeInBlock(std::int32_t sequence, AudioPacket packet);

    void recordMissing(std::int32_t sequence);

    // Handles a parity packet: attempts to recover a single missing audio packet.
    void handleParity(const AudioPacket& parityPacket);

    static std::int64_t defaultNowMs();

    Emitter emitter_;
    std::int64_t blockTimeoutMs_;
    std::int64_t pendingIdleMs_;
    Clock clock_ = &FecDecoder::defaultNowMs;
    std::map<std::int32_t, FecBlock> activeBlocks_;
    std::int32_t nextEmitSeq_ = -1;  // set-only ordering hint
    std::int64_t packetsRecoveredByFec_ = 0;
    std::int64_t fecBlocksComplete_ = 0;
    std::int64_t fecBlocksFailed_ = 0;
    std::int64_t fecBlocksUnreconciled_ = 0;
    std::int64_t pendingPacketsDiscarded_ = 0;
};

}  // namespace naudio
