// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — transport value type.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <optional>
#include <string>

namespace naudio {

// Transport protocol type for audio streaming.
enum class TransportType {
    Tcp,   // Reliable, ordered delivery. Default.
    Udp,   // Lower latency, no head-of-line blocking.
    Dual,  // Accepts both TCP and UDP clients simultaneously.
};

// The enum constant name ("TCP", "UDP", "DUAL").
inline const char* transportTypeName(TransportType t) {
    switch (t) {
        case TransportType::Tcp: return "TCP";
        case TransportType::Udp: return "UDP";
        case TransportType::Dual: return "DUAL";
    }
    return "UNKNOWN";
}

// Resolves a variant by its constant name (nullopt on an unknown name).
inline std::optional<TransportType> transportTypeFromName(const std::string& name) {
    if (name == "TCP") return TransportType::Tcp;
    if (name == "UDP") return TransportType::Udp;
    if (name == "DUAL") return TransportType::Dual;
    return std::nullopt;
}

}  // namespace naudio
