// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — UDP ServerTransport (demux + anti-spoof).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "naudio/AudioPacket.hpp"
#include "naudio/net/Socket.hpp"
#include "naudio/net/Transport.hpp"
#include "naudio/net/UdpClientConnection.hpp"

namespace naudio::net {

// UDP implementation of ServerTransport. A single bound
// UDP socket serves all clients; one demux thread reads datagrams and routes them
// by sender address to the owning UdpClientConnection. New senders are accepted
// only when they present a valid CONNECT_REQUEST (anti-spoof) and the
// bounded pending registry has room.
//
// Bind address is configurable like TcpServerTransport: bindHost defaults to "" =
// wildcard "0.0.0.0" (LAN-reachable); pass "127.0.0.1" for loopback-only. It is a
// ctor knob with the wildcard default.
//
// Threading: a single stateMutex_ guards both routing maps AND the pending deque
// (one lock avoids any by_address/by_id lock-ordering hazard — all critical
// sections are brief and hold no socket I/O). The
// demux thread copies the target shared_ptr out under the lock and calls
// enqueueReceived OUTSIDE it; acceptClient blocks on pendingCv_.
class UdpServerTransport : public ServerTransport {
public:
    // Spoofed datagrams must not exhaust session slots — cap unaccepted
    // connections and expire stale ones.
    static constexpr std::size_t MAX_PENDING_CONNECTIONS = 8;
    static constexpr std::int64_t PENDING_TTL_MS = 10'000;

    explicit UdpServerTransport(std::string bindHost = "")
        : bindHost_(std::move(bindHost)) {}
    ~UdpServerTransport() override;

    UdpServerTransport(const UdpServerTransport&) = delete;
    UdpServerTransport& operator=(const UdpServerTransport&) = delete;

    // Sets the per-connection reliability configuration applied to every new
    // client. Call before bind(); ignored for connections already created.
    void setReliabilityConfig(const UdpReliabilityConfig& cfg) { cfg_ = cfg; }

    bool bind(std::uint16_t port, std::string* err) override;
    bool isBound() const override;
    int port() const override;

    IoStatus acceptClient(int timeoutMs, std::shared_ptr<ClientConnection>& out,
                          std::string* err) override;
    void disconnectClient(const std::shared_ptr<ClientConnection>& connection) override;

    std::int64_t packetsSent() const override;
    std::int64_t packetsReceived() const override;
    std::int64_t bytesSent() const override;
    std::int64_t bytesReceived() const override;
    int crcErrors() const override;

    void close() override;

private:
    struct PendingEntry {
        std::shared_ptr<UdpClientConnection> conn;
        std::int64_t since;  // registration time (ms) for TTL expiry
    };

    void demuxLoop();
    // Creates + registers a connection if the pending registry has room (after a
    // stale sweep). Returns null when still full. Caller holds stateMutex_.
    std::shared_ptr<UdpClientConnection> tryCreateConnectionLocked(
        const std::string& host, std::uint16_t port, const std::string& addrKey);
    // Drops pending entries past TTL or already closed. Caller holds stateMutex_.
    void expireStaleLocked();

    static std::string addressKey(const std::string& host, std::uint16_t port) {
        return host + ":" + std::to_string(port);
    }
    // True if the datagram is a deserializable CONTROL / CONNECT_REQUEST.
    static bool isConnectRequest(const std::optional<AudioPacket>& packet);
    static std::int64_t nowMs();

    std::string bindHost_;
    UdpReliabilityConfig cfg_;
    Socket socket_;
    std::atomic<bool> bound_{false};
    std::atomic<bool> running_{false};
    std::atomic<int> clientIdCounter_{1};  // counter from 1

    mutable std::mutex stateMutex_;
    std::condition_variable pendingCv_;
    std::unordered_map<std::string, std::shared_ptr<UdpClientConnection>> byAddress_;  // "host:port"
    std::unordered_map<std::string, std::shared_ptr<UdpClientConnection>> byId_;
    std::deque<PendingEntry> pending_;

    std::thread demuxThread_;
};

}  // namespace naudio::net
