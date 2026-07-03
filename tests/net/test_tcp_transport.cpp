// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — TCP transport stack (end-to-end over loopback).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// The TCP transport stack end-to-end over loopback:
// TcpServerTransport + TcpClientTransport + TcpClientConnection round-tripping
// 0xAF01 frames, plus the bind/accept/registry/stats/lifecycle behaviors. The
// FSM's adversarial resilience (partial reads, resync, CRC abort) is proven at
// the handler level in test_protocol_handler.cpp — the same AudioProtocolHandler
// every TcpClientConnection delegates to. Hardware-free.

#include "naudio/net/TcpClientTransport.hpp"
#include "naudio/net/TcpServerTransport.hpp"
#include "naudio/net/Transport.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace naudio;       // AudioPacket, PacketType, ControlMessage
using namespace naudio::net;  // transports

namespace {

// Binds the given server, connects a client to it over loopback, and accepts —
// all single-threaded (loopback connect completes into the listen backlog).
bool connectPair(ServerTransport& server, ClientTransport& client,
                 std::shared_ptr<ClientConnection>& serverConn,
                 std::shared_ptr<ClientConnection>& clientConn) {
    std::string err;
    if (!server.bind(0, &err)) return false;
    auto port = static_cast<std::uint16_t>(server.port());
    clientConn = client.connect("127.0.0.1", port, 2000, &err);
    if (!clientConn) return false;
    return server.acceptClient(2000, serverConn, &err) == IoStatus::Ok && serverConn;
}

std::optional<AudioPacket> recvFrame(ClientConnection& c, int maxCalls = 64) {
    for (int i = 0; i < maxCalls; i++) {
        ReceiveResult r = c.receivePacket(2000);
        if (r.hasPacket()) return std::move(r.packet);
        if (r.closed) return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

TEST(TcpTransport, PortBeforeBindIsMinusOne) {
    TcpServerTransport server;
    EXPECT_EQ(server.port(), -1);
    EXPECT_FALSE(server.isBound());
}

TEST(TcpTransport, BindAssignsEphemeralPort) {
    TcpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err)) << err;
    EXPECT_TRUE(server.isBound());
    EXPECT_GT(server.port(), 0);
}

TEST(TcpTransport, DoubleBindFails) {
    TcpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    EXPECT_FALSE(server.bind(0, &err));
}

TEST(TcpTransport, AcceptBeforeBindErrors) {
    TcpServerTransport server;
    std::shared_ptr<ClientConnection> out;
    std::string err;
    EXPECT_EQ(server.acceptClient(100, out, &err), IoStatus::Error);
}

TEST(TcpTransport, AcceptTimesOutWithNoClient) {
    TcpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    std::shared_ptr<ClientConnection> out;
    EXPECT_EQ(server.acceptClient(100, out, &err), IoStatus::TimedOut);
    EXPECT_EQ(out, nullptr);
}

// The core gate: a frame of each kind round-trips intact through the full stack.
TEST(TcpTransport, RoundTripsEachFrameTypeBothDirections) {
    TcpServerTransport server;
    TcpClientTransport client;
    std::shared_ptr<ClientConnection> sc, cc;
    ASSERT_TRUE(connectPair(server, client, sc, cc));

    // client -> server: CONTROL
    ASSERT_TRUE(cc->sendControl(ControlMessage::connectRequest("tester", AudioPacket::VERSION)));
    auto got = recvFrame(*sc);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->packetType(), PacketType::Control);

    // server -> client: AUDIO_RX with payload
    std::vector<std::uint8_t> audio = {11, 22, 33, 44, 55};
    ASSERT_TRUE(sc->sendRxAudio(audio.data(), 0, audio.size()));
    got = recvFrame(*cc);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->packetType(), PacketType::AudioRx);
    EXPECT_EQ(got->payload(), audio);

    // client -> server: HEARTBEAT
    ASSERT_TRUE(cc->sendHeartbeat());
    got = recvFrame(*sc);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->packetType(), PacketType::Heartbeat);

    // server -> client: TX audio (the client's "receive TX" path is unusual but
    // the frame must still transit intact)
    std::vector<std::uint8_t> tx = {7, 7};
    ASSERT_TRUE(sc->sendTxAudio(tx.data(), tx.size()));
    got = recvFrame(*cc);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->packetType(), PacketType::AudioTx);
    EXPECT_EQ(got->payload(), tx);

    EXPECT_EQ(sc->crcErrors(), 0);
    EXPECT_EQ(cc->crcErrors(), 0);
}

