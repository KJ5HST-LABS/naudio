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
        // Symmetric with the server's shared socket: a maximum-payload audio packet must fit,
        // or a legal packet fails to send on macOS (see Socket::setSendBufferAtLeast). The
        // receive side matters most here — this socket takes the server's RX fan-out.
        socket.setSendBufferAtLeast(static_cast<int>(UdpClientConnection::MAX_DATAGRAM_SIZE) * 8);
        socket.setRecvBufferAtLeast(static_cast<int>(UdpClientConnection::MAX_DATAGRAM_SIZE) * 8);
        // Ask the kernel to count what it discards on this socket (issue #29). Deliberately
        // AFTER the receive buffer is raised, so the counter measures the buffer the client
        // actually runs with rather than the default it was born with. The return value is not
        // checked because false is the expected answer everywhere but Linux, and the counter
        // reports -1 there — an unavailable mechanism is not a connect failure. Enabled here,
        // before the socket is moved into the connection below, which is safe because the
        // counter travels with the descriptor (Socket::adoptDropCounter).
        socket.enableReceiveDropCounter();

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
