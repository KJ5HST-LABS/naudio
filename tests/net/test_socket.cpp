// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — cross-platform Socket primitives.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Direct tests of the cross-platform Socket primitives. These prove
// the socket layer (TCP accept/connect/stream I/O + UDP datagram I/O) in
// isolation, beneath the transport classes that build on it. Hardware-free:
// everything runs over OS loopback (127.0.0.1).

#include "naudio/net/Socket.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace naudio::net;

namespace {

// Establishes a connected TCP triple over loopback: a listening server, the
// client end, and the server-accepted end. Single-threaded — on loopback the
// connect completes into the listen backlog, then accept() dequeues it.
bool makeTcpPair(Socket& server, Socket& client, Socket& accepted) {
    std::string err;
    server = Socket::listenTcp("", 0, true, &err);
    if (!server.valid()) return false;
    std::uint16_t port = server.localPort();
    if (port == 0) return false;
    client = Socket::connectTcp("127.0.0.1", port, 2000, &err);
    if (!client.valid()) return false;
    return server.acceptTcp(2000, accepted, &err) == IoStatus::Ok && accepted.valid();
}

}  // namespace

TEST(Socket, ListenAssignsEphemeralPort) {
    std::string err;
    Socket server = Socket::listenTcp("", 0, true, &err);
    ASSERT_TRUE(server.valid()) << err;
    EXPECT_GT(server.localPort(), 0);
}

TEST(Socket, TcpLoopbackRoundTripsBytesBothWays) {
    Socket server, client, accepted;
    ASSERT_TRUE(makeTcpPair(server, client, accepted));

    const std::vector<std::uint8_t> toServer = {1, 2, 3, 4, 5};
    ASSERT_TRUE(client.sendAll(toServer.data(), toServer.size()));

    std::vector<std::uint8_t> buf(toServer.size());
    RecvResult r = accepted.recv(buf.data(), buf.size());
    ASSERT_EQ(r.status, IoStatus::Ok);
    ASSERT_EQ(r.bytes, toServer.size());
    EXPECT_EQ(buf, toServer);

    // And back the other direction.
    const std::vector<std::uint8_t> toClient = {9, 8, 7};
    ASSERT_TRUE(accepted.sendAll(toClient.data(), toClient.size()));
    std::vector<std::uint8_t> buf2(toClient.size());
    RecvResult r2 = client.recv(buf2.data(), buf2.size());
    ASSERT_EQ(r2.status, IoStatus::Ok);
    EXPECT_EQ(buf2, toClient);
}

TEST(Socket, AcceptTimesOutWithNoClient) {
    std::string err;
    Socket server = Socket::listenTcp("", 0, true, &err);
    ASSERT_TRUE(server.valid()) << err;
    Socket out;
    EXPECT_EQ(server.acceptTcp(100, out, &err), IoStatus::TimedOut);
    EXPECT_FALSE(out.valid());
}

TEST(Socket, RecvTimesOutWhenNoData) {
    Socket server, client, accepted;
    ASSERT_TRUE(makeTcpPair(server, client, accepted));
    ASSERT_TRUE(accepted.setRecvTimeout(100));
    std::uint8_t b = 0;
    RecvResult r = accepted.recv(&b, 1);
    EXPECT_EQ(r.status, IoStatus::TimedOut);
}

