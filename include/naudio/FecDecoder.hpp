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
// A fourth outcome guards the three above: recovery is DECLINED (counted by
// fecBlocksUnreconciled) rather than run, whenever the parity's sequence range
// cannot be shown to BE the encoder's block. The range is only the block while the
// block's audio packets are contiguous, and the connection's sequence counter is
// shared with control/heartbeat traffic — so a stranger taking a sequence inside
// the range displaces a member out of it, and running the XOR over the wrong member
// set emits either a frame of wrong samples (issue #23) or a byte-exact duplicate of
// audio already delivered (issues #52, #55). The stranger need not arrive to do
// this: it is invisible to the decoder when control reliability consumes it, which
// is what made #55 reachable at ZERO packet loss.
// The decline triggers are enumerated on fecBlocksUnreconciled() below, which owns
// that contract. See handleParity() for the mechanism.
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
    // Hard ceiling on packets retained for repair, enforced on INSERT rather than on a
    // tick. The two bounds are not redundant and neither subsumes the other: the idle
    // timeout bounds STALENESS and cannot bound volume, because a peer that keeps
    // sending refreshes touchedAtMs on every insert and so holds the bound open
    // indefinitely; this cap bounds VOLUME and cannot bound staleness, because it only
    // moves when something arrives. Enforcing on insert is also what makes the cap hold
    // when nothing ticks the decoder at all — an application that stops calling
    // receivePacket() gets no tick in either connection mode (see checkTimeout()).
    // Sized as four maximum blocks: the block in flight, the leftovers the last parity
    // re-filed, the reorder window's front-run, and one block of slack. The 10 is
    // derived from the encoder's validated range; the 4 is a stated headroom choice.
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
    //
    // WHO DRIVES THIS (issue #52). The decoder has no thread and no timer, so the idle
    // bound above exists only to the extent that something calls this. That driver is
    // the CONSUMER, in both connection modes, and it is deliberately not the arrival
    // path: an arrival-driven tick cannot fire during a PAUSE in traffic, which is
    // precisely when a block goes stale. Nor is a pause the only case — datagrams can
    // keep arriving while NOTHING reaches this decoder, because a consumed control ACK,
    // a CRC failure, and a reorder buffer dropping late sequences all return before the
    // decoder is reached (the last is unbounded: PacketReorderBuffer drops a sequence
    // below nextExpected_ with no emission and no timer). A consumer-driven tick covers
    // those too, because in every one of them the ordered queue is empty as well, which
    // is the condition the tick hangs off. UdpClientConnection therefore ticks from
    // whichever limb observes "nothing was ready within the deadline" —
    // receiveFromSocket's IoStatus::TimedOut limb when the connection owns its socket,
    // and receivePacket's server-fed limb when the server demux feeds it.
    //
    // The two modes are held deliberately SYMMETRIC. Before #52 the server-fed limb had
    // no tick, so the same class with the same config behaved differently depending on
    // which mode constructed it, and the difference was measurable: after 3x udpWan's
    // derived 490 ms bound with a live consumer, a client-owned connection released all
    // 12 held packets and a server-fed one released 0. If a third receive mode is ever
    // added, it owes a tick on its own no-data path — the reachability of this method is
    // a property of the CALLER, and nothing in this class can assert it.
    //
    // A consumer that stops polling gets no tick and is bounded by MAX_PENDING_PACKETS
    // alone; that is the cap's job, not a gap in this one.
    void checkTimeout();

    // Flushes timed-out blocks with an explicit "now" (the testable form). The
    // pending (unassigned) block uses 2x the timeout. Collect-then-emit: blocks
    // are removed first, NULLs emitted afterward (no emit while iterating).
    void checkTimeoutAt(std::int64_t nowMs);

    std::int64_t packetsRecoveredByFec() const;
    std::int64_t fecBlocksComplete() const;
    std::int64_t fecBlocksFailed() const;

    // THIS COMMENT OWNS THE COUNTER'S CONTRACT. Every other statement of it — the public
    // C ABI (`include/naudio.h`), Transport.hpp, docs/protocols.md R5 — defers here by
    // name rather than restating the list, because a second copy is a second thing to
    // drift. No site, including this one, states a CARDINALITY: the causes are enumerated
    // by the `++fecBlocksUnreconciled_` sites in FecDecoder.cpp, and a count written into
    // prose goes stale the next time one is added.
    //
    // Blocks whose sequence range could not be reconciled with the parity's audio count,
    // and so were declined rather than recovered — see handleParity(). Always a loss of
    // opportunity, never a loss of correctness. The counter cannot tell these apart:
    //   - control/heartbeat traffic interleaving with audio inside a block, so the
    //     parity's range holds a stranger and is not the block (issue #23);
    //   - a slot whose stored copy THIS CLASS discarded for retention — recovering it
    //     would emit a byte-exact duplicate of already-delivered audio (issue #52).
    //     This one moves pendingPacketsDiscarded() as well; the others do not;
    //   - a block member DISPLACED past the range end by a stranger that never reached
    //     the decoder at all — a consumed CONTROL_ACK is the deterministic case, and it
    //     needs no packet loss whatsoever (issue #55). Note what this cause implies: a
    //     decline here does NOT mean a packet was lost. Every member was delivered, and
    //     the slot that looked missing held a control message;
    //   - a range whose present members disagree about their audio type, or a parity
    //     whose declared block size is outside FecEncoder's validated range — neither is
    //     one encoder's block, and neither can be reconciled (issue #55).
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
    // Sequences whose STORED packet this class destroyed for retention reasons (the idle
    // timeout, or the MAX_PENDING_PACKETS cap). Those packets were already emitted to the
    // application on arrival, so without this record a later parity reads the hole as peer
    // loss and "recovers" a byte-exact duplicate of delivered audio. handleParity declines
    // any range containing one, and prunes everything below each parity's end; capPending
    // bounds it for a peer that never sends parity at all. Evicted missingSequences
    // entries are deliberately NOT recorded — a slot that was always missing is
    // legitimately recoverable.
    std::set<std::int32_t> discardedSequences_;
    std::int32_t nextEmitSeq_ = -1;  // set-only ordering hint
    std::int64_t packetsRecoveredByFec_ = 0;
    std::int64_t fecBlocksComplete_ = 0;
    std::int64_t fecBlocksFailed_ = 0;
    std::int64_t fecBlocksUnreconciled_ = 0;
    std::int64_t pendingPacketsDiscarded_ = 0;
};

}  // namespace naudio
