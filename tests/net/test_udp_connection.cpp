// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — UdpClientConnection (the reliability hub).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// UdpClientConnection (the reliability hub) over both receive modes:
//   server-fed   — enqueueReceived() feeds the pipeline; receivePacket() drains
//                  the ordered queue. The reorder/FEC wiring is exercised here
//                  with DETERMINISTIC induced loss (we choose exactly which
//                  packets reach enqueueReceived — the §3.9 loss-injection point,
//                  realized at the connection seam rather than the wire so the
//                  FEC-recovery gate is 100% reproducible, not timing-luck).
//   client-owned — receivePacket() reads a real loopback UDP socket; a raw peer
//                  socket drives it (round-trip, over-the-wire FEC recovery with
//                  one datagram withheld, and the ARQ NACK-retransmit / ACK path).
// Hardware-free.

#include "naudio/FecEncoder.hpp"
#include "naudio/net/Socket.hpp"
#include "naudio/net/UdpClientConnection.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace naudio;       // AudioPacket, ControlMessage, FecEncoder
using namespace naudio::net;  // UdpClientConnection, Socket

namespace {

UdpReliabilityConfig passthroughCfg() {
    UdpReliabilityConfig c;
    c.reorderWindowSize = 0;  // no reorder, no FEC — straight passthrough
    return c;
}

AudioPacket rxPacket(std::int32_t seq, std::vector<std::uint8_t> payload) {
    return AudioPacket::createRxAudio(seq, std::move(payload));
}

// Distinct, deterministic 4-byte payload for a sequence number.
std::vector<std::uint8_t> payloadFor(std::int32_t seq) {
    return {static_cast<std::uint8_t>(seq), static_cast<std::uint8_t>(seq + 100), 0xAB, 0xCD};
}

// Drains exactly n packets from a connection (server-fed mode), asserting each
// poll yields a packet.
std::vector<AudioPacket> drainN(ClientConnection& c, int n) {
    std::vector<AudioPacket> out;
    for (int i = 0; i < n; i++) {
        ReceiveResult r = c.receivePacket(1000);
        if (!r.hasPacket()) break;
        out.push_back(std::move(*r.packet));
    }
    return out;
}

// Reads one datagram from a raw peer socket and deserializes it.
std::optional<AudioPacket> recvFromPeer(Socket& peer, int timeoutMs = 2000) {
    peer.setRecvTimeout(timeoutMs);
    std::vector<std::uint8_t> buf(UdpClientConnection::MAX_DATAGRAM_SIZE);
    RecvFromResult rr = peer.recvFrom(buf.data(), buf.size());
    if (rr.status != IoStatus::Ok) return std::nullopt;
    return AudioPacket::deserialize(buf.data(), rr.bytes);
}

}  // namespace

// ---------------------------------------------------------------------------
// Server-fed mode (no socket read)
// ---------------------------------------------------------------------------

TEST(UdpConnection, ServerFedPassthroughRoundTripsEachFrameType) {
    Socket shared = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    ASSERT_TRUE(shared.valid());
    UdpClientConnection conn(&shared, "127.0.0.1", 9999,
                             ClientAddress("udp-1", "127.0.0.1", 9999), passthroughCfg());

    conn.enqueueReceived(AudioPacket::createControl(0, ControlMessage::heartbeat().serialize()), 32);
    conn.enqueueReceived(rxPacket(1, {5, 6, 7, 8}), 23);
    conn.enqueueReceived(AudioPacket::createHeartbeat(2), 23);

    std::vector<AudioPacket> got = drainN(conn, 3);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].packetType(), PacketType::Control);
    EXPECT_EQ(got[1].packetType(), PacketType::AudioRx);
    EXPECT_EQ(got[1].payload(), (std::vector<std::uint8_t>{5, 6, 7, 8}));
    EXPECT_EQ(got[2].packetType(), PacketType::Heartbeat);
    EXPECT_EQ(conn.packetsReceived(), 3);
    EXPECT_EQ(conn.crcErrors(), 0);
}

TEST(UdpConnection, ServerFedTimeoutReturnsNoDataNotDead) {
    Socket shared = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    UdpClientConnection conn(&shared, "127.0.0.1", 9999,
                             ClientAddress("udp-1", "127.0.0.1", 9999), passthroughCfg());
    ReceiveResult r = conn.receivePacket(50);  // nothing enqueued
    EXPECT_FALSE(r.hasPacket());
    EXPECT_FALSE(r.closed);
}

