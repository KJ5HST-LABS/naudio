// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — per-subscription RX format negotiation (spec 1.2, §6.2.1).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Server-side B3 (#91): the grant matrix (exact-grant-or-native, decline paths,
// the 15-byte-reply-only-to-requesters rule), converted-stream bit-exactness
// against the B2 conversion units, the native path's byte-identity while a
// converted subscriber shares the same broadcast, and FEC recovery of a
// converted subscription under induced loss (through the lossy relay fixture,
// client_stats_proxy.cpp — loopback drops nothing, L292).
//
// The local "expected" conversions below reuse naudio::Decimator/downmixToMono.
// That does NOT make the assertions circular: the units themselves are pinned
// bit-exactly by independent-reference goldens (tests/test_format_conversion.cpp);
// what THESE arms check is the server's wiring — grant bookkeeping, per-chunk
// streaming state, packet framing, ordering, and loss recovery around them.
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "naudio/AudioPacket.hpp"
#include "naudio/AudioStreamConfig.hpp"
#include "naudio/ControlMessage.hpp"
#include "naudio/FormatConversion.hpp"
#include "naudio/net/AudioStreamServer.hpp"
#include "naudio/net/TcpClientTransport.hpp"
#include "naudio/net/UdpClientConnection.hpp"
#include "naudio/net/UdpClientTransport.hpp"

using namespace naudio;
using namespace naudio::net;

// The lossy in-process UDP relay (tests/client_stats_proxy.cpp).
extern "C" {
void* naproxy_start(int server_port, int block_size, int drop_ordinal, int corrupt_ordinal,
                    int drop_parity, int* out_port);
long long naproxy_audio_seen(void* handle);
long long naproxy_audio_dropped(void* handle);
void naproxy_stop(void* handle);
}

namespace {

std::optional<AudioPacket> recvUntil(ClientConnection& c, PacketType type,
                                     std::optional<ControlType> ctype, int budgetMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        ReceiveResult r = c.receivePacket(100);
        if (r.closed) return std::nullopt;
        if (!r.hasPacket()) continue;
        if (r.packet->packetType() != type) continue;
        if (type == PacketType::Control && ctype.has_value()) {
            auto msg = ControlMessage::deserialize(r.packet->payload());
            if (!msg || msg->messageType() != *ctype) continue;
        }
        return std::move(r.packet);
    }
    return std::nullopt;
}

// What the server's AUDIO_CONFIG reply actually said, form and fields.
struct NegotiationReply {
    std::size_t configDataBytes = 0;  // 14 = v1 form, 15 = extended (§6.2.1)
    AudioStreamConfig config;
    std::optional<std::uint8_t> grantedLayout;
};

// Handshakes with an optional §6.2.1 format request and captures the reply.
std::optional<NegotiationReply> handshake(ClientConnection& c, const std::string& name,
                                          const std::optional<RxFormatRequest>& req) {
    const ControlMessage m =
        req.has_value()
            ? ControlMessage::connectRequestV12(name, AudioPacket::VERSION, nullptr, nullptr, *req)
            : ControlMessage::connectRequest(name, AudioPacket::VERSION);
    if (!c.sendControl(m)) return std::nullopt;
    auto cfgPacket = recvUntil(c, PacketType::Control, ControlType::AudioConfig, 3000);
    if (!cfgPacket.has_value()) return std::nullopt;
    auto cfgMsg = ControlMessage::deserialize(cfgPacket->payload());
    if (!cfgMsg.has_value()) return std::nullopt;
    NegotiationReply reply;
    reply.configDataBytes = cfgMsg->data().size();
    if (!cfgMsg->applyAudioConfigTo(reply.config)) return std::nullopt;
    reply.grantedLayout = cfgMsg->parseAudioConfigGrantedLayout();
    if (!recvUntil(c, PacketType::Control, ControlType::ConnectAccept, 3000).has_value())
        return std::nullopt;
    return reply;
}

// Drains until a CLIENTS_UPDATE reporting `expected` clients — the inject barrier
// (registration with the broadcaster precedes the roster broadcast).
bool waitForClientsUpdate(ClientConnection& c, int expected, int budgetMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto p = recvUntil(c, PacketType::Control, ControlType::ClientsUpdate, 200);
        if (!p.has_value()) continue;
        auto msg = ControlMessage::deserialize(p->payload());
        if (!msg) continue;
        auto info = msg->parseClientsUpdate();
        if (info.has_value() && info->clientCount == expected) return true;
    }
    return false;
}

