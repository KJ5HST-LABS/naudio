// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — composite TCP+UDP ServerTransport.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "naudio/AudioStreamConfig.hpp"
#include "naudio/net/TcpServerTransport.hpp"
#include "naudio/net/Transport.hpp"
#include "naudio/net/UdpClientConnection.hpp"  // UdpReliabilityConfig
#include "naudio/net/UdpServerTransport.hpp"

namespace naudio::net {

// A ServerTransport that serves both TCP and UDP clients at once. It binds a
// TcpServerTransport and a
// UdpServerTransport to the same port number — TCP and UDP ports are independent
// at the OS level — and acceptClient() polls both. Stats aggregate across both;
// close() closes both.
//
// Header-only: pure composition over the two sub-transports, each of which
// already carries its own thread/lock state. The bind address is forwarded to
// both (default wildcard).
class DualServerTransport : public ServerTransport {
public:
    explicit DualServerTransport(const AudioStreamConfig& config = AudioStreamConfig{},
                                 std::string bindHost = "")
        : tcp_(bindHost), udp_(std::move(bindHost)) {
        // Map the audio-stream config onto the UDP reliability knobs.
        UdpReliabilityConfig cfg;
        cfg.reorderWindowSize = config.reorderBufferSize;
        cfg.reorderMaxHoldMs = config.reorderMaxHoldMs;
        cfg.fecEnabled = config.fecEnabled;
        cfg.fecBlockSize = config.fecBlockSize;
        cfg.frameDurationMs = config.frameDurationMs;
        cfg.maxAudioPayloadBytes = config.udpMaxAudioPayload();  // #86
        cfg.adaptiveJitterEnabled = config.adaptiveJitterEnabled;
        cfg.jitterMinMs = config.bufferMinMs;
        cfg.jitterMaxMs = config.bufferMaxMs;
        cfg.jitterMultiplier = config.jitterMultiplier;
        cfg.controlReliabilityEnabled = config.controlReliabilityEnabled;
        cfg.controlRetransmitMaxAttempts = config.controlRetransmitMaxAttempts;
        udp_.setReliabilityConfig(cfg);
    }

    // Paired binds attempted when the caller named NO port. See bind().
    //
    // The number has to clear a CONTIGUOUS reserved block, not just one unlucky port,
    // because the OS hands out ephemeral ports SEQUENTIALLY: measured on macOS/arm64,
    // 24 close-then-rebind cycles advanced by exactly +1 every time and never re-issued
    // a just-released port (0/11 immediate repeats, twice). That measurement is what
    // rules out parking the failed ports to force distinct ones — the allocator already
    // does — but it also means each retry steps just ONE port further into a reserved
    // range. A budget of "a few" would walk into a 16-port WinNAT block and give up
    // inside it, fixing nothing. Attempts cost two syscalls each, so the budget is set
    // well clear of the block widths those hosts typically reserve.
    static constexpr int kPort0BindAttempts = 32;

    // Binds TCP, then binds UDP to the port TCP was actually assigned. TCP and UDP port
    // spaces are independent at the OS level, which is what makes one port number serve
    // both — a contract the C ABI publishes (naudio.h: "a DUAL server serves TCP+UDP on
    // one port").
    //
    // WHEN THE CALLER NAMED NO PORT (issue #73) the OS picks the TCP port freely, and
    // nothing then guarantees the same number is available on UDP. On Windows hosts
    // running WinNAT / Hyper-V, whole blocks of UDP ports are reserved, and a bind into
    // one returns WSAEACCES (10013, "permission denied") rather than WSAEADDRINUSE — so
    // whenever the OS's free TCP pick lands in a reserved UDP block, the paired bind
    // fails. It is a function of which port the OS chose, so it is intermittent by
    // construction and unrelated to the caller. Five CI arms failed this way on a
    // documentation-only commit.
    //
    // So a port-0 caller gets the pair retried on a fresh OS-assigned port. That is
    // behaviour-preserving: it asked for "any port", and it still gets any port.
    //
    // A caller that NAMED a port gets exactly one attempt — that port or nothing. It
    // asked for a specific number, and silently serving a different one would break the
    // promise rather than keep it. This split is also what keeps the retry from masking
    // a genuine permission problem: a real privilege refusal (a port below 1024 without
    // the rights to it) is only reachable when the caller named the port, which is the
    // path that does not retry. The OS never hands out a privileged port for 0.
    bool bind(std::uint16_t port, std::string* err) override {
        const int attempts = (port == 0) ? kPort0BindAttempts : 1;
        std::string udpErr;
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            if (!tcp_.bind(port, err)) return false;  // TCP itself refused — not our business
            // Resolve the actual port (handles ephemeral port 0), bind UDP to it.
            const int actualPort = tcp_.port();
            if (bindUdpTo(static_cast<std::uint16_t>(actualPort), &udpErr)) {
                bindAttempts_ = attempt;
                return true;
            }
            tcp_.close();  // rollback so a partial bind never strands TCP
        }
        bindAttempts_ = attempts;
        // Report the UDP refusal itself, never a summary of it: the retry must not turn a
        // diagnosable error into "bind failed". The attempt count is what distinguishes an
        // exhausted retry from a single refusal.
        if (err) {
            *err = "UDP bind failed on " + std::to_string(attempts) +
                   (attempts == 1 ? " attempt: " : " OS-assigned ports, last error: ") + udpErr;
        }
        return false;
    }

