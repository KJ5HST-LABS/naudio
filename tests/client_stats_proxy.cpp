// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — a lossy in-process UDP relay for the C stats test.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Test-only fixture for c_client_stats.c. Loopback is loss-free, so a client with FEC on and a
// client with FEC off deliver byte-identical output and packets_recovered_by_fec can only ever
// read 0 — a counter that has never been seen to increment is an unverified claim, not a feature.
// This sits between the client and the server and DROPS ONE AUDIO PACKET PER FEC BLOCK, which is
// exactly what single-parity XOR FEC is specified to repair.
//
// Wrapped behind a tiny extern "C" surface so the PURE-C test never names a C++ type, the same
// shape net_smoke_server.cpp uses. NOT part of the public ABI.
//
// Drop selection is BY AUDIO-PACKET ORDINAL, never `sequence % N`. A FEC block is N audio packets
// in send order, but the sender's sequence counter is shared with control and heartbeat packets and
// the parity itself consumes one, so a modulo of the sequence number does not align with the
// encoder's blocks and would drop two packets from some blocks (unrecoverable) and none from
// others. Only AUDIO_RX is ever dropped: dropping the parity would remove the repair rather than
// the damage, and dropping control or heartbeat traffic would break the handshake or time the
// connection out.
//
// It ALSO corrupts on demand, which is a different fault from dropping: a dropped datagram never
// reaches the client, while a corrupted one arrives and fails its CRC. Only the second moves
// crc_errors, so the counter cannot be provoked by the drop path at any rate. Corruption flips one
// PAYLOAD byte and leaves the header intact, so the datagram still passes the client's
// expected-sender and truncation gates (UdpClientConnection.cpp:257, which return early WITHOUT
// counting) and lands on the CRC check that does count. Corruption is applied by the same
// per-block ordinal selection as dropping, and to AUDIO_RX only, for the same reasons.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "naudio/AudioPacket.hpp"
#include "naudio/net/Socket.hpp"

namespace {

struct Proxy {
    naudio::net::Socket sock;
    std::uint16_t serverPort = 0;
    int blockSize = 5;
    int dropOrdinal = 2;     // which audio packet within each block to drop (0-based)
    int corruptOrdinal = -1;  // which to corrupt instead; < 0 corrupts nothing

    std::thread worker;
    std::atomic<bool> stop{false};

    // Counters (single writer: the relay thread).
    std::atomic<long long> audioSeen{0};
    std::atomic<long long> audioDropped{0};
    std::atomic<long long> audioCorrupted{0};
    std::atomic<long long> parityForwarded{0};
    std::atomic<long long> clientToServer{0};
};

// One relay hop. The server is the only peer on `serverPort`, so the sender's port tells the two
// directions apart without any handshake parsing.
void relayLoop(Proxy* p) {
    std::vector<std::uint8_t> buf(naudio::AudioPacket::HEADER_SIZE +
                                 naudio::AudioPacket::MAX_PAYLOAD +
                                 naudio::AudioPacket::CRC_SIZE + 64);
    std::string clientHost;
    std::uint16_t clientPort = 0;
    long long audioOrdinal = 0;

    p->sock.setRecvTimeout(50);  // so stop is observed promptly at teardown
    while (!p->stop.load()) {
        const naudio::net::RecvFromResult rr = p->sock.recvFrom(buf.data(), buf.size());
        if (rr.status == naudio::net::IoStatus::TimedOut) continue;
        if (rr.status != naudio::net::IoStatus::Ok) break;  // Closed / Error — teardown

        if (rr.senderPort == p->serverPort) {
            // server -> client. Inspect the frame; drop one audio packet per block.
            if (clientPort == 0) continue;  // nothing to forward to yet
            const std::optional<naudio::AudioPacket> pkt =
                naudio::AudioPacket::deserialize(buf.data(), rr.bytes);
            if (pkt && pkt->packetType() == naudio::PacketType::AudioRx) {
                const long long ordinalInBlock = audioOrdinal % p->blockSize;
                ++audioOrdinal;
                p->audioSeen.fetch_add(1);
                if (ordinalInBlock == p->dropOrdinal) {
                    p->audioDropped.fetch_add(1);
                    continue;  // the induced loss
                }
                if (ordinalInBlock == p->corruptOrdinal &&
                    rr.bytes > naudio::AudioPacket::HEADER_SIZE) {
                    // Flip the first payload byte. CRC32 is computed over header + payload, so a
                    // single-byte change always fails it — the packet arrives and is rejected.
                    buf[naudio::AudioPacket::HEADER_SIZE] ^= 0xFF;
                    p->audioCorrupted.fetch_add(1);
                }
            } else if (pkt && pkt->packetType() == naudio::PacketType::FecParity) {
                p->parityForwarded.fetch_add(1);
            }
            p->sock.sendTo(buf.data(), rr.bytes, clientHost, clientPort);
        } else {
            // client -> server. Latch the client endpoint and forward verbatim.
            clientHost = rr.senderHost;
            clientPort = rr.senderPort;
            p->clientToServer.fetch_add(1);
            p->sock.sendTo(buf.data(), rr.bytes, "127.0.0.1", p->serverPort);
        }
    }
}

}  // namespace

