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
#include <vector>

#include "naudio/AudioPacket.hpp"

namespace naudio {

// XOR-based forward error correction encoder.
//
// Accumulates N audio packet payloads and emits a parity packet after every N
// sends. The parity payload is the XOR of all N payloads (shorter ones implicitly
// zero-padded to the block's max length), allowing the receiver to recover any
// single lost packet per block.
//
// Parity packet payload format (big-endian):
//   Offset  Size  Field
//   0       4     Start sequence number (first packet in block)
//   4       1     Block size (N)
//   5       M     XOR parity data (M = max payload length in block)
//
// Pure: no I/O. The constructor validates the block size (throwing
// std::invalid_argument on a non-positive size).
//
// Compiled into naudio_core (definitions in FecEncoder.cpp).
class FecEncoder {
public:
    // Overhead bytes in the parity payload: 4 (startSeq) + 1 (blockSize).
    static constexpr std::size_t PARITY_HEADER_SIZE = 5;

    // Creates an encoder. blockSize is the number of audio packets per FEC block
    // and must be 2-10.
    explicit FecEncoder(std::size_t blockSize);

    // Records an audio packet's payload for FEC calculation. Returns a parity
    // packet once the block is complete, or nullopt while still accumulating.
    std::optional<AudioPacket> recordAndMaybeEmit(const AudioPacket& packet);

    // Resets the accumulator for the next block.
    void reset();

    // The configured block size.
    std::size_t blockSize() const;
    // The number of packets accumulated in the current block.
    std::size_t currentCount() const;

private:
    // Builds the XOR parity packet from accumulated payloads. The parity packet
    // uses the sequence number after the last audio packet.
    AudioPacket buildParityPacket(std::int32_t lastSequence) const;

    std::size_t blockSize_;
    std::vector<std::vector<std::uint8_t>> payloads_;
    std::int32_t startSequence_ = 0;
    std::size_t maxPayloadLen_ = 0;
};

}  // namespace naudio
