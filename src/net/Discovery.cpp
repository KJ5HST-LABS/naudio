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