TEST(UdpConnection, ServerFedReorderEmitsInSequenceOrder) {
    UdpReliabilityConfig cfg;
    cfg.reorderWindowSize = 8;
    cfg.reorderMaxHoldMs = 1000;  // long hold — no timeout flush during the test
    Socket shared = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    UdpClientConnection conn(&shared, "127.0.0.1", 9999,
                             ClientAddress("udp-1", "127.0.0.1", 9999), cfg);

    conn.enqueueReceived(rxPacket(0, payloadFor(0)), 23);
    conn.enqueueReceived(rxPacket(2, payloadFor(2)), 23);  // ahead — buffered
    conn.enqueueReceived(rxPacket(1, payloadFor(1)), 23);  // fills the gap -> drains 1,2
    conn.enqueueReceived(rxPacket(3, payloadFor(3)), 23);

    std::vector<AudioPacket> got = drainN(conn, 4);
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[0].sequence(), 0);
    EXPECT_EQ(got[1].sequence(), 1);
    EXPECT_EQ(got[2].sequence(), 2);
    EXPECT_EQ(got[3].sequence(), 3);
    EXPECT_GE(conn.packetsReordered(), 1);  // seq 2 was buffered then reordered out
}

// THE GATE (deterministic): one data packet of a parity block is withheld; the
// FEC decoder must reconstruct it and emit the recovered AUDIO_RX.
TEST(UdpConnection, ServerFedFecRecoversSingleLostPacket) {
    UdpReliabilityConfig cfg;
    cfg.reorderWindowSize = 0;  // isolate FEC from reordering
    cfg.fecEnabled = true;
    cfg.fecBlockSize = 5;
    Socket shared = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    UdpClientConnection conn(&shared, "127.0.0.1", 9999,
                             ClientAddress("udp-1", "127.0.0.1", 9999), cfg);

    // Build a full block of 5 and derive the parity exactly as a sender would.
    std::vector<AudioPacket> block;
    for (std::int32_t s = 0; s < 5; s++) block.push_back(rxPacket(s, payloadFor(s)));
    FecEncoder encoder(5);
    std::optional<AudioPacket> parity;
    for (const AudioPacket& p : block) parity = encoder.recordAndMaybeEmit(p);
    ASSERT_TRUE(parity.has_value());

    // Deliver every packet EXCEPT seq 2, then the parity.
    for (std::int32_t s = 0; s < 5; s++) {
        if (s == 2) continue;  // induced loss
        conn.enqueueReceived(block[s], 23);
    }
    conn.enqueueReceived(*parity, 23);

    // Drain: the four delivered packets plus the recovered seq 2.
    std::vector<AudioPacket> got = drainN(conn, 5);
    ASSERT_EQ(got.size(), 5u);
    EXPECT_EQ(conn.packetsRecoveredByFec(), 1);

    bool foundRecovered = false;
    for (const AudioPacket& p : got) {
        if (p.sequence() == 2) {
            foundRecovered = true;
            EXPECT_EQ(p.packetType(), PacketType::AudioRx);
            EXPECT_EQ(p.payload(), payloadFor(2));  // bytes reconstructed exactly
        }
    }
    EXPECT_TRUE(foundRecovered);
}

TEST(UdpConnection, ServerFedTwoLossesPerBlockAreNotRecovered) {
    UdpReliabilityConfig cfg;
    cfg.reorderWindowSize = 0;
    cfg.fecEnabled = true;
    cfg.fecBlockSize = 5;
    Socket shared = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    UdpClientConnection conn(&shared, "127.0.0.1", 9999,
                             ClientAddress("udp-1", "127.0.0.1", 9999), cfg);

    std::vector<AudioPacket> block;
    for (std::int32_t s = 0; s < 5; s++) block.push_back(rxPacket(s, payloadFor(s)));
    FecEncoder encoder(5);
    std::optional<AudioPacket> parity;
    for (const AudioPacket& p : block) parity = encoder.recordAndMaybeEmit(p);

    // Drop seq 1 AND seq 3 — unrecoverable.
    for (std::int32_t s = 0; s < 5; s++) {
        if (s == 1 || s == 3) continue;
        conn.enqueueReceived(block[s], 23);
    }
    conn.enqueueReceived(*parity, 23);
    EXPECT_EQ(conn.packetsRecoveredByFec(), 0);  // 2 missing -> no recovery
}

// ---------------------------------------------------------------------------
// Client-owned mode (real loopback socket, raw peer drives it)
// ---------------------------------------------------------------------------

