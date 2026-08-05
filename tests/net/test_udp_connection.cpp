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

#include "naudio/FecDecoder.hpp"
#include "naudio/FecEncoder.hpp"
#include "naudio/net/Socket.hpp"
#include "naudio/net/UdpClientConnection.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
    // Reordering is not loss: every slot was eventually delivered, so no gap was emitted.
    EXPECT_EQ(conn.sequenceGaps(), 0);
}

// sequenceGaps() is MEASURED only when a reorder buffer exists, and reports -1 otherwise --
// the same "unavailable is not zero" convention packetsLost() uses, pointed the other way.
//
// This pair lives here rather than in the C-ABI arms because the -1 branch is UNREACHABLE from
// the public C surface: na_client_set_reliability_profile offers lan/wan/ft8 and every one of
// them sets reorderWindowSize = 8, so no C consumer can build a connection that takes it. A
// mutation turning that -1 into 0 therefore passes the entire C-ABI suite -- the TCP arm there
// asserts -1, but TCP is a different transport whose value comes from Transport's base default,
// so it never exercises this branch at all. Without these two tests the branch is untested.
TEST(UdpConnection, SequenceGapsUnmeasuredWithoutAReorderBuffer) {
    Socket shared = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    UdpClientConnection conn(&shared, "127.0.0.1", 9999,
                             ClientAddress("udp-1", "127.0.0.1", 9999), passthroughCfg());
    // Not 0: nothing is counting here, and a 0 would read as "nothing was lost".
    EXPECT_EQ(conn.sequenceGaps(), -1);
    // The complement holds in this direction too -- passthrough is where the gap TRACKER runs.
    EXPECT_TRUE(conn.measuresSequenceGaps());
}

