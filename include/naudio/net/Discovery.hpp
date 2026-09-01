// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — client-side server discovery (spec 1.4, §6.8).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "naudio/ControlMessage.hpp"  // DiscoveryInfo

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
