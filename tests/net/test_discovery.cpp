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
#include "naudio/net/Discovery.hpp"
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

// --- The client probe: discoverServers() (spec 1.4, §6.8) --------------------
//
// HONEST LIMIT OF THESE ARMS. A real sweep is one broadcast reaching N servers;
// N servers cannot be staged on one host because they would all need the same
// port. So the fan-out is exercised with FAKE responders answering from distinct
// source ports — which proves the collection, token and dedupe logic, and does
// NOT prove that a broadcast datagram is delivered to several hosts. That last
// step is the operating system's, and it is unproven here. (P3's shape: say
// which paths ran.)

namespace {

// A stand-in server: receives one probe on `port` and answers however the caller
// says. Returns the token it saw, or 0 if none arrived.
struct FakeResponder {
    Socket sock;
    std::uint16_t port = 0;

    explicit FakeResponder(const std::string& host = "127.0.0.1") {
        sock = Socket::bindUdp(host, 0, false, nullptr);
        port = sock.localPort();
    }

    // Waits for a probe, then sends `replies` DISCOVER_REPLYs back — the first
    // from this socket, any others from fresh sockets so they arrive from
    // DIFFERENT source endpoints, which is what a multi-server sweep looks like.
    std::uint32_t answer(int timeoutMs, int replies, std::uint32_t tokenDelta,
                         std::vector<Socket>* extraSockets) {
        sock.setRecvTimeout(timeoutMs);
        std::vector<std::uint8_t> buf(2048);
        RecvFromResult rr = sock.recvFrom(buf.data(), buf.size());
        if (rr.status != IoStatus::Ok) return 0;
        auto packet = AudioPacket::deserialize(buf.data(), rr.bytes);
        if (!packet) return 0;
        auto msg = ControlMessage::deserialize(packet->payload());
        if (!msg) return 0;
        auto token = msg->parseDiscoverToken();
        if (!token) return 0;

        for (int i = 0; i < replies; ++i) {
            ControlMessage reply = ControlMessage::discoverReply(
                *token + tokenDelta, static_cast<std::uint16_t>(4533 + i), 0x02, 48000, 16, 2,
                static_cast<std::uint8_t>(i), 4, i == 0 ? "alpha" : "beta");
            std::vector<std::uint8_t> bytes =
                AudioPacket::createControl(0, reply.serialize()).serialize();
            if (i == 0) {
                sock.sendTo(bytes.data(), bytes.size(), rr.senderHost, rr.senderPort);
            } else {
                Socket other = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
                other.sendTo(bytes.data(), bytes.size(), rr.senderHost, rr.senderPort);
                extraSockets->push_back(std::move(other));  // keep the source port alive
            }
        }
        return *token;
    }
};

DiscoveryOptions unicastTo(std::uint16_t port, int timeoutMs) {
    DiscoveryOptions o;
    o.address = "127.0.0.1";  // see the note above: loopback, not the segment
    o.port = port;
    o.timeoutMs = timeoutMs;
    o.bindHost = "127.0.0.1";
    return o;
}

}  // namespace

// The probe finds a real server and reports what it published.
TEST(Discovery, DiscoverServersFindsARunningServer) {
    AudioStreamConfig cfg = udpServerConfig();
    cfg.serverName = "alpha";
    AudioStreamServer server(0, cfg);
    std::string err;
    ASSERT_TRUE(server.start(&err)) << err;

    auto found = discoverServers(unicastTo(static_cast<std::uint16_t>(server.port()), 1000), &err);
    ASSERT_EQ(1u, found.size()) << "discoverServers did not find a running server: " << err;
    EXPECT_EQ("alpha", found[0].name);
    EXPECT_EQ(server.port(), found[0].port);
    EXPECT_EQ("127.0.0.1", found[0].host);
    EXPECT_EQ(4, found[0].maxClients);
    EXPECT_EQ(0, server.clientCount()) << "the probe consumed a slot";
    server.stop();
}

// Nobody answering is a normal outcome, not an error.
TEST(Discovery, DiscoverServersReturnsEmptyWhenNobodyAnswers) {
    // A port with nothing on it: bind and immediately release so it is plausibly free.
    std::uint16_t dead = 0;
    {
        Socket s = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
        dead = s.localPort();
    }
    std::string err = "sentinel";
    auto found = discoverServers(unicastTo(dead, 300), &err);
    EXPECT_TRUE(found.empty());
    EXPECT_EQ("sentinel", err) << "silence was reported as a send failure";
}

// §6.8: "MUST ignore a reply whose token it did not send." Staged with a fake
// responder that echoes token+1 — on a broadcast segment this is the only thing
// separating our replies from another prober's round.
TEST(Discovery, DiscoverServersIgnoresAReplyCarryingTheWrongToken) {
    FakeResponder fake;
    std::vector<Socket> extra;
    std::uint32_t seen = 0;
    std::thread responder([&] { seen = fake.answer(2000, 1, /*tokenDelta=*/1, &extra); });

    std::string err;
    auto found = discoverServers(unicastTo(fake.port, 800), &err);
    responder.join();

    ASSERT_NE(0u, seen) << "control: the fake never received the probe, so nothing was ignored";
    EXPECT_TRUE(found.empty()) << "a reply with a token we never sent was accepted";
}

// The control for the arm above, and the multi-reply collection path: the SAME
// fake, echoing the token correctly, is accepted — and two replies from distinct
// source endpoints come back as two servers.
TEST(Discovery, DiscoverServersCollectsRepliesFromDistinctEndpoints) {
    FakeResponder fake;
    std::vector<Socket> extra;
    std::uint32_t seen = 0;
    std::thread responder([&] { seen = fake.answer(2000, 2, /*tokenDelta=*/0, &extra); });

    std::string err;
    auto found = discoverServers(unicastTo(fake.port, 800), &err);
    responder.join();

    ASSERT_NE(0u, seen) << "the fake never received the probe";
    ASSERT_EQ(2u, found.size()) << "two replies from different source ports must be two servers";
    // Both came from 127.0.0.1; they are distinct because their endpoints are.
    EXPECT_NE(found[0].port, found[1].port);
    EXPECT_EQ(0, found[0].clientCount);
    EXPECT_EQ(1, found[1].clientCount);
}

// maxServers bounds a hostile segment: the collection loop stops, it does not grow.
TEST(Discovery, DiscoverServersHonoursMaxServers) {
    FakeResponder fake;
    std::vector<Socket> extra;
    std::thread responder([&] { fake.answer(2000, 2, 0, &extra); });

    DiscoveryOptions opts = unicastTo(fake.port, 800);
    opts.maxServers = 1;
    std::string err;
    auto found = discoverServers(opts, &err);
    responder.join();

    EXPECT_EQ(1u, found.size()) << "maxServers did not bound the result";
}
