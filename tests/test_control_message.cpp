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

// --- Spec 1.2 (§6.2.1) format-request codec, pinned to the vectors-v1_2.ini ---
// golden hex (authored independently by conformance/tools/gen_vectors.py at the
// B1 spec revision — matching it is a cross-check, not self-confirmation).

TEST(ControlMessage, ConnectRequestV12FormatVector) {
    naudio::RxFormatRequest req{12000, 1};
    auto m = ControlMessage::connectRequestV12("W1AW-client", 1, nullptr, nullptr, req);
    EXPECT_EQ("01010b573141572d636c69656e74000000002ee001", hex(m.serialize()));
    auto d = ControlMessage::deserialize(m.serialize());
    ASSERT_TRUE(d.has_value());
    auto fr = d->parseConnectRequestFormatRequest();
    ASSERT_TRUE(fr.has_value());
    EXPECT_EQ(12000u, fr->sampleRate);
    EXPECT_EQ(1, fr->layout);
    // The v1 view of the same payload: no buffer config, no client info.
    EXPECT_FALSE(d->parseConnectRequestConfig().has_value());
    EXPECT_FALSE(d->parseConnectRequestClientInfo().has_value());
}

TEST(ControlMessage, ConnectRequestV12FullVector) {
    AudioStreamConfig cfg{};
    cfg.bufferTargetMs = 100;
    cfg.bufferMinMs = 40;
    cfg.bufferMaxMs = 300;
    ClientInfo info{"W1AW", "Hiram", "Newington CT"};
    naudio::RxFormatRequest req{12000, 2};
    auto m = ControlMessage::connectRequestV12("W1AW-client", 1, &cfg, &info, req);
    EXPECT_EQ(
        "01010b573141572d636c69656e740100640028012c18045731415705486972616d0c4e6577696e67746f6e2043"
        "5400002ee002",
        hex(m.serialize()));
    auto d = ControlMessage::deserialize(m.serialize());
    ASSERT_TRUE(d.has_value());
    // The 1.2 view: the tail parses.
    auto fr = d->parseConnectRequestFormatRequest();
    ASSERT_TRUE(fr.has_value());
    EXPECT_EQ(12000u, fr->sampleRate);
    EXPECT_EQ(2, fr->layout);
    // The v1 view is intact: buffer prefs and client info parse exactly as before.
    auto pc = d->parseConnectRequestConfig();
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(100, pc->bufferTargetMs);
    auto pi = d->parseConnectRequestClientInfo();
    ASSERT_TRUE(pi.has_value());
    EXPECT_EQ("W1AW", pi->callsign);
    EXPECT_EQ("Newington CT", pi->location);
}

TEST(ControlMessage, ConnectRequestV12ProbeVector) {
    // The 0,0 probe: a valid request for the native format unchanged.
    naudio::RxFormatRequest req{0, 0};
    auto m = ControlMessage::connectRequestV12("W1AW-client", 1, nullptr, nullptr, req);
    EXPECT_EQ("01010b573141572d636c69656e7400000000000000", hex(m.serialize()));
    auto fr = ControlMessage::deserialize(m.serialize())->parseConnectRequestFormatRequest();
    ASSERT_TRUE(fr.has_value());
    EXPECT_EQ(0u, fr->sampleRate);
    EXPECT_EQ(0, fr->layout);
}

TEST(ControlMessage, V1RequestCarriesNoFormatRequest) {
    auto m = ControlMessage::connectRequest("W1AW-client", 1);
    EXPECT_FALSE(m.parseConnectRequestFormatRequest().has_value());
}

TEST(ControlMessage, ShortTailIsNotAFormatRequest) {
    // The neg-short-tail tolerance vector: a 2-byte trailing run after the v1
    // body must NOT parse as a format request.
    const std::vector<std::uint8_t> payload = {
        0x01, 0x01, 0x0b, 0x57, 0x31, 0x41, 0x57, 0x2d, 0x63, 0x6c, 0x69, 0x65, 0x6e, 0x74,
        0x01, 0x00, 0x64, 0x00, 0x28, 0x01, 0x2c, 0x00, 0x2e, 0xe0};
    auto d = ControlMessage::deserialize(payload);
    ASSERT_TRUE(d.has_value());
    EXPECT_FALSE(d->parseConnectRequestFormatRequest().has_value());
    // The v1 view still parses.
    auto pc = d->parseConnectRequestConfig();
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(100, pc->bufferTargetMs);
}

