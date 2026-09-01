// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — UdpServerTransport.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// UdpServerTransport: bind/port lifecycle, the anti-spoof
// CONNECT_REQUEST gate, demux routing to the owning connection, the
// bounded pending registry, disconnect, and aggregate stats. A raw UDP socket
// stands in for clients so the server's demux/accept path is exercised directly
// over OS loopback. Hardware-free.

#include "naudio/net/Socket.hpp"
#include "naudio/net/UdpServerTransport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace naudio;       // AudioPacket, ControlMessage
using namespace naudio::net;  // UdpServerTransport, Socket

namespace {

UdpReliabilityConfig passthrough() {
    UdpReliabilityConfig c;
    c.reorderWindowSize = 0;  // straight passthrough so routed packets surface immediately
    return c;
}

void sendDatagram(Socket& from, const AudioPacket& packet, std::uint16_t port) {
    std::vector<std::uint8_t> bytes = packet.serialize();
    ASSERT_TRUE(from.sendTo(bytes.data(), bytes.size(), "127.0.0.1", port));
}

AudioPacket connectRequest() {
    return AudioPacket::createControl(0, ControlMessage::connectRequest("tester",
                                                                        AudioPacket::VERSION)
                                             .serialize());
}

}  // namespace

TEST(UdpTransport, PortBeforeBindIsMinusOne) {
    UdpServerTransport server;
    EXPECT_EQ(server.port(), -1);
    EXPECT_FALSE(server.isBound());
}

// Issue #83. A second server must NOT be able to bind a port another server is already serving.
//
// This reads as an OS-level truism and is not one: it held only because of a flag naudio chose.
// While UdpServerTransport passed reuseAddr=true, Linux let both sockets bind — two servers up,
// neither told, datagrams delivered to one of them — and na_server_start() reported NA_OK for a
// port it did not have. macOS refused it regardless, which is exactly what made the bug survive:
// the platform most bridge development happens on could not reproduce it.
//
// THE PLATFORM ASYMMETRY IS ALSO THIS TEST'S HONEST LIMIT. Against the pre-fix code it is RED on
// Linux and GREEN on macOS, because macOS never permitted the double bind in the first place. Its
// regression value is therefore Linux-side, and a green macOS run is not evidence the flag is
// still off. Stated rather than left for a reader to discover (Learning 63).
//
// Binds the first server to port 0 so the OS picks a free one: a hard-coded port would make this
// fail for the unrelated reason that something else on the machine already holds it.
TEST(UdpTransport, ASecondServerCannotBindAPortAlreadyBeingServed) {
    UdpServerTransport first;
    std::string err;
    ASSERT_TRUE(first.bind(0, &err)) << err;
    ASSERT_GT(first.port(), 0);
    const std::uint16_t served = static_cast<std::uint16_t>(first.port());

    UdpServerTransport second;
    std::string secondErr;
    EXPECT_FALSE(second.bind(served, &secondErr))
        << "a second server bound port " << served << " while the first was serving it — "
        << "na_server_start would report success for a port it does not have (issue #83)";
    EXPECT_FALSE(second.isBound());
    EXPECT_EQ(second.port(), -1);
    EXPECT_FALSE(secondErr.empty()) << "a refused bind must say why";
}

// The other half of the contract, and the control for the test above: refusing the second bind
// must not come from having made binding harder in general. Once the first server releases the
// port, the next one must get it — with no TIME_WAIT wait, which is precisely why UDP does not
// need the SO_REUSEADDR that caused #83. A fix that traded the double-bind for a port that
// cannot be reclaimed after restart would pass the test above and break every supervised restart.
TEST(UdpTransport, APortIsImmediatelyRebindableOnceItsServerIsGone) {
    std::uint16_t served = 0;
    {
        UdpServerTransport first;
        std::string err;
        ASSERT_TRUE(first.bind(0, &err)) << err;
        served = static_cast<std::uint16_t>(first.port());
    }  // destroyed: socket closed

    UdpServerTransport second;
    std::string err;
    EXPECT_TRUE(second.bind(served, &err))
        << "port " << served << " was not immediately rebindable after its server went away: "
        << err;
}

TEST(UdpTransport, BindAssignsEphemeralPort) {
    UdpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err)) << err;
    EXPECT_TRUE(server.isBound());
    EXPECT_GT(server.port(), 0);
    server.close();
}

TEST(UdpTransport, DoubleBindFails) {
    UdpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    EXPECT_FALSE(server.bind(0, &err));
    server.close();
}

TEST(UdpTransport, AcceptBeforeBindErrors) {
    UdpServerTransport server;
    std::shared_ptr<ClientConnection> out;
    std::string err;
    EXPECT_EQ(server.acceptClient(100, out, &err), IoStatus::Error);
}

