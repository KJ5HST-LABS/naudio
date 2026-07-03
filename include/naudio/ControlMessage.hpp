// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — control-plane codec.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "naudio/AudioStreamConfig.hpp"

namespace naudio {

// Control message types — the control-plane envelope carried as the payload of a
// CONTROL-type AudioPacket.
enum class ControlType : std::uint8_t {
    ConnectRequest = 0x01,   // Client requesting connection.
    ConnectAccept = 0x02,    // Server accepting connection.
    ConnectReject = 0x03,    // Server rejecting connection.
    AudioConfig = 0x04,      // Audio configuration negotiation.
    StreamStart = 0x10,      // Start streaming audio.
    StreamStop = 0x11,       // Stop streaming audio.
    StreamPause = 0x12,      // Pause streaming (e.g., during PTT transition).
    StreamResume = 0x13,     // Resume streaming.
    Heartbeat = 0x20,        // Heartbeat/keepalive.
    HeartbeatAck = 0x21,     // Heartbeat response.
    LatencyProbe = 0x22,     // Latency probe request.
    LatencyResponse = 0x23,  // Latency probe response.
    StatsUpdate = 0x30,      // Statistics update.
    TxGranted = 0x40,        // TX channel granted to this client.
    TxDenied = 0x41,         // TX channel request denied.
    TxPreempted = 0x42,      // Client preempted by a higher priority client.
    TxReleased = 0x43,       // TX channel released.
    ClientsUpdate = 0x44,    // Client list update (broadcast).
    Nack = 0x50,             // Request retransmission of a missing control packet.
    ControlAck = 0x51,       // Acknowledge a critical control packet.
    Error = 0xFE,            // Error notification.
    Disconnect = 0xFF,       // Graceful disconnect.
};

// Resolves a control type from its byte value (nullopt if unknown; deserialize
// rejects in that case).
std::optional<ControlType> controlTypeFromValue(std::uint8_t value);
// The enum constant name.
const char* controlTypeName(ControlType type);

// Rejection reasons.
enum class RejectReason : std::uint8_t {
    Busy = 0x01,                // Server is busy with another client.
    VersionMismatch = 0x02,     // Protocol version mismatch.
    FormatNotSupported = 0x03,  // Audio format not supported.
    AuthFailed = 0x04,          // Authentication failed.
    Rejected = 0xFF,            // Generic rejection.
};

// Resolves a reason from its byte value, defaulting to Rejected for an unknown
// value (never nullopt).
RejectReason rejectReasonFromValue(std::uint8_t value);
const char* rejectReasonName(RejectReason reason);

// Client identification (callsign, name, location) so connected clients can see
// who they share the radio with. Empty strings stand in for absent fields.
struct ClientInfo {
    std::string callsign;
    std::string name;
    std::string location;

    bool isEmpty() const { return callsign.empty() && name.empty() && location.empty(); }
    // The most specific identification available: "callsign (name, location)".
    std::string displayString() const;
};

// Parsed data from a CLIENTS_UPDATE message.
struct ClientsUpdateInfo {
    std::int32_t clientCount = 0;
    std::int32_t maxClients = 0;
    std::optional<std::string> txOwner;
    std::vector<std::string> clientIds;
    std::map<std::string, ClientInfo> clientInfoMap;

    // The display string for a client ID (its ClientInfo display string if
    // available, otherwise the ID itself).
    std::string getClientDisplayString(const std::string& clientId) const;
};

// A control message: a type tag plus an opaque type-specific data payload.
class ControlMessage {
public:
    ControlMessage(ControlType type, std::vector<std::uint8_t> data)
        : type_(type), data_(std::move(data)) {}
    // A control message with no data.
    static ControlMessage ofType(ControlType type) { return ControlMessage(type, {}); }