// Deterministic int16 PCM (the language-neutral LCG of the B2 goldens).
std::vector<std::int16_t> lcgSamples(std::uint32_t seed, std::size_t count) {
    std::vector<std::int16_t> out;
    out.reserve(count);
    std::uint32_t x = seed;
    for (std::size_t i = 0; i < count; ++i) {
        x = (1103515245u * x + 12345u) & 0x7FFFFFFFu;
        out.push_back(static_cast<std::int16_t>(
            static_cast<std::int32_t>((x >> 8) % 65536u) - 32768));
    }
    return out;
}

std::vector<std::uint8_t> toBytes(const std::vector<std::int16_t>& samples) {
    std::vector<std::uint8_t> bytes(samples.size() * sizeof(std::int16_t));
    std::memcpy(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

// The reference conversion: downmix (layout 1) then decimate by `factor`.
std::vector<std::uint8_t> expectedMonoDecimated(const std::vector<std::int16_t>& stereo,
                                                int factor) {
    const std::size_t frames = stereo.size() / 2;
    std::vector<std::int16_t> mono(frames);
    downmixToMono(stereo.data(), frames, mono.data());
    Decimator d(factor, 1);
    std::vector<std::int16_t> out(d.outputFramesFor(frames));
    out.resize(d.process(mono.data(), frames, out.data()));
    return toBytes(out);
}

}  // namespace

// ---------------------------------------------------------------------------
// The grant matrix (§6.2.1): exact-grant-or-native over one stereo server.
// ---------------------------------------------------------------------------

TEST(FormatNegotiation, GrantMatrixOnAStereoServer) {
    AudioStreamConfig scfg{};  // TCP, 48 kHz / 16 / stereo native
    scfg.maxClients = 16;      // the matrix below runs sequential connections
    AudioStreamServer server{0, scfg};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    struct Case {
        const char* label;
        std::optional<RxFormatRequest> req;
        std::size_t expectBytes;  // 14 = v1 reply, 15 = extended
        std::int32_t expectRate;
        std::int32_t expectChannels;
        std::uint8_t expectLayout;  // meaningful only for 15-byte replies
    };
    const Case cases[] = {
        // A v1 client (no tail) must get the exact v1 form — never the 15-byte one.
        {"v1-client", std::nullopt, 14, 48000, 2, 0},
        // The 0,0 probe: understood, nothing changes, but the reply form says 1.2.
        {"probe", RxFormatRequest{0, 0}, 15, 48000, 2, 0},
        // Exact grants.
        {"granted-12k-downmix", RxFormatRequest{12000, 1}, 15, 12000, 1, 1},
        {"granted-24k-native-layout", RxFormatRequest{24000, 0}, 15, 24000, 2, 0},
        {"granted-8k-left", RxFormatRequest{8000, 2}, 15, 8000, 1, 2},
        // Declines: answered native, never rejected, never partially granted.
        {"declined-non-divisor", RxFormatRequest{9000, 1}, 15, 48000, 2, 0},
        {"declined-unsupported-divisor", RxFormatRequest{9600, 0}, 15, 48000, 2, 0},
        {"declined-unknown-layout", RxFormatRequest{0, 7}, 15, 48000, 2, 0},
        // A decline is TOTAL: the servable rate must not be granted beside the
        // unservable layout (exact-grant-or-native, no substitution).
        {"declined-mixed-good-rate-bad-layout", RxFormatRequest{12000, 7}, 15, 48000, 2, 0},
    };

    for (const Case& c : cases) {
        SCOPED_TRACE(c.label);
        TcpClientTransport client;
        auto cc = client.connect("127.0.0.1", static_cast<std::uint16_t>(server.port()), 2000, &err);
        ASSERT_TRUE(cc) << err;
        auto reply = handshake(*cc, std::string("matrix-") + c.label, c.req);
        ASSERT_TRUE(reply.has_value()) << "handshake failed";
        EXPECT_EQ(c.expectBytes, reply->configDataBytes) << "reply form";
        EXPECT_EQ(c.expectRate, reply->config.sampleRate);
        EXPECT_EQ(c.expectChannels, reply->config.channels);
        if (c.expectBytes == 15) {
            ASSERT_TRUE(reply->grantedLayout.has_value());
            EXPECT_EQ(c.expectLayout, *reply->grantedLayout);
        } else {
            EXPECT_FALSE(reply->grantedLayout.has_value());
        }
        cc->close();
    }
    server.stop();
}

TEST(FormatNegotiation, StereoLayoutsDeclineOnAMonoServer) {
    AudioStreamConfig scfg{};
    scfg.channels = 1;  // mono native: layouts 1-3 are undefined reductions
    AudioStreamServer server{0, scfg};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    TcpClientTransport client;
    auto cc = client.connect("127.0.0.1", static_cast<std::uint16_t>(server.port()), 2000, &err);
    ASSERT_TRUE(cc) << err;
    auto reply = handshake(*cc, "mono-downmix-ask", RxFormatRequest{12000, 1});
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(15u, reply->configDataBytes);
    EXPECT_EQ(48000, reply->config.sampleRate) << "decline must be total (no rate substitution)";
    EXPECT_EQ(1, reply->config.channels);
    ASSERT_TRUE(reply->grantedLayout.has_value());
    EXPECT_EQ(0, *reply->grantedLayout);
    server.stop();
}

// ---------------------------------------------------------------------------
// Converted stream content — and the native path beside it, byte-identical.
// ---------------------------------------------------------------------------

TEST(FormatNegotiation, ConvertedStreamIsBitExactWhileNativePeerIsUntouched) {
    AudioStreamConfig scfg{};
    scfg.maxClients = 4;
    AudioStreamServer server{0, scfg};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    // Client A: granted 12 kHz mono-downmix (4:1 + (L+R)>>1). Client B: v1 native.
    TcpClientTransport ta, tb;
    auto ca = ta.connect("127.0.0.1", static_cast<std::uint16_t>(server.port()), 2000, &err);
    ASSERT_TRUE(ca) << err;
    auto ra = handshake(*ca, "converted", RxFormatRequest{12000, 1});
    ASSERT_TRUE(ra.has_value());
    ASSERT_EQ(12000, ra->config.sampleRate);
    auto cb = tb.connect("127.0.0.1", static_cast<std::uint16_t>(server.port()), 2000, &err);
    ASSERT_TRUE(cb) << err;
    auto rb = handshake(*cb, "native", std::nullopt);
    ASSERT_TRUE(rb.has_value());
    ASSERT_EQ(14u, rb->configDataBytes);
    ASSERT_TRUE(waitForClientsUpdate(*ca, 2, 3000));
    ASSERT_TRUE(waitForClientsUpdate(*cb, 2, 3000));

    // 8 chunks x 960 native stereo frames of deterministic noise.
    const std::size_t kChunks = 8, kFramesPerChunk = 960;
    const std::vector<std::int16_t> all =
        lcgSamples(20260822u, kChunks * kFramesPerChunk * 2);
    const std::vector<std::uint8_t> allBytes = toBytes(all);
    const std::size_t chunkBytes = kFramesPerChunk * 2 * sizeof(std::int16_t);
    for (std::size_t i = 0; i < kChunks; ++i)
        server.injectAudio(std::vector<std::uint8_t>(
            allBytes.begin() + static_cast<std::ptrdiff_t>(i * chunkBytes),
            allBytes.begin() + static_cast<std::ptrdiff_t>((i + 1) * chunkBytes)));

    // Expected: the same units run over the same stream (chunk-invariance is a
    // B2-pinned property, so one pass over the concatenation is the reference).
    const std::vector<std::uint8_t> expectA = expectedMonoDecimated(all, 4);
    ASSERT_EQ(kChunks * kFramesPerChunk / 4 * 2, expectA.size());

    std::vector<std::uint8_t> gotA, gotB;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((gotA.size() < expectA.size() || gotB.size() < allBytes.size()) &&
           std::chrono::steady_clock::now() < deadline) {
        if (gotA.size() < expectA.size()) {
            auto p = recvUntil(*ca, PacketType::AudioRx, std::nullopt, 100);
            if (p.has_value())
                gotA.insert(gotA.end(), p->payload().begin(), p->payload().end());
        }
        if (gotB.size() < allBytes.size()) {
            auto p = recvUntil(*cb, PacketType::AudioRx, std::nullopt, 100);
            if (p.has_value())
                gotB.insert(gotB.end(), p->payload().begin(), p->payload().end());
        }
    }
    // The converted subscriber received EXACTLY the unit-defined conversion...
    EXPECT_EQ(expectA, gotA) << "converted stream is not the B2 units' output";
    // ...while the native peer received the injected bytes UNCHANGED, from the
    // same broadcast, at the same time (the fast path must not leak conversion).
    EXPECT_EQ(allBytes, gotB) << "native stream changed while a converted peer was subscribed";

    server.stop();
}

// ---------------------------------------------------------------------------
// The plan's DONE criterion: a converted subscriber recovers via FEC under
// induced loss. Loopback drops nothing (L292), so the loss is the relay's.
// ---------------------------------------------------------------------------

TEST(FormatNegotiation, ConvertedSubscriberRecoversViaFecUnderInducedLoss) {
    AudioStreamConfig scfg = AudioStreamConfig::udpWan();  // FEC on, block size 5
    ASSERT_TRUE(scfg.fecEnabled);
    ASSERT_EQ(5, scfg.fecBlockSize);
    AudioStreamServer server{0, scfg};
    server.setInjectOnlyMode(true);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    int proxyPort = 0;
    void* proxy = naproxy_start(server.port(), scfg.fecBlockSize, /*drop_ordinal=*/2,
                                /*corrupt_ordinal=*/-1, /*drop_parity=*/0, &proxyPort);
    ASSERT_TRUE(proxy != nullptr) << "relay failed to start";

    UdpReliabilityConfig rcfg;
    rcfg.fecEnabled = true;
    rcfg.fecBlockSize = scfg.fecBlockSize;
    rcfg.reorderWindowSize = 8;
    rcfg.reorderMaxHoldMs = 200;  // hold gaps long enough for the parity to land
    rcfg.frameDurationMs = scfg.frameDurationMs;
    UdpClientTransport transport{rcfg};
    auto cc = transport.connect("127.0.0.1", static_cast<std::uint16_t>(proxyPort), 2000, &err);
    ASSERT_TRUE(cc) << err;

    auto reply = handshake(*cc, "lossy-converted", RxFormatRequest{12000, 1});
    ASSERT_TRUE(reply.has_value()) << "handshake through the relay failed";
    ASSERT_EQ(12000, reply->config.sampleRate);
    ASSERT_EQ(1, reply->config.channels);
    ASSERT_TRUE(waitForClientsUpdate(*cc, 1, 3000));

    // 25 injects x 960 native stereo frames -> 25 converted audio packets (480 B
    // each, one datagram) -> exactly 5 FEC blocks; the relay drops ordinal 2 of
    // each block, which single-parity XOR FEC is specified to repair.
    //
    // The injects are paced at the real 20 ms frame cadence, NOT burst: CI's
    // Windows job measured a 2 ms burst losing one datagram to loopback UDP
    // buffering BEFORE the relay (24 of 25 seen), which makes that block
    // double-lossy — unrecoverable by single-parity FEC, correctly. Real
    // producers pace at frame cadence; the burst was this test's artifact.
    const std::size_t kInjects = 25, kFramesPerInject = 960;
    const std::vector<std::int16_t> all =
        lcgSamples(910003u, kInjects * kFramesPerInject * 2);
    const std::vector<std::uint8_t> allBytes = toBytes(all);
    const std::size_t chunkBytes = kFramesPerInject * 2 * sizeof(std::int16_t);
    for (std::size_t i = 0; i < kInjects; ++i) {
        server.injectAudio(std::vector<std::uint8_t>(
            allBytes.begin() + static_cast<std::ptrdiff_t>(i * chunkBytes),
            allBytes.begin() + static_cast<std::ptrdiff_t>((i + 1) * chunkBytes)));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const std::vector<std::uint8_t> expected = expectedMonoDecimated(all, 4);
    std::vector<std::uint8_t> got;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (got.size() < expected.size() && std::chrono::steady_clock::now() < deadline) {
        auto p = recvUntil(*cc, PacketType::AudioRx, std::nullopt, 200);
        if (p.has_value()) got.insert(got.end(), p->payload().begin(), p->payload().end());
    }

    // The fault was really induced (the drop counter is the negative control —
    // byte-exact delivery below cannot have happened trivially)...
    EXPECT_EQ(5, naproxy_audio_dropped(proxy)) << "the relay did not induce the loss";
    EXPECT_EQ(25, naproxy_audio_seen(proxy));
    // ...and the converted stream still arrived complete and bit-exact: every
    // dropped packet was rebuilt from parity computed over THIS subscription's
    // converted payloads — the per-connection FEC the grant rules require.
    EXPECT_EQ(expected, got) << "converted stream incomplete or wrong under induced loss";

    cc->close();
    naproxy_stop(proxy);
    server.stop();
}