// Issue #69. Without SO_SNDTIMEO, sendAll has no deadline at all: a peer that stops
// reading closes our window, the send buffer fills, and ::send parks in the kernel with
// nothing to wake it. The consequence is not a slow send — AudioProtocolHandler holds one
// mutex across sendAll, so a parked writer wedges every other sender on the connection,
// and AudioStreamClient::disconnect() then blocks on the very socket whose close is the
// only thing that would release it.
//
// THE WEDGE IS ASSERTED, NOT ASSUMED. A send that failed immediately for some unrelated
// reason would satisfy the deadline assertion while proving nothing, so the arm requires
// that a substantial volume was accepted first — that is what proves the fault fired, as
// distinct from the setup having run.
//
// THE SEND RUNS ON ITS OWN THREAD WITH A RESCUE. With the deadline removed the call never
// returns, and asserting inline would hang ctest rather than reddening: the rescue drains
// the peer so the parked send completes, joins, and FAILs. A mutation of this arm must be
// a red, not a timeout.
TEST(Socket, SendTimesOutWhenPeerStopsReading) {
    Socket server, client, accepted;
    ASSERT_TRUE(makeTcpPair(server, client, accepted));

    constexpr int kDeadlineMs = 200;
    ASSERT_TRUE(accepted.setSendTimeout(kDeadlineMs));
    // Only so the rescue below cannot itself block; the send path is unaffected.
    client.setRecvTimeout(50);

    // `client` never reads. Push until the kernel refuses. The ceiling exists so a
    // platform with an enormous auto-tuned buffer reports a precondition failure rather
    // than running away.
    const std::vector<std::uint8_t> chunk(64 * 1024, 0xAB);
    constexpr long kFillCeilingBytes = 64L * 1024 * 1024;

    std::atomic<bool> finished{false};
    std::atomic<bool> everFailed{false};
    std::atomic<long> accepted_bytes{0};
    std::atomic<long> failedCallMs{-1};

    std::thread filler([&]() {
        while (accepted_bytes.load() < kFillCeilingBytes) {
            const auto t0 = std::chrono::steady_clock::now();
            if (!accepted.sendAll(chunk.data(), chunk.size())) {
                failedCallMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - t0)
                                       .count());
                everFailed.store(true);
                break;
            }
            accepted_bytes.fetch_add(static_cast<long>(chunk.size()));
        }
        finished.store(true);
    });

    // 25x the deadline — ample margin for a loaded CI runner, and still far short of any
    // ctest timeout.
    const auto ceiling =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kDeadlineMs * 25);
    while (!finished.load() && std::chrono::steady_clock::now() < ceiling)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (!finished.load()) {
        std::vector<std::uint8_t> sink(1 << 20);
        while (!finished.load()) client.recv(sink.data(), sink.size());
        filler.join();
        FAIL() << "sendAll never returned against a peer that stopped reading — the send "
                  "deadline is not in effect (accepted "
               << accepted_bytes.load() << " bytes first)";
    }
    filler.join();

    ASSERT_TRUE(everFailed.load())
        << "the fill reached its ceiling without ever blocking, so no wedge existed and "
           "this arm proves nothing";
    ASSERT_GT(accepted_bytes.load(), 64 * 1024)
        << "sendAll failed before the peer's window filled — the failure is not the wedge";

    // It waited (so the deadline is doing the work, not an instant refusal) and it stopped
    // waiting (so the deadline is bounded). Both bounds are derived from kDeadlineMs
    // rather than hand-picked.
    EXPECT_GE(failedCallMs.load(), kDeadlineMs / 2);
    EXPECT_LT(failedCallMs.load(), kDeadlineMs * 25);
}

