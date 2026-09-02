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

// Issue #29: the kernel receive-drop counter must actually be ENABLED on the socket a real
// client connection runs on, not merely implemented in Socket. The arm is split by platform
// rather than skipped, because "-1 where the mechanism does not exist" is half the contract.
//
// This is the wiring assertion. Whether the counter MOVES under induced loss is a separate
// question, asked where the loss can be induced (tests/c_client_stats.c).
TEST(DualTransport, AClientOwnedUdpConnectionMeasuresKernelReceiveDrops) {
    UdpServerTransport server;
    server.setReliabilityConfig(passthrough());
    std::string err;
    ASSERT_TRUE(server.bind(0, &err)) << err;
    auto port = static_cast<std::uint16_t>(server.port());

    UdpClientTransport client(passthrough());
    auto cc = client.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(cc) << err;

#if defined(__linux__)
    // 0, not -1: measured, and nothing has been dropped on a connection this young. A -1 here
    // means UdpClientTransport stopped enabling the counter and every consumer silently lost
    // the reading — which looks exactly like running on macOS.
    EXPECT_EQ(cc->socketReceiveDrops(), 0);
#else
    EXPECT_EQ(cc->socketReceiveDrops(), -1);
#endif
}

// A SERVER-side connection shares the server's socket, which nobody enables the counter on, so
// it reports unmeasured on every platform. Pinned so the -1 is a decision rather than an
// accident that could be "fixed" into a misleading 0.
TEST(DualTransport, AServerSideUdpConnectionDoesNotMeasureKernelReceiveDrops) {
    UdpServerTransport server;
    server.setReliabilityConfig(passthrough());
    std::string err;
    ASSERT_TRUE(server.bind(0, &err)) << err;
    auto port = static_cast<std::uint16_t>(server.port());

    UdpClientTransport client(passthrough());
    auto cc = client.connect("127.0.0.1", port, 2000, &err);
    ASSERT_TRUE(cc) << err;
    // App-level handshake, as UdpClientServerFullLoopbackRoundTrip below: UDP has no
    // transport-level handshake, so the server's demux only registers a session once the
    // client announces itself.
    ASSERT_TRUE(cc->sendControl(ControlMessage::connectRequest("u", AudioPacket::VERSION)));

    std::shared_ptr<ClientConnection> sc;
    ASSERT_EQ(server.acceptClient(2000, sc, &err), IoStatus::Ok) << err;
    ASSERT_TRUE(sc);
    EXPECT_EQ(sc->socketReceiveDrops(), -1);

    server.close();
    client.close();
}

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
// Issue #101 — and the retry DRAWS the port, because asking the OS again walks.
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
//
// `squats` (issue #98) makes the OS do the refusing instead of the injector: before a bind
// that has reached the REAL path, occupy the paired UDP port with a live UdpServerTransport.
// That transport passes reuseAddr=false deliberately (issue #83), so the second bind to the
// same port is refused on every platform — the portable stand-in for the WSAEACCES this whole
// section is about, produced by the kernel rather than by a `return false`.
//
// `blockWidth` (issue #101) is a reserved block: every UDP bind whose port lies within
// `blockWidth` ports above the FIRST port asked for is refused, exactly as a WinNAT
// reservation refuses the run of ports the OS is about to hand out next. The first port
// asked is the OS's own pick, so the block always contains it and a retry is always owed.
//
// `tcpFailures` (issue #101) refuses the first `tcpFailures` DRAWN ports on the TCP side, the
// way a port another socket already holds is refused — the other half of the retry, which
// reaches no UDP bind at all.
class FlakyUdpBindTransport : public DualServerTransport {
public:
    explicit FlakyUdpBindTransport(int failures, int squats = 0, int blockWidth = 0,
                                   int tcpFailures = 0)
        : failures_(failures), squats_(squats), blockWidth_(blockWidth),
          tcpFailures_(tcpFailures) {}
    int udpBindCalls() const { return calls_; }

    // Refusals this fixture injected: the first `failures` calls, plus every call inside
    // the block.
    int injectedRefusals() const { return injected_; }
    int injectedTcpRefusals() const { return injectedTcp_; }

