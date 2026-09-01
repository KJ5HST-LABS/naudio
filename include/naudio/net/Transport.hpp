// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — transport abstractions (TCP/UDP-agnostic).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "naudio/AudioPacket.hpp"
#include "naudio/ControlMessage.hpp"
#include "naudio/Provenance.hpp"
#include "naudio/net/ClientAddress.hpp"
#include "naudio/net/Socket.hpp"  // IoStatus (reused for acceptClient)

namespace naudio::net {

// The result of receivePacket — a no-throw value (instead of returning a packet,
// returning none, or throwing):
//   packet has value          -> a frame was decoded
//   empty && !closed          -> no data this call (timeout / single transient
//                                skip); the caller retries
//   empty && closed           -> the connection is dead (EOF / I/O error / too
//                                many consecutive errors); the caller tears down
struct ReceiveResult {
    std::optional<AudioPacket> packet;
    bool closed = false;
    // How `packet` reached this result — see Provenance.hpp. Meaningful only when
    // hasPacket(); the two empty results carry Live because nothing was repaired.
    //
    // DELIBERATELY UNDEFAULTED, and this is load-bearing rather than stylistic
    // (issue #65). This project passes no warning flags, so a member with a default
    // initializer would let all three factories below keep compiling untouched while
    // silently reporting Live forever — the feature would ship doing nothing and no
    // build would say so. With no default, every construction site must state it.
    // Do not add a default value here, and do not replace this with a bool: `bool`
    // and a scoped enum are not interconvertible, which is what makes the three
    // fields below impossible to transpose in the aggregate initializers.
    Provenance provenance;

    bool hasPacket() const { return packet.has_value(); }

    static ReceiveResult of(AudioPacket p, Provenance provenance) {
        return {std::move(p), false, provenance};
    }
    // No packet, so nothing was reconstructed. Live is both the accurate reading and
    // the safe one — it is what every path did before provenance existed.
    static ReceiveResult noData() { return {std::nullopt, false, Provenance::Live}; }
    static ReceiveResult dead() { return {std::nullopt, true, Provenance::Live}; }
};

// Per-client connection handle for bidirectional audio streaming. Both
// server-side (one per accepted client) and client-side (the connection to the
// server) implement it.
//
// Threading: a UDP demux thread feeds the connection (enqueue) while the
// application thread calls receivePacket on the SAME connection. So these are
// non-const methods called concurrently through a shared_ptr — each
// implementation is responsible for its own synchronization (mutex / atomics).
// `const` is reserved for the
// genuinely read-only getters.
class ClientConnection {
public:
    virtual ~ClientConnection() = default;

    // The identity of this connection's remote peer.
    virtual const ClientAddress& clientAddress() const = 0;

    // --- Send (return false on I/O failure) ---
    virtual bool sendControl(const ControlMessage& message) = 0;

    // A courtesy control send for TEARDOWN paths that must not block: sends only if this
    // connection's send path is free at the instant of the call, and returns false
    // immediately rather than waiting behind a sender that already holds it (issue #77).
    //
    // Returns true ONLY if the message was actually handed to the socket. false covers both
    // "declined — a send was already in flight" and "the send failed"; no caller separates
    // them, because both mean the frame will not arrive promptly and a retry would only wait
    // on the same wedge.
    //
    // PURE VIRTUAL, not a defaulted forward to sendControl, for the reason
    // ServerTransport::controlRetransmits below gives: all three implementations are in-tree,
    // and the plausible-looking default here — "just call sendControl" — silently reinstates
    // the exact block this method exists to avoid. A forgotten override would be
    // indistinguishable from a correct one until a peer wedged in production. Implement it in
    // the new connection type; do not default it here.
    virtual bool trySendControl(const ControlMessage& message) = 0;
    virtual bool sendRxAudio(const std::uint8_t* data, std::size_t offset,
                             std::size_t length) = 0;
    virtual bool sendTxAudio(const std::uint8_t* data, std::size_t length) = 0;
    virtual bool sendHeartbeat() = 0;
    virtual bool sendPacket(const AudioPacket& packet) = 0;

