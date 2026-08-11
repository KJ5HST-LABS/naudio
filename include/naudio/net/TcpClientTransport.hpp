// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — TCP ClientTransport.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "naudio/net/ClientAddress.hpp"
#include "naudio/net/Socket.hpp"
#include "naudio/net/TcpClientConnection.hpp"
#include "naudio/net/Transport.hpp"

namespace naudio::net {

// TCP implementation of ClientTransport. Opens one TCP
// socket to the server and wraps it in a TcpClientConnection.
class TcpClientTransport : public ClientTransport {
public:
    std::shared_ptr<ClientConnection> connect(const std::string& host,
                                              std::uint16_t port, int timeoutMs,
                                              std::string* err) override {
        if (connection_ && !connection_->isClosed()) {
            if (err) *err = "Already connected";
            return nullptr;
        }
        Socket socket = Socket::connectTcp(host, port, timeoutMs, err);
        if (!socket.valid()) return nullptr;

        // The send deadline is armed by AudioProtocolHandler's constructor, which this
        // socket reaches through TcpClientConnection below. #69 armed it here instead,
        // explicitly deferring the server's half to #56 on the grounds that the handler
        // covers both; #56's half is now done, so the handler is the one owner and this
        // site would be a second copy to keep in step.
        connection_ = std::make_shared<TcpClientConnection>(
            std::move(socket), ClientAddress("client", host, port));
        return connection_;
    }

    void close() override {
        if (connection_) {
            connection_->close();
            connection_.reset();
        }
    }

private:
    std::shared_ptr<TcpClientConnection> connection_;
};

}  // namespace naudio::net