TEST(UdpTransport, AcceptTimesOutWithNoClient) {
    UdpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    std::shared_ptr<ClientConnection> out;
    EXPECT_EQ(server.acceptClient(150, out, &err), IoStatus::TimedOut);
    EXPECT_EQ(out, nullptr);
    server.close();
}

// Anti-spoof: a non-CONNECT datagram from an unknown sender registers nothing.
TEST(UdpTransport, NonConnectFromUnknownRegistersNothing) {
    UdpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    auto port = static_cast<std::uint16_t>(server.port());

    Socket spoof = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    sendDatagram(spoof, AudioPacket::createHeartbeat(0), port);  // not a CONNECT_REQUEST

    std::shared_ptr<ClientConnection> out;
    EXPECT_EQ(server.acceptClient(300, out, &err), IoStatus::TimedOut);  // no session created
    EXPECT_EQ(out, nullptr);
    server.close();
}

// A valid CONNECT_REQUEST registers a connection that acceptClient yields, and
// the request itself is delivered to that connection.
TEST(UdpTransport, ConnectRequestRegistersAndAccepts) {
    UdpServerTransport server;
    server.setReliabilityConfig(passthrough());
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    auto port = static_cast<std::uint16_t>(server.port());

    Socket client = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    sendDatagram(client, connectRequest(), port);

    std::shared_ptr<ClientConnection> conn;
    ASSERT_EQ(server.acceptClient(2000, conn, &err), IoStatus::Ok) << err;
    ASSERT_TRUE(conn);
    EXPECT_EQ(conn->clientAddress().id(), "udp-1");
    EXPECT_TRUE(conn->clientAddress().hasEndpoint());

    // The CONNECT_REQUEST was routed into the connection's pipeline.
    ReceiveResult r = conn->receivePacket(1000);
    ASSERT_TRUE(r.hasPacket());
    EXPECT_EQ(r.packet->packetType(), PacketType::Control);
    server.close();
}

// After accept, subsequent datagrams from the same sender route to the connection.
TEST(UdpTransport, DemuxRoutesSubsequentPacketsToConnection) {
    UdpServerTransport server;
    server.setReliabilityConfig(passthrough());
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    auto port = static_cast<std::uint16_t>(server.port());

    Socket client = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    sendDatagram(client, connectRequest(), port);
    std::shared_ptr<ClientConnection> conn;
    ASSERT_EQ(server.acceptClient(2000, conn, &err), IoStatus::Ok);
    ASSERT_TRUE(conn->receivePacket(1000).hasPacket());  // drain the CONNECT_REQUEST

    std::vector<std::uint8_t> audio = {9, 8, 7};
    sendDatagram(client, AudioPacket::createRxAudio(1, audio), port);

    ReceiveResult r = conn->receivePacket(1000);
    ASSERT_TRUE(r.hasPacket());
    EXPECT_EQ(r.packet->packetType(), PacketType::AudioRx);
    EXPECT_EQ(r.packet->payload(), audio);
    EXPECT_GE(server.packetsReceived(), 2);  // CONNECT_REQUEST + audio
    server.close();
}

// The pending registry is bounded: spoofed CONNECT_REQUESTs from many distinct
// sources cannot register more than MAX_PENDING_CONNECTIONS unaccepted sessions.
TEST(UdpTransport, PendingRegistryIsBounded) {
    UdpServerTransport server;
    server.setReliabilityConfig(passthrough());
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    auto port = static_cast<std::uint16_t>(server.port());

    const int attempts = static_cast<int>(UdpServerTransport::MAX_PENDING_CONNECTIONS) + 4;
    std::vector<Socket> clients;
    clients.reserve(attempts);
    for (int i = 0; i < attempts; i++) {
        clients.push_back(Socket::bindUdp("127.0.0.1", 0, false, nullptr));
        sendDatagram(clients.back(), connectRequest(), port);
    }
    // Let the demux drain all datagrams before accepting (none accepted yet, so
    // the pending cap is the binding constraint). Poll for the observable part —
    // each REGISTERED pending session routes its CONNECT_REQUEST into a connection
    // (which packetsReceived() sums); the excess are dropped uncounted, so give
    // those a short settle after the poll. A fixed sleep alone can undershoot a
    // starved demux thread on a loaded CI runner.
    const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.packetsReceived() <
               static_cast<std::int64_t>(UdpServerTransport::MAX_PENDING_CONNECTIONS) &&
           std::chrono::steady_clock::now() < drainDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int accepted = 0;
    std::shared_ptr<ClientConnection> conn;
    while (server.acceptClient(150, conn, &err) == IoStatus::Ok) {
        ++accepted;
        conn.reset();
    }
    EXPECT_GE(accepted, 1);
    EXPECT_LE(accepted, static_cast<int>(UdpServerTransport::MAX_PENDING_CONNECTIONS));
    server.close();
}

