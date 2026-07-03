// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — conformance harness.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Validates the naudio codec against the language-neutral golden vectors in
// `conformance/vectors/vectors.ini`. The vectors are independently derived
// (CRC32 from python's zlib, NOT any implementation under test), so reproducing
// their exact bytes is the conformance proof for the spec.
//
// This harness checks the 13 CONTRACT vectors (8 frame + 4 control + 1 crc32). The
// 8 reliability vectors (fec_encode/fec_recover/reorder/jitter) are exercised by the
// dedicated GTests (test_fec/test_reorder/test_jitter) and are SKIPPED here —
// counted, not failed.
//
// Locating the vectors: NA_CONFORMANCE_DIR env var, else the sibling default
// baked in at configure time (NA_CONFORMANCE_DIR_DEFAULT). If absent the suite
// SKIPS (the C++ tree must stay buildable/testable without the conformance dir).
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "naudio/AudioPacket.hpp"
#include "naudio/AudioStreamConfig.hpp"
#include "naudio/ControlMessage.hpp"
#include "naudio/Crc32.hpp"
#include "naudio/FecDecoder.hpp"
#include "naudio/FecEncoder.hpp"
#include "naudio/JitterEstimator.hpp"
#include "naudio/PacketReorderBuffer.hpp"

#ifndef NA_CONFORMANCE_DIR_DEFAULT
#define NA_CONFORMANCE_DIR_DEFAULT ""
#endif

namespace {

using naudio::AudioPacket;
using naudio::AudioStreamConfig;
using naudio::ClientInfo;
using naudio::ControlMessage;
using naudio::PacketType;
using naudio::RejectReason;

using Section = std::map<std::string, std::string>;
using Vectors = std::vector<std::pair<std::string, Section>>;

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Minimal INI parser: ordered sections,
// key=value pairs, '#'-comments and blank lines skipped.
Vectors parseIni(const std::string& text) {
    Vectors out;
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            out.emplace_back(trim(line.substr(1, line.size() - 2)), Section{});
        } else {
            auto eq = line.find('=');
            if (eq != std::string::npos && !out.empty()) {
                out.back().second[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
            }
        }
    }
    return out;
}

std::string get(const Section& s, const std::string& key) {
    auto it = s.find(key);
    return it == s.end() ? "" : it->second;
}

// Requires a key; records a GTest failure and returns "" if missing.
std::string req(const Section& s, const std::string& key, bool& ok) {
    auto it = s.find(key);
    if (it == s.end()) {
        ADD_FAILURE() << "vector missing key: " << key;
        ok = false;
        return "";
    }
    return it->second;
}

std::vector<std::uint8_t> hexToBytes(const std::string& in) {
    std::string s = trim(in);
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::string toHex(const std::vector<std::uint8_t>& b) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    for (std::uint8_t x : b) {
        s.push_back(kHex[x >> 4]);
        s.push_back(kHex[x & 0xF]);
    }
    return s;
}

// Parses an int that may be decimal or 0x-prefixed hex (type/flags/controlType).
long intLoose(const std::string& in) {
    std::string s = trim(in);
    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
        return std::stol(s.substr(2), nullptr, 16);
    }
    return std::stol(s);
}

// Splits a comma-separated list (payloadsHex, inputSeqs, presentHex). An empty
// input yields one empty element; callers trim each element as needed.
std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (true) {
        std::size_t comma = s.find(',', pos);
        if (comma == std::string::npos) {
            out.push_back(trim(s.substr(pos)));
            break;
        }
        out.push_back(trim(s.substr(pos, comma - pos)));
        pos = comma + 1;
    }
    return out;
}

// ---- per-kind handlers (record GTest failures; SCOPED_TRACE supplies context) ----

