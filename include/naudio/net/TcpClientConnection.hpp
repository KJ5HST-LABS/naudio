// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — TCP ClientConnection (delegates to handler).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "naudio/net/AudioProtocolHandler.hpp"
#include "naudio/net/ClientAddress.hpp"
#include "naudio/net/Socket.hpp"
#include "naudio/net/Transport.hpp"

namespace naudio::net {

// TCP implementation of ClientConnection. All protocol
// concerns — CRC, heartbeat timing, stream resync — live in the owned
// AudioProtocolHandler; this is pure delegation plus a closed flag. The handler
// (and thus the socket) is owned by value, so the connection is non-movable and
// is always held via shared_ptr.
class TcpClientConnection : public ClientConnection {
public:
    TcpClientConnection(Socket socket, ClientAddress address)
        : protocol_(std::move(socket)), address_(std::move(address)) {}

    const ClientAddress& clientAddress() const override { return address_; }

    bool sendControl(const ControlMessage& message) override {
        return protocol_.sendControl(message);
    }
    bool sendRxAudio(const std::uint8_t* data, std::size_t offset,
                     std::size_t length) override {
        return protocol_.sendRxAudio(data, offset, length);
    }
    bool sendTxAudio(const std::uint8_t* data, std::size_t length) override {
        return protocol_.sendTxAudio(data, length);
    }
    bool sendHeartbeat() override { return protocol_.sendHeartbeat(); }
    bool sendPacket(const AudioPacket& packet) override {
        return protocol_.sendPacket(packet);
    }

    ReceiveResult receivePacket(int timeoutMs) override {
        return protocol_.receivePacket(timeoutMs);
    }

    bool shouldSendHeartbeat() override { return protocol_.shouldSendHeartbeat(); }
    bool isConnectionTimedOut() const override { return protocol_.isConnectionTimedOut(); }
    std::int64_t timeSinceLastReceive() const override {
        return protocol_.timeSinceLastReceive();
    }

    std::int64_t packetsSent() const override { return protocol_.packetsSent(); }
    std::int64_t packetsReceived() const override { return protocol_.packetsReceived(); }
    std::int64_t bytesSent() const override { return protocol_.bytesSent(); }
    std::int64_t bytesReceived() const override { return protocol_.bytesReceived(); }
    int crcErrors() const override { return protocol_.crcErrors(); }

    std::string remoteAddress() const override { return protocol_.remoteAddress(); }
    bool isClosed() const override { return closed_.load() || protocol_.isClosed(); }

    void close() override {
        if (closed_.exchange(true)) return;
        protocol_.close();
    }

private:
    AudioProtocolHandler protocol_;
    ClientAddress address_;
    std::atomic<bool> closed_{false};
};

}  // namespace naudio::net
