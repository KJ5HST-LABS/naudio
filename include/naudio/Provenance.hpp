// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — reliability algorithms.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

namespace naudio {

// How a packet reached the consumer: straight off the wire, or reconstructed by
// the FEC parity layer.
//
// This is NOT a wire field. It is never serialized, never appears in AudioPacket,
// and adds nothing to the frozen 0xAF01 frame — it is out-of-band metadata that
// travels alongside a packet through the receive pipeline so a consumer can tell
// a delivered frame apart from a repaired one.
//
// WHY A SCOPED ENUM AND NOT `bool recovered` (issue #65):
// this project compiles with no warning flags at all — the real line is
// `-g -std=c++17 -arch arm64 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden`,
// and a tree-wide grep for -Wall/-Wextra/-Werror returns nothing. A `bool` with a
// default member initializer therefore widens an aggregate silently: every
// positional factory keeps compiling, keeps reporting false, and no configuration
// available to this project makes that loud (measured — it is clean even under
// -Wall -Wextra, because the default initializer suppresses
// -Wmissing-field-initializers). A scoped enum with no default turns the same
// transposition into a hard error, so the TYPE is the detector. Do not give this
// enum a default value anywhere it is carried, and do not replace it with a bool.
//
// Consumers must treat an unrecognized value as Live: Live is the safe reading,
// because it is what every path did before provenance existed.
enum class Provenance {
    // Delivered as received — the packet arrived on the wire and was emitted on
    // arrival. Everything that predates issue #65 is this.
    Live,
    // Reconstructed by FEC from a parity frame and its surviving block members.
    // The packet never arrived; its payload is the XOR remainder. It is byte-exact
    // only for uniform-length blocks (see FecDecoder's class comment).
    Recovered,
};

}  // namespace naudio
