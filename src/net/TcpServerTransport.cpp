// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — TCP ServerTransport (impl).
//
// Copyright (C) 2025-2026 Terrell Deppe
//

#include "naudio/net/TcpServerTransport.hpp"

#include <utility>

namespace naudio::net {

namespace {

// Builds a ClientAddress from the assigned id and the accepted socket's peer
// string ("ip:port"). The endpoint is informational — ClientAddress identity is
// by id only — so a missing/odd peer string degrades to an id-only address.
ClientAddress addressFor(const std::string& id, const std::string& remote) {
    std::size_t colon = remote.rfind(':');
    if (colon == std::string::npos || colon + 1 >= remote.size()) {
        return ClientAddress(id);
    }
    std::string host = remote.substr(0, colon);
    std::uint16_t port = 0;
    for (char c : remote.substr(colon + 1)) {
        if (c < '0' || c > '9') return ClientAddress(id);
        port = static_cast<std::uint16_t>(port * 10 + (c - '0'));
    }
    return ClientAddress(id, host, port);
}

}  // namespace

bool TcpServerTransport::bind(std::uint16_t port, std::string* err) {
    if (bound_.load()) {
        if (err) *err = "Already bound";
        return false;
    }
    listener_ = Socket::listenTcp(bindHost_, port, /*reuseAddr=*/true, err);
    if (!listener_.valid()) return false;
    bound_.store(true);
    return true;
}

bool TcpServerTransport::isBound() const {
    return bound_.load() && listener_.valid();
}

int TcpServerTransport::port() const {
    return isBound() ? static_cast<int>(listener_.localPort()) : -1;
}

IoStatus TcpServerTransport::acceptClient(int timeoutMs,
                                          std::shared_ptr<ClientConnection>& out,
                                          std::string* err) {
    if (!isBound()) {
        if (err) *err = "Transport not bound";
        return IoStatus::Error;
    }

    Socket accepted;
    IoStatus st = listener_.acceptTcp(timeoutMs, accepted, err);
    if (st != IoStatus::Ok) return st;  // TimedOut (retry) or Error

    std::string clientId = "tcp-" + std::to_string(clientIdCounter_.fetch_add(1));
    ClientAddress address = addressFor(clientId, accepted.remoteAddress());

    auto connection =
        std::make_shared<TcpClientConnection>(std::move(accepted), std::move(address));
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_[clientId] = connection;
    }
    out = connection;
    return IoStatus::Ok;
}

void TcpServerTransport::disconnectClient(
    const std::shared_ptr<ClientConnection>& connection) {
    if (!connection) return;
    const std::string& clientId = connection->clientAddress().id();

    std::shared_ptr<TcpClientConnection> removed;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = connections_.find(clientId);
        if (it != connections_.end()) {
            removed = it->second;
            connections_.erase(it);
        }
    }
    if (removed) removed->close();
}

std::int64_t TcpServerTransport::packetsSent() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    std::int64_t total = 0;
    for (const auto& [id, conn] : connections_) total += conn->packetsSent();
    return total;
}

std::int64_t TcpServerTransport::packetsReceived() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    std::int64_t total = 0;
    for (const auto& [id, conn] : connections_) total += conn->packetsReceived();
    return total;
}

std::int64_t TcpServerTransport::bytesSent() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    std::int64_t total = 0;
    for (const auto& [id, conn] : connections_) total += conn->bytesSent();
    return total;
}

std::int64_t TcpServerTransport::bytesReceived() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    std::int64_t total = 0;
    for (const auto& [id, conn] : connections_) total += conn->bytesReceived();
    return total;
}

int TcpServerTransport::crcErrors() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    int total = 0;
    for (const auto& [id, conn] : connections_) total += conn->crcErrors();
    return total;
}

void TcpServerTransport::close() {
    bound_.store(false);

    std::unordered_map<std::string, std::shared_ptr<TcpClientConnection>> drained;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        drained.swap(connections_);
    }
    for (auto& [id, conn] : drained) conn->close();

    listener_.close();
}

}  // namespace naudio::net
