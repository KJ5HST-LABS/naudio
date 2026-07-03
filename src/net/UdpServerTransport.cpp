// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — UDP ServerTransport (impl).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
//
// UDP server transport. The demux thread reads the shared socket with
// a 500 ms recv timeout (to observe the running flag), routes valid datagrams to
// the owning connection, and registers a new connection only on a CONNECT_REQUEST
// from an unknown sender (anti-spoof). All map/pending mutation is under one lock;
// enqueueReceived runs outside it.

#include "naudio/net/UdpServerTransport.hpp"

#include <chrono>
#include <utility>
#include <vector>

#include "naudio/ControlMessage.hpp"

namespace naudio::net {

UdpServerTransport::~UdpServerTransport() { close(); }

std::int64_t UdpServerTransport::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool UdpServerTransport::isConnectRequest(const std::optional<AudioPacket>& packet) {
    if (!packet || packet->packetType() != PacketType::Control) return false;
    std::optional<ControlMessage> msg = ControlMessage::deserialize(packet->payload());
    return msg && msg->messageType() == ControlType::ConnectRequest;
}

bool UdpServerTransport::bind(std::uint16_t port, std::string* err) {
    if (bound_.load()) {
        if (err) *err = "Already bound";
        return false;
    }
    socket_ = Socket::bindUdp(bindHost_, port, /*reuseAddr=*/true, err);
    if (!socket_.valid()) return false;
    bound_.store(true);
    running_.store(true);
    demuxThread_ = std::thread([this] { demuxLoop(); });
    return true;
}

bool UdpServerTransport::isBound() const {
    return bound_.load() && socket_.valid();
}

int UdpServerTransport::port() const {
    return isBound() ? static_cast<int>(socket_.localPort()) : -1;
}

void UdpServerTransport::demuxLoop() {
    std::vector<std::uint8_t> buf(UdpClientConnection::MAX_DATAGRAM_SIZE);

    while (running_.load()) {
        socket_.setRecvTimeout(500);  // wake periodically to re-check running
        RecvFromResult rr = socket_.recvFrom(buf.data(), buf.size());
        if (rr.status == IoStatus::TimedOut) continue;
        if (rr.status != IoStatus::Ok) {
            if (!running_.load()) break;  // socket closed during shutdown
            continue;                     // transient — keep serving
        }

        const std::string key = addressKey(rr.senderHost, rr.senderPort);
        std::optional<AudioPacket> packet = AudioPacket::deserialize(buf.data(), rr.bytes);

        std::shared_ptr<UdpClientConnection> conn;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            auto it = byAddress_.find(key);
            if (it != byAddress_.end()) conn = it->second;

            if (conn && conn->isClosed()) {
                // Stale mapping — remove; the CONNECT_REQUEST gate re-applies.
                byId_.erase(conn->clientAddress().id());
                byAddress_.erase(key);
                conn.reset();
            }
            if (!conn) {
                // Unknown (or just-cleared) sender: only a valid CONNECT_REQUEST
                // creates a connection (anti-spoof). Anything else is dropped.
                if (isConnectRequest(packet)) {
                    conn = tryCreateConnectionLocked(rr.senderHost, rr.senderPort, key);
                }
            }
        }

        if (!conn) continue;  // dropped: non-CONNECT from unknown, or pending full
        if (packet) {
            conn->enqueueReceived(*packet, static_cast<int>(rr.bytes));
        } else {
            conn->recordCrcError();  // known client, undeserializable datagram
        }
    }
}

std::shared_ptr<UdpClientConnection> UdpServerTransport::tryCreateConnectionLocked(
    const std::string& host, std::uint16_t port, const std::string& addrKey) {
    if (pending_.size() >= MAX_PENDING_CONNECTIONS) {
        expireStaleLocked();
        if (pending_.size() >= MAX_PENDING_CONNECTIONS) {
            return nullptr;  // still full after the sweep — drop the CONNECT_REQUEST
        }
    }
    const std::string id = "udp-" + std::to_string(clientIdCounter_.fetch_add(1));
    ClientAddress address(id, host, port);
    // Server-side connections borrow the shared socket (ownsSocket == false).
    auto conn = std::make_shared<UdpClientConnection>(&socket_, host, port,
                                                      std::move(address), cfg_);
    byAddress_[addrKey] = conn;
    byId_[id] = conn;
    pending_.push_back({conn, nowMs()});
    pendingCv_.notify_one();
    return conn;
}