TEST(UdpTransport, DisconnectClosesConnectionIdempotently) {
    UdpServerTransport server;
    server.setReliabilityConfig(passthrough());
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    auto port = static_cast<std::uint16_t>(server.port());

    Socket client = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    sendDatagram(client, connectRequest(), port);
    std::shared_ptr<ClientConnection> conn;
    ASSERT_EQ(server.acceptClient(2000, conn, &err), IoStatus::Ok);

    EXPECT_FALSE(conn->isClosed());
    server.disconnectClient(conn);
    EXPECT_TRUE(conn->isClosed());
    server.disconnectClient(conn);      // harmless second call
    server.disconnectClient(nullptr);   // null-safe
    server.close();
}

TEST(UdpTransport, AggregateStatsZeroWithNoClients) {
    UdpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    EXPECT_EQ(server.packetsSent(), 0);
    EXPECT_EQ(server.packetsReceived(), 0);
    EXPECT_EQ(server.bytesSent(), 0);
    EXPECT_EQ(server.bytesReceived(), 0);
    EXPECT_EQ(server.crcErrors(), 0);
    server.close();
}

TEST(UdpTransport, CloseReleasesPort) {
    UdpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    auto port = static_cast<std::uint16_t>(server.port());
    server.close();
    EXPECT_FALSE(server.isBound());
    EXPECT_EQ(server.port(), -1);

    UdpServerTransport server2;
    EXPECT_TRUE(server2.bind(port, &err)) << err;  // port is free again
    server2.close();
}

// --- Discovery (spec 1.4, §6.8) ---------------------------------------------
//
// The demux gate's SECOND exempt case. Everything here is about the difference
// between the two exemptions: a CONNECT_REQUEST from an unknown sender creates a
// connection, and a DISCOVER from one is answered and forgotten. The assertion
// that matters — and the one that fails against the pre-1.4 code — is that
// acceptClient still times out after a probe.

namespace {

DiscoveryFacts factsFor(std::uint16_t port, std::uint8_t clients, std::uint8_t maxClients) {
    DiscoveryFacts f;
    f.enabled = true;
    f.port = port;
    f.transports = static_cast<std::uint8_t>(DiscoveryTransport::Udp);
    f.sampleRate = 48000;
    f.bitsPerSample = 16;
    f.channels = 2;
    f.clientCount = clients;
    f.maxClients = maxClients;
    f.name = "shack";
    return f;
}

AudioPacket discoverProbe(std::uint32_t token) {
    return AudioPacket::createControl(0, ControlMessage::discover(token).serialize());
}

// One datagram, or nullopt once timeoutMs elapses with nothing arriving.
std::optional<AudioPacket> recvOne(Socket& s, int timeoutMs) {
    s.setRecvTimeout(timeoutMs);
    std::vector<std::uint8_t> buf(2048);
    RecvFromResult rr = s.recvFrom(buf.data(), buf.size());
    if (rr.status != IoStatus::Ok) return std::nullopt;
    return AudioPacket::deserialize(buf.data(), rr.bytes);
}

std::optional<DiscoveryInfo> recvReply(Socket& s, int timeoutMs) {
    auto packet = recvOne(s, timeoutMs);
    if (!packet || packet->packetType() != PacketType::Control) return std::nullopt;
    auto msg = ControlMessage::deserialize(packet->payload());
    if (!msg) return std::nullopt;
    return msg->parseDiscoverReply();
}

}  // namespace

// THE acceptance assertion of issue #100: a probe is answered, and it costs
// nothing. Against the pre-1.4 code the probe was simply dropped (not a
// CONNECT_REQUEST), so the reply never arrives and this is red on the first
// ASSERT rather than the second.
TEST(UdpTransport, DiscoverIsAnsweredAndCreatesNoConnection) {
    UdpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err)) << err;
    const auto port = static_cast<std::uint16_t>(server.port());
    server.setDiscoveryFacts([port] { return factsFor(port, 1, 4); });

    Socket prober = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    sendDatagram(prober, discoverProbe(0xABCDEF01u), port);

    auto info = recvReply(prober, 2000);
    ASSERT_TRUE(info.has_value()) << "the server did not answer the probe";
    EXPECT_EQ(0xABCDEF01u, info->token) << "the token must be echoed verbatim";
    EXPECT_EQ(port, info->port);
    EXPECT_EQ(48000u, info->sampleRate);
    EXPECT_EQ(1, info->clientCount);
    EXPECT_EQ(4, info->maxClients);
    EXPECT_EQ("shack", info->name);

    // The whole point: no connection, no client slot, no stream.
    std::shared_ptr<ClientConnection> out;
    EXPECT_EQ(server.acceptClient(300, out, &err), IoStatus::TimedOut)
        << "a DISCOVER registered a connection — it must be answered and forgotten";
    EXPECT_EQ(out, nullptr);
    server.close();
}

