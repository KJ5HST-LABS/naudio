// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — 0xAF01 wire codec.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/AudioPacket.hpp"

#include <chrono>
#include <utility>

#include "naudio/ByteCursor.hpp"
#include "naudio/Crc32.hpp"

namespace naudio {

namespace {

// Wall-clock nanoseconds since the Unix epoch. Carried only for latency
// measurement; round-trips verbatim.
std::int64_t nowNanos() {
    using namespace std::chrono;
    return static_cast<std::int64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

}  // namespace

std::optional<PacketType> packetTypeFromValue(std::uint8_t value) {
    switch (value) {
        case 0x00: return PacketType::AudioRx;
        case 0x01: return PacketType::AudioTx;
        case 0x02: return PacketType::Control;
        case 0x03: return PacketType::Heartbeat;
        case 0x04: return PacketType::FecParity;
        default: return std::nullopt;
    }
}

const char* packetTypeName(PacketType type) {
    switch (type) {
        case PacketType::AudioRx: return "AUDIO_RX";
        case PacketType::AudioTx: return "AUDIO_TX";
        case PacketType::Control: return "CONTROL";
        case PacketType::Heartbeat: return "HEARTBEAT";
        case PacketType::FecParity: return "FEC_PARITY";
    }
    return "UNKNOWN";
}

AudioPacket::AudioPacket(PacketType type, std::int32_t sequence, std::vector<std::uint8_t> payload)
    : version_(VERSION), type_(type), flags_(0), sequence_(sequence),
      timestamp_(nowNanos()), payload_(std::move(payload)) {}

AudioPacket AudioPacket::createRxAudio(std::int32_t sequence, std::vector<std::uint8_t> audioData) {
    return AudioPacket(PacketType::AudioRx, sequence, std::move(audioData));
}
AudioPacket AudioPacket::createTxAudio(std::int32_t sequence, std::vector<std::uint8_t> audioData) {
    return AudioPacket(PacketType::AudioTx, sequence, std::move(audioData));
}
AudioPacket AudioPacket::createControl(std::int32_t sequence, std::vector<std::uint8_t> controlData) {
    return AudioPacket(PacketType::Control, sequence, std::move(controlData));
}
AudioPacket AudioPacket::createHeartbeat(std::int32_t sequence) {
    return AudioPacket(PacketType::Heartbeat, sequence, std::vector<std::uint8_t>{});
}

std::vector<std::uint8_t> AudioPacket::serialize() const {
    // W5: the length field is u16 and deserialize() rejects payloadLen > MAX_PAYLOAD,
    // so clamp here — the encoder must never emit a frame the decoder would reject (or
    // a u16 that wrapped). Real audio frames are far below MAX_PAYLOAD (16 KB), so this
    // guard never triggers on the live path and leaves normal frames byte-identical.
    std::size_t payloadLen = std::min(payload_.size(), MAX_PAYLOAD);
    ByteWriter w(HEADER_SIZE + payloadLen + CRC_SIZE);
    // Header (big-endian).
    w.putU16(MAGIC);
    w.putU8(version_);
    w.putU8(static_cast<std::uint8_t>(type_));
    w.putU8(flags_);
    w.putI32(sequence_);
    w.putI64(timestamp_);
    w.putU16(static_cast<std::uint16_t>(payloadLen));
    // Payload.
    w.putBytes(payload_.data(), payloadLen);
    // CRC over header + payload (the bytes accumulated so far).
    std::uint32_t crc = crc32(w.bytes());
    w.putU32(crc);
    return w.take();
}

std::optional<AudioPacket> AudioPacket::deserialize(const std::uint8_t* data, std::size_t length) {
    if (length < HEADER_SIZE + CRC_SIZE) {
        return std::nullopt;
    }

    // Check magic.
    std::uint16_t magic = static_cast<std::uint16_t>((data[0] << 8) | data[1]);
    if (magic != MAGIC) {
        return std::nullopt;
    }

    std::uint8_t version = data[2];
    std::uint8_t typeByte = data[3];
    std::uint8_t flags = data[4];
    std::int32_t sequence = static_cast<std::int32_t>(
        (static_cast<std::uint32_t>(data[5]) << 24) | (static_cast<std::uint32_t>(data[6]) << 16) |
        (static_cast<std::uint32_t>(data[7]) << 8) | static_cast<std::uint32_t>(data[8]));
    std::int64_t timestamp = 0;
    for (std::size_t i = 9; i < 17; ++i) timestamp = (timestamp << 8) | data[i];
    std::size_t payloadLen = static_cast<std::size_t>((data[17] << 8) | data[18]);

    auto type = packetTypeFromValue(typeByte);
    if (!type) {
        return std::nullopt;
    }

    // Validate payload length.
    if (payloadLen > MAX_PAYLOAD || length < HEADER_SIZE + payloadLen + CRC_SIZE) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> payload(data + HEADER_SIZE, data + HEADER_SIZE + payloadLen);

    // Verify CRC (over header + payload).
    std::size_t crcOff = HEADER_SIZE + payloadLen;
    std::uint32_t receivedCrc =
        (static_cast<std::uint32_t>(data[crcOff]) << 24) |
        (static_cast<std::uint32_t>(data[crcOff + 1]) << 16) |
        (static_cast<std::uint32_t>(data[crcOff + 2]) << 8) |
        static_cast<std::uint32_t>(data[crcOff + 3]);
    std::uint32_t crc = crc32(data, crcOff);
    if (crc != receivedCrc) {
        return std::nullopt;
    }

    return AudioPacket(version, *type, flags, sequence, timestamp, std::move(payload));
}

std::optional<AudioPacket> AudioPacket::deserialize(const std::vector<std::uint8_t>& data) {
    return deserialize(data.data(), data.size());
}

}  // namespace naudio
