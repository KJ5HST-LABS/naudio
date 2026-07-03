// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — control-plane codec (ControlMessage).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
//
// Pins the 4 golden `control` vectors byte-for-byte and round-trips the wider
// ControlMessage surface (connect request config/client-info, clients-update
// framing, error text, TX client id, latency).
#include "naudio/ControlMessage.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using naudio::AudioStreamConfig;
using naudio::ClientInfo;
using naudio::ControlMessage;
using naudio::ControlType;
using naudio::RejectReason;

namespace {

std::string hex(const std::vector<std::uint8_t>& b) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    for (std::uint8_t x : b) {
        s.push_back(kHex[x >> 4]);
        s.push_back(kHex[x & 0xF]);
    }
    return s;
}

}  // namespace

// --- The 4 conformance `control` vectors, byte-exact ---

TEST(ControlMessage, AudioConfigDefaultVector) {
    auto m = ControlMessage::audioConfig(AudioStreamConfig{});
    EXPECT_EQ("040000bb801002001400640028012c", hex(m.serialize()));
    // Round-trip the parse.
    auto decoded = ControlMessage::deserialize(m.serialize());
    ASSERT_TRUE(decoded.has_value());
    auto cfg = decoded->parseAudioConfig();
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(48000, cfg->sampleRate);
    EXPECT_EQ(16, cfg->bitsPerSample);
    EXPECT_EQ(2, cfg->channels);
    EXPECT_EQ(20, cfg->frameDurationMs);
    EXPECT_EQ(100, cfg->bufferTargetMs);
    EXPECT_EQ(40, cfg->bufferMinMs);
    EXPECT_EQ(300, cfg->bufferMaxMs);
}

TEST(ControlMessage, NackVector) {
    auto m = ControlMessage::nack(12345);
    EXPECT_EQ("5000003039", hex(m.serialize()));
    auto decoded = ControlMessage::deserialize(m.serialize());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(12345, decoded->parseNackSequence());
}

TEST(ControlMessage, ControlAckVector) {
    auto m = ControlMessage::controlAck(67890);
    EXPECT_EQ("5100010932", hex(m.serialize()));
    auto decoded = ControlMessage::deserialize(m.serialize());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(67890, decoded->parseControlAckSequence());
}

TEST(ControlMessage, ConnectRejectBusyVector) {
    auto m = ControlMessage::connectReject(RejectReason::Busy, "Server is busy");
    EXPECT_EQ("03010e5365727665722069732062757379", hex(m.serialize()));
    auto decoded = ControlMessage::deserialize(m.serialize());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ("Server is busy", decoded->parseErrorMessage().value());
}

// --- Wider surface round-trips ---

TEST(ControlMessage, DeserializeRejectsEmptyAndUnknownType) {
    EXPECT_FALSE(ControlMessage::deserialize(std::vector<std::uint8_t>{}).has_value());
    EXPECT_FALSE(ControlMessage::deserialize(std::vector<std::uint8_t>{0x99, 0x01}).has_value());
}

TEST(ControlMessage, NackWrongTypeSentinel) {
    EXPECT_EQ(-1, ControlMessage::heartbeat().parseNackSequence());
    EXPECT_EQ(-1, ControlMessage::heartbeat().parseControlAckSequence());
}

TEST(ControlMessage, ConnectRejectNoMessageReturnsReasonName) {
    auto m = ControlMessage::connectReject(RejectReason::VersionMismatch);
    EXPECT_EQ("VERSION_MISMATCH", m.parseErrorMessage().value());
}

TEST(ControlMessage, ErrorMessageRoundTrip) {
    auto m = ControlMessage::error("Test error message");
    EXPECT_EQ(ControlType::Error, m.messageType());
    auto decoded = ControlMessage::deserialize(m.serialize());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ("Test error message", decoded->parseErrorMessage().value());
}

TEST(ControlMessage, LatencyProbeRoundTrip) {
    auto m = ControlMessage::latencyProbe(9876543210LL);
    auto decoded = ControlMessage::deserialize(m.serialize());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(9876543210LL, decoded->parseLatencyTimestamp());
}