TEST(TcpTransport, ServerAggregatesPerClientStats) {
    TcpServerTransport server;
    TcpClientTransport client;
    std::shared_ptr<ClientConnection> sc, cc;
    ASSERT_TRUE(connectPair(server, client, sc, cc));

    EXPECT_EQ(server.packetsReceived(), 0);
    EXPECT_EQ(server.packetsSent(), 0);

    ASSERT_TRUE(cc->sendHeartbeat());
    ASSERT_TRUE(cc->sendControl(ControlMessage::heartbeat()));
    ASSERT_TRUE(recvFrame(*sc).has_value());  // heartbeat
    ASSERT_TRUE(recvFrame(*sc).has_value());  // control
    std::vector<std::uint8_t> one = {1};
    ASSERT_TRUE(sc->sendRxAudio(one.data(), 0, one.size()));  // RX frame, server -> client

    EXPECT_EQ(server.packetsReceived(), 2);
    EXPECT_EQ(server.packetsSent(), 1);
}

TEST(TcpTransport, SequentialClientsGetDistinctIds) {
    TcpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    auto port = static_cast<std::uint16_t>(server.port());

    TcpClientTransport c1, c2;
    auto cc1 = c1.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(cc1);
    std::shared_ptr<ClientConnection> sc1;
    ASSERT_EQ(server.acceptClient(2000, sc1, &err), IoStatus::Ok);

    auto cc2 = c2.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(cc2);
    std::shared_ptr<ClientConnection> sc2;
    ASSERT_EQ(server.acceptClient(2000, sc2, &err), IoStatus::Ok);

    EXPECT_EQ(sc1->clientAddress().id(), "tcp-1");
    EXPECT_EQ(sc2->clientAddress().id(), "tcp-2");
    EXPECT_TRUE(sc1->clientAddress().hasEndpoint());
}

TEST(TcpTransport, DisconnectClosesServerConnectionIdempotently) {
    TcpServerTransport server;
    TcpClientTransport client;
    std::shared_ptr<ClientConnection> sc, cc;
    ASSERT_TRUE(connectPair(server, client, sc, cc));

    EXPECT_FALSE(sc->isClosed());
    server.disconnectClient(sc);
    EXPECT_TRUE(sc->isClosed());
    server.disconnectClient(sc);  // harmless second call
    server.disconnectClient(nullptr);  // null-safe
}

TEST(TcpTransport, AggregateStatsZeroWithNoClients) {
    TcpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    EXPECT_EQ(server.packetsSent(), 0);
    EXPECT_EQ(server.packetsReceived(), 0);
    EXPECT_EQ(server.bytesSent(), 0);
    EXPECT_EQ(server.bytesReceived(), 0);
    EXPECT_EQ(server.crcErrors(), 0);
}

TEST(TcpTransport, CloseReleasesPort) {
    TcpServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    auto port = static_cast<std::uint16_t>(server.port());
    EXPECT_TRUE(server.isBound());

    server.close();
    EXPECT_FALSE(server.isBound());
    EXPECT_EQ(server.port(), -1);

    TcpServerTransport server2;
    EXPECT_TRUE(server2.bind(port, &err)) << err;
}

TEST(TcpTransport, ConnectTwiceWhileLiveFails) {
    TcpServerTransport server;
    TcpClientTransport client;
    std::shared_ptr<ClientConnection> sc, cc;
    ASSERT_TRUE(connectPair(server, client, sc, cc));

    std::string err;
    auto again = client.connect("127.0.0.1", static_cast<std::uint16_t>(server.port()),
                                2000, &err);
    EXPECT_EQ(again, nullptr);
    EXPECT_EQ(err, "Already connected");
}

// Validates the bind knob: a loopback-only server still accepts a loopback
// client (and the default-wildcard path is what every other test exercises).
TEST(TcpTransport, LoopbackBindConfigAcceptsLoopbackClient) {
    TcpServerTransport server("127.0.0.1");  // loopback-only
    TcpClientTransport client;
    std::shared_ptr<ClientConnection> sc, cc;
    ASSERT_TRUE(connectPair(server, client, sc, cc));

    ASSERT_TRUE(cc->sendHeartbeat());
    auto got = recvFrame(*sc);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->packetType(), PacketType::Heartbeat);
}