// A server at capacity still answers. Occupancy is a fact the chooser needs, not
// a reason for silence (§6.8 item 3) — and it is the one a full server would be
// most tempted to withhold.
TEST(UdpTransport, AServerAtCapacityStillAnswers) {
    UdpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    const auto port = static_cast<std::uint16_t>(server.port());
    server.setDiscoveryFacts([port] { return factsFor(port, 4, 4); });

    Socket prober = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    sendDatagram(prober, discoverProbe(7), port);

    auto info = recvReply(prober, 2000);
    ASSERT_TRUE(info.has_value()) << "a full server went silent instead of reporting it is full";
    EXPECT_EQ(4, info->clientCount);
    EXPECT_EQ(info->maxClients, info->clientCount);
    server.close();
}

// No provider installed: silent. This is what keeps every transport built without
// a server — including the bare ones all over this file — from answering.
TEST(UdpTransport, DiscoverIsSilentWithNoFactsProvider) {
    UdpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    const auto port = static_cast<std::uint16_t>(server.port());

    Socket prober = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    sendDatagram(prober, discoverProbe(1), port);
    EXPECT_FALSE(recvOne(prober, 400).has_value());
    server.close();
}

// The operator opt-out. Distinct from the case above: a provider IS installed and
// returns enabled=false, which is the path na_server_set_discoverable(s,0) takes.
TEST(UdpTransport, DiscoverIsSilentWhenTheOperatorOptedOut) {
    UdpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    const auto port = static_cast<std::uint16_t>(server.port());
    server.setDiscoveryFacts([port] {
        DiscoveryFacts f = factsFor(port, 0, 4);
        f.enabled = false;
        return f;
    });

    Socket prober = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    sendDatagram(prober, discoverProbe(1), port);
    EXPECT_FALSE(recvOne(prober, 400).has_value());
    server.close();
}

// The rate limit (§6.8 security note). Two probes back to back from one source
// yield exactly one reply — and the CONTROL is the test above proving a single
// probe does get answered, without which "no second reply" would be vacuous.
TEST(UdpTransport, RepliesAreRateLimitedPerSource) {
    UdpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    const auto port = static_cast<std::uint16_t>(server.port());
    server.setDiscoveryFacts([port] { return factsFor(port, 0, 4); });

    Socket prober = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    sendDatagram(prober, discoverProbe(1), port);
    ASSERT_TRUE(recvReply(prober, 2000).has_value()) << "the first probe must be answered";

    sendDatagram(prober, discoverProbe(2), port);
    EXPECT_FALSE(recvOne(prober, 400).has_value())
        << "a second probe inside the window was answered — the limiter is not limiting";

    // A DIFFERENT source is unaffected: the limit is per-source, not global.
    Socket other = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    sendDatagram(other, discoverProbe(3), port);
    auto info = recvReply(other, 2000);
    ASSERT_TRUE(info.has_value()) << "the limiter is global, not per-source";
    EXPECT_EQ(3u, info->token);
    server.close();
}

// The anti-spoof property §6.8 promises to preserve. A datagram from an unknown
// sender that is neither a CONNECT_REQUEST nor a DISCOVER must still be dropped —
// and, now that the server can speak to unknown senders at all, unanswered.
TEST(UdpTransport, AntiSpoofStillDropsEverythingThatIsNeither) {
    UdpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    const auto port = static_cast<std::uint16_t>(server.port());
    server.setDiscoveryFacts([port] { return factsFor(port, 0, 4); });

    Socket spoof = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    sendDatagram(spoof, AudioPacket::createHeartbeat(0), port);
    sendDatagram(spoof, AudioPacket::createRxAudio(1, {1, 2, 3}), port);
    sendDatagram(spoof, AudioPacket::createControl(2, ControlMessage::disconnect().serialize()),
                 port);

    EXPECT_FALSE(recvOne(spoof, 400).has_value()) << "the server answered a non-DISCOVER";
    std::shared_ptr<ClientConnection> out;
    EXPECT_EQ(server.acceptClient(300, out, &err), IoStatus::TimedOut);
    EXPECT_EQ(out, nullptr);

    // Control: the same socket, now sending a real probe, IS answered — so the
    // silence above is the gate working and not a broken prober.
    sendDatagram(spoof, discoverProbe(99), port);
    auto info = recvReply(spoof, 2000);
    ASSERT_TRUE(info.has_value()) << "control failed: this socket cannot hear replies at all";
    EXPECT_EQ(99u, info->token);
    server.close();
}