    // --- Receive ---
    virtual ReceiveResult receivePacket(int timeoutMs) = 0;

    // --- Heartbeat / timeout ---
    // Non-const because the UDP implementation piggybacks a control-retransmit
    // pass (which sends) on this poll; the TCP implementation is a pure check.
    virtual bool shouldSendHeartbeat() = 0;

    // Runs ONE control-ARQ retransmit sweep, resending any critical control whose timeout has
    // expired. Defaulted rather than pure virtual — unlike trySendControl above, where a
    // plausible default would silently reinstate the bug: TCP is a reliable ordered stream and
    // has no control-ARQ at all, so doing nothing is the CORRECT behaviour there, not a
    // placeholder waiting for an override. Same shape as the UDP-only statistics defaults below.
    //
    // Exists because the sweep's other pump — shouldSendHeartbeat — is reached only from the
    // heartbeat loop, and that loop is gated on connected_, which is not set until AFTER
    // performHandshake returns. So a critical control sent DURING the handshake has no pump at
    // all unless something calls this. That gap is why ControlType::Disconnect is tracked and
    // never actually resent, and it would have made ConnectRequest inert in the same way
    // (issue #29 option 4).
    virtual void pumpControlRetransmits() {}
    virtual bool isConnectionTimedOut() const = 0;
    virtual std::int64_t timeSinceLastReceive() const = 0;

    // --- Statistics ---
    virtual std::int64_t packetsSent() const = 0;
    virtual std::int64_t packetsReceived() const = 0;
    virtual std::int64_t bytesSent() const = 0;
    virtual std::int64_t bytesReceived() const = 0;
    virtual int crcErrors() const = 0;

    // UDP-only reliability stats; the TCP defaults are 0 / -1.
    virtual std::int64_t packetsLost() const { return 0; }
    virtual std::int64_t packetsOutOfOrder() const { return 0; }
    virtual double packetLossRate() const { return 0.0; }
    virtual std::int64_t packetsReordered() const { return 0; }
    virtual std::int64_t packetsRecoveredByFec() const { return 0; }
    virtual double jitterMs() const { return 0.0; }
    virtual int adaptiveBufferTargetMs() const { return -1; }
    virtual std::int64_t controlRetransmits() const { return 0; }

    // FEC blocks whose parity range could not be reconciled with the block, so
    // recovery was DECLINED rather than run. A lost opportunity to recover, never a
    // correctness failure. Several distinct causes reach it, and
    // FecDecoder::fecBlocksUnreconciled() owns that contract. Do not restate them
    // here, and do not state how many there are — a second copy is a second thing to
    // drift, which is exactly how this comment came to name only one of them and then
    // to claim there were two.
    virtual std::int64_t fecBlocksUnreconciled() const { return 0; }

    // Packets discarded from the ordered queue because it was at capacity (a
    // stalled or too-slow consumer). Non-zero means audio was dropped locally,
    // after the network delivered it.
    virtual std::int64_t orderedQueueDrops() const { return 0; }

    // Sequence slots the reorder buffer gave up on and emitted as a gap — the
    // pipeline could not deliver them in order. This is the POST-REORDER loss
    // measure, and it is the complement of packetsLost(): exactly one of the two
    // is measured on any given connection, because the gap tracker runs only when
    // NO reorder buffer is engaged and this counter exists only when one IS.
    //
    // Counted BEFORE the FEC decoder sees the stream (the pipeline is
    // reorder -> FEC -> ordered queue), so a slot counted here may still be
    // refilled; the unrecovered remainder is this minus packetsRecoveredByFec().
    // Counts every packet type sharing the sequence space — audio, parity and
    // control alike — not audio packets alone.
    //
    // Defaults to -1, NOT to 0 like its neighbours above: 0 here would read as
    // "nothing was lost", which is the one thing a loss counter must never say
    // when it is not measuring. TCP and any passthrough profile keep the -1.
    virtual std::int64_t sequenceGaps() const { return -1; }

