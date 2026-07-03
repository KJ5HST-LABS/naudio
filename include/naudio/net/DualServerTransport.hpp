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
        cfg.adaptiveJitterEnabled = config.adaptiveJitterEnabled;
        cfg.jitterMinMs = config.bufferMinMs;
        cfg.jitterMaxMs = config.bufferMaxMs;
        cfg.jitterMultiplier = config.jitterMultiplier;
        cfg.controlReliabilityEnabled = config.controlReliabilityEnabled;
        cfg.controlRetransmitMaxAttempts = config.controlRetransmitMaxAttempts;
        udp_.setReliabilityConfig(cfg);
    }

    bool bind(std::uint16_t port, std::string* err) override {
        if (!tcp_.bind(port, err)) return false;
        // Resolve the actual port (handles ephemeral port 0), bind UDP to it.
        const int actualPort = tcp_.port();
        if (!udp_.bind(static_cast<std::uint16_t>(actualPort), err)) {
            tcp_.close();  // rollback so a partial bind never strands TCP
            return false;
        }
        return true;
    }

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

    void close() override {
        tcp_.close();
        udp_.close();
    }

private:
    TcpServerTransport tcp_;
    UdpServerTransport udp_;
};

}  // namespace naudio::net