void runFrame(const Section& v) {
    bool ok = true;
    auto frame = hexToBytes(req(v, "frameHex", ok));
    if (!ok) return;
    if (get(v, "expectDecode") == "reject") {
        EXPECT_FALSE(AudioPacket::deserialize(frame).has_value())
            << "expected decode to be rejected, but it parsed";
        return;
    }
    long type = intLoose(req(v, "type", ok));
    long seq = std::stol(req(v, "sequence", ok));
    long flags = intLoose(req(v, "flags", ok));
    long long ts = std::stoll(req(v, "timestamp", ok));
    auto payload = hexToBytes(get(v, "payloadHex"));
    if (!ok) return;

    // Decode.
    auto p = AudioPacket::deserialize(frame);
    ASSERT_TRUE(p.has_value()) << "decode returned nullopt for accept vector";
    EXPECT_EQ(type, static_cast<long>(static_cast<std::uint8_t>(p->packetType()))) << "type";
    EXPECT_EQ(seq, p->sequence()) << "sequence";
    EXPECT_EQ(flags, p->flags()) << "flags";
    EXPECT_EQ(ts, p->timestamp()) << "timestamp";
    EXPECT_EQ(payload, p->payload()) << "payload";

    // Encode (byte-exact).
    auto pt = naudio::packetTypeFromValue(static_cast<std::uint8_t>(type));
    ASSERT_TRUE(pt.has_value()) << "bad type value";
    AudioPacket e(*pt, static_cast<std::int32_t>(seq), payload);
    e.setFlags(static_cast<std::uint8_t>(flags));
    e.setTimestamp(static_cast<std::int64_t>(ts));
    EXPECT_EQ(toHex(frame), toHex(e.serialize())) << "serialize did not reproduce frameHex";
}