// Issue #70. SO_SNDTIMEO is a deadline per ::send, so the arm above only proves the case
// where NOTHING moves. A peer that opens a small window on a timer makes every send return
// a partial count, and sendAll's success limb then arms a FRESH deadline — so the call
// lasts as long as the peer cares to drip-feed, with the deadline setting the granularity
// rather than the bound. MEASURED before this arm existed: 9 partial writes and 3263 ms
// against a 200 ms deadline (16.3x) for a peer draining 32 KB every 40 ms.
//
// THE PEER BURSTS AND THEN GOES IDLE FOR LONGER THAN THE DEADLINE, deliberately. In that
// same measurement the partial writes arose from scheduling JITTER — the drainer's sleep
// occasionally overrunning the deadline — which is not something to build an assertion on.
// Here the idle window (kBurstIdleMs) is longer than the deadline by construction, so every
// ::send is guaranteed to hand back a partial count and the re-arm is not left to chance.
//
// WHAT PROVES THE FAULT FIRED, as opposed to the setup merely running. A dead peer also
// makes this call return false at about one deadline, so the elapsed time alone cannot tell
// "the budget stopped a call that was making progress" from "nothing ever moved" — the arm
// would pass while testing the arm above (L136's shape). The discriminator is that the peer
// really was draining WHILE the send ran: the sender is blocked in ::send, so any room the
// peer frees is room the kernel immediately fills from our buffer, and a nonzero drain
// during the call is therefore forward progress that was interrupted.
TEST(Socket, SendAllStopsAtItsBudgetWhenThePeerOnlyTrickles) {
    Socket server, client, accepted;
    ASSERT_TRUE(makeTcpPair(server, client, accepted));

    constexpr int kDeadlineMs = 200;
    constexpr std::size_t kBurstBytes = 256 * 1024;
    constexpr int kBurstIdleMs = 300;  // > kDeadlineMs, so the next send always re-arms
    ASSERT_TRUE(accepted.setSendTimeout(kDeadlineMs));
    client.setRecvTimeout(50);

    // Fill to real backpressure first, so the timed call below starts wedged rather than
    // streaming into an empty buffer.
    const std::vector<std::uint8_t> chunk(64 * 1024, 0xAB);
    constexpr long kFillCeilingBytes = 64L * 1024 * 1024;
    long filled = 0;
    while (filled < kFillCeilingBytes && accepted.sendAll(chunk.data(), chunk.size()))
        filled += static_cast<long>(chunk.size());
    ASSERT_LT(filled, kFillCeilingBytes)
        << "the fill reached its ceiling without ever blocking, so no wedge existed and this "
           "arm proves nothing";

    std::atomic<bool> stop{false};
    std::atomic<bool> sending{false};
    std::atomic<long> drainedDuringCall{0};
    std::thread burster([&]() {
        std::vector<std::uint8_t> sink(kBurstBytes);
        // Hold the first burst until the timed call is in flight. MEASURED without this:
        // the burst that opens the window lands BEFORE the send starts, the budget then
        // ends the call at ~200 ms, and the next burst is not due until 300 ms — so the
        // call is bracketed by two bursts and drainedDuringCall reads 0, tripping the
        // precondition below. The window still opens for the send either way; what this
        // fixes is that the evidence lands inside the interval being measured.
        while (!sending.load() && !stop.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        while (!stop.load()) {
            RecvResult r = client.recv(sink.data(), sink.size());
            if (r.status == IoStatus::Ok && sending.load())
                drainedDuringCall.fetch_add(static_cast<long>(r.bytes));
            std::this_thread::sleep_for(std::chrono::milliseconds(kBurstIdleMs));
        }
    });

    // 8 MB at one burst per kBurstIdleMs is ~9.6 s of drip-feeding, so without the budget
    // this call cannot finish inside the ceiling below — it reddens through the rescue path
    // rather than through a tight wall-clock threshold.
    const std::vector<std::uint8_t> big(8 * 1024 * 1024, 0xCD);
    std::atomic<bool> finished{false};
    std::atomic<bool> sendOk{true};
    std::atomic<long> callMs{-1};

    std::thread sender([&]() {
        sending.store(true);
        const auto t0 = std::chrono::steady_clock::now();
        sendOk.store(accepted.sendAll(big.data(), big.size()));
        callMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count());
        sending.store(false);
        finished.store(true);
    });

    const auto ceiling =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kDeadlineMs * 25);
    while (!finished.load() && std::chrono::steady_clock::now() < ceiling)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (!finished.load()) {
        stop.store(true);
        std::vector<std::uint8_t> sink(1 << 20);  // rescue: drain hard so the send can finish
        while (!finished.load()) client.recv(sink.data(), sink.size());
        burster.join();
        sender.join();
        FAIL() << "sendAll ran past " << kDeadlineMs * 25
               << " ms against a peer draining " << kBurstBytes << " bytes every "
               << kBurstIdleMs
               << " ms — the whole-call budget is not in effect, so each partial write is "
                  "re-arming the per-send deadline (issue #70)";
    }
    stop.store(true);
    burster.join();
    sender.join();

    EXPECT_FALSE(sendOk.load())
        << "the peer never took 8 MB at one burst per " << kBurstIdleMs
        << " ms, so this call did not end at the budget";
    ASSERT_GT(drainedDuringCall.load(), 64 * 1024)
        << "the peer drained nothing while the send was running, so this arm measured a "
           "DEAD peer — the same thing SendTimesOutWhenPeerStopsReading already covers, and "
           "the re-arm was never exercised";

    // It waited (so the budget is doing the work, not an instant refusal) and it stopped
    // well inside the drip-feed's own timescale. Both bounds derive from kDeadlineMs.
    EXPECT_GE(callMs.load(), kDeadlineMs / 2);
    EXPECT_LT(callMs.load(), kDeadlineMs * 8);
}

