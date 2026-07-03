// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — transport-agnostic client identity.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "naudio/AudioPacket.hpp"

namespace naudio::net {

// Opaque client identity for transport-agnostic addressing. For TCP the id is
// the assigned session id ("tcp-N"); for UDP it also carries the source endpoint
// from the datagram. Equality and hashing are BY ID ONLY
// — deliberate, so a client is the same client regardless of how its endpoint is
// observed. Maps in the transports are keyed by the id string directly.
class ClientAddress {
public:
    explicit ClientAddress(std::string id) : id_(std::move(id)) {}

    ClientAddress(std::string id, std::string host, std::uint16_t port)
        : id_(std::move(id)), host_(std::move(host)), port_(port) {}

    const std::string& id() const { return id_; }

    // The source endpoint (UDP) — nullopt for TCP clients identified by id only.
    bool hasEndpoint() const { return host_.has_value(); }
    const std::string& host() const { return *host_; }
    std::uint16_t port() const { return port_; }

    bool operator==(const ClientAddress& other) const { return id_ == other.id_; }
    bool operator!=(const ClientAddress& other) const { return id_ != other.id_; }

    std::string toString() const {
        if (host_.has_value()) {
            return "ClientAddress[" + id_ + " @ " + *host_ + ":" + std::to_string(port_) + "]";
        }
        return "ClientAddress[" + id_ + "]";
    }

private:
    std::string id_;
    std::optional<std::string> host_;
    std::uint16_t port_ = 0;
};

// A received audio packet paired with the sender's address — used by UDP server
// demux to attribute a datagram to a client.
class ReceivedPacket {
public:
    ReceivedPacket(AudioPacket packet, ClientAddress sender)
        : packet_(std::move(packet)), sender_(std::move(sender)) {}

    const AudioPacket& packet() const { return packet_; }
    const ClientAddress& sender() const { return sender_; }

private:
    AudioPacket packet_;
    ClientAddress sender_;
};

}  // namespace naudio::net

// Hash by id only, consistent with ClientAddress::operator== (equality by id) — lets ClientAddress
// be used directly as an unordered_map/set key when the id-string isn't used.
template <>
struct std::hash<naudio::net::ClientAddress> {
    std::size_t operator()(const naudio::net::ClientAddress& a) const {
        return std::hash<std::string>{}(a.id());
    }
};
