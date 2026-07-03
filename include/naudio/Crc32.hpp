// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — wire codec primitives.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace naudio {

// CRC-32/ISO-HDLC — the exact algorithm zlib and standard CRC-32 libraries
// compute (reflected poly 0xEDB88320, init 0xFFFFFFFF,
// reflected in/out, final XOR 0xFFFFFFFF). The AudioPacket frame CRC covers the
// header + payload (NOT the trailing CRC field), big-endian on the wire.
//
// Implemented in-tree (no third-party dependency) to honour the offline-build
// constraint. The KAT for "123456789" is 0xCBF43926;
// the conformance `crc-kat-check` vector pins it to Python's zlib.crc32.
//
// Compiled into naudio_core (the lookup table + the loop live in Crc32.cpp), so
// the algorithm is built once rather than re-emitted into every including TU.

// Computes the ISO-HDLC CRC-32 over `len` bytes at `data`.
std::uint32_t crc32(const std::uint8_t* data, std::size_t len);

// Convenience overload over a byte vector.
std::uint32_t crc32(const std::vector<std::uint8_t>& data);

}  // namespace naudio