void UdpServerTransport::expireStaleLocked() {
    const std::int64_t now = nowMs();
    for (auto it = pending_.begin(); it != pending_.end();) {
        const bool expired = (now - it->since > PENDING_TTL_MS) || it->conn->isClosed();
        if (expired) {
            const std::string& id = it->conn->clientAddress().id();
            const std::string key = addressKey(it->conn->clientAddress().host(),
                                               it->conn->clientAddress().port());
            byId_.erase(id);
            byAddress_.erase(key);
            it->conn->close();
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

IoStatus UdpServerTransport::acceptClient(int timeoutMs,
                                          std::shared_ptr<ClientConnection>& out,
                                          std::string* err) {
    if (!isBound()) {
        if (err) *err = "Transport not bound";
        return IoStatus::Error;
    }

    std::unique_lock<std::mutex> lock(stateMutex_);
    auto hasWork = [this] { return !pending_.empty() || !running_.load(); };

    if (pending_.empty()) {
        if (timeoutMs <= 0) {
            pendingCv_.wait(lock, hasWork);
        } else {
            if (!pendingCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), hasWork)) {
                return IoStatus::TimedOut;  // deadline elapsed, none arrived
            }
        }
    }
    if (!running_.load()) return IoStatus::Error;  // closing

    // Pop the first still-open pending connection.
    while (!pending_.empty()) {
        PendingEntry entry = pending_.front();
        pending_.pop_front();
        if (!entry.conn->isClosed()) {
            out = entry.conn;
            return IoStatus::Ok;
        }
    }
    return IoStatus::TimedOut;  // all pending were closed
}

void UdpServerTransport::disconnectClient(
    const std::shared_ptr<ClientConnection>& connection) {
    if (!connection) return;
    const std::string& id = connection->clientAddress().id();

    std::shared_ptr<UdpClientConnection> removed;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        auto it = byId_.find(id);
        if (it != byId_.end()) {
            removed = it->second;
            byId_.erase(it);
            if (removed->clientAddress().hasEndpoint()) {
                byAddress_.erase(addressKey(removed->clientAddress().host(),
                                            removed->clientAddress().port()));
            }
        }
    }
    if (removed) removed->close();
}

std::int64_t UdpServerTransport::packetsSent() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::int64_t total = 0;
    for (const auto& [id, conn] : byId_) total += conn->packetsSent();
    return total;
}

std::int64_t UdpServerTransport::packetsReceived() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::int64_t total = 0;
    for (const auto& [id, conn] : byId_) total += conn->packetsReceived();
    return total;
}

std::int64_t UdpServerTransport::bytesSent() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::int64_t total = 0;
    for (const auto& [id, conn] : byId_) total += conn->bytesSent();
    return total;
}

std::int64_t UdpServerTransport::bytesReceived() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::int64_t total = 0;
    for (const auto& [id, conn] : byId_) total += conn->bytesReceived();
    return total;
}

int UdpServerTransport::crcErrors() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    int total = 0;
    for (const auto& [id, conn] : byId_) total += conn->crcErrors();
    return total;
}

void UdpServerTransport::close() {
    bool wasRunning = running_.exchange(false);
    bound_.store(false);
    pendingCv_.notify_all();  // wake any blocked acceptClient

    if (wasRunning && demuxThread_.joinable()) {
        demuxThread_.join();  // exits within the 500 ms recv timeout
    }

    std::vector<std::shared_ptr<UdpClientConnection>> drained;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (auto& [id, conn] : byId_) drained.push_back(conn);
        byId_.clear();
        byAddress_.clear();
        pending_.clear();
    }
    for (auto& conn : drained) conn->close();  // closed after the lock is released

    socket_.close();
}

}  // namespace naudio::net
