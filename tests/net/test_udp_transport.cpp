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
