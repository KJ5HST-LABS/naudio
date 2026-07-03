// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — wire codec primitives.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/Crc32.hpp"

#include <array>

namespace naudio {
namespace {

// 256-entry lookup table, built once on first use (thread-safe static init).
// File-local: an internal helper, not part of the public surface.
const std::array<std::uint32_t, 256>& crc32Table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t len) {
    const std::array<std::uint32_t, 256>& kTable = crc32Table();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc = kTable[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t crc32(const std::vector<std::uint8_t>& data) {
    return crc32(data.data(), data.size());
}

}  // namespace naudio
