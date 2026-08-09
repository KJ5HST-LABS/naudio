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

#include "naudio/net/AudioProtocolHandler.hpp"
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

        // Give the send path a deadline; nothing else bounds it (issue #69). Half the
        // no-RX death window, so a frame that cannot be pushed in this long is given up on
        // no later than the point at which the heartbeat watchdog would already have
        // declared the peer dead — derived from that constant rather than hand-picked. A
        // failed send tears the connection down, which is the right outcome: a peer that
        // has not made room for one audio frame in five seconds is not coming back.
        //
        // Armed HERE, on the client's own socket, and deliberately not in
        // AudioProtocolHandler's constructor: TcpServerTransport wraps the same
        // TcpClientConnection, so arming it there would put every server session under the
        // same deadline. That is worth doing and is issue #56's remaining half, not this
        // one's.
        socket.setSendTimeout(AudioProtocolHandler::CONNECTION_TIMEOUT_MS / 2);

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