TEST(ControlMessage, AudioConfigV12GrantedVector) {
    AudioStreamConfig cfg{};
    cfg.sampleRate = 12000;
    cfg.channels = 1;
    auto m = ControlMessage::audioConfigV12(cfg, 1);
    EXPECT_EQ("0400002ee01001001400640028012c01", hex(m.serialize()));
    auto d = ControlMessage::deserialize(m.serialize());
    ASSERT_TRUE(d.has_value());
    auto layout = d->parseAudioConfigGrantedLayout();
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(1, *layout);
    // The v1 view: the 14-byte prefix parses as before (granted fields ride it).
    AudioStreamConfig got{};
    ASSERT_TRUE(d->applyAudioConfigTo(got));
    EXPECT_EQ(12000, got.sampleRate);
    EXPECT_EQ(1, got.channels);
    EXPECT_EQ(20, got.frameDurationMs);
}

TEST(ControlMessage, AudioConfigV12DeclinedNativeVector) {
    auto m = ControlMessage::audioConfigV12(AudioStreamConfig{}, 0);
    EXPECT_EQ("040000bb801002001400640028012c00", hex(m.serialize()));
    auto layout = ControlMessage::deserialize(m.serialize())->parseAudioConfigGrantedLayout();
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(0, *layout);
}

TEST(ControlMessage, V1AudioConfigCarriesNoGrantedLayout) {
    // 14-byte form: the "who answered" discriminator must say v1.
    auto m = ControlMessage::audioConfig(AudioStreamConfig{});
    EXPECT_FALSE(m.parseAudioConfigGrantedLayout().has_value());
}

// --- Discovery (spec 1.4, §6.8) ---------------------------------------------
//
// The expected bytes below are derived BY HAND from §6.8's field table, not by
// printing what the encoder produced. A round-trip alone would pass just as
// happily against a wrong-but-self-consistent layout.

TEST(ControlMessage, DiscoverVector) {
    // 0x60 | token u32 BE. Nothing else -- a probe carries no identity.
    auto m = ControlMessage::discover(0xDEADBEEFu);
    EXPECT_EQ("60deadbeef", hex(m.serialize()));

    auto d = ControlMessage::deserialize(m.serialize());
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(ControlType::Discover, d->messageType());
    auto token = d->parseDiscoverToken();
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(0xDEADBEEFu, *token);
}

TEST(ControlMessage, DiscoverReplyVector) {
    // 0x61 | token deadbeef | port 4533=0x11b5 | transports 0x03 (TCP|UDP)
    //      | rate 48000=0x0000bb80 | bits 0x10 | ch 0x02 | clients 0x01
    //      | max 0x04 | nameLen 0x05 | "shack" = 736861636b
    auto m = ControlMessage::discoverReply(0xDEADBEEFu, 4533, 0x03, 48000, 16, 2, 1, 4, "shack");
    EXPECT_EQ("61deadbeef11b5030000bb801002010405736861636b", hex(m.serialize()));

    auto d = ControlMessage::deserialize(m.serialize());
    ASSERT_TRUE(d.has_value());
    ASSERT_EQ(ControlType::DiscoverReply, d->messageType());
    auto info = d->parseDiscoverReply();
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(0xDEADBEEFu, info->token);
    EXPECT_EQ(4533, info->port);
    EXPECT_EQ(0x03, info->transports);
    EXPECT_EQ(48000u, info->sampleRate);
    EXPECT_EQ(16, info->bitsPerSample);
    EXPECT_EQ(2, info->channels);
    EXPECT_EQ(1, info->clientCount);
    EXPECT_EQ(4, info->maxClients);
    EXPECT_EQ("shack", info->name);
    // The address is NOT a wire field (§6.8) -- the prober fills it from the
    // datagram's source address, so the codec must leave it empty.
    EXPECT_EQ("", info->host);
}