    // Datagrams the KERNEL discarded for want of receive-buffer room on this connection's
    // socket — audio lost locally, before naudio ever saw it. -1 means not measured.
    //
    // This is the one loss mode nothing else here can report, and sequenceGaps() above is
    // precisely the counter that cannot: a full receive buffer tail-drops, so a consumer that
    // falls behind reads an unbroken PREFIX and stops early, leaving no hole to count. The two
    // are complements, not overlaps — a gap says the pipeline could not order what arrived, this
    // says the arrival never happened.
    //
    // -1 on every transport that does not measure it, which today is every one except a
    // client-owned UDP connection on Linux (see Socket::enableReceiveDropCounter for why the
    // mechanism is not portable and why no derived estimate stands in for it).
    virtual std::int64_t socketReceiveDrops() const { return -1; }

    // Audio packets dropped from a PENDING FEC block without ever being offered to a parity —
    // repair capacity that expired, and NOT audio loss: the decoder emits each packet to the
    // application on arrival and only then caches a copy, so this counts copies leaving the
    // cache. -1 on every transport with no FEC decoder, which is all of them but a UDP
    // connection on an FEC-enabled profile. FecDecoder::pendingPacketsDiscarded owns the
    // trigger list; do not restate it here (issue #55's lesson about duplicated cause lists).
    virtual std::int64_t fecPendingPacketsDiscarded() const { return -1; }

    // Whether packetsLost() / packetsOutOfOrder() / packetLossRate() are actually
    // MEASURED on this connection. When false they are unavailable, and a consumer
    // must not read their 0 as "nothing was lost".
    //
    // False is the common case: the sequence-gap tracker runs only when no reorder
    // buffer is engaged (the reorder buffer owns ordering, and no post-reorder loss
    // accounting exists), and TCP never tracks gaps at all. EVERY built-in UDP
    // profile configures a reorder buffer, so on every one of them these three are
    // unmeasured while packetsReordered / packetsRecoveredByFec are live.
    virtual bool measuresSequenceGaps() const { return false; }

    // The remote address as a string ("ip:port"), or "" if unavailable.
    virtual std::string remoteAddress() const = 0;

    virtual bool isClosed() const = 0;
    virtual void close() = 0;

    // Reads and discards already-buffered input so that the close which follows emits a FIN
    // rather than an RST, and returns how many bytes went. Bounded by `budgetMs`.
    //
    // DEFAULTS TO A NO-OP, and that default is correct for every non-TCP connection rather than
    // merely convenient. UDP has no such rule to work around — there is no connection state to
    // reset — and a UDP server connection does not even own its socket: it borrows the one shared
    // demux socket, so "draining pending input" there would consume OTHER clients' datagrams.
    //
    // Only the server's reject path calls this. See TcpClientConnection for the full rule.
    virtual std::size_t discardPendingInput(int /*budgetMs*/) { return 0; }
};

// Client-side transport — connects to a server and yields a ClientConnection.
class ClientTransport {
public:
    virtual ~ClientTransport() = default;

