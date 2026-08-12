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
//
// listenerRecvBuf, when nonzero, shrinks the LISTENER's receive buffer before the peer
// connects, so the accepted socket inherits a bounded receive window. That ordering is the
// whole point of the parameter and cannot be replaced by setting the accepted socket
// afterwards: a TCP receive window is negotiated during the handshake, and MEASURED on
// windows-latest a post-connect SO_RCVBUF is reported back by getsockopt while the connection
// goes on absorbing 8 MB in 39 ms (issue #74). Only SendAllStopsAtItsBudgetWhenThePeerOnly-
// Trickles passes it; every other caller keeps the kernel's defaults.
bool makeTcpPair(Socket& server, Socket& client, Socket& accepted, int listenerRecvBuf = 0) {
    std::string err;
    server = Socket::listenTcp("", 0, true, &err);
    if (!server.valid()) return false;
    if (listenerRecvBuf > 0) server.setRecvBufferSize(listenerRecvBuf);
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
// WHAT PROVES THE FAULT FIRED, as opposed to the setup merely running. A dead peer also
// makes this call return false at about one deadline, so the elapsed time alone cannot tell
// "the budget stopped a call that was making progress" from "nothing ever moved" — the arm
// would pass while testing the arm above (L136's shape). The discriminator is that the peer
// really was draining WHILE the send ran: the sender is blocked in ::send, so any room the
// peer frees is room the kernel immediately fills from our buffer, and a nonzero drain
// during the call is therefore forward progress that was interrupted.
//
// THE WEDGE IS BUILT BY SHRINKING THE SOCKET BUFFERS, NOT BY FILLING THEM (issue #74). The
// arm used to push 64 KB chunks until sendAll returned false, which staged the premise using
// THE VERY PRIMITIVE UNDER TEST — so on Winsock the budget terminated its own setup: MEASURED
// on windows-latest (run 31448166393) the fill stopped early with the buffer not full and the
// timed 8 MB send then SUCCEEDED in 545 ms with the peer draining 0 bytes. Re-tuning the fill
// cannot fix that shape, only move it. Shrinking SO_SNDBUF/SO_RCVBUF instead makes the
// capacity small and KNOWN, so a payload many times larger than it arrives already wedged
// after the first instant absorption, and there is no setup for the budget to terminate.
//
// THE RECEIVE WINDOW IS BOUNDED BEFORE THE HANDSHAKE, AND THAT IS NOT INTERCHANGEABLE WITH
// AFTER. This arm shrinks the same buffer twice, on purpose, because the two platforms need
// opposite things and each ignores the other's:
//
//   * on the LISTENER, before the client connects (inherited by the accepted socket).
//     MEASURED on windows-latest: without this, a post-connect SO_RCVBUF is reported back by
//     getsockopt as 65536 while the connection absorbs the whole 8 MB payload in 39 ms with
//     the peer reading nothing — a TCP receive window is negotiated during the handshake, so
//     setting it afterwards is simply too late there.
//   * on the endpoints, after connect. MEASURED on macOS/arm64: a post-connect request is
//     honoured exactly (65536 -> 65536) while a pre-connect one is clamped up to a floor
//     (8192 -> 65328 / 326640) because auto-sizing has not been pinned yet, so the listener
//     set alone leaves the receiver at 326640 and the arm runs 2654..3312 ms.
//
// THE CLIENT SENDS AND THE ACCEPTED SOCKET RECEIVES, which is the reverse of the other arms
// here and is forced by the above: the receiver has to be the socket that can inherit a
// pre-handshake window, and only the accepted socket can.
//
// THE PEER DRAINS OFTEN AND IN SMALL PIECES — kDrainIdleMs is far SHORTER than the deadline,
// which is the opposite of what this arm used to do and is the change that made it work
// everywhere. #74 recorded the surviving band as D < I < 2D, and that is the constraint for a
// LARGE, RARE drain: a deadline window can then fall entirely between two bursts, the ::send
// returns zero progress, and sendAll fails on its own limb with the budget never consulted —
// which is exactly how the un-budgeted control used to pass. With I << D every window contains
// several bursts, so a zero-progress window is impossible by construction and only the budget
// can end the call.
//
// MEASURED, 4 shipped + 4 mutant runs per configuration, mutation = the budget check deleted:
//
//   buffers      shipped call ms    mutant caught
//   default      202..303           2 of 4      <- what this arm used to be
//   64 KiB       206..207           4 of 4      <- shipped below
//
// The old configuration was a COIN FLIP, not a detector, and that is the finding this rewrite
// rests on rather than the Windows gate alone: re-running the documented M1 mutation against
// the shipped arm on macOS/arm64 caught it in 1 of 3 runs, failing on `callMs 1631 vs 1600` —
// a 31 ms margin on a tuned wall-clock threshold. The configuration below fails the mutant
// through the RESCUE PATH at the 5 s ceiling instead, a ~20x margin that needs no threshold.
//
// A NEGATIVE CONTROL, so the drain rate is not read as arbitrary: doubling it to 64 KB every
// 40 ms takes BOTH the shipped arm and the mutant green — the peer then consumes the payload
// inside the ceiling and there is nothing left to interrupt. The payload must outrun the
// drain over the whole ceiling (8 MiB vs ~800 KB/s x 5 s), and that relation, not the literal
// 8 MiB, is what the arm depends on.
TEST(Socket, SendAllStopsAtItsBudgetWhenThePeerOnlyTrickles) {
    constexpr int kDeadlineMs = 200;
    constexpr int kBufferBytes = 64 * 1024;
    constexpr std::size_t kDrainChunk = 32 * 1024;
    constexpr int kDrainIdleMs = 40;  // << kDeadlineMs — see the note above
    constexpr std::size_t kPayloadBytes = 8 * 1024 * 1024;

    Socket server, client, accepted;
    ASSERT_TRUE(makeTcpPair(server, client, accepted, kBufferBytes));

    const int effSnd = client.setSendBufferSize(kBufferBytes);
    const int effRcv = accepted.setRecvBufferSize(kBufferBytes);
    ASSERT_TRUE(client.setSendTimeout(kDeadlineMs));
    accepted.setRecvTimeout(50);

    // THE KERNEL ACCEPTED THE REQUEST — which is a weaker statement than it looks, and is
    // deliberately not called the premise. MEASURED on windows-latest: getsockopt reported
    // 65536 back on a connection that then absorbed 8 MB in 39 ms, so A READBACK IS NOT
    // EVIDENCE THE WINDOW SHRANK ON THE WIRE. What this pair of assertions actually catches is
    // a setter that does nothing at all (proved: a resizeSocketBuffer stubbed to skip its
    // setsockopt reddens here 2 of 2 in 0 ms). The BEHAVIOURAL premise — that the socket really
    // wedged — is carried by EXPECT_FALSE(sendOk) and the drain check below, and their failure
    // messages say so. 4x leaves room for the rounding conventions (Linux commonly reports back
    // double) without leaving room for a platform that ignored the call.
    ASSERT_GT(effSnd, 0) << "SO_SNDBUF could not be read back";
    ASSERT_GT(effRcv, 0) << "SO_RCVBUF could not be read back";
    ASSERT_LE(effSnd, 4 * kBufferBytes)
        << "this platform declined the send-buffer shrink (asked " << kBufferBytes << ", got "
        << effSnd << ")";
    ASSERT_LE(effRcv, 4 * kBufferBytes)
        << "this platform declined the receive-buffer shrink (asked " << kBufferBytes << ", got "
        << effRcv << ")";

    std::atomic<bool> stop{false};
    std::atomic<bool> sending{false};
    std::atomic<long> drainedDuringCall{0};
    std::thread burster([&]() {
        std::vector<std::uint8_t> sink(kDrainChunk);
        // Hold the first drain until the timed call is in flight, so the evidence the
        // precondition below reads lands INSIDE the interval being measured.
        while (!sending.load() && !stop.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        while (!stop.load()) {
            RecvResult r = accepted.recv(sink.data(), sink.size());
            if (r.status == IoStatus::Ok && sending.load())
                drainedDuringCall.fetch_add(static_cast<long>(r.bytes));
            std::this_thread::sleep_for(std::chrono::milliseconds(kDrainIdleMs));
        }
    });

    // Larger than the peer can take inside the ceiling, so without the budget this call
    // reddens through the rescue path rather than through a tight wall-clock threshold.
    const std::vector<std::uint8_t> big(kPayloadBytes, 0xCD);
    std::atomic<bool> finished{false};
    std::atomic<bool> sendOk{true};
    std::atomic<long> callMs{-1};

    std::thread sender([&]() {
        sending.store(true);
        const auto t0 = std::chrono::steady_clock::now();
        sendOk.store(client.sendAll(big.data(), big.size()));
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
        while (!finished.load()) accepted.recv(sink.data(), sink.size());
        burster.join();
        sender.join();
        FAIL() << "sendAll ran past " << kDeadlineMs * 25 << " ms against a peer draining "
               << kDrainChunk << " bytes every " << kDrainIdleMs
               << " ms — the whole-call budget is not in effect, so each partial write is "
                  "re-arming the per-send deadline (issue #70)";
    }
    stop.store(true);
    burster.join();
    sender.join();

    // THIS IS THE BEHAVIOURAL PREMISE, not just an outcome: a call that succeeded never
    // wedged, and the readback assertions above cannot tell you that (windows-latest reported
    // 65536 while absorbing 8 MB in 39 ms). So the message names the likely cause rather than
    // only the symptom — the next reader's first question is "did the shrink reach the wire".
    EXPECT_FALSE(sendOk.load())
        << "the peer took the whole " << kPayloadBytes << " byte payload at " << kDrainChunk
        << " bytes every " << kDrainIdleMs << " ms (SO_SNDBUF " << effSnd << ", SO_RCVBUF "
        << effRcv
        << " as reported by getsockopt) — the socket never wedged, so either this platform's "
           "receive window is not bounded by the listener's SO_RCVBUF or the drain outran the "
           "payload; this call did not end at the budget";
    ASSERT_GT(drainedDuringCall.load(), 64 * 1024)
        << "the peer drained nothing while the send was running, so this arm measured a "
           "DEAD peer — the same thing SendTimesOutWhenPeerStopsReading already covers, and "
           "the re-arm was never exercised";

    // It waited (so the budget is doing the work, not an instant refusal) and it stopped
    // well inside the drip-feed's own timescale. Both bounds derive from kDeadlineMs.
    // MEASURED at 206..207 ms shipped, so the upper bound carries ~7x headroom for a loaded
    // runner while still sitting 3x below the ceiling the mutant runs to.
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