    // Paired binds the OS itself refused, counted apart from the injected ones (issue #98).
    // The bind that follows the injected run is real, and on a Windows host with WinNAT
    // reserved ranges it can genuinely fail — the exact fault #73 exists for. Without this
    // the arms below assert "the first real bind always succeeds", which is false by
    // construction on the one platform they were written for.
    int realRefusals() const { return realRefusals_; }

    // Drawn ports the OS refused on the TCP side (issue #101): a port another socket holds,
    // or a TCP-reserved one. Each costs an attempt and reaches no UDP bind, so the arms owe
    // it to bindAttempts() and not to udpBindCalls().
    int realTcpRefusals() const { return realTcpRefusals_; }

    // Every port the TCP half was asked for, in order: 0 for the OS's pick, then the draws.
    const std::vector<std::uint16_t>& tcpPortsAsked() const { return tcpAsked_; }

    bool inBlock(int port) const {
        return blockWidth_ > 0 && port >= blockStart_ && port < blockStart_ + blockWidth_;
    }

protected:
    bool bindTcpTo(std::uint16_t port, std::string* err) override {
        tcpAsked_.push_back(port);
        if (port != 0 && injectedTcp_ < tcpFailures_) {
            ++injectedTcp_;
            // A drawn port another socket already holds: address-in-use, not a reservation.
            if (err) *err = "bind() failed (errno=10048)";
            return false;
        }
        const bool ok = DualServerTransport::bindTcpTo(port, err);
        if (!ok && port != 0) ++realTcpRefusals_;
        return ok;
    }

    bool bindUdpTo(std::uint16_t port, std::string* err) override {
        ++calls_;
        if (blockWidth_ > 0 && blockStart_ < 0) blockStart_ = port;  // the block starts at the OS's pick
        if (calls_ <= failures_ || inBlock(port)) {
            ++injected_;
            // The shape of the real refusal: Windows reports a reserved range as a
            // permission denial, not as an address already in use.
            if (err) *err = "bind() failed (errno=10013)";
            return false;
        }
        UdpServerTransport squatter;
        std::string squatErr;
        const bool squatted = squats_ > 0 && squatter.bind(port, &squatErr);
        if (squatted) --squats_;
        const bool ok = DualServerTransport::bindUdpTo(port, err);
        if (squatted) squatter.close();  // the retry gets a fresh port either way
        if (!ok) ++realRefusals_;
        return ok;
    }

private:
    int failures_;
    int squats_;
    int blockWidth_;
    int tcpFailures_;
    int blockStart_{-1};
    int calls_{0};
    int injected_{0};
    int injectedTcp_{0};
    int realRefusals_{0};
    int realTcpRefusals_{0};
    std::vector<std::uint16_t> tcpAsked_;
};

}  // namespace

// A port-0 caller survives a run of refusals and lands on a working pair.
TEST(DualTransport, Port0BindRetriesPastRefusedUdpPorts) {
    // Eight rather than "a few": every retry below is a DRAW, and the range assertion at
    // the bottom is only as strong as the number of draws it sees — a draw from the wrong
    // range lands in the right one about one time in four, so eight of them shrink a
    // mis-ranged draw slipping through to ~1e-5.
    constexpr int kRefusals = 8;
    FlakyUdpBindTransport server(kRefusals);
    std::string err;

    ASSERT_TRUE(server.bind(0, &err)) << err;
    EXPECT_TRUE(server.isBound());
    EXPECT_GT(server.port(), 0);
    // It retried, and it retried exactly as far as it had to. realRefusals() and
    // realTcpRefusals() are part of the count, not an escape from it (issue #98): the binds
    // after the injected run are real, so on a WinNAT host they can be refused for precisely
    // the reason this arm exists, and going round again is then the behaviour #73 shipped.
    // Adding the OS's refusals to what is owed keeps the assertion EXACT — it is not
    // EXPECT_GE, and it still fails if the code stops retrying (bind() fails above) or
    // spends one attempt more than something refused it.
    const int owed = kRefusals + server.realRefusals() + server.realTcpRefusals() + 1;
    EXPECT_EQ(server.bindAttempts(), owed)
        << server.realRefusals() << " genuine UDP and " << server.realTcpRefusals()
        << " genuine TCP refusal(s)";
    EXPECT_EQ(server.udpBindCalls(), kRefusals + server.realRefusals() + 1);
    // THE MECHANISM (issue #101): the first ask is the OS's pick and every retry is a draw
    // from the dynamic range — never the OS's next port along, which sits inside the block
    // that refused the last one. The bounds are LITERALS, not kDrawLow/kDrawHigh: the range
    // is the IANA dynamic/private range the CHANGELOG promises, and an assertion written
    // against the constants would mutate along with them — measured: a kDrawLow of 1 passed
    // this arm green while it still compared against kDrawLow.
    const auto& asked = server.tcpPortsAsked();
    ASSERT_EQ(asked.size(), static_cast<std::size_t>(owed));
    EXPECT_EQ(asked[0], 0);
    for (std::size_t i = 1; i < asked.size(); ++i) {
        EXPECT_GE(asked[i], 49152) << "retry " << i;
        EXPECT_LE(asked[i], 65535) << "retry " << i;
    }
    server.close();
}