TEST(ControlMessage, DiscoverReplyEmptyNameIsTheMinimalValidForm) {
    auto m = ControlMessage::discoverReply(0, 4533, 0x02, 48000, 16, 2, 0, 4, "");
    // 1 type + 16 fixed, no name.
    EXPECT_EQ(17u, m.serialize().size());
    auto info = ControlMessage::deserialize(m.serialize())->parseDiscoverReply();
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ("", info->name);
    EXPECT_EQ(0, info->clientCount);
}

TEST(ControlMessage, DiscoverReplyNameCannotOverflowItsLengthPrefix) {
    // strBytes() does not clamp. A 300-byte name written behind a u8 prefix would
    // wrap to 300-256 = 44 and mis-frame the reply -- the CLIENTS_UPDATE W1 bug.
    const std::string huge(300, 'x');
    auto m = ControlMessage::discoverReply(1, 4533, 0x02, 48000, 16, 2, 0, 4, huge);
    const auto bytes = m.serialize();
    EXPECT_EQ(1u + 16u + 255u, bytes.size());
    EXPECT_EQ(255, bytes[16]);  // the nameLen byte: last of the 16-byte fixed part
    auto info = ControlMessage::deserialize(bytes)->parseDiscoverReply();
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(255u, info->name.size());
}

TEST(ControlMessage, DiscoverReplyRejectsANameLengthThePayloadDoesNotCarry) {
    // A declared name that is not there is malformed, not a tolerable trailing
    // extension (§11): the prefix promises bytes that are absent.
    auto good = ControlMessage::discoverReply(1, 4533, 0x02, 48000, 16, 2, 0, 4, "shack").serialize();
    ASSERT_TRUE(ControlMessage::deserialize(good)->parseDiscoverReply().has_value())
        << "control: the un-truncated reply must parse, or this test proves nothing";

    std::vector<std::uint8_t> truncated(good.begin(), good.end() - 3);  // nameLen still says 5
    auto d = ControlMessage::deserialize(truncated);
    ASSERT_TRUE(d.has_value()) << "the CONTROL envelope itself is still well-formed";
    EXPECT_FALSE(d->parseDiscoverReply().has_value());
}

TEST(ControlMessage, DiscoverReplyRejectsAShortFixedPart) {
    auto good = ControlMessage::discoverReply(1, 4533, 0x02, 48000, 16, 2, 0, 4, "").serialize();
    ASSERT_EQ(17u, good.size());
    std::vector<std::uint8_t> shortened(good.begin(), good.end() - 1);  // 15 data bytes, need 16
    EXPECT_FALSE(ControlMessage::deserialize(shortened)->parseDiscoverReply().has_value());
}

TEST(ControlMessage, DiscoveryParsersRejectTheWrongType) {
    // Each parser is keyed to its own type -- a DISCOVER is not half a reply.
    auto probe = ControlMessage::discover(7);
    EXPECT_FALSE(probe.parseDiscoverReply().has_value());
    auto reply = ControlMessage::discoverReply(7, 4533, 0x02, 48000, 16, 2, 0, 4, "");
    EXPECT_FALSE(reply.parseDiscoverToken().has_value());
    EXPECT_FALSE(ControlMessage::disconnect().parseDiscoverToken().has_value());
    EXPECT_FALSE(ControlMessage::disconnect().parseDiscoverReply().has_value());
}

TEST(ControlMessage, DiscoveryTypeBytesResolve) {
    // 0x60/0x61 must be known to the envelope decoder, or a DISCOVER from the wire
    // is rejected at deserialize() and never reaches the demux gate.
    auto probe = ControlMessage::deserialize(std::vector<std::uint8_t>{0x60, 0, 0, 0, 1});
    ASSERT_TRUE(probe.has_value());
    EXPECT_EQ(ControlType::Discover, probe->messageType());
    EXPECT_STREQ("DISCOVER", naudio::controlTypeName(ControlType::Discover));
    EXPECT_STREQ("DISCOVER_REPLY", naudio::controlTypeName(ControlType::DiscoverReply));
}
