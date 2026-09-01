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
    Discover = 0x60,         // Client probe for servers on the segment (spec 1.4, §6.8).
    DiscoverReply = 0x61,    // Server's unicast answer to a DISCOVER (spec 1.4, §6.8).
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

// Channel-layout wire values for the spec-1.2 (§6.2.1) per-subscription RX
// format request. Layouts 1-3 reduce native stereo to one payload channel.
enum class RxLayout : std::uint8_t {
    Native = 0,       // No layout change.
    MonoDownmix = 1,  // (L+R)/2 of the native stereo.
    LeftOnly = 2,     // Native channel 0 (e.g. VFO-A).
    RightOnly = 3,    // Native channel 1 (e.g. VFO-B).
};

// The §6.2.1 CONNECT_REQUEST tail: a requested RX rate + channel layout.
// `layout` is kept as the raw wire byte (not RxLayout) because unknown values
// can arrive from the wire and the server declines them rather than the codec
// rejecting them.
struct RxFormatRequest {
    std::uint32_t sampleRate = 0;  // 0 = no rate preference (keep native).
    std::uint8_t layout = 0;       // RxLayout wire byte.
};

// Transport bits carried in a DISCOVER_REPLY (§6.8). A DUAL server sets both.
enum class DiscoveryTransport : std::uint8_t {
    Tcp = 0x01,
    Udp = 0x02,
};

// Parsed data from a DISCOVER_REPLY (§6.8, since spec 1.4).
//
// Deliberately narrow: this message is answerable to an unauthenticated stranger
// over a broadcast, so it carries what a chooser needs to pick a server and
// nothing that would reward harvesting it. The spec forbids extending it with
// client-identifying fields -- no roster, no callsigns, no clientInfo, no station
// location, no TX-owner state, no statistics.
struct DiscoveryInfo {
    std::uint32_t token = 0;      // Echoed from the DISCOVER that caused this reply.
    std::uint16_t port = 0;       // The port this server serves audio on.
    std::uint8_t transports = 0;  // DiscoveryTransport bitmask (DUAL sets both).
    std::uint32_t sampleRate = 0;
    std::uint8_t bitsPerSample = 0;
    std::uint8_t channels = 0;
    std::uint8_t clientCount = 0;  // Saturates at 255 on the wire.
    std::uint8_t maxClients = 0;   // Saturates at 255 on the wire.
    std::string name;              // Operator-supplied label; MAY be empty.

    // NOT a wire field. The spec omits the server's address deliberately (§6.8) --
    // the only trustworthy value is the source address of the reply datagram, so
    // the prober fills this in from the datagram, never from the payload.
    std::string host;
};

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
    // The spec-1.2 (§6.2.1) form: connectRequestFull plus the 5-byte format-request
    // tail (requestedRate u32 BE + requestedLayout u8) appended after clientInfo.
    static ControlMessage connectRequestV12(const std::string& clientName,
                                            std::uint8_t protocolVersion,
                                            const AudioStreamConfig* requestedConfig,
                                            const ClientInfo* clientInfo,
                                            const RxFormatRequest& formatRequest);
    // The §6.2.1 format request, if the message carries one. A trailing run
    // shorter than 5 bytes is NOT a format request (nullopt) — the tolerance
    // vectors pin that; a longer tail's extra bytes are ignored (§11).
    std::optional<RxFormatRequest> parseConnectRequestFormatRequest() const;

    // --- Connect accept/reject ---
    static ControlMessage connectAccept() { return ofType(ControlType::ConnectAccept); }
    static ControlMessage connectReject(RejectReason reason, const std::string& message = "");

    // --- Audio config ---
    static ControlMessage audioConfig(const AudioStreamConfig& config);
    std::optional<AudioStreamConfig> parseAudioConfig() const;
    // Applies the audio-format fields onto an existing config in place (handles
    // the old 8-byte and new 14-byte formats); true if it was a parseable AUDIO_CONFIG.
    bool applyAudioConfigTo(AudioStreamConfig& target) const;
    // The spec-1.2 (§6.2.1) extended AUDIO_CONFIG: the 14-byte form plus one
    // appended grantedLayout byte. Sent only on connections whose
    // CONNECT_REQUEST carried a format request; the granted rate/channels ride
    // the EXISTING fields of `config`.
    static ControlMessage audioConfigV12(const AudioStreamConfig& config,
                                         std::uint8_t grantedLayout);
    // grantedLayout if this is the 15-byte extended form; nullopt for the v1
    // 8/14-byte forms (the client-side "who answered" discriminator, §6.2.1).
    std::optional<std::uint8_t> parseAudioConfigGrantedLayout() const;

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

    // --- Discovery (§6.8, since spec 1.4) ---
    // The probe. `token` is opaque and echoed verbatim by every answering server;
    // a prober MUST ignore a reply carrying a token it did not send.
    static ControlMessage discover(std::uint32_t token);
    // The probe's token, or nullopt if this is not a well-formed DISCOVER.
    std::optional<std::uint32_t> parseDiscoverToken() const;
    // The answer. Sent by UNICAST to the prober's source address, never broadcast,
    // and it creates nothing: no connection, no client slot, no stream.
    // `serverName` is clamped to 255 bytes to fit its u8 length prefix.
    static ControlMessage discoverReply(std::uint32_t token, std::uint16_t port,
                                        std::uint8_t transports, std::uint32_t sampleRate,
                                        std::uint8_t bitsPerSample, std::uint8_t channels,
                                        std::uint8_t clientCount, std::uint8_t maxClients,
                                        const std::string& serverName);
    // Parsed reply, or nullopt if the type is wrong or the message is malformed
    // (short fixed part, or a nameLen the payload does not actually carry).
    // `host` is left empty -- the caller fills it from the datagram's source address.
    std::optional<DiscoveryInfo> parseDiscoverReply() const;

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