// A drawn port TCP refuses — another socket holds it, or it sits in a TCP-reserved block,
// both measured on windows-latest (issue #101) — costs one attempt and is drawn again. It
// reaches no UDP bind, so it is owed to bindAttempts() and not to udpBindCalls(). Driven
// RED by returning on that refusal instead of continuing: bind() then fails on the first.
TEST(DualTransport, Port0BindDrawsAgainWhenTcpRefusesADrawnPort) {
    constexpr int kUdpRefusals = 1;  // the OS's pick, so a draw is owed
    constexpr int kTcpRefusals = 2;  // two draws TCP refuses before one is accepted
    FlakyUdpBindTransport server(kUdpRefusals, /*squats=*/0, /*blockWidth=*/0, kTcpRefusals);
    std::string err;

    ASSERT_TRUE(server.bind(0, &err)) << err;
    EXPECT_TRUE(server.isBound());
    EXPECT_EQ(server.injectedTcpRefusals(), kTcpRefusals);
    const int owed = kUdpRefusals + kTcpRefusals + server.realRefusals() +
                     server.realTcpRefusals() + 1;
    EXPECT_EQ(server.bindAttempts(), owed);
    EXPECT_EQ(server.udpBindCalls(), kUdpRefusals + server.realRefusals() + 1);
    EXPECT_EQ(server.tcpPortsAsked().size(), static_cast<std::size_t>(owed));
    server.close();
}

// A first-try success must not look like a retry — the negative control for the arm
// above, without which bindAttempts() could be a constant and both would pass.
TEST(DualTransport, Port0BindReportsOneAttemptWhenUdpNeverRefuses) {
    FlakyUdpBindTransport server(0);
    std::string err;
    ASSERT_TRUE(server.bind(0, &err)) << err;
    // Nothing is injected here, so every bind is real and a genuine refusal is the ONLY thing
    // that may legitimately push this past 1 (issue #98) — accounted for, never tolerated. A
    // fix that retried when it must not still reads >1 against both counters at 0 and fails.
    EXPECT_EQ(server.bindAttempts(), server.realRefusals() + server.realTcpRefusals() + 1)
        << server.realRefusals() << " genuine UDP and " << server.realTcpRefusals()
        << " genuine TCP refusal(s)";
    server.close();
}