TEST(UdpConnection, SequenceGapsCountsSlotsTheReorderBufferGaveUpOn) {
    UdpReliabilityConfig cfg;
    cfg.reorderWindowSize = 4;
    cfg.reorderMaxHoldMs = 0;  // flush on the next check rather than waiting on a real clock
    Socket shared = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    UdpClientConnection conn(&shared, "127.0.0.1", 9999,
                             ClientAddress("udp-1", "127.0.0.1", 9999), cfg);

    EXPECT_EQ(conn.sequenceGaps(), 0);        // measured, and nothing lost yet
    EXPECT_FALSE(conn.measuresSequenceGaps());  // ...so the OTHER counters are the dead ones

    conn.enqueueReceived(rxPacket(0, payloadFor(0)), 23);
    // 1 and 2 never arrive. 3 sits ahead of nextExpected and cannot drain; a zero hold means the
    // next timeout check force-flushes, emitting one gap per missing slot.
    conn.enqueueReceived(rxPacket(3, payloadFor(3)), 23);
    conn.receivePacket(20);
    conn.receivePacket(20);

    EXPECT_EQ(conn.sequenceGaps(), 2) << "one gap per slot the buffer gave up on (1 and 2)";
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

// ---------------------------------------------------------------------------
// FEC pending-block retention (issue #47)
// ---------------------------------------------------------------------------

// Pins the DERIVATION, not the constants it is built from. The decoder's pending
// idle bound is computed in initPipeline from the negotiated stream shape; this
// arm feeds it the shape udpWan actually ships and asserts the number that comes
// out, so a fill site that silently stops copying frameDurationMs — or a term
// quietly dropped from the formula — fails here rather than degrading recovery in
// the field, where the failure is invisible (nothing is emitted and no other
// counter moves).
TEST(UdpConnection, FecPendingIdleBoundIsDerivedFromTheNegotiatedStreamShape) {
    Socket shared = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    ASSERT_TRUE(shared.valid());

    // Exactly what AudioStreamConfig::udpWan() produces, the only preset with FEC on.
    UdpReliabilityConfig cfg;
    cfg.reorderWindowSize = 8;
    cfg.reorderMaxHoldMs = 40;
    cfg.fecEnabled = true;
    cfg.fecBlockSize = 5;
    cfg.frameDurationMs = 10;   // UDP_FRAME_MS
    cfg.jitterMinMs = 60;
    cfg.jitterMaxMs = 400;      // UDP_WAN_BUFFER_MAX_MS
    UdpClientConnection conn(&shared, "127.0.0.1", 9999,
                             ClientAddress("udp-wan", "127.0.0.1", 9999), cfg);

    // block period (5 * 10) + reorder hold (40) + jitter max (400)
    EXPECT_EQ(490, conn.fecPendingIdleTimeoutMs());
    EXPECT_GT(conn.fecPendingIdleTimeoutMs(), FecDecoder::MIN_PENDING_IDLE_MS)
        << "the derived term is entirely masked by the floor — the derivation is decoration";
    EXPECT_EQ(0, conn.fecPendingPacketsDiscarded());
}

// The clamp is applied to the DERIVED value, not just to hand-set ones. A peer
// supplies frameDurationMs as an unvalidated u16 (ControlMessage), and the client
// applies it during the handshake, so an absurd cadence must not turn the repair
// cache into an unbounded hold.
TEST(UdpConnection, AnAbsurdPeerSuppliedCadenceCannotUnboundThePendingBlock) {
    Socket shared = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    ASSERT_TRUE(shared.valid());

    UdpReliabilityConfig cfg;
    cfg.fecEnabled = true;
    cfg.fecBlockSize = 10;
    cfg.frameDurationMs = 65535;  // the widest a u16 can carry
    cfg.jitterMaxMs = 400;
    UdpClientConnection conn(&shared, "127.0.0.1", 9999,
                             ClientAddress("udp-absurd", "127.0.0.1", 9999), cfg);

    EXPECT_EQ(FecDecoder::MAX_PENDING_IDLE_MS, conn.fecPendingIdleTimeoutMs());
}

// FEC off: no decoder, so both accessors report "unmeasured" rather than zero.
TEST(UdpConnection, FecAccessorsReportUnmeasuredWhenFecIsOff) {
    Socket shared = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    ASSERT_TRUE(shared.valid());
    UdpClientConnection conn(&shared, "127.0.0.1", 9999,
                             ClientAddress("udp-nofec", "127.0.0.1", 9999), passthroughCfg());
    EXPECT_EQ(-1, conn.fecPendingIdleTimeoutMs());
    EXPECT_EQ(-1, conn.fecPendingPacketsDiscarded());
}

// ---------------------------------------------------------------------------
// Consumer-driven hold timeouts, in BOTH receive modes (issue #52)
// ---------------------------------------------------------------------------
//
// The reorder buffer and the FEC decoder both hold packets against a deadline, and
// neither owns a thread — so each hold expires only when a caller ticks it. That caller
// is the CONSUMER's no-data limb in both modes, never the arrival path, because an
// arrival-driven tick cannot fire during a pause in traffic and a pause is exactly when a
// hold goes stale. Before #52 only the client-owned limb ticked; the server-fed limb
// (the connection na_server_set_reliability_profile(NA_RELIABILITY_UDP_WAN) builds) did
// not, so the same class with the same config behaved differently by construction mode.
//
// Both arms below assert the two modes AGREE, rather than asserting a number per mode.
// That is deliberate: the defect was a divergence, so the assertion that catches it
// coming back is an equality. The client-owned side doubles as #52's criterion-3
// evidence — if a fix had changed client behaviour, these arms would fail on the client
// leg, not silently pass.

namespace {

// The shape both arms drive, with the derived pending bound pinned to the FLOOR
// (MIN_PENDING_IDLE_MS) so an idle-timeout arm costs ~120 ms instead of udpWan's 490.
// The derivation itself is pinned by
// FecPendingIdleBoundIsDerivedFromTheNegotiatedStreamShape above; this is not that test.
UdpReliabilityConfig floorBoundCfg(int reorderMaxHoldMs) {
    UdpReliabilityConfig c;
    c.reorderWindowSize = 8;  // what every shipping UDP preset sets
    c.reorderMaxHoldMs = reorderMaxHoldMs;
    c.fecEnabled = true;
    // The encoder's own validated minimum, not a literal — initPipeline constructs a
    // FecEncoder from this field and it throws outside MIN..MAX_BLOCK_SIZE.
    c.fecBlockSize = static_cast<int>(FecEncoder::MIN_BLOCK_SIZE);
    c.frameDurationMs = 1;
    c.jitterMaxMs = 0;  // block period (2) + hold + 0 -> clamped up to the floor
    return c;
}

// Polls the way AudioStreamServer::ClientSession::receiveLoop does, until `done` holds on
// what has been delivered so far, or the budget expires. Bounded by COMPLETION rather
// than wall clock: slower hardware simply takes another poll. Safe in this direction —
// waiting longer only makes an already-elapsed hold timeout more certain to be observed,
// never less. The budget is a FAILURE ceiling, not the expected cost.
// Returns the sequences delivered while polling.
using Delivered = std::vector<std::int32_t>;
Delivered pollUntil(ClientConnection& c, int budgetMs,
                    const std::function<bool(const Delivered&)>& done) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    Delivered seqs;
    while (std::chrono::steady_clock::now() < deadline) {
        ReceiveResult r = c.receivePacket(25);
        if (r.hasPacket()) seqs.push_back(r.packet->sequence());
        if (done(seqs)) break;
    }
    return seqs;
}

}  // namespace

