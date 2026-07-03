// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — transport abstractions (TCP/UDP-agnostic).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "naudio/AudioPacket.hpp"
#include "naudio/ControlMessage.hpp"
#include "naudio/net/ClientAddress.hpp"
#include "naudio/net/Socket.hpp"  // IoStatus (reused for acceptClient)

namespace naudio::net {

// The result of receivePacket — a no-throw value (instead of returning a packet,
// returning none, or throwing):
//   packet has value          -> a frame was decoded
//   empty && !closed          -> no data this call (timeout / single transient
//                                skip); the caller retries
//   empty && closed           -> the connection is dead (EOF / I/O error / too
//                                many consecutive errors); the caller tears down
struct ReceiveResult {
    std::optional<AudioPacket> packet;
    bool closed = false;

    bool hasPacket() const { return packet.has_value(); }

    static ReceiveResult of(AudioPacket p) { return {std::move(p), false}; }
    static ReceiveResult noData() { return {std::nullopt, false}; }
    static ReceiveResult dead() { return {std::nullopt, true}; }
};

// Per-client connection handle for bidirectional audio streaming. Both
// server-side (one per accepted client) and client-side (the connection to the
// server) implement it.
//
// Threading: a UDP demux thread feeds the connection (enqueue) while the
// application thread calls receivePacket on the SAME connection. So these are
// non-const methods called concurrently through a shared_ptr — each
// implementation is responsible for its own synchronization (mutex / atomics).
// `const` is reserved for the
// genuinely read-only getters.
class ClientConnection {
public:
    virtual ~ClientConnection() = default;

    // The identity of this connection's remote peer.
    virtual const ClientAddress& clientAddress() const = 0;

    // --- Send (return false on I/O failure) ---
    virtual bool sendControl(const ControlMessage& message) = 0;
    virtual bool sendRxAudio(const std::uint8_t* data, std::size_t offset,
                             std::size_t length) = 0;
    virtual bool sendTxAudio(const std::uint8_t* data, std::size_t length) = 0;
    virtual bool sendHeartbeat() = 0;
    virtual bool sendPacket(const AudioPacket& packet) = 0;

    // --- Receive ---
    virtual ReceiveResult receivePacket(int timeoutMs) = 0;

    // --- Heartbeat / timeout ---
    // Non-const because the UDP implementation piggybacks a control-retransmit
    // pass (which sends) on this poll; the TCP implementation is a pure check.
    virtual bool shouldSendHeartbeat() = 0;
    virtual bool isConnectionTimedOut() const = 0;
    virtual std::int64_t timeSinceLastReceive() const = 0;

    // --- Statistics ---
    virtual std::int64_t packetsSent() const = 0;
    virtual std::int64_t packetsReceived() const = 0;
    virtual std::int64_t bytesSent() const = 0;
    virtual std::int64_t bytesReceived() const = 0;
    virtual int crcErrors() const = 0;

    // UDP-only reliability stats; the TCP defaults are 0 / -1.
    virtual std::int64_t packetsLost() const { return 0; }
    virtual std::int64_t packetsOutOfOrder() const { return 0; }
    virtual double packetLossRate() const { return 0.0; }
    virtual std::int64_t packetsReordered() const { return 0; }
    virtual std::int64_t packetsRecoveredByFec() const { return 0; }
    virtual double jitterMs() const { return 0.0; }
    virtual int adaptiveBufferTargetMs() const { return -1; }
    virtual std::int64_t controlRetransmits() const { return 0; }

    // The remote address as a string ("ip:port"), or "" if unavailable.
    virtual std::string remoteAddress() const = 0;

    virtual bool isClosed() const = 0;
    virtual void close() = 0;
};

// Client-side transport — connects to a server and yields a ClientConnection.
class ClientTransport {
public:
    virtual ~ClientTransport() = default;

    // Connects to host:port (timeoutMs == 0 blocks). Returns null and fills err
    // on failure.
    virtual std::shared_ptr<ClientConnection> connect(const std::string& host,
                                                      std::uint16_t port,
                                                      int timeoutMs,
                                                      std::string* err) = 0;
    virtual void close() = 0;
};

// Server-side transport — binds a port and yields one ClientConnection per
// client. TCP uses one socket per client with an accept loop; UDP uses a single
// socket with address-based demultiplexing.
class ServerTransport {
public:
    virtual ~ServerTransport() = default;

    // Binds to a port (0 for ephemeral). Returns false and fills err on failure.
    virtual bool bind(std::uint16_t port, std::string* err) = 0;
    virtual bool isBound() const = 0;
    // The bound local port, or -1 if not bound.
    virtual int port() const = 0;

    // Waits for a new client. timeoutMs == 0 blocks. On Ok, `out` receives the
    // connection; TimedOut means none arrived (retry); Error is fatal.
    virtual IoStatus acceptClient(int timeoutMs,
                                  std::shared_ptr<ClientConnection>& out,
                                  std::string* err) = 0;

    // Disconnects a specific client and releases its resources.
    virtual void disconnectClient(const std::shared_ptr<ClientConnection>& connection) = 0;

    // Aggregate statistics across all clients.
    virtual std::int64_t packetsSent() const = 0;
    virtual std::int64_t packetsReceived() const = 0;
    virtual std::int64_t bytesSent() const = 0;
    virtual std::int64_t bytesReceived() const = 0;
    virtual int crcErrors() const = 0;

    virtual void close() = 0;
};

}  // namespace naudio::net