void runControl(const Section& v) {
    bool ok = true;
    long controlType = intLoose(req(v, "controlType", ok));
    auto expected = hexToBytes(req(v, "expectedPayloadHex", ok));
    if (!ok) return;
    switch (controlType) {
        case 0x04: {
            auto m = ControlMessage::audioConfig(AudioStreamConfig{});
            EXPECT_EQ(toHex(expected), toHex(m.serialize())) << "AUDIO_CONFIG serialize";
            EXPECT_TRUE(ControlMessage::deserialize(expected).has_value()) << "AUDIO_CONFIG decode";
            break;
        }
        case 0x50: {
            long seq = std::stol(req(v, "sequence", ok));
            auto m = ControlMessage::nack(static_cast<std::int32_t>(seq));
            EXPECT_EQ(toHex(expected), toHex(m.serialize())) << "NACK serialize";
            auto d = ControlMessage::deserialize(expected);
            ASSERT_TRUE(d.has_value()) << "NACK decode failed";
            EXPECT_EQ(seq, d->parseNackSequence()) << "NACK seq";
            break;
        }
        case 0x51: {
            long seq = std::stol(req(v, "sequence", ok));
            auto m = ControlMessage::controlAck(static_cast<std::int32_t>(seq));
            EXPECT_EQ(toHex(expected), toHex(m.serialize())) << "CONTROL_ACK serialize";
            auto d = ControlMessage::deserialize(expected);
            ASSERT_TRUE(d.has_value()) << "CONTROL_ACK decode failed";
            EXPECT_EQ(seq, d->parseControlAckSequence()) << "ACK seq";
            break;
        }
        case 0x03: {
            auto m = ControlMessage::connectReject(RejectReason::Busy, req(v, "message", ok));
            EXPECT_EQ(toHex(expected), toHex(m.serialize())) << "CONNECT_REJECT serialize";
            auto d = ControlMessage::deserialize(expected);
            ASSERT_TRUE(d.has_value()) << "CONNECT_REJECT decode failed";
            EXPECT_EQ(get(v, "message"), d->parseErrorMessage().value_or("")) << "CONNECT_REJECT message";
            break;
        }
        case 0x01: {  // CONNECT_REQUEST — minimal (no config) or with-config
            std::string clientName = req(v, "clientName", ok);
            long version = std::stol(req(v, "protocolVersion", ok));
            if (!ok) return;
            if (get(v, "hasConfig") == "1") {
                AudioStreamConfig cfg{};
                cfg.bufferTargetMs = static_cast<std::int32_t>(std::stol(req(v, "bufferTargetMs", ok)));
                cfg.bufferMinMs = static_cast<std::int32_t>(std::stol(req(v, "bufferMinMs", ok)));
                cfg.bufferMaxMs = static_cast<std::int32_t>(std::stol(req(v, "bufferMaxMs", ok)));
                if (!ok) return;
                auto m = ControlMessage::connectRequestWithConfig(
                    clientName, static_cast<std::uint8_t>(version), &cfg);
                EXPECT_EQ(toHex(expected), toHex(m.serialize())) << "CONNECT_REQUEST(config) serialize";
                auto d = ControlMessage::deserialize(expected);
                ASSERT_TRUE(d.has_value()) << "CONNECT_REQUEST decode failed";
                auto pc = d->parseConnectRequestConfig();
                ASSERT_TRUE(pc.has_value()) << "CONNECT_REQUEST config decode";
                EXPECT_EQ(cfg.bufferTargetMs, pc->bufferTargetMs) << "buffer target";
                EXPECT_EQ(cfg.bufferMinMs, pc->bufferMinMs) << "buffer min";
                EXPECT_EQ(cfg.bufferMaxMs, pc->bufferMaxMs) << "buffer max";
            } else {
                auto m = ControlMessage::connectRequest(clientName, static_cast<std::uint8_t>(version));
                EXPECT_EQ(toHex(expected), toHex(m.serialize())) << "CONNECT_REQUEST serialize";
                auto d = ControlMessage::deserialize(expected);
                ASSERT_TRUE(d.has_value()) << "CONNECT_REQUEST decode failed";
                EXPECT_FALSE(d->parseConnectRequestConfig().has_value()) << "no config expected";
            }
            break;
        }
        case 0x22:
        case 0x23: {  // LATENCY_PROBE / LATENCY_RESPONSE
            long long ts = std::stoll(req(v, "timestamp", ok));
            if (!ok) return;
            auto m = (controlType == 0x22)
                         ? ControlMessage::latencyProbe(static_cast<std::int64_t>(ts))
                         : ControlMessage::latencyResponse(static_cast<std::int64_t>(ts));
            EXPECT_EQ(toHex(expected), toHex(m.serialize())) << "LATENCY serialize";
            auto d = ControlMessage::deserialize(expected);
            ASSERT_TRUE(d.has_value()) << "LATENCY decode failed";
            EXPECT_EQ(ts, d->parseLatencyTimestamp()) << "latency timestamp";
            break;
        }
        case 0xFE: {  // ERROR
            std::string message = req(v, "message", ok);
            if (!ok) return;
            auto m = ControlMessage::error(message);
            EXPECT_EQ(toHex(expected), toHex(m.serialize())) << "ERROR serialize";
            auto d = ControlMessage::deserialize(expected);
            ASSERT_TRUE(d.has_value()) << "ERROR decode failed";
            EXPECT_EQ(message, d->parseErrorMessage().value_or("")) << "ERROR message";
            break;
        }
        case 0x41:
        case 0x42: {  // TX_DENIED / TX_PREEMPTED
            std::string clientId = req(v, "clientId", ok);
            if (!ok) return;
            auto m = (controlType == 0x41) ? ControlMessage::txDenied(clientId)
                                           : ControlMessage::txPreempted(clientId);
            EXPECT_EQ(toHex(expected), toHex(m.serialize())) << "TX serialize";
            auto d = ControlMessage::deserialize(expected);
            ASSERT_TRUE(d.has_value()) << "TX decode failed";
            EXPECT_EQ(clientId, d->parseTxClientId().value_or("")) << "TX client id";
            break;
        }
        case 0x44: {  // CLIENTS_UPDATE — the W1 clamp round-trip
            long clientCount = std::stol(req(v, "clientCount", ok));
            long maxClients = std::stol(req(v, "maxClients", ok));
            std::string txOwner = req(v, "txOwner", ok);
            std::string clientId = req(v, "clientId", ok);
            if (!ok) return;
            std::map<std::string, ClientInfo> infoMap;
            infoMap[clientId] =
                ClientInfo{get(v, "infoCallsign"), get(v, "infoName"), get(v, "infoLocation")};
            std::vector<std::string> ids{clientId};
            auto m = ControlMessage::clientsUpdateWithInfo(
                static_cast<std::int32_t>(clientCount), static_cast<std::int32_t>(maxClients),
                txOwner, ids, &infoMap);
            EXPECT_EQ(toHex(expected), toHex(m.serialize())) << "CLIENTS_UPDATE serialize (clamped)";
            auto d = ControlMessage::deserialize(expected);
            ASSERT_TRUE(d.has_value()) << "CLIENTS_UPDATE decode failed";
            auto cu = d->parseClientsUpdate();
            ASSERT_TRUE(cu.has_value()) << "CLIENTS_UPDATE parse failed";
            EXPECT_EQ(clientCount, cu->clientCount) << "clientCount";
            EXPECT_EQ(maxClients, cu->maxClients) << "maxClients";
            ASSERT_EQ(1u, cu->clientIds.size()) << "clientIds size";
            EXPECT_EQ(clientId, cu->clientIds[0]) << "clientId";
            auto it = cu->clientInfoMap.find(clientId);
            ASSERT_TRUE(it != cu->clientInfoMap.end()) << "clientInfo present";
            EXPECT_EQ(get(v, "expectedCallsign"), it->second.callsign) << "clamped callsign";
            EXPECT_EQ(get(v, "expectedName"), it->second.name) << "clamped name";
            EXPECT_EQ(get(v, "expectedLocation"), it->second.location) << "clamped location";
            break;
        }
        default:
            ADD_FAILURE() << "unhandled controlType 0x" << std::hex << controlType;
    }
}