// The control for the arms above: with the same deadline armed, a peer that DRAINS must
// still take everything. Without it, "sendAll returned false" could not distinguish a
// working deadline from a deadline that simply breaks sends.
TEST(Socket, SendTimeoutDoesNotBreakADrainingPeer) {
    Socket server, client, accepted;
    ASSERT_TRUE(makeTcpPair(server, client, accepted));
    ASSERT_TRUE(accepted.setSendTimeout(200));
    client.setRecvTimeout(1000);

    std::atomic<bool> stop{false};
    std::thread drain([&]() {
        std::vector<std::uint8_t> sink(1 << 16);
        while (!stop.load()) client.recv(sink.data(), sink.size());
    });

    const std::vector<std::uint8_t> chunk(64 * 1024, 0xCD);
    bool allOk = true;
    for (int i = 0; i < 64 && allOk; i++)  // 4 MB, several times any send buffer
        allOk = accepted.sendAll(chunk.data(), chunk.size());

    stop.store(true);
    accepted.close();
    drain.join();
    EXPECT_TRUE(allOk) << "the send deadline broke a healthy send to a draining peer";
}

TEST(Socket, RecvReportsClosedOnPeerHangup) {
    Socket server, client, accepted;
    ASSERT_TRUE(makeTcpPair(server, client, accepted));
    client.close();  // peer closes
    std::uint8_t b = 0;
    RecvResult r = accepted.recv(&b, 1);
    EXPECT_EQ(r.status, IoStatus::Closed);
}

TEST(Socket, ConnectRefusedReturnsInvalid) {
    // Reserve an ephemeral port, then close it so nothing listens there.
    std::string err;
    Socket tmp = Socket::listenTcp("", 0, true, &err);
    ASSERT_TRUE(tmp.valid());
    std::uint16_t deadPort = tmp.localPort();
    tmp.close();

    Socket c = Socket::connectTcp("127.0.0.1", deadPort, 1000, &err);
    EXPECT_FALSE(c.valid());
}

TEST(Socket, ReuseAddrAllowsRebindAfterClose) {
    std::string err;
    Socket first = Socket::listenTcp("", 0, true, &err);
    ASSERT_TRUE(first.valid());
    std::uint16_t port = first.localPort();
    first.close();

    Socket second = Socket::listenTcp("", port, true, &err);
    EXPECT_TRUE(second.valid()) << err;
}

TEST(Socket, RemoteAddressReportsPeer) {
    Socket server, client, accepted;
    ASSERT_TRUE(makeTcpPair(server, client, accepted));
    // The server-accepted end's peer is the client; both sit on loopback.
    EXPECT_NE(accepted.remoteAddress().rfind("127.0.0.1:", 0), std::string::npos);
}

