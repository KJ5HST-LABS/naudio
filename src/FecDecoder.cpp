// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — reliability algorithms.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/FecDecoder.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "naudio/ByteCursor.hpp"
#include "naudio/FecEncoder.hpp"

namespace naudio {

FecDecoder::FecDecoder(Emitter emitter, std::int64_t blockTimeoutMs)
    : emitter_(std::move(emitter)),
      blockTimeoutMs_(blockTimeoutMs),
      pendingIdleMs_(blockTimeoutMs * 2) {}

void FecDecoder::setClock(Clock clock) { clock_ = std::move(clock); }

void FecDecoder::setPendingIdleTimeoutMs(std::int64_t idleMs) {
    pendingIdleMs_ = std::clamp(idleMs, MIN_PENDING_IDLE_MS, MAX_PENDING_IDLE_MS);
}

std::int64_t FecDecoder::pendingIdleTimeoutMs() const { return pendingIdleMs_; }

void FecDecoder::process(std::optional<AudioPacket> packet, std::int32_t sequence) {
    if (packet.has_value() && packet->packetType() == PacketType::FecParity) {
        handleParity(*packet);
        return;
    }
    if (nextEmitSeq_ < 0) nextEmitSeq_ = sequence;
    if (packet.has_value()) {
        // Live: this packet arrived on the wire. It is emitted before it is stored,
        // so the copy the repair cache holds has already been delivered as Live.
        emit(&*packet, Provenance::Live);  // emit immediately, then store for FEC
        storeInBlock(sequence, std::move(*packet));
    } else {
        recordMissing(sequence);  // gap — don't emit yet (FEC may recover it)
    }
}

void FecDecoder::processPacket(AudioPacket packet) {
    const std::int32_t sequence = packet.sequence();
    process(std::move(packet), sequence);
}

void FecDecoder::checkTimeout() { checkTimeoutAt(clock_()); }

void FecDecoder::checkTimeoutAt(std::int64_t nowMs) {
    std::vector<std::int32_t> toRemove;
    std::size_t nullsToEmit = 0;
    std::int64_t failedIncrements = 0;
    std::int64_t discarded = 0;
    for (const auto& [key, block] : activeBlocks_) {
        // The PENDING limb is an IDLE bound measured from the last insert; the
        // per-block limb below it is DEAD CODE and kept only so the shape is
        // explicit. getOrCreateBlock is the sole insertion into activeBlocks_ and
        // stamps every entry FecBlock(PENDING_KEY, 0, ...) regardless of `key`, so
        // covers() (which needs sequence < startSeq + blockSize) is false for every
        // sequence, matchingBlockKey always returns nullopt, and handleParity's
        // block is a local that is never re-inserted. activeBlocks_ therefore holds
        // at most one entry and it is always PENDING_KEY.
        const std::int64_t limitMs = (key == PENDING_KEY) ? pendingIdleMs_ : blockTimeoutMs_;
        if (nowMs - block.touchedAtMs > limitMs) {
            nullsToEmit += block.missingSequences.size();
            if (!block.missingSequences.empty()) ++failedIncrements;
            discarded += static_cast<std::int64_t>(block.packets.size());
            // Record what we are about to destroy, so a later parity cannot mistake these
            // slots for packets the peer never sent and "recover" a duplicate of audio the
            // application already received. See handleParity's discarded-slot guard.
            for (const auto& [seq, pkt] : block.packets) discardedSequences_.insert(seq);
            toRemove.push_back(key);
        }
    }
    for (std::int32_t key : toRemove) activeBlocks_.erase(key);
    fecBlocksFailed_ += failedIncrements;
    pendingPacketsDiscarded_ += discarded;
    // Silence gaps carry no packet, so there is nothing to have been repaired: Live.
    for (std::size_t i = 0; i < nullsToEmit; ++i) emit(nullptr, Provenance::Live);
}

std::int64_t FecDecoder::packetsRecoveredByFec() const { return packetsRecoveredByFec_; }

std::int64_t FecDecoder::fecBlocksComplete() const { return fecBlocksComplete_; }

std::int64_t FecDecoder::fecBlocksFailed() const { return fecBlocksFailed_; }

std::int64_t FecDecoder::fecBlocksUnreconciled() const { return fecBlocksUnreconciled_; }

std::int64_t FecDecoder::pendingPacketsDiscarded() const { return pendingPacketsDiscarded_; }

bool FecDecoder::isAudio(PacketType type) {
    return type == PacketType::AudioRx || type == PacketType::AudioTx;
}

void FecDecoder::reset() {
    activeBlocks_.clear();
    discardedSequences_.clear();
    nextEmitSeq_ = -1;
    packetsRecoveredByFec_ = 0;
    fecBlocksComplete_ = 0;
    fecBlocksFailed_ = 0;
    fecBlocksUnreconciled_ = 0;
    pendingPacketsDiscarded_ = 0;
}

void FecDecoder::emit(const AudioPacket* p, Provenance provenance) {
    if (emitter_) emitter_(p, provenance);
}

std::optional<std::int32_t> FecDecoder::matchingBlockKey(std::int32_t sequence) const {
    for (const auto& [k, b] : activeBlocks_) {
        if (b.covers(sequence)) return k;
    }
    return std::nullopt;
}

FecDecoder::FecBlock& FecDecoder::getOrCreateBlock(std::int32_t key) {
    auto it = activeBlocks_.find(key);
    if (it == activeBlocks_.end()) {
        it = activeBlocks_.emplace(key, FecBlock(PENDING_KEY, 0, clock_())).first;
    } else if (key == PENDING_KEY) {
        // Idle bound: every insert path reaches the pending block through here, so
        // refreshing on access is what keeps the timeout measuring the gap BETWEEN
        // arrivals rather than the block's total lifetime (issue #47).
        it->second.touchedAtMs = clock_();
    }
    return it->second;
}

void FecDecoder::capPending(FecBlock& block) {
    while (block.packets.size() > MAX_PENDING_PACKETS) {
        // Same reason as checkTimeoutAt's record: this packet was emitted on arrival, so
        // once it is erased a later parity must not treat the hole as peer loss.
        discardedSequences_.insert(block.packets.begin()->first);
        block.packets.erase(block.packets.begin());
        ++pendingPacketsDiscarded_;
    }
    while (block.missingSequences.size() > MAX_PENDING_PACKETS) {
        // NOT recorded: a slot that was always missing is legitimately missing, and
        // recovering it is the feature working rather than a fabrication.
        block.missingSequences.erase(block.missingSequences.begin());
    }
    // The record is pruned by handleParity, but a peer that never sends parity would let
    // it grow, so bound it here on the same principle as the packet cap itself.
    while (discardedSequences_.size() > MAX_PENDING_PACKETS) {
        discardedSequences_.erase(discardedSequences_.begin());
    }
}

void FecDecoder::storeInBlock(std::int32_t sequence, AudioPacket packet) {
    const std::int32_t key = matchingBlockKey(sequence).value_or(PENDING_KEY);
    FecBlock& block = getOrCreateBlock(key);
    block.packets.insert_or_assign(sequence, std::move(packet));
    if (key == PENDING_KEY) capPending(block);
}

void FecDecoder::recordMissing(std::int32_t sequence) {
    const std::int32_t key = matchingBlockKey(sequence).value_or(PENDING_KEY);
    FecBlock& block = getOrCreateBlock(key);
    block.missingSequences.insert(sequence);
    if (key == PENDING_KEY) capPending(block);
}

void FecDecoder::handleParity(const AudioPacket& parityPacket) {
    const std::vector<std::uint8_t>& payload = parityPacket.payload();
    if (payload.size() < FecEncoder::PARITY_HEADER_SIZE) return;

    ByteReader reader(payload);
    const std::int32_t startSeq = reader.getI32();
    const std::int32_t blockSize = static_cast<std::int32_t>(reader.getU8());
    const std::size_t xorLen = payload.size() - FecEncoder::PARITY_HEADER_SIZE;
    std::vector<std::uint8_t> xorData = reader.readBytes(xorLen);
    const std::int64_t blockEnd = static_cast<std::int64_t>(startSeq) + blockSize;

    // blockSize is a wire u8, so 0..255 reaches here while the encoder's validated range
    // is MIN_BLOCK_SIZE..MAX_BLOCK_SIZE (FecEncoder's constructor enforces that on the SEND
    // side only — nothing validates a RECEIVED header). Decline out-of-range sizes here,
    // COUNT the decline: a silent return would leave every observable at zero, so a peer
    // sending malformed parity would be indistinguishable from a peer sending none at all.
    // This also makes the member-type derivation below total: blockSize >= 2 guarantees at
    // least one present member whenever exactly one slot is missing.
    if (blockSize < static_cast<std::int32_t>(FecEncoder::MIN_BLOCK_SIZE) ||
        blockSize > static_cast<std::int32_t>(FecEncoder::MAX_BLOCK_SIZE)) {
        ++fecBlocksUnreconciled_;
        return;
    }

    // Create/update the block with the real start sequence and size.
    FecBlock block(startSeq, blockSize, clock_());
    {
        auto it = activeBlocks_.find(startSeq);
        if (it != activeBlocks_.end()) {
            block = std::move(it->second);
            block.blockSize = blockSize;
            activeBlocks_.erase(it);
        }
    }

    // Move matching packets/missing-slots out of the pending block; non-
    // matching entries go back into a fresh pending block.
    auto pendingIt = activeBlocks_.find(PENDING_KEY);
    if (pendingIt != activeBlocks_.end()) {
        FecBlock pending = std::move(pendingIt->second);
        activeBlocks_.erase(pendingIt);
        for (auto& [seq, pkt] : pending.packets) {
            if (seq >= startSeq && static_cast<std::int64_t>(seq) < blockEnd) {
                block.packets.insert_or_assign(seq, std::move(pkt));
            } else {
                getOrCreateBlock(PENDING_KEY).packets.insert_or_assign(seq, std::move(pkt));
            }
        }
        for (std::int32_t seq : pending.missingSequences) {
            if (seq >= startSeq && static_cast<std::int64_t>(seq) < blockEnd) {
                block.missingSequences.insert(seq);
            } else {
                getOrCreateBlock(PENDING_KEY).missingSequences.insert(seq);
            }
        }
        // The re-filed leftovers bypass storeInBlock/recordMissing, so cap here too.
        // Counting only — capPending never emits, which preserves this class's
        // invariant that nothing is emitted while internal state is mid-flight.
        auto refiled = activeBlocks_.find(PENDING_KEY);
        if (refiled != activeBlocks_.end()) capPending(refiled->second);
    }

    // Count missing packets in the block's range, and check that the range really
    // IS the encoder's block.
    //
    // The parity header carries a start sequence and a COUNT of audio packets;
    // reading that count as the contiguous range [startSeq, startSeq + blockSize)
    // is only valid while the block's audio packets hold consecutive sequence
    // numbers. They need not. FecEncoder::recordAndMaybeEmit is driven exclusively
    // from the audio send paths, but the connection's sequence counter is shared
    // with control and heartbeat traffic — so a single control message sent
    // between two audio packets of the same block takes a sequence number INSIDE
    // this range and displaces the block's last audio packet just past the end of
    // it. The range then holds blockSize-1 of the block's packets plus a stranger.
    //
    // Left unchecked that is silent audio corruption, not a failed recovery: one
    // slot reads as missing, the XOR runs over a set that is neither the parity's
    // nor a subset of it, and the emitted "recovered" packet is the XOR of the
    // truly-lost frame with the displaced one — a full frame of wrong samples,
    // delivered as if it were good audio. It stays invisible without loss (the
    // stranger fills the slot, so nothing looks missing) and it survives the
    // obvious content check, because XOR of two same-signal S16 payloads preserves
    // bit15 == bit14 and so almost always lands back inside the source's range.
    //
    // A true block is audio-only, so a non-audio packet in range proves the range
    // is not the block. There is no wire-safe way to recover the real membership —
    // the FROZEN parity header carries no per-slot sequence list — so decline:
    // count it and leave the lost packet lost, exactly as with FEC disabled. That
    // costs one block's recovery per interleaved control message and never emits a
    // frame the sender did not send. See issue #23.
    // A SECOND way the range can fail to be the block, with the same consequence and a
    // different cause: a slot THIS DECODER discarded. Retention is bounded two ways
    // (the idle timeout in checkTimeoutAt, the packet cap in capPending) and both
    // ERASE stored packets — but the packets were already emitted to the application on
    // arrival, so an erased slot reads here as "the peer never sent it" while in truth
    // it was delivered. With exactly one such slot the XOR remainder is precisely that
    // frame, so recovery would emit a BYTE-EXACT DUPLICATE of audio the application
    // already has and count it as a repair. Measured before this guard existed: 6 frames
    // delivered for 5 sent, at ZERO packet loss, on both receive modes.
    //
    // Hence discardedSequences_: the sequences retention destroyed. An absent slot found
    // in it proves the range is unreconcilable for the same reason a stranger does, and
    // is declined the same way. Note the asymmetry — only erased PACKETS are recorded,
    // never evicted missingSequences entries: a slot that was always missing is
    // legitimately missing, and recovering it is the feature working.
    std::int32_t missingCount = 0;
    std::int32_t missingSeq = -1;
    bool rangeIsAudioOnly = true;
    bool rangeHasDiscardedSlot = false;
    // The block's own audio type, read off a present member rather than assumed. The
    // encoder records from BOTH audio send paths (sendRxAudio and sendTxAudio), so a block
    // is an AudioRx block or an AudioTx block, and a recovered packet must carry the type
    // its block carried — see the recovery limb. A range holding both is not one encoder's
    // block, and picking either type would be a coin flip, so it is declined below.
    bool memberTypeSeen = false;
    bool rangeTypeUniform = true;
    PacketType memberType = PacketType::AudioRx;
    for (std::int64_t s = startSeq; s < blockEnd; ++s) {
        const std::int32_t seq = static_cast<std::int32_t>(s);
        auto pit = block.packets.find(seq);
        if (pit == block.packets.end()) {
            ++missingCount;
            missingSeq = seq;
            if (discardedSequences_.count(seq) != 0) rangeHasDiscardedSlot = true;
        } else if (!isAudio(pit->second.packetType())) {
            rangeIsAudioOnly = false;
        } else if (!memberTypeSeen) {
            memberType = pit->second.packetType();
            memberTypeSeen = true;
        } else if (pit->second.packetType() != memberType) {
            rangeTypeUniform = false;
        }
    }

    // THE DISPLACEMENT WITNESS (issue #55). Computed BEFORE the prune below, which would
    // otherwise remove part of its evidence.
    //
    // The two guards above see only what is IN the range. The stranger that breaks the
    // range need never arrive at all: a consumed CONTROL_ACK draws a sequence and is
    // returned before the reorder/FEC pipeline (see checkTimeout()'s note, which already
    // owns that enumeration), so its slot reads exactly like a lost audio packet. With
    // exactly one such slot the XOR remainder IS the block's displaced last member — audio
    // the application already received — and recovery would emit a byte-exact DUPLICATE.
    //
    // What proves displacement is therefore not the stranger but the DISPLACED MEMBER: an
    // audio packet the decoder is holding at or past the parity's declared block end. The
    // reorder buffer emits in strictly ascending sequence order and drops anything below
    // nextExpected_, so at the instant a parity is handled nothing above its own sequence
    // has been delivered; an audio packet at or after blockEnd can therefore only be a
    // member of THIS block that a stranger pushed out of the range.
    //
    // Three sources are searched, and each is load-bearing:
    //   - the re-filed pending block, where an out-of-range leftover normally lands;
    //   - `block` itself, because when startSeq == PENDING_KEY the find() above moves the
    //     pending block wholesale into it and leaves no PENDING_KEY entry to search;
    //   - discardedSequences_, because the displaced member may have been evicted by
    //     capPending before the parity arrived (the eviction is lowest-first over a SIGNED
    //     map, so at the int32 wrap the highest sequence sorts lowest and goes first).
    //
    // Distances are WRAPPING and unsigned, bounded by the forward half-space: "at or after
    // blockEnd" is a statement about a counter §3.4 defines as two's-complement, so a
    // signed comparison is wrong for every block whose end straddles the wrap. The
    // half-space needs no tuning constant and reads no field the wire format leaves
    // unspecified — in particular NOT the parity frame's own sequence number, which is
    // deliberately never consulted: §3.4 constrains it only to be monotonic and shared, so
    // a conforming peer may number it any way it likes (naudio's own conformance harness
    // numbers it differently from its connection layer). A guard keyed to it would decline
    // every block from such a peer, i.e. silently disable FEC against a legal sender.
    const std::uint32_t uStart = static_cast<std::uint32_t>(startSeq);
    const std::uint32_t uSize = static_cast<std::uint32_t>(blockSize);
    const auto atOrPastBlockEnd = [uStart, uSize](std::int32_t seq) {
        const std::uint32_t d = static_cast<std::uint32_t>(seq) - uStart;
        return d >= uSize && d < 0x80000000u;
    };
    bool rangeHasDisplacedMember = false;
    const auto scanForMember = [&](const std::map<std::int32_t, AudioPacket>& packets) {
        for (const auto& [seq, pkt] : packets) {
            if (isAudio(pkt.packetType()) && atOrPastBlockEnd(seq)) {
                rangeHasDisplacedMember = true;
            }
        }
    };
    scanForMember(block.packets);
    if (auto leftovers = activeBlocks_.find(PENDING_KEY); leftovers != activeBlocks_.end()) {
        scanForMember(leftovers->second.packets);
    }
    for (std::int32_t seq : discardedSequences_) {
        if (atOrPastBlockEnd(seq)) rangeHasDisplacedMember = true;
    }

    // Every sequence below this block's end is now unreachable by any future parity
    // (ranges advance), so the record is pruned here rather than growing forever.
    const auto pruneEnd =
        discardedSequences_.lower_bound(static_cast<std::int32_t>(blockEnd));
    discardedSequences_.erase(discardedSequences_.begin(), pruneEnd);

    if (!rangeIsAudioOnly || rangeHasDiscardedSlot || !rangeTypeUniform) {
        ++fecBlocksUnreconciled_;
        return;  // `block` is a local — dropping it discards the range
    }

    if (missingCount == 0) {
        // All present — parity is redundant.
        ++fecBlocksComplete_;
    } else if (missingCount == 1) {
        // Declined here rather than above, deliberately. The witness is only evidence of a
        // FABRICATION when the recovery limb would actually run: with nothing missing the
        // block is complete and the XOR never happens, and with 2+ missing recovery is
        // already impossible. Checking it pre-switch would also mask the two guards above,
        // which are the only detectors #23 and #52 have.
        if (rangeHasDisplacedMember) {
            ++fecBlocksUnreconciled_;
            return;  // `block` is a local — dropping it discards the range
        }

        // Exactly one missing — recover via XOR of parity ^ all present.
        //
        // LOSSLESS ONLY FOR UNIFORM-LENGTH BLOCKS. The recovered buffer is
        // exactly `xorLen` bytes — the block-max payload length carried by the
        // parity. The FROZEN 5-byte parity header (startSeq i32 + blockSize u8)
        // has NO per-slot length, so if the missing packet was originally
        // SHORTER than the block max its true length cannot be reconstructed:
        // it is recovered zero-extended to `xorLen` (the XOR math zeroes the
        // trailing pad bytes, but the emitted length is inflated to the block
        // max). For the fixed-size PCM audio frames this decoder is built for —
        // the production path — every packet in a block is the same size, so
        // recovery is byte-exact. A variable-length block recovers correctly
        // ONLY when the missing packet was the block-max one (see
        // FecDecoder.VariablePayloadSizeRecovery in tests/test_fec.cpp).
        //
        // This is documented rather than "fixed": a per-slot length field would
        // change the frozen 0xAF01 wire and the conformance vectors, which is
        // not allowed. No runtime assert/clamp is added either — at recovery
        // time there is no wire-safe signal that tells a legitimately max-length
        // missing packet apart from a shorter one, so any guard would either
        // false-positive on the valid variable-length case above or change the
        // emitted bytes.
        std::vector<std::uint8_t> recovered = std::move(xorData);
        for (std::int64_t s = startSeq; s < blockEnd; ++s) {
            const std::int32_t seq = static_cast<std::int32_t>(s);
            auto pit = block.packets.find(seq);
            if (pit != block.packets.end()) {
                const std::vector<std::uint8_t>& p = pit->second.payload();
                const std::size_t n = std::min(p.size(), xorLen);
                for (std::size_t j = 0; j < n; ++j) recovered[j] ^= p[j];
            }
        }
        // The block's OWN audio type, not a hardcoded AudioRx. The encoder is driven from
        // both audio send paths, so the TX lane is FEC-protected too — and a recovered TX
        // frame typed AudioRx is routed to `default: break` by the server's receive switch
        // and silently dropped, while packetsRecoveredByFec has already counted it (#55).
        // memberTypeSeen is guaranteed by the blockSize floor at the top of this function:
        // blockSize >= 2 with exactly one slot missing leaves at least one present member.
        AudioPacket recoveredPacket(memberType, missingSeq, std::move(recovered));
        // THE one Recovered emit in the project. This frame never arrived on the
        // wire — its payload is the XOR remainder of the block. Every guard above
        // exists to make sure that remainder is a genuinely missing slot rather
        // than one this decoder discarded (#52) or displaced (#55); provenance is
        // what lets a downstream consumer act on the distinction (#65).
        emit(&recoveredPacket, Provenance::Recovered);
        block.missingSequences.erase(missingSeq);
        ++packetsRecoveredByFec_;
        ++fecBlocksComplete_;
    } else {
        // 2+ missing — cannot recover; emit silence for each known gap.
        for (std::int64_t s = startSeq; s < blockEnd; ++s) {
            const std::int32_t seq = static_cast<std::int32_t>(s);
            if (block.packets.find(seq) == block.packets.end() &&
                block.missingSequences.count(seq) != 0) {
                // A gap, not a repair — nothing was reconstructed here.
                emit(nullptr, Provenance::Live);
            }
        }
        ++fecBlocksFailed_;
    }
    // `block` is discarded here (the local goes out of scope).
}

std::int64_t FecDecoder::defaultNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace naudio
