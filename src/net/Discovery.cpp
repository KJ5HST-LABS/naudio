// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — client-side server discovery (impl).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
//
// One broadcast DISCOVER out, one unicast DISCOVER_REPLY back per server, then
// silence — §6.8's N+1 exchange. Everything here is deliberately stateless: the
// probe socket is created, used and closed inside the call, so a discovery sweep
// leaves nothing behind on either side of the wire.

#include "naudio/net/Discovery.hpp"

#include <chrono>
#include <random>
#include <set>
#include <utility>

#include "naudio/AudioPacket.hpp"
#include "naudio/net/Socket.hpp"

namespace naudio::net {

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// §6.8: "A client SHOULD choose it unpredictably." A predictable token lets a
// third party on the segment forge a reply that a prober would accept, which is
// the only thing the token defends against — it is not a nonce for anything else.
std::uint32_t freshToken() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint32_t> dist;
    return dist(gen);
}

}  // namespace

bool DiscoveryReplier::allow(const std::string& addrKey, std::int64_t nowMs) {
    auto it = seen_.find(addrKey);
    if (it != seen_.end()) {
        if (nowMs - it->second < MIN_INTERVAL_MS) return false;
        it->second = nowMs;
        return true;
    }
    // New source. At the cap the OLDEST entry goes rather than the table growing:
    // evicting oldest-first means a flood of forged addresses cannot push out a
    // real prober's entry any faster than it would have aged out anyway.
    if (seen_.size() >= MAX_SOURCES) {
        auto oldest = seen_.begin();
        for (auto i = seen_.begin(); i != seen_.end(); ++i) {
            if (i->second < oldest->second) oldest = i;
        }
        seen_.erase(oldest);
    }
    seen_[addrKey] = nowMs;
    return true;
}

std::vector<std::uint8_t> DiscoveryReplier::buildReply(std::uint32_t token,
                                                       const DiscoveryFacts& facts) {
    ControlMessage reply = ControlMessage::discoverReply(
        token, facts.port, facts.transports, facts.sampleRate, facts.bitsPerSample,
        facts.channels, facts.clientCount, facts.maxClients, facts.name);
    return AudioPacket(PacketType::Control, seq_++, reply.serialize()).serialize();
}

DiscoveryResponder::~DiscoveryResponder() { stop(); }

bool DiscoveryResponder::start(const std::string& bindHost, std::uint16_t port,
                               DiscoveryFactsProvider provider, std::string* err) {
    if (running_.load()) {
        if (err) *err = "Discovery responder already running";
        return false;
    }
    if (!provider) {
        if (err) *err = "Discovery responder needs a facts provider";
        return false;
    }
    // No SO_REUSEADDR, for issue #83's reason: a bind that "succeeds" for a port we
    // do not have is worse than one that refuses, and here a refusal is the normal,
    // handled outcome rather than a failure.
    socket_ = Socket::bindUdp(bindHost, port, /*reuseAddr=*/false, err);
    if (!socket_.valid()) return false;

    // Set BEFORE the thread exists, and unreachable afterwards because there is no
    // setter -- so this needs no lock and, unlike the transport's seam, no caller
    // can break the rule by accident.
    provider_ = std::move(provider);
    running_.store(true);
    thread_ = std::thread([this] { loop(); });
    return true;
}

void DiscoveryResponder::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    socket_.close();
    if (thread_.joinable()) thread_.join();
}

int DiscoveryResponder::port() const {
    return running_.load() && socket_.valid() ? static_cast<int>(socket_.localPort()) : -1;
}