    // Paired binds the last bind() call made — 1 on a first-try success or a named port,
    // up to kPort0BindAttempts. The observable for issue #73: it distinguishes "retried and
    // recovered" from "never had to", which no timing measurement can do reliably.
    int bindAttempts() const { return bindAttempts_; }

    bool isBound() const override { return tcp_.isBound() && udp_.isBound(); }
    int port() const override { return tcp_.port(); }  // the canonical (TCP) port

    IoStatus acceptClient(int timeoutMs, std::shared_ptr<ClientConnection>& out,
                          std::string* err) override {
        if (!isBound()) {
            if (err) *err = "Transport not bound";
            return IoStatus::Error;
        }
        // Split the deadline: poll TCP first, then UDP with the remainder.
        // For a positive timeout, floor each slice at 1 ms. A small value like
        // timeoutMs == 1 would otherwise collapse a slice to 0, and
        // Socket::acceptTcp treats a <=0 timeout as a BLOCKING accept on the
        // blocking listener — that would hang this dual accept thread and
        // starve the UDP side. timeoutMs <= 0 keeps its documented "blocks"
        // semantics (see ServerTransport::acceptClient in Transport.hpp).
        const int tcpTimeout =
            timeoutMs > 0 ? (timeoutMs / 2 < 1 ? 1 : timeoutMs / 2) : timeoutMs / 2;
        const int udpTimeout =
            timeoutMs > 0 ? (timeoutMs - tcpTimeout < 1 ? 1 : timeoutMs - tcpTimeout)
                          : timeoutMs - tcpTimeout;
        if (tcp_.acceptClient(tcpTimeout, out, err) == IoStatus::Ok) return IoStatus::Ok;
        return udp_.acceptClient(udpTimeout, out, err);
    }

    void disconnectClient(const std::shared_ptr<ClientConnection>& connection) override {
        // Safe to call both — each ignores unknown connections.
        tcp_.disconnectClient(connection);
        udp_.disconnectClient(connection);
    }

    std::int64_t packetsSent() const override { return tcp_.packetsSent() + udp_.packetsSent(); }
    std::int64_t packetsReceived() const override {
        return tcp_.packetsReceived() + udp_.packetsReceived();
    }
    std::int64_t bytesSent() const override { return tcp_.bytesSent() + udp_.bytesSent(); }
    std::int64_t bytesReceived() const override {
        return tcp_.bytesReceived() + udp_.bytesReceived();
    }
    int crcErrors() const override { return tcp_.crcErrors() + udp_.crcErrors(); }
    // The TCP half contributes a structural 0 to both (no control ARQ, no ordered queue), so on
    // DUAL these report the UDP half's roster alone — which is the half that can produce the
    // events at all.
    std::int64_t controlRetransmits() const override {
        return tcp_.controlRetransmits() + udp_.controlRetransmits();
    }
    std::int64_t orderedQueueDrops() const override {
        return tcp_.orderedQueueDrops() + udp_.orderedQueueDrops();
    }

    void close() override {
        tcp_.close();
        udp_.close();
    }

protected:
    // TEST SEAM (issue #73). The paired UDP bind, as one overridable call.
    //
    // The retry above exists for a refusal NO TEST CAN PROVOKE ON DEMAND: it needs the OS
    // to reserve the exact port its own allocator just handed out, which is why the bug
    // only ever appeared as an intermittent on Windows CI. Overriding this is the only way
    // to assert the retry does what it claims. Production behaviour is one non-virtual-in-
    // practice call to udp_.bind — a test double is the sole other implementation.
    virtual bool bindUdpTo(std::uint16_t port, std::string* err) { return udp_.bind(port, err); }

private:
    TcpServerTransport tcp_;
    UdpServerTransport udp_;
    int bindAttempts_{0};
};

}  // namespace naudio::net