void runCrc32(const Section& v) {
    bool ok = true;
    auto input = hexToBytes(req(v, "inputHex", ok));
    std::string expectedStr = req(v, "expected", ok);
    if (!ok) return;
    std::uint32_t expected = static_cast<std::uint32_t>(std::stoul(trim(expectedStr), nullptr, 16));
    EXPECT_EQ(expected, naudio::crc32(input)) << "CRC32 KAT";
}

// Jitter vectors are structural-only (empty -> -1, first-packet -> minMs): the
// arrival timestamps don't affect the asserted fields, so we drive the testable
// recordArrivalAt(send, arrival) form with (0, 0) per packet.
void runJitter(const Section& v) {
    bool ok = true;
    long minMs = std::stol(req(v, "minMs", ok));
    long maxMs = std::stol(req(v, "maxMs", ok));
    double mult = std::stod(req(v, "multiplier", ok));
    long packets = std::stol(req(v, "packets", ok));
    std::string expectedTarget = req(v, "expectedTargetMs", ok);
    std::string expectedCount = req(v, "expectedPacketCount", ok);
    if (!ok) return;
    naudio::JitterEstimator j(static_cast<std::int32_t>(minMs), static_cast<std::int32_t>(maxMs), mult);
    for (long i = 0; i < packets; ++i) j.recordArrivalAt(0, 0);
    EXPECT_EQ(std::stol(expectedTarget), static_cast<long>(j.adaptiveBufferTargetMs()))
        << "adaptive target";
    EXPECT_EQ(std::stoll(expectedCount), static_cast<long long>(j.packetCount())) << "packet count";
}