void DiscoveryResponder::loop() {
    std::vector<std::uint8_t> buf(2048);
    while (running_.load()) {
        socket_.setRecvTimeout(500);  // wake periodically to observe running_
        RecvFromResult rr = socket_.recvFrom(buf.data(), buf.size());
        if (rr.status == IoStatus::TimedOut) continue;
        if (rr.status != IoStatus::Ok) {
            if (!running_.load()) break;  // closed during shutdown
            continue;                     // transient
        }

        // This socket answers DISCOVER and nothing else. It creates no connection
        // and routes nothing, so there is no anti-spoof gate to interact with --
        // every other datagram is simply dropped.
        std::optional<AudioPacket> packet = AudioPacket::deserialize(buf.data(), rr.bytes);
        if (!packet || packet->packetType() != PacketType::Control) continue;
        std::optional<ControlMessage> msg = ControlMessage::deserialize(packet->payload());
        if (!msg || msg->messageType() != ControlType::Discover) continue;
        std::optional<std::uint32_t> token = msg->parseDiscoverToken();
        if (!token) continue;

        const DiscoveryFacts facts = provider_();
        if (!facts.enabled) continue;  // opted out: no reply, and no limiter work
        const std::string key = rr.senderHost + ":" + std::to_string(rr.senderPort);
        if (!replier_.allow(key, nowMs())) continue;

        const std::vector<std::uint8_t> bytes = replier_.buildReply(*token, facts);
        // UNICAST back to the prober. The reply may leave by a different socket than
        // the one the server serves audio on, which is exactly why the port is a
        // PAYLOAD field and the address is not: the address the prober sees is this
        // host, and the port it needs is in the reply.
        socket_.sendTo(bytes.data(), bytes.size(), rr.senderHost, rr.senderPort);
    }
}

std::vector<DiscoveredServer> discoverServers(const DiscoveryOptions& options,
                                              std::string* err) {
    std::vector<DiscoveredServer> found;
    if (options.maxServers == 0) return found;

    Socket probe = Socket::bindUdp(options.bindHost, 0, /*reuseAddr=*/false, err);
    if (!probe.valid()) return found;

    // Without this the sendTo below fails with EACCES. It is off by default in
    // every OS, and it is exactly why a naudio client could not do this before.
    if (!probe.setBroadcast(true)) {
        if (err) *err = "could not enable broadcast on the probe socket (SO_BROADCAST)";
        return found;
    }

    const std::uint32_t token = freshToken();
    const std::vector<std::uint8_t> out =
        AudioPacket::createControl(0, ControlMessage::discover(token).serialize()).serialize();
    if (!probe.sendTo(out.data(), out.size(), options.address, options.port)) {
        if (err) *err = "could not send the DISCOVER probe to " + options.address;
        return found;
    }

    // Collect until the window closes. The deadline is absolute, so a stream of
    // junk datagrams cannot extend the call by resetting a per-recv timeout.
    const std::int64_t deadline = nowMs() + (options.timeoutMs < 0 ? 0 : options.timeoutMs);
    std::set<std::string> seen;
    std::vector<std::uint8_t> buf(2048);

    while (found.size() < options.maxServers) {
        const std::int64_t remaining = deadline - nowMs();
        if (remaining <= 0) break;
        probe.setRecvTimeout(static_cast<int>(remaining));

        RecvFromResult rr = probe.recvFrom(buf.data(), buf.size());
        if (rr.status != IoStatus::Ok) break;  // timed out, or the socket went away

        std::optional<AudioPacket> packet = AudioPacket::deserialize(buf.data(), rr.bytes);
        if (!packet || packet->packetType() != PacketType::Control) continue;
        std::optional<ControlMessage> msg = ControlMessage::deserialize(packet->payload());
        if (!msg) continue;
        std::optional<DiscoveryInfo> info = msg->parseDiscoverReply();
        if (!info) continue;
        // §6.8: "MUST ignore a reply whose token it did not send." On a broadcast
        // segment this is the only thing separating our replies from someone
        // else's probe round happening at the same time.
        if (info->token != token) continue;

        // The address is not a wire field. This assignment is the discovery result.
        info->host = rr.senderHost;

        // Dedupe on the ENDPOINT, not the name: two servers may share a label, and
        // one server on a multi-homed host may answer from more than one address —
        // in which case both entries are real and separately connectable.
        const std::string key = info->host + ":" + std::to_string(info->port);
        if (!seen.insert(key).second) continue;

        found.push_back(std::move(*info));
    }
    return found;
}

}  // namespace naudio::net
