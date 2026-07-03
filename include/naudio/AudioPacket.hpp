// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — 0xAF01 wire codec.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace naudio {

// Audio streaming packet types (the on-wire byte values).
//
enum class PacketType : std::uint8_t {
    AudioRx = 0x00,    // Audio data from radio to client (RX).
    AudioTx = 0x01,    // Audio data from client to radio (TX).
    Control = 0x02,    // Control message.
    Heartbeat = 0x03,  // Heartbeat/keepalive.
    FecParity = 0x04,  // FEC parity packet for loss recovery.
};

// Resolves a packet type from its byte value (nullopt if unknown; deserialize
// rejects the frame in that case).
std::optional<PacketType> packetTypeFromValue(std::uint8_t value);

// The enum constant name ("AUDIO_RX" … "FEC_PARITY").
const char* packetTypeName(PacketType type);

// An audio streaming packet.
//
// Wire format (all multi-byte fields big-endian):
//   Offset  Size  Field
//   0       2     Magic (0xAF01)
//   2       1     Version
//   3       1     Type
//   4       1     Flags
//   5       4     Sequence number
//   9       8     Timestamp (nanos)
//   17      2     Payload length (unsigned-16)
//   19      N     Payload
//   19+N    4     CRC32 (over header + payload)
//
// deserialize() returns nullopt on ANY validation failure (short buffer, wrong
// magic, unknown type, over-long payload, CRC mismatch) — the frozen-contract
// behaviour the conformance `frame` reject vectors assert.
class AudioPacket {
public:
    static constexpr std::uint16_t MAGIC = 0xAF01;     // Magic bytes identifying audio packets.
    static constexpr std::uint8_t VERSION = 1;         // Current protocol version.
    static constexpr std::size_t HEADER_SIZE = 19;     // Header size (no payload, no CRC).
    static constexpr std::size_t CRC_SIZE = 4;         // CRC size in bytes.
    static constexpr std::size_t MAX_PAYLOAD = 16384;  // 16KB (50ms @ 48kHz stereo 16-bit = 9600).
    // Advisory MTU cap: a serialized datagram larger than this IP-fragments on a
    // standard ~1500-byte path, and losing one fragment drops the whole datagram
    // (defeating FEC). NOT enforced by the codec — the wire permits payloads up to
    // MAX_PAYLOAD. UDP callers should size audio frames so packetSize(payload) stays
    // at or below this. UdpClientConnection::sendPacket counts datagrams that exceed
    // it (see UdpClientConnection::oversizedDatagrams()).
    static constexpr std::size_t UDP_MAX_PAYLOAD = 1400;  // Max safe UDP payload (no IP fragmentation).

    static constexpr std::uint8_t FLAG_COMPRESSED = 0x01;      // Payload is compressed.
    static constexpr std::uint8_t FLAG_LOW_BANDWIDTH = 0x02;   // Low bandwidth mode (12kHz).

    // Creates a packet, stamping the current wall-clock timestamp (carried
    // only for latency measurement).
    AudioPacket(PacketType type, std::int32_t sequence, std::vector<std::uint8_t> payload);

    // Convenience factories.
    static AudioPacket createRxAudio(std::int32_t sequence, std::vector<std::uint8_t> audioData);
    static AudioPacket createTxAudio(std::int32_t sequence, std::vector<std::uint8_t> audioData);
    static AudioPacket createControl(std::int32_t sequence, std::vector<std::uint8_t> controlData);
    static AudioPacket createHeartbeat(std::int32_t sequence);

    // Serializes the packet (header + payload + CRC32) to a byte vector.
    std::vector<std::uint8_t> serialize() const;

    // Deserializes a packet (nullopt if invalid).
    static std::optional<AudioPacket> deserialize(const std::uint8_t* data, std::size_t length);
    static std::optional<AudioPacket> deserialize(const std::vector<std::uint8_t>& data);

    // The total packet size for a given payload length.
    static std::size_t packetSize(std::size_t payloadLength) {
        return HEADER_SIZE + payloadLength + CRC_SIZE;
    }

    // --- Getters / setters ---
    std::uint8_t version() const { return version_; }
    PacketType packetType() const { return type_; }
    void setType(PacketType type) { type_ = type; }
    std::uint8_t flags() const { return flags_; }
    void setFlags(std::uint8_t flags) { flags_ = flags; }
    bool hasFlag(std::uint8_t flag) const { return (flags_ & flag) != 0; }
    void setFlag(std::uint8_t flag, bool value) {
        if (value) flags_ |= flag; else flags_ &= static_cast<std::uint8_t>(~flag);
    }
    std::int32_t sequence() const { return sequence_; }
    void setSequence(std::int32_t sequence) { sequence_ = sequence; }
    std::int64_t timestamp() const { return timestamp_; }
    void setTimestamp(std::int64_t timestamp) { timestamp_ = timestamp; }
    const std::vector<std::uint8_t>& payload() const { return payload_; }
    void setPayload(std::vector<std::uint8_t> payload) { payload_ = std::move(payload); }
    std::size_t payloadLength() const { return payload_.size(); }

private:
    // Full-field constructor for deserialize (no clock stamp).
    AudioPacket(std::uint8_t version, PacketType type, std::uint8_t flags, std::int32_t sequence,
                std::int64_t timestamp, std::vector<std::uint8_t> payload)
        : version_(version), type_(type), flags_(flags), sequence_(sequence),
          timestamp_(timestamp), payload_(std::move(payload)) {}

    std::uint8_t version_;
    PacketType type_;
    std::uint8_t flags_;
    std::int32_t sequence_;
    std::int64_t timestamp_;
    std::vector<std::uint8_t> payload_;
};

}  // namespace naudio
