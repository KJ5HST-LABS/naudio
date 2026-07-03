// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — TCP ServerTransport.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "naudio/net/Socket.hpp"
#include "naudio/net/TcpClientConnection.hpp"
#include "naudio/net/Transport.hpp"

namespace naudio::net {

// TCP implementation of ServerTransport: one socket per
// accepted client, a blocking accept loop with a timeout, and a registry keyed by
// client id for disconnect/stats.
//
// Bind address is configurable — the ctor's bindHost defaults to "" = wildcard
// "0.0.0.0" (LAN-reachable); pass "127.0.0.1" for loopback-only. It is a
// constructor knob with the wildcard default.
class TcpServerTransport : public ServerTransport {
public:
    explicit TcpServerTransport(std::string bindHost = "")
        : bindHost_(std::move(bindHost)) {}

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
    std::string bindHost_;
    Socket listener_;
    std::atomic<bool> bound_{false};
    std::atomic<int> clientIdCounter_{1};  // counter from 1

    mutable std::mutex connectionsMutex_;
    std::unordered_map<std::string, std::shared_ptr<TcpClientConnection>> connections_;
};

}  // namespace naudio::net
