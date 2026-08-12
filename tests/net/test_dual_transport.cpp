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

// ---------------------------------------------------------------------------
// Issue #73 — the paired UDP bind is retried when the caller named no port.
//
// WHY A DOUBLE. The fault is the OS refusing a UDP bind on the very port its own
// allocator just handed out to TCP (WSAEACCES, from a WinNAT/Hyper-V reserved block).
// No test can provoke that on demand — it depends on which port the OS chose, which is
// exactly why it only ever surfaced as a Windows CI intermittent, on a commit that
// touched documentation alone. So the refusal is injected through the bindUdpTo seam
// and everything else in the bind path stays real: the TCP bind, the rollback, the
// port resolution, the loop, the error text.
//
// THE ASSERTION IS A COUNT, NEVER A DURATION. bindAttempts() cannot pass vacuously in
// either direction — a fix that stops retrying reads 1, and one that retries when it
// must not reads >1. Timing could not separate those.
namespace {

// Refuses the first `failures` paired UDP binds, then behaves normally.
class FlakyUdpBindTransport : public DualServerTransport {
public:
    explicit FlakyUdpBindTransport(int failures) : failures_(failures) {}
    int udpBindCalls() const { return calls_; }

protected:
    bool bindUdpTo(std::uint16_t port, std::string* err) override {
        if (++calls_ <= failures_) {
            // The shape of the real refusal: Windows reports a reserved range as a
            // permission denial, not as an address already in use.
            if (err) *err = "bind() failed (errno=10013)";
            return false;
        }
        return DualServerTransport::bindUdpTo(port, err);
    }

private:
    int failures_;
    int calls_{0};
};

}  // namespace

// A port-0 caller survives a run of refusals and lands on a working pair.
TEST(DualTransport, Port0BindRetriesPastRefusedUdpPorts) {
    constexpr int kRefusals = 3;
    FlakyUdpBindTransport server(kRefusals);
    std::string err;

    ASSERT_TRUE(server.bind(0, &err)) << err;
    EXPECT_TRUE(server.isBound());
    EXPECT_GT(server.port(), 0);
    // It retried, and it retried exactly as far as it had to.
    EXPECT_EQ(server.bindAttempts(), kRefusals + 1);
    EXPECT_EQ(server.udpBindCalls(), kRefusals + 1);
    server.close();
}

// A first-try success must not look like a retry — the negative control for the arm
// above, without which bindAttempts() could be a constant and both would pass.
TEST(DualTransport, Port0BindReportsOneAttemptWhenUdpNeverRefuses) {
    FlakyUdpBindTransport server(0);
    std::string err;
    ASSERT_TRUE(server.bind(0, &err)) << err;
    EXPECT_EQ(server.bindAttempts(), 1);
    server.close();
}

// THE GUARD ON THE RETRY'S SCOPE. A caller that NAMED a port gets exactly one attempt.
// Retrying there would either serve a different port than the one asked for, or paper
// over a genuine privilege refusal — the cost that made this a decision rather than an
// obvious fix. Confining the retry to port 0 is what removes it.
TEST(DualTransport, NamedPortBindIsNeverRetried) {
    // Take a real port first so the named bind below asks for something plausible.
    DualServerTransport donor;
    std::string err;
    ASSERT_TRUE(donor.bind(0, &err)) << err;
    const auto named = static_cast<std::uint16_t>(donor.port());
    donor.close();

    FlakyUdpBindTransport server(1);  // one refusal is enough to prove it does not retry
    EXPECT_FALSE(server.bind(named, &err));
    EXPECT_EQ(server.bindAttempts(), 1);
    EXPECT_EQ(server.udpBindCalls(), 1);
    EXPECT_FALSE(server.isBound());
}

// Exhaustion is bounded, reports the UNDERLYING refusal rather than a summary of it,
// and strands no TCP listener.
TEST(DualTransport, Port0BindGivesUpAfterBudgetAndPreservesTheRealError) {
    FlakyUdpBindTransport server(DualServerTransport::kPort0BindAttempts + 1);  // never succeeds
    std::string err;

    EXPECT_FALSE(server.bind(0, &err));
    EXPECT_EQ(server.bindAttempts(), DualServerTransport::kPort0BindAttempts);
    EXPECT_EQ(server.udpBindCalls(), DualServerTransport::kPort0BindAttempts);
    // The diagnosable part of the error survives the retry.
    EXPECT_NE(err.find("10013"), std::string::npos) << err;
    // Rollback held on every attempt, not just the first.
    EXPECT_FALSE(server.isBound());
    EXPECT_EQ(server.port(), -1);
}

// The budget must clear a CONTIGUOUS reserved block, not one unlucky port: ephemeral
// ports are handed out sequentially (+1 per bind, measured), so each retry steps one
// port further into a reserved range. A budget trimmed to "a few" would give up inside
// a typical 16-port WinNAT block and fix nothing. This pins the reasoning, not the
// number — the constant is free to move above the floor.
TEST(DualTransport, Port0BindBudgetClearsAContiguousReservedBlock) {
    EXPECT_GT(DualServerTransport::kPort0BindAttempts, 16);
}