    // Serializes the control message ([type byte] + data).
    std::vector<std::uint8_t> serialize() const;
    // Deserializes a control message (nullopt if empty or an unknown type byte).
    static std::optional<ControlMessage> deserialize(const std::uint8_t* payload, std::size_t len);
    static std::optional<ControlMessage> deserialize(const std::vector<std::uint8_t>& payload);

    // --- Connect request ---
    static ControlMessage connectRequest(const std::string& clientName, std::uint8_t protocolVersion);
    static ControlMessage connectRequestWithConfig(const std::string& clientName,
                                                   std::uint8_t protocolVersion,
                                                   const AudioStreamConfig* requestedConfig);
    static ControlMessage connectRequestFull(const std::string& clientName,
                                             std::uint8_t protocolVersion,
                                             const AudioStreamConfig* requestedConfig,
                                             const ClientInfo* clientInfo);
    std::optional<AudioStreamConfig> parseConnectRequestConfig() const;
    std::optional<ClientInfo> parseConnectRequestClientInfo() const;

    // --- Connect accept/reject ---
    static ControlMessage connectAccept() { return ofType(ControlType::ConnectAccept); }
    static ControlMessage connectReject(RejectReason reason, const std::string& message = "");

    // --- Audio config ---
    static ControlMessage audioConfig(const AudioStreamConfig& config);
    std::optional<AudioStreamConfig> parseAudioConfig() const;
    // Applies the audio-format fields onto an existing config in place (handles
    // the old 8-byte and new 14-byte formats); true if it was a parseable AUDIO_CONFIG.
    bool applyAudioConfigTo(AudioStreamConfig& target) const;

    // --- Stream control ---
    static ControlMessage streamStart() { return ofType(ControlType::StreamStart); }
    static ControlMessage streamStop() { return ofType(ControlType::StreamStop); }
    static ControlMessage streamPause() { return ofType(ControlType::StreamPause); }
    static ControlMessage streamResume() { return ofType(ControlType::StreamResume); }

    // --- Heartbeat / latency ---
    static ControlMessage heartbeat() { return ofType(ControlType::Heartbeat); }
    static ControlMessage heartbeatAck() { return ofType(ControlType::HeartbeatAck); }
    static ControlMessage latencyProbe(std::int64_t probeTimestamp);
    static ControlMessage latencyResponse(std::int64_t originalTimestamp);
    std::int64_t parseLatencyTimestamp() const;

    // --- Error / disconnect ---
    static ControlMessage error(const std::string& errorMessage = "");
    std::optional<std::string> parseErrorMessage() const;
    static ControlMessage disconnect() { return ofType(ControlType::Disconnect); }

    // --- NACK / control ACK ---
    static ControlMessage nack(std::int32_t missingSeq);
    std::int32_t parseNackSequence() const;
    static ControlMessage controlAck(std::int32_t ackedSeq);
    std::int32_t parseControlAckSequence() const;

    // --- TX channel ---
    static ControlMessage txGranted() { return ofType(ControlType::TxGranted); }
    static ControlMessage txDenied(const std::string& holdingClientId = "");
    static ControlMessage txPreempted(const std::string& preemptingClientId = "");
    static ControlMessage txReleased() { return ofType(ControlType::TxReleased); }
    std::optional<std::string> parseTxClientId() const;

    // --- Clients update ---
    static ControlMessage clientsUpdate(std::int32_t clientCount, std::int32_t maxClients,
                                        const std::string& txOwner,
                                        const std::vector<std::string>& clientIds);
    static ControlMessage clientsUpdateWithInfo(
        std::int32_t clientCount, std::int32_t maxClients, const std::string& txOwner,
        const std::vector<std::string>& clientIds,
        const std::map<std::string, ClientInfo>* clientInfoMap);
    std::optional<ClientsUpdateInfo> parseClientsUpdate() const;

    // --- Getters ---
    ControlType messageType() const { return type_; }
    const std::vector<std::uint8_t>& data() const { return data_; }

private:
    ControlType type_;
    std::vector<std::uint8_t> data_;
};

}  // namespace naudio