extern "C" {

// Start a relay to 127.0.0.1:server_port on an ephemeral loopback port. Of every `block_size` audio
// packets it DROPS ordinal `drop_ordinal` and CORRUPTS ordinal `corrupt_ordinal`; pass either < 0 to
// disable that fault (both < 0 is the lossless, uncorrupted control arm — same extra hop, nothing
// touched). Writes the bound port to *out_port. Returns an opaque handle, or nullptr on failure.
void* naproxy_start(int server_port, int block_size, int drop_ordinal, int corrupt_ordinal,
                    int* out_port) {
    if (server_port <= 0 || server_port > 65535 || block_size <= 0) return nullptr;
    auto* p = new Proxy();
    p->sock = naudio::net::Socket::bindUdp("127.0.0.1", 0, false, nullptr);
    if (!p->sock.valid()) {
        delete p;
        return nullptr;
    }
    // A maximum-payload 0xAF01 audio packet exceeds macOS's 9216-byte default UDP buffer, and the
    // relay both receives and re-sends one (see Socket::setSendBufferAtLeast).
    const int kDatagramRoom = static_cast<int>(naudio::AudioPacket::HEADER_SIZE +
                                               naudio::AudioPacket::MAX_PAYLOAD +
                                               naudio::AudioPacket::CRC_SIZE + 64);
    p->sock.setRecvBufferAtLeast(kDatagramRoom);
    p->sock.setSendBufferAtLeast(kDatagramRoom);

    p->serverPort = static_cast<std::uint16_t>(server_port);
    p->blockSize = block_size;
    p->dropOrdinal = drop_ordinal;        // < 0 never matches an ordinal -> lossless
    p->corruptOrdinal = corrupt_ordinal;  // < 0 never matches an ordinal -> uncorrupted
    if (out_port != nullptr) *out_port = p->sock.localPort();
    p->worker = std::thread(relayLoop, p);
    return p;
}

// Audio packets the relay saw travelling server -> client.
long long naproxy_audio_seen(void* handle) {
    if (handle == nullptr) return -1;
    return static_cast<Proxy*>(handle)->audioSeen.load();
}

// Audio packets the relay deliberately discarded.
long long naproxy_audio_dropped(void* handle) {
    if (handle == nullptr) return -1;
    return static_cast<Proxy*>(handle)->audioDropped.load();
}

// Audio packets the relay deliberately corrupted and then forwarded. This is the independently-known
// quantity that bounds the client's crc_errors from ABOVE — the relay is the only source of
// undeserializable datagrams on this path, so the client can never legitimately count more.
long long naproxy_audio_corrupted(void* handle) {
    if (handle == nullptr) return -1;
    return static_cast<Proxy*>(handle)->audioCorrupted.load();
}

// FEC parity packets the relay forwarded (never dropped). Zero means the server was not sending
// parity at all, which distinguishes "FEC could not repair the loss" from "there was no FEC".
long long naproxy_parity_forwarded(void* handle) {
    if (handle == nullptr) return -1;
    return static_cast<Proxy*>(handle)->parityForwarded.load();
}

void naproxy_stop(void* handle) {
    if (handle == nullptr) return;
    auto* p = static_cast<Proxy*>(handle);
    p->stop.store(true);
    if (p->worker.joinable()) p->worker.join();
    p->sock.close();
    delete p;
}

}  // extern "C"