    // Connects to host:port (timeoutMs == 0 blocks). Returns null and fills err
    // on failure.
    virtual std::shared_ptr<ClientConnection> connect(const std::string& host,
                                                      std::uint16_t port,
                                                      int timeoutMs,
                                                      std::string* err) = 0;
    virtual void close() = 0;
};

// Server-side transport — binds a port and yields one ClientConnection per
// client. TCP uses one socket per client with an accept loop; UDP uses a single
// socket with address-based demultiplexing.
// The facts a server publishes in a DISCOVER_REPLY (spec 1.4, §6.8). Gathered
// FRESH at each probe rather than cached at bind: clientCount changes, and a
// stale occupancy is exactly the field a chooser would act on wrongly.
struct DiscoveryFacts {
    bool enabled = false;         // false = stay silent (the operator opted out).
    std::uint16_t port = 0;
    std::uint8_t transports = 0;  // DiscoveryTransport bitmask (DUAL sets both).
    std::uint32_t sampleRate = 0;
    std::uint8_t bitsPerSample = 0;
    std::uint8_t channels = 0;
    std::uint8_t clientCount = 0;  // Saturated to 255 by the caller.
    std::uint8_t maxClients = 0;   // Saturated to 255 by the caller.
    std::string name;
};

// Supplies those facts on demand. Invoked on the demux thread, once per probe,
// so it MUST NOT block and MUST NOT re-enter the transport.
using DiscoveryFactsProvider = std::function<DiscoveryFacts()>;

class ServerTransport {
public:
    virtual ~ServerTransport() = default;

    // Installs the source of DISCOVER_REPLY facts (§6.8). Call BEFORE bind():
    // bind() starts the demux thread, and a provider installed after it races
    // with the first probe.
    //
    // Defaulted to a no-op, and deliberately NOT pure virtual like the counters
    // below. Their rule is that a silent 0 is indistinguishable from a real
    // measurement -- but here a silent no-op IS the truthful answer for a
    // transport with no datagram path to an unknown sender: §6.8 item 6 says a
    // TCP-only server is undiscoverable, which is a property of TCP, not a
    // forgotten override. Making it pure would also break every out-of-tree
    // ServerTransport, which setTransportFactory makes a supported thing to have.
    virtual void setDiscoveryFacts(DiscoveryFactsProvider provider) { (void)provider; }

    // Binds to a port (0 for ephemeral). Returns false and fills err on failure.
    virtual bool bind(std::uint16_t port, std::string* err) = 0;
    virtual bool isBound() const = 0;
    // The bound local port, or -1 if not bound.
    virtual int port() const = 0;

    // Waits for a new client. timeoutMs == 0 blocks. On Ok, `out` receives the
    // connection; TimedOut means none arrived (retry); Error is fatal.
    virtual IoStatus acceptClient(int timeoutMs,
                                  std::shared_ptr<ClientConnection>& out,
                                  std::string* err) = 0;

    // Disconnects a specific client and releases its resources.
    virtual void disconnectClient(const std::shared_ptr<ClientConnection>& connection) = 0;

    // Aggregate statistics across all clients.
    //
    // THESE SUM OVER THE CURRENTLY REGISTERED CONNECTIONS ONLY. disconnectClient
    // erases a connection from the routing map, and its counters leave the sum with
    // it — so every aggregate here can DECREASE, and a roster that churns loses the
    // departed clients' history entirely. They are a snapshot of the live roster, not
    // a lifetime total, and no caller may treat them as monotonic.
    virtual std::int64_t packetsSent() const = 0;
    virtual std::int64_t packetsReceived() const = 0;
    virtual std::int64_t bytesSent() const = 0;
    virtual std::int64_t bytesReceived() const = 0;
    virtual int crcErrors() const = 0;

    // Control-ARQ resends, summed over the live roster. Pure virtual rather than a
    // defaulted 0 like ClientConnection's: all three implementations are in-tree, and
    // a silent 0 from a forgotten override is indistinguishable from "no retransmits
    // happened" — the one thing this counter must never say when it is not counting.
    virtual std::int64_t controlRetransmits() const = 0;
    // Packets discarded from a connection's ordered queue because it was at capacity —
    // audio lost LOCALLY after the network delivered it. Unlike the client side, this is
    // reachable here: a demux thread fills the queue while the session's application
    // thread drains it, so a slow consumer does not stall its own producer.
    virtual std::int64_t orderedQueueDrops() const = 0;

    virtual void close() = 0;
};

}  // namespace naudio::net