TEST(Socket, UdpDatagramRoundTripCarriesSenderEndpoint) {
    std::string err;
    Socket receiver = Socket::bindUdp("", 0, true, &err);
    ASSERT_TRUE(receiver.valid()) << err;
    std::uint16_t rxPort = receiver.localPort();
    ASSERT_GT(rxPort, 0);

    Socket sender = Socket::bindUdp("", 0, true, &err);
    ASSERT_TRUE(sender.valid()) << err;
    std::uint16_t senderPort = sender.localPort();

    const std::vector<std::uint8_t> payload = {0xAF, 0x01, 0x42};
    ASSERT_TRUE(sender.sendTo(payload.data(), payload.size(), "127.0.0.1", rxPort));

    std::vector<std::uint8_t> buf(64);
    RecvFromResult r = receiver.recvFrom(buf.data(), buf.size());
    ASSERT_EQ(r.status, IoStatus::Ok);
    ASSERT_EQ(r.bytes, payload.size());
    buf.resize(r.bytes);
    EXPECT_EQ(buf, payload);
    EXPECT_EQ(r.senderHost, "127.0.0.1");
    EXPECT_EQ(r.senderPort, senderPort);
}

TEST(Socket, MoveLeavesSourceInvalid) {
    std::string err;
    Socket a = Socket::listenTcp("", 0, true, &err);
    ASSERT_TRUE(a.valid());
    Socket b = std::move(a);
    EXPECT_TRUE(b.valid());
    EXPECT_FALSE(a.valid());
}

// C4: close() may land while another thread is in recv() reading the same socket
// handle (the receive-worker-vs-close race S630's TSan flagged). With the handle
// now atomic, the concurrent close()-store and recv()-load are data-race-free and
// the reader returns cleanly instead of crashing. (The teardown value is proven
// under TSan; this also guards against a hang/crash on every platform.)
TEST(Socket, CloseDuringRecvIsRaceFree) {
    Socket server, client, accepted;
    ASSERT_TRUE(makeTcpPair(server, client, accepted));
    accepted.setRecvTimeout(100);  // poll-recv, like the real receive worker (SO_RCVTIMEO)

    std::atomic<bool> done{false};
    std::thread reader([&]() {
        std::uint8_t b = 0;
        for (int i = 0; i < 50 && !done.load(); i++) {
            RecvResult r = accepted.recv(&b, 1);  // each call atomically loads the handle
            if (r.status == IoStatus::Closed || r.status == IoStatus::Error) break;
        }
        done.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    accepted.close();  // atomic exchange — races the reader's handle loads with no UB
    reader.join();
    EXPECT_TRUE(done.load());
    EXPECT_FALSE(accepted.valid());
}

// C8: recvFrom must never overflow the supplied buffer on an oversized datagram,
// and reports the discard distinctly. The byte count is always clamped to the
// buffer; Linux MSG_TRUNC and Winsock WSAEMSGSIZE additionally set truncated.
TEST(Socket, RecvFromClampsAndFlagsOversizedDatagram) {
    std::string err;
    Socket receiver = Socket::bindUdp("", 0, true, &err);
    ASSERT_TRUE(receiver.valid()) << err;
    std::uint16_t rxPort = receiver.localPort();
    Socket sender = Socket::bindUdp("", 0, true, &err);
    ASSERT_TRUE(sender.valid()) << err;

    std::vector<std::uint8_t> big(2000, 0xAB);
    ASSERT_TRUE(sender.sendTo(big.data(), big.size(), "127.0.0.1", rxPort));

    std::vector<std::uint8_t> small(500);
    RecvFromResult r = receiver.recvFrom(small.data(), small.size());
    ASSERT_EQ(r.status, IoStatus::Ok);
    EXPECT_LE(r.bytes, small.size());  // never reports more than the buffer holds (no overflow)
    EXPECT_EQ(r.senderHost, "127.0.0.1");
#if defined(__linux__) || defined(_WIN32)
    // Linux (MSG_TRUNC) and Winsock (WSAEMSGSIZE) both make the discard detectable.
    EXPECT_TRUE(r.truncated);
    EXPECT_EQ(r.bytes, small.size());
#endif
}