TEST(ControlMessage, ConnectRequestWithConfigRoundTrip) {
    AudioStreamConfig requested;
    requested.bufferTargetMs = 55;
    requested.bufferMinMs = 33;
    requested.bufferMaxMs = 111;
    auto m = ControlMessage::connectRequestWithConfig("cli", 1, &requested);
    auto parsed = m.parseConnectRequestConfig();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(55, parsed->bufferTargetMs);
    EXPECT_EQ(33, parsed->bufferMinMs);
    EXPECT_EQ(111, parsed->bufferMaxMs);
    // A plain connect request (no config) parses back to nullopt.
    EXPECT_FALSE(ControlMessage::connectRequest("cli", 1).parseConnectRequestConfig().has_value());
}

TEST(ControlMessage, ConnectRequestClientInfoRoundTrip) {
    ClientInfo info{"N0CALL", "Alice", "Testville"};
    auto m = ControlMessage::connectRequestFull("cli", 1, nullptr, &info);
    auto parsed = m.parseConnectRequestClientInfo();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ("N0CALL", parsed->callsign);
    EXPECT_EQ("Alice", parsed->name);
    EXPECT_EQ("Testville", parsed->location);
    EXPECT_EQ("N0CALL (Alice, Testville)", parsed->displayString());
}

TEST(ControlMessage, TxClientIdRoundTrip) {
    EXPECT_EQ("N0CALL", ControlMessage::txDenied("N0CALL").parseTxClientId().value());
    EXPECT_EQ("W1AW", ControlMessage::txPreempted("W1AW").parseTxClientId().value());
    EXPECT_FALSE(ControlMessage::txDenied().parseTxClientId().has_value());
    EXPECT_FALSE(ControlMessage::heartbeat().parseTxClientId().has_value());
}

TEST(ControlMessage, ClientsUpdateRoundTrip) {
    std::vector<std::string> ids = {"alice", "bob"};
    std::map<std::string, ClientInfo> infoMap;
    infoMap["alice"] = ClientInfo{"N0CALL", "", ""};
    auto m = ControlMessage::clientsUpdateWithInfo(2, 4, "alice", ids, &infoMap);
    auto parsed = m.parseClientsUpdate();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(2, parsed->clientCount);
    EXPECT_EQ(4, parsed->maxClients);
    ASSERT_TRUE(parsed->txOwner.has_value());
    EXPECT_EQ("alice", parsed->txOwner.value());
    EXPECT_EQ(ids, parsed->clientIds);
    EXPECT_EQ("N0CALL", parsed->getClientDisplayString("alice"));
    EXPECT_EQ("bob", parsed->getClientDisplayString("bob"));  // no info → id itself
}

TEST(ControlMessage, ClientInfoDisplayStringVariants) {
    EXPECT_EQ("", (ClientInfo{"", "", ""}).displayString());
    EXPECT_EQ("N0CALL", (ClientInfo{"N0CALL", "", ""}).displayString());
    EXPECT_EQ("N0CALL (Testville)", (ClientInfo{"N0CALL", "", "Testville"}).displayString());
    EXPECT_EQ("Alice (Testville)", (ClientInfo{"", "Alice", "Testville"}).displayString());
    EXPECT_EQ("Testville", (ClientInfo{"", "", "Testville"}).displayString());
}

TEST(ControlMessage, TypeAndRejectReasonRoundTrip) {
    for (auto t : {ControlType::ConnectRequest, ControlType::AudioConfig, ControlType::Nack,
                   ControlType::ControlAck, ControlType::Error, ControlType::Disconnect}) {
        auto back = naudio::controlTypeFromValue(static_cast<std::uint8_t>(t));
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(t, *back);
    }
    EXPECT_FALSE(naudio::controlTypeFromValue(0x77).has_value());
    for (auto r : {RejectReason::Busy, RejectReason::VersionMismatch, RejectReason::FormatNotSupported,
                   RejectReason::AuthFailed, RejectReason::Rejected}) {
        EXPECT_EQ(r, naudio::rejectReasonFromValue(static_cast<std::uint8_t>(r)));
    }
    // Unknown reason byte defaults to Rejected.
    EXPECT_EQ(RejectReason::Rejected, naudio::rejectReasonFromValue(0x55));
}