// THE COUNTER'S OWN CONTROL — and the only arm where the OS does the refusing (issue #98).
//
// The two arms above subtract realRefusals() from what they demand. If that counter could
// never rise, they would be exact by luck rather than by construction and the Windows
// intermittent would still fail them, so something has to prove it can. Here the injector
// passes every call and a real UdpServerTransport occupies the paired port instead; it sets
// no SO_REUSEADDR (issue #83), so the kernel refuses the second bind on Linux, macOS and
// Windows alike. The retry then does on demand what #73 only ever did by lottery.
//
// This is also the first arm in which #73's retry recovers from a refusal that no test wrote:
// every other one asserts against `return false`. realRefusals() is EXPECT_GE rather than
// EXPECT_EQ for the same reason the arms above changed — the retry's own second bind is real
// too, and may itself be refused — but the accounting stays exact against whatever it reads.
TEST(DualTransport, Port0BindRecoversFromAGenuineUdpRefusal) {
    FlakyUdpBindTransport server(/*failures=*/0, /*squats=*/1);
    std::string err;

    ASSERT_TRUE(server.bind(0, &err)) << err;
    EXPECT_TRUE(server.isBound());
    EXPECT_GT(server.port(), 0);
    EXPECT_GE(server.realRefusals(), 1) << "the squatter never refused a real bind — the "
                                           "counter the other two arms rely on cannot rise";
    EXPECT_EQ(server.bindAttempts(), server.realRefusals() + server.realTcpRefusals() + 1);
    EXPECT_EQ(server.udpBindCalls(), server.realRefusals() + 1);
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
    // And the rollback held on the named path too: isBound() is false whenever EITHER half
    // is unbound, so only port() can see a TCP listener stranded on the port it was refused.
    EXPECT_EQ(server.port(), -1);
}

// Exhaustion is bounded, reports the UNDERLYING refusal rather than a summary of it,
// and strands no TCP listener.
TEST(DualTransport, Port0BindGivesUpAfterBudgetAndPreservesTheRealError) {
    FlakyUdpBindTransport server(DualServerTransport::kPort0BindAttempts + 1);  // never succeeds
    std::string err;

    EXPECT_FALSE(server.bind(0, &err));
    EXPECT_EQ(server.bindAttempts(), DualServerTransport::kPort0BindAttempts);
    // A drawn port the OS refused on the TCP side spends an attempt without reaching UDP
    // (issue #101) — accounted for, so this stays exact rather than sliding to EXPECT_LE.
    EXPECT_EQ(server.udpBindCalls(),
              DualServerTransport::kPort0BindAttempts - server.realTcpRefusals());
    // The diagnosable part of the error survives the retry: the text is the last refusal's
    // own, and that is the injected one unless the OS refused the final draw on the TCP side.
    EXPECT_NE(err.find("last error: bind() failed"), std::string::npos) << err;
    if (server.realTcpRefusals() == 0) EXPECT_NE(err.find("10013"), std::string::npos) << err;
    // Rollback held on every attempt, not just the first.
    EXPECT_FALSE(server.isBound());
    EXPECT_EQ(server.port(), -1);
}

// THE #101 ARM: a reserved block wider than the budget cannot exhaust it.
//
// The block starts at the OS's own pick — the port the walk #73 shipped stepped through
// one at a time. Measured on windows-latest that block is 200 ports wide, so 32 sequential
// picks give up inside it, which is exactly what CI reported three times. Against the draw,
// each retry lands in the block with probability width/16384 — one in 32 here — so the
// pair lands outside it within a handful of attempts and never needs the budget. Driven
// RED by the mutation that asks the OS again on every retry (the #73 walk): every ask
// falls inside the block and bind() fails.
TEST(DualTransport, Port0BindSurvivesAReservedBlockWiderThanTheBudget) {
    constexpr int kWidth = 512;  // wider than the 200 measured, and than the budget
    static_assert(kWidth > DualServerTransport::kPort0BindAttempts,
                  "a block the walk could clear proves nothing about the draw");
    FlakyUdpBindTransport server(/*failures=*/0, /*squats=*/0, /*blockWidth=*/kWidth);
    std::string err;

    ASSERT_TRUE(server.bind(0, &err)) << err;
    EXPECT_TRUE(server.isBound());
    EXPECT_FALSE(server.inBlock(server.port())) << "landed on port " << server.port();
    // The OS's pick is the block's first port, so it was refused and at least one draw
    // followed; the accounting is exact against whatever the OS refused underneath.
    EXPECT_GE(server.injectedRefusals(), 1);
    EXPECT_EQ(server.bindAttempts(), server.injectedRefusals() + server.realRefusals() +
                                         server.realTcpRefusals() + 1);
    EXPECT_EQ(server.udpBindCalls(), server.injectedRefusals() + server.realRefusals() + 1);
    server.close();
}
