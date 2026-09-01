// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — discovery, end to end through a real AudioStreamServer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Issue #100's acceptance criteria, at the level they were written about. The
// transport-level gate is pinned in test_udp_transport.cpp; what is proved here
// is that a real server publishes TRUE facts — the OS-assigned port, the live
// roster size, the transport bits — and that a discovery sweep costs a server
// nothing, which is the assertion that failed before spec 1.4.
//
// A probe here is unicast to loopback rather than broadcast. That is deliberate:
// the code path under test (the demux gate, the responder, the limiter) is
// identical either way, and a test that put real broadcast traffic on the
// operator's segment would be antisocial and would fail in CI containers.
// Hardware-free.

#include "naudio/net/AudioStreamServer.hpp"
#include "naudio/net/Socket.hpp"
#include "naudio/net/UdpClientTransport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace naudio;
using namespace naudio::net;

namespace {

AudioStreamConfig udpServerConfig() {
    AudioStreamConfig c = AudioStreamConfig::udpLan();
    c.maxClients = 4;
    return c;
}

std::optional<DiscoveryInfo> probe(Socket& from, std::uint16_t port, std::uint32_t token,
                                   int timeoutMs) {
    AudioPacket out = AudioPacket::createControl(0, ControlMessage::discover(token).serialize());
    std::vector<std::uint8_t> bytes = out.serialize();
    if (!from.sendTo(bytes.data(), bytes.size(), "127.0.0.1", port)) return std::nullopt;

    from.setRecvTimeout(timeoutMs);
    std::vector<std::uint8_t> buf(2048);
    RecvFromResult rr = from.recvFrom(buf.data(), buf.size());
    if (rr.status != IoStatus::Ok) return std::nullopt;
    auto packet = AudioPacket::deserialize(buf.data(), rr.bytes);
    if (!packet || packet->packetType() != PacketType::Control) return std::nullopt;
    auto msg = ControlMessage::deserialize(packet->payload());
    if (!msg) return std::nullopt;
    auto info = msg->parseDiscoverReply();
    // The server's address is not a wire field (§6.8) — it is where the reply came
    // FROM, and filling it here is what a real prober does.
    if (info) info->host = rr.senderHost;
    return info;
}

// Waits until the roster reaches n (bounded), so the test never races the accept
// thread. Returns the final observation either way.
bool waitForClients(AudioStreamServer& server, int n, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (server.clientCount() == n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return server.clientCount() == n;
}

}  // namespace

// Issue #100's headline: a sweep finds the server and costs it nothing.
TEST(Discovery, AProbeFindsTheServerAndConsumesNoClientSlot) {
    AudioStreamConfig cfg = udpServerConfig();
    cfg.serverName = "shack";
    AudioStreamServer server(0, cfg);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());
    ASSERT_GT(port, 0);
    ASSERT_EQ(0, server.clientCount());

    Socket prober = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    auto info = probe(prober, port, 0x11223344u, 2000);

    ASSERT_TRUE(info.has_value()) << "the server did not answer a DISCOVER";
    EXPECT_EQ(0x11223344u, info->token);
    EXPECT_EQ("127.0.0.1", info->host) << "the reply's source address IS the discovery result";
    EXPECT_EQ(port, info->port) << "the reply must carry the OS-assigned port, not the 0 asked for";
    EXPECT_EQ("shack", info->name);
    EXPECT_EQ(4, info->maxClients);
    EXPECT_EQ(static_cast<std::uint8_t>(DiscoveryTransport::Udp), info->transports);

    // THE assertion. Before spec 1.4 a probe was a CONNECT_REQUEST and this was 1.
    EXPECT_EQ(0, server.clientCount())
        << "the probe consumed a client slot — a discovery sweep can deny service";
    server.stop();
}

// The reply's occupancy must be LIVE, not sampled at bind. A chooser acts on this
// field, and a cached zero would send every client at the fullest server.
TEST(Discovery, TheReplyReportsTheLiveRosterNotACachedZero) {
    AudioStreamServer server(0, udpServerConfig());
    server.setInjectOnlyMode(true);  // hardware-free: no capture device is opened
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    Socket prober = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    auto before = probe(prober, port, 1, 2000);
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(0, before->clientCount) << "control: an empty server must report 0 first";

    // A real client, through the real handshake — UdpClientTransport rather than a
    // raw datagram, because the server counts SESSIONS and a session is what the
    // full handshake produces.
    UdpReliabilityConfig ccfg;
    ccfg.reorderWindowSize = 0;
    UdpClientTransport client{ccfg};
    auto cc = client.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(cc) << err;
    ASSERT_TRUE(cc->sendControl(ControlMessage::connectRequest("tester", AudioPacket::VERSION)));
    ASSERT_TRUE(waitForClients(server, 1, 3000))
        << "the client never connected; the rest of this test would prove nothing";

    // A different source socket, so the per-source limiter does not swallow this.
    Socket prober2 = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    auto after = probe(prober2, port, 2, 2000);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(1, after->clientCount) << "occupancy was cached at bind, not read live";
    server.stop();
}

// The operator opt-out, through the config the C ABI sets.
TEST(Discovery, AServerThatOptedOutIsSilent) {
    AudioStreamConfig cfg = udpServerConfig();
    cfg.discoverable = false;
    AudioStreamServer server(0, cfg);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    Socket prober = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    EXPECT_FALSE(probe(prober, port, 1, 500).has_value());

    // Control: the same server with discovery on DOES answer, so the silence above
    // is the opt-out and not a broken probe helper.
    server.stop();
    AudioStreamConfig on = udpServerConfig();
    AudioStreamServer server2(0, on);
    ASSERT_TRUE(server2.start(&err)) << err;
    Socket prober2 = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    EXPECT_TRUE(probe(prober2, static_cast<std::uint16_t>(server2.port()), 1, 2000).has_value())
        << "control failed: the probe helper cannot elicit a reply at all";
    server2.stop();
}

// A DUAL server is discoverable over its UDP half and says so in both bits.
TEST(Discovery, ADualServerReportsBothTransports) {
    AudioStreamConfig cfg = AudioStreamConfig::udpLan();
    cfg.transportType = TransportType::Dual;
    cfg.maxClients = 2;
    AudioStreamServer server(0, cfg);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    Socket prober = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    auto info = probe(prober, port, 5, 2000);
    ASSERT_TRUE(info.has_value()) << "a DUAL server did not answer over its UDP half";
    EXPECT_EQ(static_cast<std::uint8_t>(DiscoveryTransport::Tcp) |
                  static_cast<std::uint8_t>(DiscoveryTransport::Udp),
              info->transports);
    EXPECT_EQ(port, info->port);
    server.stop();
}

// A TCP-only server is undiscoverable — §6.8 item 6. Stated as a test so the
// limitation is a measured fact rather than a sentence in the spec.
TEST(Discovery, ATcpOnlyServerIsUndiscoverable) {
    AudioStreamConfig cfg;  // defaults to TCP
    cfg.maxClients = 2;
    AudioStreamServer server(0, cfg);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());

    Socket prober = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    EXPECT_FALSE(probe(prober, port, 1, 500).has_value())
        << "a TCP server answered a UDP datagram, which it has no path to do";
    server.stop();
}