// Drives the encoder with the block's payloads and asserts the emitted parity
// packet's payload is byte-exact.
void runFecEncode(const Section& v) {
    bool ok = true;
    long blockSize = std::stol(req(v, "blockSize", ok));
    long start = std::stol(req(v, "startSequence", ok));
    std::string payloadsHex = req(v, "payloadsHex", ok);
    auto expectedParity = hexToBytes(req(v, "expectedParityPayloadHex", ok));
    if (!ok) return;
    naudio::FecEncoder enc(static_cast<std::size_t>(blockSize));
    std::optional<AudioPacket> parity;
    long i = 0;
    for (const std::string& ph : splitCsv(payloadsHex)) {
        auto pkt = AudioPacket::createRxAudio(static_cast<std::int32_t>(start + i), hexToBytes(ph));
        auto emitted = enc.recordAndMaybeEmit(pkt);
        if (emitted.has_value()) parity = std::move(emitted);
        ++i;
    }
    ASSERT_TRUE(parity.has_value()) << "encoder did not emit a parity packet";
    EXPECT_EQ(toHex(expectedParity), toHex(parity->payload())) << "parity payload";
}

// Inserts the input sequence numbers and asserts the emitted order (GAP = a NULL
// silence gap) plus the reordered/gaps/dropped-late counters. windowSize-based
// force-flush only (maxHoldMs=0,
// no timeout path), so it runs without a clock.
void runReorder(const Section& v) {
    bool ok = true;
    long window = std::stol(req(v, "windowSize", ok));
    long maxHold = std::stol(req(v, "maxHoldMs", ok));
    std::string inputSeqs = req(v, "inputSeqs", ok);
    std::string expectedEmitted = req(v, "expectedEmitted", ok);
    std::string expReordered = req(v, "expectedReordered", ok);
    std::string expGaps = req(v, "expectedGaps", ok);
    std::string expDropped = req(v, "expectedDroppedLate", ok);
    if (!ok) return;

    std::vector<std::string> emitted;
    naudio::PacketReorderBuffer rb(
        static_cast<std::size_t>(window), static_cast<std::int64_t>(maxHold),
        [&emitted](const AudioPacket* p) {
            emitted.push_back(p == nullptr ? std::string("GAP") : std::to_string(p->sequence()));
        });
    for (const std::string& s : splitCsv(inputSeqs)) {
        rb.insert(AudioPacket::createRxAudio(static_cast<std::int32_t>(std::stol(s)), {}));
    }

    std::string joined;
    for (std::size_t i = 0; i < emitted.size(); ++i) {
        if (i != 0) joined += ",";
        joined += emitted[i];
    }
    EXPECT_EQ(expectedEmitted, joined) << "emitted order";
    EXPECT_EQ(std::stoll(expReordered), static_cast<long long>(rb.packetsReordered())) << "reordered";
    EXPECT_EQ(std::stoll(expGaps), static_cast<long long>(rb.gapsEmitted())) << "gaps";
    EXPECT_EQ(std::stoll(expDropped), static_cast<long long>(rb.packetsDroppedLate())) << "dropped-late";
}

