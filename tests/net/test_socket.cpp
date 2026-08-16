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
// which is the opposite of what this arm used to do. #74 recorded the surviving band as
// D < I < 2D, and that is the constraint for a LARGE, RARE drain: a deadline window can then
// fall entirely between two bursts, the ::send returns zero progress, and sendAll fails on its
// own limb with the budget never consulted — which is exactly how the un-budgeted control used
// to pass. With I << D a window is far likelier to contain several bursts, so the budget is
// the only thing left that can end the call. "Far likelier" is deliberate wording; see the
// coverage table below for the two platforms where it still does not hold.
//
// MEASURED, 4 shipped + 4 mutant runs per configuration on macOS/arm64, mutation = the budget
// check deleted:
//
//   buffers      shipped call ms    mutant caught
//   default      202..303           2 of 4      <- what this arm used to be
//   64 KiB       206..207           4 of 4      <- shipped below
//
// The old configuration was a COIN FLIP, not a detector, and that is the finding this rewrite
// rests on rather than the Windows gate alone: re-running the documented M1 mutation against
// the shipped arm on macOS/arm64 caught it in 1 of 3 runs, failing on `callMs 1631 vs 1600` —
// a 31 ms margin on a tuned wall-clock threshold.
//
// WHERE THE DISCRIMINATION USED TO STOP, AND WHY IT NO LONGER DOES (issue #88 item 6, from
// #74). The arm RUNS everywhere and passes everywhere, and for a while that was all it did —
// passing everywhere is not detecting everywhere. Same M1 mutation, one run per platform on CI,
// back when every assertion here was derived from the clock:
//
//   platform            shipped   mutant           discriminated?
//   macOS/arm64 local   ~300 ms   5088 ms rescue   YES (4 of 4, and 6 of 6 earlier)
//   ubuntu-latest CI    0.25 s    5.06 s rescue    YES
//   macos-latest CI     0.26 s    0.47 s PASSED    NO
//   windows-latest CI   0.25 s    0.25 s PASSED    NO
//
// The mechanism of the two NOs is NOT the wedge — that stages correctly on all four. It is that
// an un-budgeted sendAll TERMINATES ITSELF quickly on those platforms: a deadline window passes
// with no room freed, ::send returns zero progress, and sendAll fails on its own limb after a
// few hundred ms. Both limbs then return false quickly, so NO WALL-CLOCK-DERIVED ASSERTION CAN
// SEPARATE THEM, and no re-tune of the drain could have: the two outcomes genuinely take the
// same time there.
//
// THE FIX WAS TO STOP ASSERTING ON TIME. Socket::lastSendStop() reports WHY the call ended, and
// SendStop::Budget is written at exactly ONE statement in sendAll — the whole-call budget check
// itself. A build with that check removed cannot produce Budget on any runner, whatever the
// scheduler does, so the EXPECT_EQ on the reason near the end of this arm discriminates BY
// CONSTRUCTION rather than by margin. That is what closes the gap the table above records.
//
// WHAT IS PROVEN LOCALLY AND WHAT IS NOT, because the distinction matters here: the single
// assignment site is mechanically checkable (`git grep 'SendStop::Budget' src include` returns
// one line) and the flipped-expectation control reddens this arm on demand, so the assertion is
// live rather than vacuous. What was NOT reproduced locally is the CI failure mode itself —
// widening kDrainIdleMs to 500 ms still ends this call at the budget on macOS/arm64, because
// the first ::send always finds room in a 64 KiB buffer and so the budget check is always
// reached. The zero-progress-from-the-first-send shape needs Winsock with SO_SNDBUF=0. The
// platform-independence claim therefore rests on the single-assignment-site argument, not on a
// local reproduction, and it is stated that way on purpose.
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

    // WINDOWS NEEDS THE SEND BUFFER OFF, NOT SMALL, and this is the only platform-conditional
    // line in the arm — it changes the setup, it does not skip the test. MEASURED on
    // windows-latest across two arrangements: a 64 KiB receive window set on the listener
    // pre-handshake AND on the accepted socket post-accept still absorbed the whole 8 MB
    // payload in 30 ms with the peer reading nothing, with getsockopt reporting 65536
    // throughout. Winsock enables dynamic send buffering by default and auto-tunes SO_SNDBUF,
    // so a nonzero request there is advisory; 0 disables buffering outright, which is the one
    // setting that forces ::send to wait on the peer's window.
