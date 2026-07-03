// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — reliability algorithms.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/FecEncoder.hpp"

#include <stdexcept>

#include "naudio/ByteCursor.hpp"

namespace naudio {

FecEncoder::FecEncoder(std::size_t blockSize) : blockSize_(blockSize) {
    if (blockSize < 2 || blockSize > 10) {
        throw std::invalid_argument("blockSize must be 2-10");
    }
}

std::optional<AudioPacket> FecEncoder::recordAndMaybeEmit(const AudioPacket& packet) {
    if (payloads_.empty()) {
        startSequence_ = packet.sequence();
        maxPayloadLen_ = 0;
    }
    const std::vector<std::uint8_t>& payload = packet.payload();
    if (payload.size() > maxPayloadLen_) maxPayloadLen_ = payload.size();
    payloads_.push_back(payload);

    if (payloads_.size() >= blockSize_) {
        AudioPacket parity = buildParityPacket(packet.sequence());
        reset();
        return parity;
    }
    return std::nullopt;
}

void FecEncoder::reset() {
    payloads_.clear();
    maxPayloadLen_ = 0;
}

std::size_t FecEncoder::blockSize() const { return blockSize_; }

std::size_t FecEncoder::currentCount() const { return payloads_.size(); }

AudioPacket FecEncoder::buildParityPacket(std::int32_t lastSequence) const {
    std::vector<std::uint8_t> xorData(maxPayloadLen_, 0);
    for (const auto& p : payloads_) {
        for (std::size_t j = 0; j < p.size(); ++j) xorData[j] ^= p[j];
    }
    ByteWriter w(PARITY_HEADER_SIZE + maxPayloadLen_);
    w.putI32(startSequence_);
    w.putU8(static_cast<std::uint8_t>(blockSize_));
    w.putBytes(xorData);
    return AudioPacket(PacketType::FecParity, lastSequence + 1, w.take());
}

}  // namespace naudio
