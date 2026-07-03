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