// Feeds the block's present packets + a known gap + the parity, then asserts the
// FEC-recovered packet (the one with sequence == missing) is byte-exact.
void runFecRecover(const Section& v) {
    bool ok = true;
    long blockSize = std::stol(req(v, "blockSize", ok));
    long start = std::stol(req(v, "startSequence", ok));
    long missing = std::stol(req(v, "missingSequence", ok));
    std::string presentHex = req(v, "presentHex", ok);
    auto parityPayload = hexToBytes(req(v, "parityPayloadHex", ok));
    auto expectedRecovered = hexToBytes(req(v, "expectedRecoveredHex", ok));
    if (!ok) return;

    // present: "seq:hex,seq:hex,..."
    std::map<long, std::vector<std::uint8_t>> present;
    for (const std::string& pair : splitCsv(presentHex)) {
        auto colon = pair.find(':');
        if (colon == std::string::npos) {
            ADD_FAILURE() << "bad present pair: " << pair;
            return;
        }
        present[std::stol(pair.substr(0, colon))] = hexToBytes(pair.substr(colon + 1));
    }

    std::vector<AudioPacket> emitted;  // non-null emits, copied for inspection
    naudio::FecDecoder dec([&emitted](const AudioPacket* p) {
        if (p != nullptr) emitted.push_back(*p);
    });
    for (long seq = start; seq < start + blockSize; ++seq) {
        auto it = present.find(seq);
        if (it != present.end()) {
            dec.processPacket(AudioPacket::createRxAudio(static_cast<std::int32_t>(seq), it->second));
        } else if (seq == missing) {
            dec.process(std::nullopt, static_cast<std::int32_t>(missing));
        }
    }
    AudioPacket parity(PacketType::FecParity, static_cast<std::int32_t>(start), parityPayload);
    dec.processPacket(std::move(parity));

    const AudioPacket* recovered = nullptr;
    for (const auto& p : emitted) {
        if (p.sequence() == missing) {
            recovered = &p;
            break;
        }
    }
    ASSERT_TRUE(recovered != nullptr) << "missing packet was not recovered";
    EXPECT_EQ(toHex(expectedRecovered), toHex(recovered->payload())) << "recovered payload";
}

std::string locateVectors() {
    const char* env = std::getenv("NA_CONFORMANCE_DIR");
    std::string dir = (env != nullptr && env[0] != '\0') ? std::string(env)
                                                         : std::string(NA_CONFORMANCE_DIR_DEFAULT);
    if (dir.empty()) return "";
    std::string file = dir + "/vectors/vectors.ini";
    std::ifstream f(file);
    return f.good() ? file : "";
}

}  // namespace

TEST(Conformance, GoldenVectors) {
    std::string path = locateVectors();
    // W2: the golden vectors are vendored in-repo and git-tracked, so their absence is
    // a real failure (broken checkout / misconfigured NA_CONFORMANCE_DIR), NOT an
    // environment quirk to skip past. Fail closed — a green run must mean the wire was
    // actually checked, never that the suite quietly asserted nothing.
    ASSERT_FALSE(path.empty())
        << "conformance vectors not found at NA_CONFORMANCE_DIR / the vendored default. "
           "They are vendored in-repo under conformance/vectors/vectors.ini; set "
           "NA_CONFORMANCE_DIR if running from an unusual layout.";
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    auto vectors = parseIni(ss.str());
    ASSERT_FALSE(vectors.empty()) << "no vectors parsed from " << path;

    int ran = 0;
    int skipped = 0;
    for (const auto& [name, sec] : vectors) {
        SCOPED_TRACE("vector [" + name + "]");
        std::string kind = get(sec, "kind");
        if (kind == "frame") {
            runFrame(sec);
            ++ran;
        } else if (kind == "control") {
            runControl(sec);
            ++ran;
        } else if (kind == "crc32") {
            runCrc32(sec);
            ++ran;
        } else if (kind == "jitter") {
            runJitter(sec);
            ++ran;
        } else if (kind == "fec_encode") {
            runFecEncode(sec);
            ++ran;
        } else if (kind == "reorder") {
            runReorder(sec);
            ++ran;
        } else if (kind == "fec_recover") {
            runFecRecover(sec);
            ++ran;
        } else {
            ADD_FAILURE() << "unknown kind: " << kind;
        }
    }
    std::cerr << "conformance: " << ran << " ran, " << skipped << " skipped\n";
    // Full gate: all 29 vectors run, 0 skipped — 21 contract (8 frame + 12 control
    // + 1 crc32) + 8 reliability (2 jitter + 2 fec_encode + 1 fec_recover + 3
    // reorder). The 12 control vectors now cover every control encoding (audio
    // config, nack, ack, connect-reject, connect-request ±config, latency
    // probe/response, error, tx denied/preempted, and the clamped clients-update).
    EXPECT_EQ(29, ran) << "expected all 29 vectors to run";
    EXPECT_EQ(0, skipped) << "expected 0 skipped vectors";
}
