// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — control-plane codec.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
//
// Control-plane codec. All multi-byte fields big-endian; strings are UTF-8
// (copied verbatim). Per-message sub-layouts and the CLIENTS_UPDATE framing
// (infoLen as a position gate) are exact — approximating them breaks the wire.
#include "naudio/ControlMessage.hpp"

#include <algorithm>
#include <utility>

#include "naudio/ByteCursor.hpp"

namespace naudio {

std::optional<ControlType> controlTypeFromValue(std::uint8_t value) {
    switch (value) {
        case 0x01: return ControlType::ConnectRequest;
        case 0x02: return ControlType::ConnectAccept;
        case 0x03: return ControlType::ConnectReject;
        case 0x04: return ControlType::AudioConfig;
        case 0x10: return ControlType::StreamStart;
        case 0x11: return ControlType::StreamStop;
        case 0x12: return ControlType::StreamPause;
        case 0x13: return ControlType::StreamResume;
        case 0x20: return ControlType::Heartbeat;
        case 0x21: return ControlType::HeartbeatAck;
        case 0x22: return ControlType::LatencyProbe;
        case 0x23: return ControlType::LatencyResponse;
        case 0x30: return ControlType::StatsUpdate;
        case 0x40: return ControlType::TxGranted;
        case 0x41: return ControlType::TxDenied;
        case 0x42: return ControlType::TxPreempted;
        case 0x43: return ControlType::TxReleased;
        case 0x44: return ControlType::ClientsUpdate;
        case 0x50: return ControlType::Nack;
        case 0x51: return ControlType::ControlAck;
        case 0xFE: return ControlType::Error;
        case 0xFF: return ControlType::Disconnect;
        default: return std::nullopt;
    }
}

const char* controlTypeName(ControlType type) {
    switch (type) {
        case ControlType::ConnectRequest: return "CONNECT_REQUEST";
        case ControlType::ConnectAccept: return "CONNECT_ACCEPT";
        case ControlType::ConnectReject: return "CONNECT_REJECT";
        case ControlType::AudioConfig: return "AUDIO_CONFIG";
        case ControlType::StreamStart: return "STREAM_START";
        case ControlType::StreamStop: return "STREAM_STOP";
        case ControlType::StreamPause: return "STREAM_PAUSE";
        case ControlType::StreamResume: return "STREAM_RESUME";
        case ControlType::Heartbeat: return "HEARTBEAT";
        case ControlType::HeartbeatAck: return "HEARTBEAT_ACK";
        case ControlType::LatencyProbe: return "LATENCY_PROBE";
        case ControlType::LatencyResponse: return "LATENCY_RESPONSE";
        case ControlType::StatsUpdate: return "STATS_UPDATE";
        case ControlType::TxGranted: return "TX_GRANTED";
        case ControlType::TxDenied: return "TX_DENIED";
        case ControlType::TxPreempted: return "TX_PREEMPTED";
        case ControlType::TxReleased: return "TX_RELEASED";
        case ControlType::ClientsUpdate: return "CLIENTS_UPDATE";
        case ControlType::Nack: return "NACK";
        case ControlType::ControlAck: return "CONTROL_ACK";
        case ControlType::Error: return "ERROR";
        case ControlType::Disconnect: return "DISCONNECT";
    }
    return "UNKNOWN";
}

RejectReason rejectReasonFromValue(std::uint8_t value) {
    switch (value) {
        case 0x01: return RejectReason::Busy;
        case 0x02: return RejectReason::VersionMismatch;
        case 0x03: return RejectReason::FormatNotSupported;
        case 0x04: return RejectReason::AuthFailed;
        default: return RejectReason::Rejected;
    }
}

const char* rejectReasonName(RejectReason reason) {
    switch (reason) {
        case RejectReason::Busy: return "BUSY";
        case RejectReason::VersionMismatch: return "VERSION_MISMATCH";
        case RejectReason::FormatNotSupported: return "FORMAT_NOT_SUPPORTED";
        case RejectReason::AuthFailed: return "AUTH_FAILED";
        case RejectReason::Rejected: return "REJECTED";
    }
    return "REJECTED";
}

// --- ClientInfo ---

std::string ClientInfo::displayString() const {
    std::string sb;
    if (!callsign.empty()) {
        sb += callsign;
        if (!name.empty() || !location.empty()) {
            sb += " (";
            if (!name.empty()) {
                sb += name;
                if (!location.empty()) sb += ", ";
            }
            if (!location.empty()) sb += location;
            sb += ")";
        }
    } else if (!name.empty()) {
        sb += name;
        if (!location.empty()) {
            sb += " (";
            sb += location;
            sb += ")";
        }
    } else if (!location.empty()) {
        sb += location;
    }
    return sb;
}

std::string ClientsUpdateInfo::getClientDisplayString(const std::string& clientId) const {
    auto it = clientInfoMap.find(clientId);
    if (it != clientInfoMap.end() && !it->second.isEmpty()) {
        return it->second.displayString();
    }
    return clientId;
}

namespace {

std::vector<std::uint8_t> strBytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

// The whole serialized ClientInfo must fit behind a u8 length-prefix (used by the
// CLIENTS_UPDATE entries and CONNECT_REQUEST). W1 fix: clamp the TOTAL to 255 bytes
// — capping each field at 255 independently could reach 3+255*3 = 768 and overflow
// that u8 prefix, mis-framing the roster. The 3 length bytes are fixed, so
// callsign/name/location share a 252-byte budget allocated in that order. Identical
// clamp in gen_vectors.py and the reference implementations, so all impls reproduce the
// same bytes; normal-size identities are well under budget and stay byte-unchanged.
constexpr std::size_t kClientInfoMaxBytes = 255;

// Serializes ClientInfo to bytes (empty if null or empty), total clamped to 255.
std::vector<std::uint8_t> serializeClientInfo(const ClientInfo* info) {
    if (info == nullptr || info->isEmpty()) return {};
    std::size_t budget = kClientInfoMaxBytes - 3;
    std::size_t callsignLen = std::min<std::size_t>(info->callsign.size(), budget);
    budget -= callsignLen;
    std::size_t nameLen = std::min<std::size_t>(info->name.size(), budget);
    budget -= nameLen;
    std::size_t locationLen = std::min<std::size_t>(info->location.size(), budget);
    ByteWriter w(3 + callsignLen + nameLen + locationLen);
    w.putU8(static_cast<std::uint8_t>(callsignLen));
    w.putBytes(reinterpret_cast<const std::uint8_t*>(info->callsign.data()), callsignLen);
    w.putU8(static_cast<std::uint8_t>(nameLen));
    w.putBytes(reinterpret_cast<const std::uint8_t*>(info->name.data()), nameLen);
    w.putU8(static_cast<std::uint8_t>(locationLen));
    w.putBytes(reinterpret_cast<const std::uint8_t*>(info->location.data()), locationLen);
    return w.take();
}

// Deserializes ClientInfo from a reader (advances it; nullopt if < 3 bytes remain).
std::optional<ClientInfo> deserializeClientInfo(ByteReader& r) {
    if (r.remaining() < 3) return std::nullopt;

    std::size_t callsignLen = r.getU8();
    std::string callsign;
    if (callsignLen > 0 && r.remaining() >= callsignLen) callsign = r.readString(callsignLen);

    if (r.remaining() < 1) return ClientInfo{callsign, "", ""};
    std::size_t nameLen = r.getU8();
    std::string name;
    if (nameLen > 0 && r.remaining() >= nameLen) name = r.readString(nameLen);

    if (r.remaining() < 1) return ClientInfo{callsign, name, ""};
    std::size_t locationLen = r.getU8();
    std::string location;
    if (locationLen > 0 && r.remaining() >= locationLen) location = r.readString(locationLen);

    return ClientInfo{callsign, name, location};
}

}  // namespace

// --- Core serialize/deserialize ---

std::vector<std::uint8_t> ControlMessage::serialize() const {
    std::vector<std::uint8_t> buf;
    buf.reserve(1 + data_.size());
    buf.push_back(static_cast<std::uint8_t>(type_));
    buf.insert(buf.end(), data_.begin(), data_.end());
    return buf;
}

std::optional<ControlMessage> ControlMessage::deserialize(const std::uint8_t* payload,
                                                         std::size_t len) {
    if (len == 0) return std::nullopt;
    auto type = controlTypeFromValue(payload[0]);
    if (!type) return std::nullopt;
    return ControlMessage(*type, std::vector<std::uint8_t>(payload + 1, payload + len));
}

std::optional<ControlMessage> ControlMessage::deserialize(const std::vector<std::uint8_t>& payload) {
    return deserialize(payload.data(), payload.size());
}

// --- Connect request ---

ControlMessage ControlMessage::connectRequest(const std::string& clientName,
                                              std::uint8_t protocolVersion) {
    return connectRequestFull(clientName, protocolVersion, nullptr, nullptr);
}

ControlMessage ControlMessage::connectRequestWithConfig(const std::string& clientName,
                                                        std::uint8_t protocolVersion,
                                                        const AudioStreamConfig* requestedConfig) {
    return connectRequestFull(clientName, protocolVersion, requestedConfig, nullptr);
}

ControlMessage ControlMessage::connectRequestFull(const std::string& clientName,
                                                  std::uint8_t protocolVersion,
                                                  const AudioStreamConfig* requestedConfig,
                                                  const ClientInfo* clientInfo) {
    auto clientInfoBytes = serializeClientInfo(clientInfo);
    ByteWriter w;
    w.putU8(protocolVersion);
    w.putU8(static_cast<std::uint8_t>(clientName.size()));
    w.putBytes(reinterpret_cast<const std::uint8_t*>(clientName.data()), clientName.size());
    // Flag indicating whether config is included.
    w.putU8(requestedConfig != nullptr ? 1 : 0);
    if (requestedConfig != nullptr) {
        w.putU16(static_cast<std::uint16_t>(requestedConfig->bufferTargetMs));
        w.putU16(static_cast<std::uint16_t>(requestedConfig->bufferMinMs));
        w.putU16(static_cast<std::uint16_t>(requestedConfig->bufferMaxMs));
    }
    // Client info (length-prefixed).
    w.putU8(static_cast<std::uint8_t>(clientInfoBytes.size()));
    w.putBytes(clientInfoBytes);
    return ControlMessage(ControlType::ConnectRequest, w.take());
}

std::optional<AudioStreamConfig> ControlMessage::parseConnectRequestConfig() const {
    if (type_ != ControlType::ConnectRequest || data_.size() < 3) return std::nullopt;
    ByteReader r(data_);
    r.getU8();  // Skip version.
    std::size_t nameLen = r.getU8();
    if (r.remaining() < nameLen + 1) return std::nullopt;  // Malformed or old protocol.
    r.setPosition(r.position() + nameLen);
    if (r.remaining() < 1) return std::nullopt;  // Old protocol without config.
    std::uint8_t hasConfig = r.getU8();
    if (hasConfig == 0 || r.remaining() < 6) return std::nullopt;  // No config included.
    AudioStreamConfig config;
    config.bufferTargetMs = r.getU16();
    config.bufferMinMs = r.getU16();
    config.bufferMaxMs = r.getU16();
    return config;
}

std::optional<ClientInfo> ControlMessage::parseConnectRequestClientInfo() const {
    if (type_ != ControlType::ConnectRequest || data_.size() < 3) return std::nullopt;
    ByteReader r(data_);
    r.getU8();  // Skip version.
    std::size_t nameLen = r.getU8();
    if (r.remaining() < nameLen + 1) return std::nullopt;
    r.setPosition(r.position() + nameLen);
    if (r.remaining() < 1) return std::nullopt;
    std::uint8_t hasConfig = r.getU8();
    if (hasConfig != 0) {
        if (r.remaining() < 6) return std::nullopt;
        r.setPosition(r.position() + 6);
    }
    if (r.remaining() < 1) return std::nullopt;  // No client info.
    std::size_t clientInfoLen = r.getU8();
    if (clientInfoLen == 0 || r.remaining() < clientInfoLen) return std::nullopt;
    return deserializeClientInfo(r);
}

// --- Connect reject ---

ControlMessage ControlMessage::connectReject(RejectReason reason, const std::string& message) {
    auto msgBytes = strBytes(message);
    ByteWriter w(2 + msgBytes.size());
    w.putU8(static_cast<std::uint8_t>(reason));
    w.putU8(static_cast<std::uint8_t>(msgBytes.size()));
    w.putBytes(msgBytes);
    return ControlMessage(ControlType::ConnectReject, w.take());
}

// --- Audio config ---

ControlMessage ControlMessage::audioConfig(const AudioStreamConfig& config) {
    ByteWriter w(14);
    w.putI32(config.sampleRate);
    w.putU8(static_cast<std::uint8_t>(config.bitsPerSample));
    w.putU8(static_cast<std::uint8_t>(config.channels));
    w.putU16(static_cast<std::uint16_t>(config.frameDurationMs));
    w.putU16(static_cast<std::uint16_t>(config.bufferTargetMs));
    w.putU16(static_cast<std::uint16_t>(config.bufferMinMs));
    w.putU16(static_cast<std::uint16_t>(config.bufferMaxMs));
    return ControlMessage(ControlType::AudioConfig, w.take());
}

std::optional<AudioStreamConfig> ControlMessage::parseAudioConfig() const {
    AudioStreamConfig config;
    if (!applyAudioConfigTo(config)) return std::nullopt;
    return config;
}

bool ControlMessage::applyAudioConfigTo(AudioStreamConfig& target) const {
    if (type_ != ControlType::AudioConfig || data_.size() < 8) return false;
    ByteReader r(data_);
    target.sampleRate = r.getI32();
    target.bitsPerSample = r.getU8();
    target.channels = r.getU8();
    target.frameDurationMs = r.getU16();
    // Buffer settings only if present (new 14-byte format).
    if (data_.size() >= 14) {
        target.bufferTargetMs = r.getU16();
        target.bufferMinMs = r.getU16();
        target.bufferMaxMs = r.getU16();
    }
    return true;
}

// --- Latency ---

ControlMessage ControlMessage::latencyProbe(std::int64_t probeTimestamp) {
    ByteWriter w(8);
    w.putI64(probeTimestamp);
    return ControlMessage(ControlType::LatencyProbe, w.take());
}

ControlMessage ControlMessage::latencyResponse(std::int64_t originalTimestamp) {
    ByteWriter w(8);
    w.putI64(originalTimestamp);
    return ControlMessage(ControlType::LatencyResponse, w.take());
}

std::int64_t ControlMessage::parseLatencyTimestamp() const {
    if (data_.size() < 8) return 0;
    ByteReader r(data_);
    return r.getI64();
}

// --- Error ---

ControlMessage ControlMessage::error(const std::string& errorMessage) {
    return ControlMessage(ControlType::Error, strBytes(errorMessage));
}

std::optional<std::string> ControlMessage::parseErrorMessage() const {
    // CONNECT_REJECT: reason byte, length byte, message.
    if (type_ == ControlType::ConnectReject && data_.size() >= 2) {
        std::size_t msgLen = data_[1];
        if (msgLen > 0 && data_.size() >= 2 + msgLen) {
            return std::string(data_.begin() + 2, data_.begin() + 2 + msgLen);
        }
        // The reason code name if no message.
        return std::string(rejectReasonName(rejectReasonFromValue(data_[0])));
    }
    // ERROR: just the message.
    if (type_ == ControlType::Error && !data_.empty()) {
        return std::string(data_.begin(), data_.end());
    }
    return std::nullopt;
}

// --- NACK / control ACK ---

ControlMessage ControlMessage::nack(std::int32_t missingSeq) {
    ByteWriter w(4);
    w.putI32(missingSeq);
    return ControlMessage(ControlType::Nack, w.take());
}

std::int32_t ControlMessage::parseNackSequence() const {
    if (type_ != ControlType::Nack || data_.size() < 4) return -1;
    ByteReader r(data_);
    return r.getI32();
}

ControlMessage ControlMessage::controlAck(std::int32_t ackedSeq) {
    ByteWriter w(4);
    w.putI32(ackedSeq);
    return ControlMessage(ControlType::ControlAck, w.take());
}

std::int32_t ControlMessage::parseControlAckSequence() const {
    if (type_ != ControlType::ControlAck || data_.size() < 4) return -1;
    ByteReader r(data_);
    return r.getI32();
}

// --- TX channel ---

ControlMessage ControlMessage::txDenied(const std::string& holdingClientId) {
    return ControlMessage(ControlType::TxDenied, strBytes(holdingClientId));
}

ControlMessage ControlMessage::txPreempted(const std::string& preemptingClientId) {
    return ControlMessage(ControlType::TxPreempted, strBytes(preemptingClientId));
}

std::optional<std::string> ControlMessage::parseTxClientId() const {
    if ((type_ != ControlType::TxDenied && type_ != ControlType::TxPreempted) || data_.empty()) {
        return std::nullopt;
    }
    return std::string(data_.begin(), data_.end());
}

// --- Clients update ---

ControlMessage ControlMessage::clientsUpdate(std::int32_t clientCount, std::int32_t maxClients,
                                             const std::string& txOwner,
                                             const std::vector<std::string>& clientIds) {
    return clientsUpdateWithInfo(clientCount, maxClients, txOwner, clientIds, nullptr);
}

ControlMessage ControlMessage::clientsUpdateWithInfo(
    std::int32_t clientCount, std::int32_t maxClients, const std::string& txOwner,
    const std::vector<std::string>& clientIds,
    const std::map<std::string, ClientInfo>* clientInfoMap) {
    auto txOwnerBytes = strBytes(txOwner);

    // Pre-serialize all client entries.
    std::vector<std::vector<std::uint8_t>> entries;
    entries.reserve(clientIds.size());
    for (const auto& id : clientIds) {
        std::vector<std::uint8_t> infoBytes;
        if (clientInfoMap != nullptr) {
            auto it = clientInfoMap->find(id);
            if (it != clientInfoMap->end()) infoBytes = serializeClientInfo(&it->second);
        }
        ByteWriter e(2 + id.size() + infoBytes.size());
        e.putU8(static_cast<std::uint8_t>(id.size()));
        e.putBytes(reinterpret_cast<const std::uint8_t*>(id.data()), id.size());
        e.putU8(static_cast<std::uint8_t>(infoBytes.size()));
        e.putBytes(infoBytes);
        entries.push_back(e.take());
    }

    ByteWriter w;
    w.putU8(static_cast<std::uint8_t>(std::min<std::int32_t>(clientCount, 255)));
    w.putU8(static_cast<std::uint8_t>(std::min<std::int32_t>(maxClients, 255)));
    w.putU8(static_cast<std::uint8_t>(txOwnerBytes.size()));
    w.putBytes(txOwnerBytes);
    w.putU8(static_cast<std::uint8_t>(entries.size()));
    for (const auto& e : entries) w.putBytes(e);
    return ControlMessage(ControlType::ClientsUpdate, w.take());
}

std::optional<ClientsUpdateInfo> ControlMessage::parseClientsUpdate() const {
    if (type_ != ControlType::ClientsUpdate || data_.size() < 4) return std::nullopt;
    ByteReader r(data_);
    ClientsUpdateInfo info;
    info.clientCount = r.getU8();
    info.maxClients = r.getU8();

    std::size_t txOwnerLen = r.getU8();
    if (txOwnerLen > 0 && r.remaining() >= txOwnerLen) {
        info.txOwner = r.readString(txOwnerLen);
    }

    if (r.remaining() >= 1) {
        std::size_t numClients = r.getU8();
        std::size_t i = 0;
        while (i < numClients && r.remaining() >= 1) {
            std::size_t idLen = r.getU8();
            if (r.remaining() < idLen) break;
            std::string clientId = r.readString(idLen);
            info.clientIds.push_back(clientId);

            // Read client info if present — enforce infoLen as a framing boundary.
            if (r.remaining() >= 1) {
                std::size_t infoLen = r.getU8();
                if (infoLen > 0 && r.remaining() >= infoLen) {
                    std::size_t startPos = r.position();
                    auto ci = deserializeClientInfo(r);
                    if (ci) info.clientInfoMap[clientId] = *ci;
                    // Advance exactly infoLen bytes regardless of what was consumed.
                    r.setPosition(startPos + infoLen);
                }
            }
            ++i;
        }
    }
    return info;
}

}  // namespace naudio
