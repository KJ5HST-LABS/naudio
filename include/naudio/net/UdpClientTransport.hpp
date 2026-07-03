// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — UDP ClientTransport.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "naudio/net/ClientAddress.hpp"
#include "naudio/net/Socket.hpp"
#include "naudio/net/Transport.hpp"
#include "naudio/net/UdpClientConnection.hpp"

namespace naudio::net {

// UDP implementation of ClientTransport. Binds an
// ephemeral local UDP socket and wraps it in a client-owned UdpClientConnection
// targeting the server. UDP has no transport-level handshake — the "connection"
// is just a socket plus the server's address; the CONNECT_REQUEST/ACCEPT exchange
// happens at the application layer above this. The connection OWNS the socket
// (closing the connection closes the socket).
//
// timeoutMs is accepted for interface symmetry but has no transport-level effect:
// the per-receive deadline lives in UdpClientConnection::receivePacket (any
// connect-time socket timeout is immediately overridden there).
class UdpClientTransport : public ClientTransport {
public:
    explicit UdpClientTransport(UdpReliabilityConfig cfg = {}) : cfg_(std::move(cfg)) {}

    std::shared_ptr<ClientConnection> connect(const std::string& host, std::uint16_t port,
                                              int timeoutMs, std::string* err) override {
        (void)timeoutMs;
        if (connection_ && !connection_->isClosed()) {
            if (err) *err = "Already connected";
            return nullptr;
        }
        Socket socket = Socket::bindUdp("", 0, /*reuseAddr=*/false, err);  // ephemeral local
        if (!socket.valid()) return nullptr;

        connection_ = std::make_shared<UdpClientConnection>(
            std::move(socket), host, port, ClientAddress("client", host, port), cfg_);
        return connection_;
    }

    void close() override {
        if (connection_) {
            connection_->close();
            connection_.reset();
        }
    }

private:
    UdpReliabilityConfig cfg_;
    std::shared_ptr<UdpClientConnection> connection_;
};

}  // namespace naudio::net