// The FEC decoder's idle bound must actually RUN in both modes. Measured before the fix:
// after 3x udpWan's 490 ms bound with a live consumer, a client-owned connection released
// all 12 held packets and a server-fed one released 0.
TEST(UdpConnection, BothReceiveModesTickTheFecIdleBound) {
    // Well under the cap, so a discard here CANNOT be a cap eviction. That is this arm's
    // discriminator: with the cap ruled out, the idle timeout is the only mechanism left
    // that can move the counter, so a non-zero reading cannot be explained any other way.
    const int fed = 6;
    ASSERT_LT(static_cast<std::size_t>(fed), FecDecoder::MAX_PENDING_PACKETS);

    std::int64_t discarded[2] = {-1, -1};
    for (int mode = 0; mode < 2; mode++) {
        const bool serverFed = (mode == 0);
        Socket sock = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
        ASSERT_TRUE(sock.valid());
        const ClientAddress addr("udp-52-fec", "127.0.0.1", 9999);
        std::unique_ptr<UdpClientConnection> conn;
        if (serverFed) {  // Socket* ctor -> ownsSocket == false
            conn = std::make_unique<UdpClientConnection>(&sock, "127.0.0.1", 9999, addr,
                                                         floorBoundCfg(0));
        } else {  // Socket-by-value ctor -> ownsSocket == true; no peer, so it times out
            conn = std::make_unique<UdpClientConnection>(std::move(sock), "127.0.0.1", 9999,
                                                         addr, floorBoundCfg(0));
        }
        ASSERT_EQ(FecDecoder::MIN_PENDING_IDLE_MS, conn->fecPendingIdleTimeoutMs());

        for (int i = 0; i < fed; i++) conn->enqueueReceived(rxPacket(i, payloadFor(i)), 23);
        ASSERT_EQ(0, conn->fecPendingPacketsDiscarded())
            << "mode " << mode << ": the cap evicted before the idle bound was under test, so a "
            << "later non-zero reading would not isolate the timeout";

        // The packets themselves are delivered on arrival — the pending block is a repair
        // cache, so what the timeout discards is never audio.
        UdpClientConnection& c = *conn;
        const Delivered got = pollUntil(
            c, 2000, [&c](const Delivered&) { return c.fecPendingPacketsDiscarded() > 0; });
        EXPECT_EQ(fed, static_cast<int>(got.size())) << "mode " << mode;

        discarded[mode] = conn->fecPendingPacketsDiscarded();
        EXPECT_GT(discarded[mode], 0)
            << (serverFed ? "server-fed" : "client-owned")
            << ": the idle bound never ran. The decoder owns no thread, so the bound exists "
               "only where a caller ticks it — this mode's no-data limb has stopped doing so "
               "(FecDecoder::checkTimeout's contract, issue #52).";
    }
    EXPECT_EQ(discarded[0], discarded[1])
        << "the two receive modes disagree about the idle bound on identical config — the "
           "asymmetry #52 removed has come back";
}

// The same dead limb also stranded REAL AUDIO, which is the more severe half: a packet
// held behind a gap was never released while traffic was paused, because the only tick
// was on arrival. Measured before the fix, after 600 ms of silence with a live consumer:
// server-fed delivered [0], client-owned delivered [0,2].
TEST(UdpConnection, BothReceiveModesReleaseAReorderHoldWhileTrafficIsPaused) {
    std::vector<std::int32_t> delivered[2];
    for (int mode = 0; mode < 2; mode++) {
        const bool serverFed = (mode == 0);
        Socket sock = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
        ASSERT_TRUE(sock.valid());
        const ClientAddress addr("udp-52-reorder", "127.0.0.1", 9999);
        std::unique_ptr<UdpClientConnection> conn;
        if (serverFed) {
            conn = std::make_unique<UdpClientConnection>(&sock, "127.0.0.1", 9999, addr,
                                                         floorBoundCfg(10));
        } else {
            conn = std::make_unique<UdpClientConnection>(std::move(sock), "127.0.0.1", 9999,
                                                         addr, floorBoundCfg(10));
        }

        // seq 1 is withheld, so seq 2 is HELD by the reorder buffer awaiting the gap. Then
        // traffic stops: nothing further arrives, so an arrival-driven tick can never fire
        // and only the consumer's own deadline can release seq 2.
        conn->enqueueReceived(rxPacket(0, payloadFor(0)), 23);
        conn->enqueueReceived(rxPacket(2, payloadFor(2)), 23);

        UdpClientConnection& c = *conn;
        delivered[mode] =
            pollUntil(c, 2000, [](const Delivered& s) { return s.size() >= 2; });
        EXPECT_EQ(Delivered({0, 2}), delivered[mode])
            << (serverFed ? "server-fed" : "client-owned")
            << ": a packet held behind a gap was not released while traffic was paused. This "
               "loses AUDIO, not just a repair opportunity — the hold timeout is ticked from "
               "the consumer's no-data limb, and this mode's has stopped (issue #52).";
    }
    EXPECT_EQ(delivered[0], delivered[1])
        << "the two receive modes disagree about releasing a reorder hold on identical config";
}