#ifdef _WIN32
    const int effSnd = client.setSendBufferSize(0);
#else
    const int effSnd = client.setSendBufferSize(kBufferBytes);
#endif
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
    // effSnd is deliberately NOT asserted positive: 0 is the requested value on Windows. The
    // recv side carries the does-the-setter-do-anything check for both platforms.
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

    std::atomic<int> stopReason{-1};
    std::thread sender([&]() {
        sending.store(true);
        const auto t0 = std::chrono::steady_clock::now();
        sendOk.store(client.sendAll(big.data(), big.size()));
        // Read HERE: lastSendStop is thread-local and answers for the calling thread, so it has
        // to be captured on this thread and before anything else on it can call sendAll.
        stopReason.store(static_cast<int>(Socket::lastSendStop()));
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
    // THE PEER WAS ALIVE. This is a LIVENESS premise and deliberately not a throughput one: a
    // dead peer drains exactly 0, which would make this arm a duplicate of
    // SendTimesOutWhenPeerStopsReading, and that confusion is the only thing this guard exists
    // to prevent.
    //
    // THE BOUND IS 0 AND MUST STAY THERE — do not "sharpen" it back. It was `> 64 * 1024`, and
    // that FALSE-REDDENED macos-latest on 2026-08-12 (run 31574019523, `actual: 65536 vs 65536`)
    // on the same sha that had passed six minutes earlier. Two independent reasons, either one
    // sufficient:
    //   * drainedDuringCall accumulates in kDrainChunk units, so 64 KiB is EXACTLY 2 chunks —
    //     a strict `>` sitting precisely on the lattice the counter lands on, i.e. the single
    //     likeliest value to meet rather than the least.
    //   * the count is scheduler THROUGHPUT — how many kDrainIdleMs cycles the burster wins
    //     inside one call — so any bound above 0 is a bet on a shared runner's scheduling, not
    //     a statement about sendAll.
    // REPRODUCED deterministically rather than inferred: widening kDrainIdleMs to 130 puts only
    // two drains inside the call and reddens the old bound 6 of 6 at exactly 65536, the CI
    // value. At the shipped 40 ms it is 5 chunks, 20 of 20 — so the old bound had no margin
    // below it at all, only above.
    //
    // Nothing is lost. The re-arm claim the old message made belongs to the rescue-path FAIL
    // above (issue #70), which is what actually catches a per-send deadline re-arming; the
    // timing claim belongs to the callMs bounds below.
    ASSERT_GT(drainedDuringCall.load(), 0)
        << "the peer drained NOTHING while the send was running, so this arm measured a DEAD "
           "peer — the same thing SendTimesOutWhenPeerStopsReading already covers, and the "
           "budget was never what ended this call";

    // THE ASSERTION THAT ACTUALLY DETECTS, AND THE ONLY ONE HERE THAT IS NOT WALL-CLOCK-DERIVED
    // (issue #88 item 6, from #74). Everything above and below this line is a premise or a
    // timing bound, and on two CI platforms the timing bounds provably cannot separate a
    // budgeted call from an un-budgeted one: an un-budgeted sendAll TERMINATES ITSELF there in
    // about the same time, measured mutant-vs-shipped at 0.25 s vs 0.25 s on windows-latest and
    // 0.47 s vs 0.26 s on macos-latest. Both limbs return false quickly, so no threshold exists
    // between them and the arm passed while detecting nothing on those runners.
    //
    // Socket::SendStop::Budget is set at exactly one statement in sendAll — the whole-call
    // budget check itself — so a build with that check removed cannot produce it however the
    // scheduler behaves. That makes the POSIX expectation below immune to scheduling, which is
    // the property the clock-based assertions could never have.
    //
    // IT DOES NOT MAKE IT PLATFORM-INDEPENDENT, and an earlier revision of this comment claimed
    // it did. windows-latest DISPROVED that on the first CI run that reached it (2026-08-15, run
    // 31915528485): got SendFailed, want Budget. The claim was wrong about WHICH exit a wedged
    // Winsock socket takes, not about the single assignment site, which still holds.
    //
    // WHY WINDOWS CANNOT REACH THE BUDGET EXIT HERE. sendAll checks the whole-call budget only
    // AFTER a partial write — inside the `n > 0` branch — because a partial write is the event
    // that re-arms a fresh SO_SNDTIMEO and so is the thing #70 exists to bound. The budget value
    // IS sendTimeoutMs(), so the whole-call limit and the per-send limit are THE SAME NUMBER;
    // whichever is consulted first ends the call. On Linux/macOS the kernel send buffer takes
    // data immediately, so partial writes happen early, the budget check is reached, and it
    // fires. On Winsock the arm must disable send buffering outright to wedge at all (see the
    // SO_SNDBUF note at the top), and a blocking ::send then spends the entire deadline window
    // inside ONE call — so the per-send timeout returns first and the budget is never consulted.
    //
    // MEASURED, and every other assertion in this arm passed on that run: sendOk false (the
    // socket really wedged), drainedDuringCall > 0 (the peer was alive and bytes moved), and the
    // call ended in 244 ms against a 200 ms deadline. So ISSUE #70's GUARANTEE HELD ON WINDOWS —
    // the call was bounded by one deadline, not by N — and SendFailed is an ACCURATE report
    // there: a ::send did time out. What Windows loses is the ability to tell a slow-but-live
    // peer from a dead one, because Winsock reports no byte count on a timed-out blocking send.
    // That is a platform limit on the information available, not a defect in sendAll, and it is
    // why na_client/Socket consumers on Windows must not read SendFailed as "the peer is gone".
    //
    // CONSEQUENCE FOR DETECTION, stated plainly rather than left for the next reader to find:
    // THIS ARM DETECTS THE #70 MUTANT ON POSIX ONLY. Deleting the budget check changes nothing
    // observable on Windows, because the branch is unreachable here. The Windows expectation
    // below is still SPECIFIC — SendFailed, not "any reason" — so it keeps catching Ok (the
    // wedge failed to stage) and NotEntered (the socket was already closed), which is the
    // difference between a narrowed assertion and a vacuous one. Do NOT relax it to a range.
#ifdef _WIN32
    EXPECT_EQ(stopReason.load(), static_cast<int>(Socket::SendStop::SendFailed))
        << "on Winsock this call must end on the PER-SEND timeout (SendFailed="
        << static_cast<int>(Socket::SendStop::SendFailed) << "), because send buffering is off "
           "here and one blocking ::send consumes the whole deadline window without ever "
           "reporting the partial write that would reach the budget check. Got "
        << stopReason.load() << ". Ok=" << static_cast<int>(Socket::SendStop::Ok)
        << " would mean the wedge did not stage; NotEntered="
        << static_cast<int>(Socket::SendStop::NotEntered)
        << " would mean the socket was already closed; Budget="
        << static_cast<int>(Socket::SendStop::Budget)
        << " would mean Winsock started reporting partial writes on a timed-out send, which "
           "would be GOOD NEWS — swap this arm to the POSIX expectation if you see it.";
#else
    EXPECT_EQ(stopReason.load(), static_cast<int>(Socket::SendStop::Budget))
        << "sendAll returned false for a reason OTHER than the whole-call budget (got "
        << stopReason.load() << ", want " << static_cast<int>(Socket::SendStop::Budget)
        << " = Budget; SendFailed=" << static_cast<int>(Socket::SendStop::SendFailed)
        << ", NotEntered=" << static_cast<int>(Socket::SendStop::NotEntered)
        << ", Ok=" << static_cast<int>(Socket::SendStop::Ok)
        << "). SendFailed here means the peer's drain never freed room inside one deadline "
           "window, so this call ended on the per-send timeout and issue #70's budget was "
           "never consulted — the wedge staged, but the thing under test did not run.";
#endif

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

// THE PERMIT-CONTROL for issue #85 — the half that a "second bind is refused" test cannot
// cover, because refusing everything passes it just as happily as refusing the right thing.
//
// This replaces a test named ReuseAddrAllowsRebindAfterClose that ASSERTED NOTHING ABOUT THE
// FLAG IT WAS NAMED FOR. It closed a listener that had never accepted a connection, and a bare
// listener leaves no TIME_WAIT behind — so the rebind it checked succeeds with ownsPort=false
// just as readily. Measured on macOS 2026-08-13 before rewriting it, with plain POSIX sockets so
// the answer describes the OS rather than naudio's opinion of it:
//
//     rebind after closing a listener that never accepted   BOUND either way   <- the old test
//     rebind after a connection closed SERVER-FIRST         refused without the flag, BOUND with
//
// The second row is the only state in which the flag does any work, so staging it is what makes
// this a control rather than a formality. It has to be a real connection closed from the SERVER
// end: TIME_WAIT is owned by whichever side sends FIN first, and only the server end holds the
// listen port in it.
//
// Why this matters here and not in #83: that issue could delete the flag outright because UDP
// has no TIME_WAIT to accommodate. TCP does, so a fix for #85 that dropped SO_REUSEADDR on POSIX
// would trade a Windows double-bind for a POSIX server that cannot restart promptly — the exact
// collateral this asserts against (Learning 188). Remove the SO_REUSEADDR line in
// Socket::listenTcp and this test reddens; that mutation was run.
//
// ON WINDOWS this is also the load-bearing half of the fix, for a different reason: the fix sets
// SO_EXCLUSIVEADDRUSE there, and if Winsock refused a rebind over TIME_WAIT under that option the
// cure would be worse than #85. This test is what answers that on the platform, since no POSIX
// box can.
TEST(Socket, APortIsRebindableOnceItsServerIsGoneEvenWithConnectionsInTimeWait) {
    std::string err;
    std::uint16_t port = 0;
    {
        Socket server, client, accepted;
        ASSERT_TRUE(makeTcpPair(server, client, accepted));
        port = server.localPort();
        ASSERT_GT(port, 0);

        // Server closes FIRST, so the accepted 5-tuple — whose local port is `port` — is the
        // side that enters TIME_WAIT. Closing the client first would park TIME_WAIT on the
        // client's ephemeral port instead and stage nothing.
        accepted.close();
        client.close();
        server.close();
    }

    Socket restarted = Socket::listenTcp("", port, /*ownsPort=*/true, &err);
    EXPECT_TRUE(restarted.valid())
        << "port " << port << " could not be reclaimed by a restarting server while its own "
        << "previous connections were in TIME_WAIT: " << err
        << " — a supervised restart is broken (issue #85's permit-control)";
}

// The refuse-control's Socket-level twin. TcpTransport.ASecondServerCannotBindAPortAlreadyBeing-
// Served is the one that maps to the na_server_start symptom; this one pins the primitive
// underneath it, so a regression is attributed to the socket layer rather than to the transport.
//
// PLATFORM ASYMMETRY, STATED RATHER THAN LEFT TO BE DISCOVERED (Learning 63): against the pre-fix
// code this is RED on Windows and GREEN on POSIX, because POSIX never permitted the double bind
// in the first place — SO_REUSEADDR there is only the TIME_WAIT accommodation above. A green
// macOS or Linux run is therefore NOT evidence that the Windows fix is still in place; only the
// windows-latest job carries that.
TEST(Socket, ASecondListenerCannotTakeAPortAlreadyBeingListenedOn) {
    std::string err;
    Socket first = Socket::listenTcp("", 0, /*ownsPort=*/true, &err);
    ASSERT_TRUE(first.valid()) << err;
    const std::uint16_t served = first.localPort();
    ASSERT_GT(served, 0);

    std::string secondErr;
    Socket second = Socket::listenTcp("", served, /*ownsPort=*/true, &secondErr);
    EXPECT_FALSE(second.valid())
        << "a second listener bound port " << served << " while the first was still listening on "
        << "it — na_server_start would report success for a port it does not have (issue #85)";
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

// --- receive-drop counter (issue #29) -------------------------------------------------------
//
// The three arms below are deliberately split by platform rather than skipped on the ones
// without the mechanism: "-1 everywhere it is unavailable" IS the contract, and an arm that
// skips there would leave the half that macOS and Windows actually ship untested.

TEST(Socket, ReceiveDropsIsUnmeasuredBeforeTheCounterIsEnabled) {
    std::string err;
    Socket rx = Socket::bindUdp("", 0, true, &err);
    ASSERT_TRUE(rx.valid()) << err;
    // -1, not 0, on EVERY platform — including the ones that could measure it but were not
    // asked to. A 0 here would say "nothing was dropped", which is the one thing an
    // unmeasured loss counter must never say.
    EXPECT_EQ(rx.receiveDrops(), -1);
}

TEST(Socket, EnablingTheDropCounterMatchesWhatThePlatformCanActuallyDo) {
    std::string err;
    Socket rx = Socket::bindUdp("", 0, true, &err);
    ASSERT_TRUE(rx.valid()) << err;
    const bool enabled = rx.enableReceiveDropCounter();
#if defined(__linux__)
    // Pinned rather than inferred: if this ever starts returning false on Linux the counter
    // degrades to a permanent -1, which reads exactly like a platform without the mechanism.
    EXPECT_TRUE(enabled);
    EXPECT_EQ(rx.receiveDrops(), 0);  // measured now, and nothing has been dropped yet
#else
    EXPECT_FALSE(enabled);
    EXPECT_EQ(rx.receiveDrops(), -1);  // and it stays unmeasured, not zero
#endif
}

TEST(Socket, TheDropCounterSurvivesBeingMovedIntoItsOwner) {
    // UdpClientConnection takes its socket BY VALUE and moves it, so a counter enabled before
    // that move has to travel with the descriptor or the connection reports -1 forever.
    std::string err;
    Socket rx = Socket::bindUdp("", 0, true, &err);
    ASSERT_TRUE(rx.valid()) << err;
    const bool enabled = rx.enableReceiveDropCounter();
    Socket moved = std::move(rx);
    ASSERT_TRUE(moved.valid());
#if defined(__linux__)
    ASSERT_TRUE(enabled);
    EXPECT_EQ(moved.receiveDrops(), 0);  // carried over as MEASURED, not reset to unmeasured
#else
    ASSERT_FALSE(enabled);
    EXPECT_EQ(moved.receiveDrops(), -1);
#endif
    EXPECT_EQ(rx.receiveDrops(), -1);  // and the moved-from socket measures nothing
}

#if defined(__linux__)
// The real thing: overflow a small receive buffer and read the kernel's own discard count back.
//
// Bounded against a quantity the socket layer never computes — datagrams the sender put on the
// wire minus datagrams the kernel actually queued — rather than against `> 0`.
TEST(Socket, ReceiveDropsReportsTheKernelsOwnDiscardCount) {
    std::string err;
    Socket rx = Socket::bindUdp("", 0, true, &err);
    ASSERT_TRUE(rx.valid()) << err;
    rx.setRecvBufferSize(4096);  // small enough that a flood cannot fit
    ASSERT_TRUE(rx.enableReceiveDropCounter());
    const std::uint16_t rxPort = rx.localPort();
    ASSERT_GT(rxPort, 0);
    rx.setRecvTimeout(50);

    Socket tx = Socket::bindUdp("", 0, true, &err);
    ASSERT_TRUE(tx.valid()) << err;

    const std::vector<std::uint8_t> payload(1024, 0xAB);
    int sent = 0;
    for (int i = 0; i < 20000; ++i) {
        if (tx.sendTo(payload.data(), payload.size(), "127.0.0.1", rxPort)) ++sent;
    }
    ASSERT_GT(sent, 0);

    std::vector<std::uint8_t> buf(2048);
    int queued = 0;
    while (rx.recvFrom(buf.data(), buf.size()).status == IoStatus::Ok) ++queued;
    ASSERT_GT(sent, queued) << "the flood fit in the receive buffer; nothing was dropped";

    // THE COUNT IS STAMPED AT ENQUEUE. Everything drained above was queued before the buffer
    // filled, so it carries a zero stamp; the reading only rides in on a datagram queued AFTER
    // the discard. Sending into the now-drained buffer is what makes the loss observable, and
    // it is why a permanently stalled consumer never learns of its own drops.
    EXPECT_EQ(rx.receiveDrops(), 0) << "drops became visible without a post-discard arrival";

    // THE TAIL SENDS MUST ALL FIT, and that is a real constraint rather than tidiness. A tail
    // datagram that is itself dropped is dropped AFTER the last packet the kernel could stamp,
    // so its discard is unobservable and the identity below goes off by exactly that many.
    // Measured while writing this arm: five 1 KiB tails into the 4 KiB buffer above lost one,
    // and the counter read 19996 where the true total was 19997. Three 64-byte tails fit with
    // room to spare. The residual is inherent — the last drop before a stream goes quiet is
    // never reported — and is documented on Socket::receiveDrops rather than asserted here.
    const std::vector<std::uint8_t> tail(64, 0xEE);
    const int kTailSends = 3;
    for (int i = 0; i < kTailSends; ++i) {
        ASSERT_TRUE(tx.sendTo(tail.data(), tail.size(), "127.0.0.1", rxPort));
    }
    int tailRead = 0;
    while (rx.recvFrom(buf.data(), buf.size()).status == IoStatus::Ok) {
        ++queued;
        ++tailRead;
    }
    ASSERT_EQ(tailRead, kTailSends) << "a tail datagram was dropped; the identity below cannot hold";

    // Two-directional, against quantities the socket layer never computes — the sender's own
    // count and the drain's — never `> 0` alone.
    EXPECT_EQ(rx.receiveDrops(), static_cast<std::int64_t>(sent + kTailSends - queued));
    EXPECT_GT(rx.receiveDrops(), 0);
}

// The negative control for the arm above: the SAME setup with a buffer big enough to hold
// everything must report exactly 0, not "some small number". Without this, an implementation
// that reported garbage on every datagram would pass the overflow arm.
TEST(Socket, ReceiveDropsStaysZeroWhenTheBufferKeepsUp) {
    std::string err;
    Socket rx = Socket::bindUdp("", 0, true, &err);
    ASSERT_TRUE(rx.valid()) << err;
    rx.setRecvBufferAtLeast(1 << 20);
    ASSERT_TRUE(rx.enableReceiveDropCounter());
    const std::uint16_t rxPort = rx.localPort();
    rx.setRecvTimeout(50);

    Socket tx = Socket::bindUdp("", 0, true, &err);
    ASSERT_TRUE(tx.valid()) << err;

    const std::vector<std::uint8_t> payload(64, 0xCD);
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(tx.sendTo(payload.data(), payload.size(), "127.0.0.1", rxPort));
    }
    std::vector<std::uint8_t> buf(256);
    int got = 0;
    while (rx.recvFrom(buf.data(), buf.size()).status == IoStatus::Ok) ++got;
    EXPECT_EQ(got, 8);
    EXPECT_EQ(rx.receiveDrops(), 0);
}
#endif  // __linux__

TEST(Socket, MoveLeavesSourceInvalid) {
    std::string err;
    Socket a = Socket::listenTcp("", 0, true, &err);
    ASSERT_TRUE(a.valid());
    Socket b = std::move(a);
    EXPECT_TRUE(b.valid());
    EXPECT_FALSE(a.valid());
}

// C4: close() may land while another thread is in recv() reading the same socket
// handle. With the handle atomic, the concurrent close()-store and recv()-load are
// data-race-free and the reader returns cleanly instead of crashing.
//
// RENAMED IN #90 FOR WHAT IT ACTUALLY DEMONSTRATES. It was called
// `CloseDuringRecvIsRaceFree`, and that name was the strongest evidence in this file that the
// descriptor boundary had been established — which is exactly why nobody re-checked it while
// TSan reported the same close()/recv() pair 17 times. What this arm proves is that the reader
// comes back and the process survives; it says nothing about whether the DESCRIPTOR was still
// ours when ::recv ran. That claim is the next arm's, and it needed a code change to become true.
TEST(Socket, CloseDuringRecvUnblocksTheReaderWithoutCrashing) {
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

// Issue #90: close() must not free the descriptor while a syscall is still running on it.
//
// THE ONE ARM THAT ASSERTS THE DEFECT ITSELF, and it exists because the instrument that found
// #90 cannot be run here: MEASURED 2026-08-13, Apple clang 21's TSan reports NOTHING for a
// close()/recv() fd race even in a 20-line program, while the same program with a plain data
// race added reports and exits 134. So macOS is structurally unable to verify the fix, and an
// arm that keys on TSan would silently test nothing on half the CI matrix.
//
// This keys on the invariant directly instead, with no sanitizer involved. `inRecv` is true for
// exactly as long as the reader is inside ::recv. If close() returns while it is still true, the
// descriptor was handed back to the OS with a live syscall on it — free for the next socket() in
// any thread to be assigned the same number. That is the whole defect, and it is observable
// without an instrument.
//
// Issue #90: close() waits for an in-flight syscall to leave before it frees the descriptor, and
// that wait must stay SHORT — otherwise the race is traded for a teardown stall.
//
// WHAT THIS ARM CAN AND CANNOT PROVE, stated plainly because the obvious reading is too generous.
// It does NOT prove the ordering (that ::close runs after the syscall returns): MEASURED on macOS
// 2026-08-13, a bare ::close ends a parked recvfrom / select / recv in ~204 ms each, so on this
// platform the syscall is out of the kernel either way and no assertion through this API can tell
// the two orderings apart. The first version of this arm was written that way, passed against
// deliberately unfixed code, and was thrown out — SESSION_RUNNER Learning #12's vacuous test.
// The ORDERING is proved by the ubuntu TSan job with tests/tsan-suppressions.txt' race:closeNative
// entry deleted; 17 arms there redden if this regresses, and no local run can substitute.
//
// What it DOES prove, on every platform, is the promptness half — and that half is falsifiable
// here. ::shutdown does not wake a select on a listener (measured: parked the full 5000 ms), so
// with the poll-slice check removed from acceptTcp the drain sits out this accept's whole 3000 ms
// deadline before ::close runs. DRIVEN RED THAT WAY BEFORE BEING TRUSTED: close() took 3006 ms
// against the 500 ms bound below. On Linux the same mutation is worse still, because close() does
// not wake a blocked select there at all.
TEST(Socket, CloseIsPromptWhileAnAcceptIsParkedOnTheSocket) {
    std::string err;
    Socket listener = Socket::listenTcp("", 0, true, &err);
    ASSERT_TRUE(listener.valid()) << err;

    const std::uint64_t forcedBefore = Socket::drainTimeouts();

    // Long enough that the accept's own deadline cannot be what ends the wait, and comfortably
    // under kDrainTimeoutMs (5000) so a COMPLETED drain rather than an expired one is what does.
    constexpr int kAcceptTimeoutMs = 3000;
    constexpr int kPromptBoundMs = 500;

    std::atomic<bool> enteredAccept{false};
    std::atomic<bool> acceptReturned{false};
    std::thread acceptor([&]() {
        Socket out;
        std::string aerr;
        enteredAccept.store(true);
        listener.acceptTcp(kAcceptTimeoutMs, out, &aerr);
        acceptReturned.store(true);
    });

    while (!enteredAccept.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // settle inside the syscall
    ASSERT_FALSE(acceptReturned.load()) << "accept left early — the arm would prove nothing";

    const auto t0 = std::chrono::steady_clock::now();
    listener.close();
    const auto closeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();

    EXPECT_LT(closeMs, kPromptBoundMs)
        << "close() sat on the parked accept for " << closeMs << " ms — the drain is waiting out "
        << "the caller's whole poll instead of the slice";

    // The negative control for the bound above, and the reason it is not just a stopwatch. A fast
    // close() would ALSO be produced by the drain being absent altogether; what separates "waited
    // and the syscall left" from "never waited" is that the first bumps no counter and the second
    // could not bump one either — so the counter is checked for the REMAINING failure: a drain
    // that ran, expired, and freed the descriptor the old racy way regardless.
    EXPECT_EQ(Socket::drainTimeouts(), forcedBefore)
        << "close() hit its drain deadline instead of waiting the syscall out";

    acceptor.join();
    EXPECT_TRUE(acceptReturned.load());
    EXPECT_FALSE(listener.valid());
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
