// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — client-side server discovery (spec 1.4, §6.8).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "naudio/ControlMessage.hpp"  // DiscoveryInfo
#include "naudio/net/Transport.hpp"  // DiscoveryFacts

namespace naudio::net {

// One server that answered a probe. This is DiscoveryInfo with its `host` filled
// in from the reply datagram's source address — which IS the discovery result,
// since §6.8 deliberately omits the address from the wire.
using DiscoveredServer = DiscoveryInfo;

struct DiscoveryOptions {
    // Where to send the probe. The limited broadcast address reaches the local
    // segment without the caller knowing its subnet. A caller that DOES know its
    // subnet broadcast (e.g. "192.168.1.255") may prefer it: some hosts and most
    // routers treat 255.255.255.255 more restrictively, and a few interfaces
    // refuse it outright.
    std::string address = "255.255.255.255";

    // The port to probe. Discovery resolves an ADDRESS, not an address+port
    // (§2.5), so this must be the port the servers actually serve on. 4533 is the
    // default service port and the right first guess; a server on another port is
    // found only by a probe aimed at that port.
    std::uint16_t port = 4533;

    // How long to listen for replies. One round is one broadcast out and one
    // unicast reply per server, so this is a collection WINDOW, not a per-server
    // timeout — the call always takes about this long when anyone answers.
    int timeoutMs = 1000;

    // Upper bound on servers returned. A hostile segment can send replies forever;
    // this is what stops a probe from growing without limit.
    std::size_t maxServers = 64;

    // Local interface to bind the probe socket to ("" = let the OS choose). On a
    // host with several interfaces this selects which segment is probed.
    std::string bindHost;
};

// The server half of §6.8, shared by every path that answers a probe: the
// per-source rate limiter and the reply frame it builds.
//
// It exists because there are TWO such paths and they must not drift apart — the
// UDP transport answering a probe aimed at its own service port, and the
// DiscoveryResponder below answering one aimed at the rendezvous port. Neither
// owns the socket; the caller sends the bytes, because only the caller knows
// which socket the reply should leave by.
//
// NOT thread-safe. Each owner keeps its own instance and touches it from one
// thread (its own receive loop).
class DiscoveryReplier {
public:
    // §6.8's security note: one reply per source per window, over a BOUNDED table.
    // The table is the amplification target as much as the reply is — forging a
    // source address costs an attacker nothing — so it never grows past the cap.
    static constexpr std::size_t MAX_SOURCES = 64;
    static constexpr std::int64_t MIN_INTERVAL_MS = 1000;

    // True if this source may be answered now. Records the reply when it returns
    // true, so a caller that asks must send.
    bool allow(const std::string& addrKey, std::int64_t nowMs);

    // The serialized 0xAF01 frame carrying the DISCOVER_REPLY for `facts`.
    // Sequence numbers come from this object's own counter: a discovery reply
    // belongs to no connection, so it cannot borrow one (§6.8).
    std::vector<std::uint8_t> buildReply(std::uint32_t token, const DiscoveryFacts& facts);

    // Visible for tests: how many sources the limiter is currently tracking.
    std::size_t trackedSources() const { return seen_.size(); }

private:
    std::map<std::string, std::int64_t> seen_;  // addressKey -> last reply time (ms)
    std::int32_t seq_ = 0;
};

// Sends ONE DISCOVER and collects DISCOVER_REPLYs until the window closes.
//
// Returns the servers that answered, in arrival order, deduplicated by host:port.
// An EMPTY result is not an error — it means nobody answered, which is the normal
// outcome on a segment with no naudio servers. `err` (optional) is filled only
// when the probe could not be SENT: no socket, no broadcast permission, no route.
//
// This is the only naudio call that transmits to a broadcast address. It creates
// no connection and consumes no client slot on any server it finds (§6.8), so it
// is safe to call at startup and safe to repeat.
std::vector<DiscoveredServer> discoverServers(const DiscoveryOptions& options,
                                              std::string* err = nullptr);

}  // namespace naudio::net