// The tick above is what MAKES the decoder's retention reachable, and reachability is not
// free: a slot the decoder discards was already delivered on arrival, so a later parity
// that reads the hole as peer loss "recovers" a byte-exact DUPLICATE. This arm drives the
// whole pipeline — reorder buffer, decoder, real FecEncoder parity — and asserts no frame
// is ever delivered twice at ZERO packet loss. Measured at 6 frames for 5 sent before
// FecDecoder's discarded-slot guard existed, on BOTH modes (the client-owned one predates
// #52 entirely, so this was a live defect, not one #52 introduced).
TEST(UdpConnection, NeitherReceiveModeFabricatesADuplicateAfterADiscard) {
    for (int mode = 0; mode < 2; mode++) {
        const bool serverFed = (mode == 0);
        Socket sock = Socket::bindUdp("127.0.0.1", 0, false, nullptr);
        ASSERT_TRUE(sock.valid());
        const ClientAddress addr("udp-52-dup", "127.0.0.1", 9999);
        std::unique_ptr<UdpClientConnection> conn;
        if (serverFed) {
            conn = std::make_unique<UdpClientConnection>(&sock, "127.0.0.1", 9999, addr,
                                                         floorBoundCfg(0));
        } else {
            conn = std::make_unique<UdpClientConnection>(std::move(sock), "127.0.0.1", 9999,
                                                         addr, floorBoundCfg(0));
        }
        UdpClientConnection& c = *conn;

        // A real 5-packet block and its real parity, so the declared range is whatever the
        // shipping encoder emits rather than something this test invented.
        FecEncoder enc(5);
        std::vector<AudioPacket> blk;
        std::optional<AudioPacket> parity;
        for (int i = 0; i < 5; i++) {
            AudioPacket p = rxPacket(i, payloadFor(i));
            blk.push_back(p);
            if (auto e = enc.recordAndMaybeEmit(p)) parity = *e;
        }
        ASSERT_TRUE(parity.has_value());
        parity->setSequence(5);

        std::map<std::int32_t, int> seen;
        auto collect = [&](int budgetMs) {
            auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
            while (std::chrono::steady_clock::now() < deadline) {
                ReceiveResult r = c.receivePacket(25);
                if (r.hasPacket()) seen[r.packet->sequence()]++;
            }
        };

        // seq 0 arrives and is delivered, then the block idles past the bound so the tick
        // discards its stored copy — leaving exactly ONE hole, which is the only shape that
        // can fabricate (two or more missing cannot be recovered at all).
        c.enqueueReceived(blk[0], 200);
        collect(120);
        ASSERT_EQ(1, seen[0]) << "mode " << mode << ": premise — seq 0 was delivered on arrival";
        collect(static_cast<int>(FecDecoder::MIN_PENDING_IDLE_MS + 200));
        ASSERT_GT(c.fecPendingPacketsDiscarded(), 0)
            << "mode " << mode << ": premise — the stored copy of seq 0 must have been discarded";

        for (int i = 1; i < 5; i++) c.enqueueReceived(blk[i], 200);
        c.enqueueReceived(*parity, 200);
        collect(300);

        int total = 0;
        for (auto& [seq, n] : seen) {
            total += n;
            EXPECT_EQ(1, n) << (serverFed ? "server-fed" : "client-owned") << ": seq " << seq
                            << " delivered " << n << " times. A discarded slot was recovered as a "
                               "duplicate of audio already delivered (FecDecoder's "
                               "discarded-slot guard, issue #52).";
        }
        EXPECT_EQ(5, total) << (serverFed ? "server-fed" : "client-owned")
                            << ": " << total << " frames delivered for 5 sent";
    }
}