TEST(UdpConnection, ClientOwnedRoundTripsOverLoopback) {
    Socket peer = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    ASSERT_TRUE(peer.valid());
    std::uint16_t peerPort = peer.localPort();

    Socket clientSock = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    ASSERT_TRUE(clientSock.valid());
    std::uint16_t clientPort = clientSock.localPort();

    UdpClientConnection conn(std::move(clientSock), "127.0.0.1", peerPort,
                             ClientAddress("client", "127.0.0.1", peerPort), passthroughCfg());

    // client -> peer (send path over the wire)
    ASSERT_TRUE(conn.sendHeartbeat());
    std::optional<AudioPacket> atPeer = recvFromPeer(peer);
    ASSERT_TRUE(atPeer.has_value());
    EXPECT_EQ(atPeer->packetType(), PacketType::Heartbeat);

    // peer -> client (receive path over the wire)
    AudioPacket toClient = rxPacket(7, {1, 2, 3});
    std::vector<std::uint8_t> bytes = toClient.serialize();
    ASSERT_TRUE(peer.sendTo(bytes.data(), bytes.size(), "127.0.0.1", clientPort));
    ReceiveResult r = conn.receivePacket(2000);
    ASSERT_TRUE(r.hasPacket());
    EXPECT_EQ(r.packet->packetType(), PacketType::AudioRx);
    EXPECT_EQ(r.packet->payload(), (std::vector<std::uint8_t>{1, 2, 3}));
}

// Over-the-wire FEC recovery: the peer withholds exactly one data datagram.
TEST(UdpConnection, ClientOwnedFecRecoversOverLoopback) {
    Socket peer = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    std::uint16_t peerPort = peer.localPort();
    Socket clientSock = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    std::uint16_t clientPort = clientSock.localPort();

    UdpReliabilityConfig cfg;
    cfg.reorderWindowSize = 0;
    cfg.fecEnabled = true;
    cfg.fecBlockSize = 5;
    UdpClientConnection conn(std::move(clientSock), "127.0.0.1", peerPort,
                             ClientAddress("client", "127.0.0.1", peerPort), cfg);

    std::vector<AudioPacket> blk;
    for (std::int32_t s = 0; s < 5; s++) blk.push_back(rxPacket(s, payloadFor(s)));
    FecEncoder encoder(5);
    std::optional<AudioPacket> parity;
    for (const AudioPacket& p : blk) parity = encoder.recordAndMaybeEmit(p);
    ASSERT_TRUE(parity.has_value());

    auto sendToClient = [&](const AudioPacket& p) {
        std::vector<std::uint8_t> b = p.serialize();
        ASSERT_TRUE(peer.sendTo(b.data(), b.size(), "127.0.0.1", clientPort));
    };
    for (std::int32_t s = 0; s < 5; s++) {
        if (s == 2) continue;  // withheld on the wire
        sendToClient(blk[s]);
    }
    sendToClient(*parity);

    // Read up to all 6 datagrams' worth of output; collect what surfaces.
    std::vector<AudioPacket> got;
    for (int i = 0; i < 10 && got.size() < 5; i++) {
        ReceiveResult r = conn.receivePacket(1000);
        if (r.hasPacket()) got.push_back(std::move(*r.packet));
        else if (r.closed) break;
    }

    EXPECT_EQ(conn.packetsRecoveredByFec(), 1);
    bool foundRecovered = false;
    for (const AudioPacket& p : got) {
        if (p.sequence() == 2) {
            foundRecovered = true;
            EXPECT_EQ(p.payload(), payloadFor(2));
        }
    }
    EXPECT_TRUE(foundRecovered);
}

// ARQ: a critical control is NACK'd by the peer and must be retransmitted; an
// ACK then clears it (consumed, never surfaced to the application).
TEST(UdpConnection, ClientOwnedControlReliabilityNackRetransmitAndAck) {
    Socket peer = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    std::uint16_t peerPort = peer.localPort();
    Socket clientSock = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    std::uint16_t clientPort = clientSock.localPort();

    UdpReliabilityConfig cfg;
    cfg.reorderWindowSize = 0;
    cfg.controlReliabilityEnabled = true;
    UdpClientConnection conn(std::move(clientSock), "127.0.0.1", peerPort,
                             ClientAddress("client", "127.0.0.1", peerPort), cfg);

    // Client sends a critical control; the peer receives it and notes its seq.
    ASSERT_TRUE(conn.sendControl(ControlMessage::txGranted()));
    std::optional<AudioPacket> atPeer = recvFromPeer(peer);
    ASSERT_TRUE(atPeer.has_value());
    EXPECT_EQ(atPeer->packetType(), PacketType::Control);
    std::int32_t criticalSeq = atPeer->sequence();

    // Peer NACKs that sequence -> the client retransmits the stored packet.
    AudioPacket nack = AudioPacket::createControl(0, ControlMessage::nack(criticalSeq).serialize());
    std::vector<std::uint8_t> nb = nack.serialize();
    ASSERT_TRUE(peer.sendTo(nb.data(), nb.size(), "127.0.0.1", clientPort));

    ReceiveResult consumed = conn.receivePacket(2000);  // NACK is consumed, not surfaced
    EXPECT_FALSE(consumed.hasPacket());

    std::optional<AudioPacket> retransmitted = recvFromPeer(peer);
    ASSERT_TRUE(retransmitted.has_value());
    EXPECT_EQ(retransmitted->packetType(), PacketType::Control);
    EXPECT_EQ(retransmitted->sequence(), criticalSeq);  // same packet resent
    EXPECT_GE(conn.controlRetransmits(), 1);

    // Peer ACKs -> consumed, pending cleared.
    AudioPacket ack = AudioPacket::createControl(0, ControlMessage::controlAck(criticalSeq).serialize());
    std::vector<std::uint8_t> ab = ack.serialize();
    ASSERT_TRUE(peer.sendTo(ab.data(), ab.size(), "127.0.0.1", clientPort));
    ReceiveResult ackConsumed = conn.receivePacket(2000);
    EXPECT_FALSE(ackConsumed.hasPacket());
}

