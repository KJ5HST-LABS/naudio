// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — UDP transport + DualServerTransport (end-to-end).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// The UDP transport stack end-to-end over loopback
// (UdpClientTransport <-> UdpServerTransport, bidirectional) plus the composite
// DualServerTransport accepting a TCP and a UDP client on one port number at the
// same time. Hardware-free.

#include "naudio/net/DualServerTransport.hpp"
#include "naudio/net/TcpClientTransport.hpp"
#include "naudio/net/Transport.hpp"
#include "naudio/net/UdpClientTransport.hpp"
#include "naudio/net/UdpServerTransport.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace naudio;       // AudioPacket, ControlMessage
using namespace naudio::net;  // transports

namespace {

UdpReliabilityConfig passthrough() {
    UdpReliabilityConfig c;
    c.reorderWindowSize = 0;
    return c;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

}  // namespace

// Full bidirectional UDP loopback: a real UdpClientTransport connection exchanges
// audio with a real UdpServerTransport-accepted connection.
TEST(DualTransport, UdpClientServerFullLoopbackRoundTrip) {
    UdpServerTransport server;
    server.setReliabilityConfig(passthrough());
    std::string err;
    ASSERT_TRUE(server.bind(0, &err)) << err;
    auto port = static_cast<std::uint16_t>(server.port());

    UdpClientTransport client(passthrough());
    auto cc = client.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(cc) << err;

    // App-level handshake: the client must announce itself so the server's demux
    // registers a session (UDP has no transport-level handshake).
    ASSERT_TRUE(cc->sendControl(ControlMessage::connectRequest("u", AudioPacket::VERSION)));

    std::shared_ptr<ClientConnection> sc;
    ASSERT_EQ(server.acceptClient(2000, sc, &err), IoStatus::Ok) << err;
    ASSERT_TRUE(sc);
    ASSERT_TRUE(sc->receivePacket(1000).hasPacket());  // drain the CONNECT_REQUEST

    // server -> client (RX audio)
    std::vector<std::uint8_t> audio = {4, 5, 6};
    ASSERT_TRUE(sc->sendRxAudio(audio.data(), 0, audio.size()));
    ReceiveResult r = cc->receivePacket(2000);
    ASSERT_TRUE(r.hasPacket());
    EXPECT_EQ(r.packet->packetType(), PacketType::AudioRx);
    EXPECT_EQ(r.packet->payload(), audio);

    // client -> server (TX audio), routed by the server demux back to sc
    std::vector<std::uint8_t> tx = {1, 2};
    ASSERT_TRUE(cc->sendTxAudio(tx.data(), tx.size()));
    ReceiveResult r2 = sc->receivePacket(2000);
    ASSERT_TRUE(r2.hasPacket());
    EXPECT_EQ(r2.packet->packetType(), PacketType::AudioTx);
    EXPECT_EQ(r2.packet->payload(), tx);

    server.close();
    client.close();
}

TEST(DualTransport, PortBeforeBindIsMinusOne) {
    DualServerTransport server;
    EXPECT_EQ(server.port(), -1);
    EXPECT_FALSE(server.isBound());
}

TEST(DualTransport, BindBindsBothAndPortIsTcpPort) {
    DualServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err)) << err;
    EXPECT_TRUE(server.isBound());
    EXPECT_GT(server.port(), 0);
    server.close();
    EXPECT_FALSE(server.isBound());
}

TEST(DualTransport, AcceptBeforeBindErrors) {
    DualServerTransport server;
    std::shared_ptr<ClientConnection> out;
    std::string err;
    EXPECT_EQ(server.acceptClient(100, out, &err), IoStatus::Error);
}

// THE GATE: one DUAL server, one port, a TCP client and a UDP client connected
// simultaneously — both are accepted.
TEST(DualTransport, AcceptsTcpAndUdpClientSimultaneously) {
    DualServerTransport server;  // default config -> UDP passthrough
    std::string err;
    ASSERT_TRUE(server.bind(0, &err)) << err;
    auto port = static_cast<std::uint16_t>(server.port());

    TcpClientTransport tcpClient;
    auto tc = tcpClient.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(tc) << err;

    UdpClientTransport udpClient;
    auto uc = udpClient.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(uc) << err;
    ASSERT_TRUE(uc->sendControl(ControlMessage::connectRequest("u", AudioPacket::VERSION)));

    bool hasTcp = false, hasUdp = false;
    for (int i = 0; i < 2; i++) {
        std::shared_ptr<ClientConnection> c;
        ASSERT_EQ(server.acceptClient(2000, c, &err), IoStatus::Ok) << "accept " << i;
        ASSERT_TRUE(c);
        if (startsWith(c->clientAddress().id(), "tcp-")) hasTcp = true;
        if (startsWith(c->clientAddress().id(), "udp-")) hasUdp = true;
    }
    EXPECT_TRUE(hasTcp);
    EXPECT_TRUE(hasUdp);

    server.close();
    tcpClient.close();
    udpClient.close();
}

// Aggregate stats sum both sub-transports (zero with no clients).
TEST(DualTransport, AggregateStatsZeroWithNoClients) {
    DualServerTransport server;
    std::string err;
    ASSERT_TRUE(server.bind(0, &err));
    EXPECT_EQ(server.packetsSent(), 0);
    EXPECT_EQ(server.packetsReceived(), 0);
    EXPECT_EQ(server.bytesSent(), 0);
    EXPECT_EQ(server.bytesReceived(), 0);
    EXPECT_EQ(server.crcErrors(), 0);
    server.close();
}
