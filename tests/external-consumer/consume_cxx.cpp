/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — external consumer of the installed C++ API.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * External consumer of the INSTALLED naudio C++ API (installed-library check).
 *
 * Proves the installed C++ headers (the naudio/ tree) + the internal static
 * archives link via find_package(naudio):
 *   naudio::crc32                       -> naudio::naudio_core   (codec/reliability)
 *   naudio::net::Socket::resolveHostV4  -> naudio::naudio_net    (transport layer)
 *
 * The C++ API ships as the internal STATIC archives. Static-link resolution
 * ignores the hidden symbol
 * visibility the shared C ABI relies on, so these unexported C++ symbols are still
 * callable when an external TU links the installed archive — which is exactly what
 * this binary demonstrates.
 */
#include <naudio/Crc32.hpp>
#include <naudio/net/Socket.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

int main() {
    // naudio_core: CRC-32/ISO-HDLC. The KAT for "123456789" is 0xCBF43926 — a real
    // correctness check, not just a link probe.
    const std::uint8_t kat[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    const std::uint32_t crc = naudio::crc32(kat, sizeof(kat));
    if (crc != 0xCBF43926u) {
        std::fprintf(stderr, "consume_cxx FAIL: crc32 KAT = 0x%08X, expected 0xCBF43926\n", crc);
        return 1;
    }

    // naudio_net: resolve a literal IPv4 address (no network I/O) to force-link the
    // archive and prove the net headers + library are installed.
    const std::string ip = naudio::net::Socket::resolveHostV4("127.0.0.1");
    if (ip.empty()) {
        std::fprintf(stderr, "consume_cxx FAIL: resolveHostV4 returned empty\n");
        return 1;
    }

    std::printf("consume_cxx OK: naudio_core (crc32 KAT=0x%08X) + naudio_net (resolveHostV4=%s)"
                " C++ API linked from the installed prefix\n",
                crc, ip.c_str());
    return 0;
}