// C2: under a flood with no consumer draining, the ordered queue must stay
// bounded (drop-oldest) instead of growing without limit.
TEST(UdpConnection, OrderedQueueBoundedUnderFlood) {
    Socket shared = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    UdpClientConnection conn(&shared, "127.0.0.1", 9999,
                             ClientAddress("udp-1", "127.0.0.1", 9999), passthroughCfg());

    const std::size_t cap = BlockingPacketQueue::kDefaultMaxSize;
    const int flood = static_cast<int>(cap) + 200;
    for (int i = 0; i < flood; i++) {
        conn.enqueueReceived(rxPacket(i, payloadFor(i)), 23);  // never drained
    }
    EXPECT_EQ(conn.orderedQueueSize(), cap);  // capped, not 'flood'
    EXPECT_EQ(conn.orderedQueueDrops(),
              static_cast<std::int64_t>(flood) - static_cast<std::int64_t>(cap));  // 200 dropped
}

// C3: a datagram from a source other than the configured server endpoint is
// dropped as no-data and must NOT count toward the CRC-error teardown limit, so a
// spoof flood cannot kill a healthy connection. The legit datagram still surfaces.
TEST(UdpConnection, ClientOwnedDropsSpoofedSource) {
    Socket peer = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    std::uint16_t peerPort = peer.localPort();
    Socket attacker = Socket::bindUdp("127.0.0.1", 0, false, nullptr);  // a different source port
    Socket clientSock = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    std::uint16_t clientPort = clientSock.localPort();

    UdpClientConnection conn(std::move(clientSock), "127.0.0.1", peerPort,
                             ClientAddress("client", "127.0.0.1", peerPort), passthroughCfg());

    // Flood valid frames from the WRONG source (the attacker's port != peerPort).
    for (int i = 0; i < 25; i++) {
        std::vector<std::uint8_t> sb = rxPacket(i, {9, 9, 9}).serialize();
        ASSERT_TRUE(attacker.sendTo(sb.data(), sb.size(), "127.0.0.1", clientPort));
    }
    // Then the real peer sends one good frame.
    std::vector<std::uint8_t> gb = rxPacket(100, {1, 2, 3}).serialize();
    ASSERT_TRUE(peer.sendTo(gb.data(), gb.size(), "127.0.0.1", clientPort));

    std::optional<AudioPacket> surfaced;
    for (int i = 0; i < 40 && !surfaced.has_value(); i++) {
        ReceiveResult r = conn.receivePacket(500);
        ASSERT_FALSE(r.closed) << "spoof flood tore the connection down";
        if (r.hasPacket()) surfaced = std::move(*r.packet);
    }
    ASSERT_TRUE(surfaced.has_value());
    EXPECT_EQ(surfaced->payload(), (std::vector<std::uint8_t>{1, 2, 3}));  // the legit frame
    EXPECT_EQ(conn.crcErrors(), 0);  // spoofed datagrams were never counted as CRC errors
}

TEST(UdpConnection, CloseMakesReceiveReturnDead) {
    Socket shared = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    UdpClientConnection conn(&shared, "127.0.0.1", 9999,
                             ClientAddress("udp-1", "127.0.0.1", 9999), passthroughCfg());
    EXPECT_FALSE(conn.isClosed());
    conn.close();
    EXPECT_TRUE(conn.isClosed());
    ReceiveResult r = conn.receivePacket(100);
    EXPECT_TRUE(r.closed);
    conn.close();  // idempotent
}
