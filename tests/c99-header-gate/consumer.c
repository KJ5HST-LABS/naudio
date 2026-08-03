/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio — the C99 consumer gate's translation unit.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * CMakeLists.txt and CONTRIBUTING.md both promise that `include/naudio.h` is C99-clean, so a
 * consumer of the C ABI is NOT obliged to build at C11 the way this project's own C sources do.
 * Nothing re-checked that: every other gate compiling C against this header does so at C11 or
 * higher (tests/external-consumer/CMakeLists.txt:8, tests/ffi-clients/run.sh:96,
 * tests/readme-snippets/check.sh:143, and the main build). Issue #45.
 *
 * check.cmake compiles this file at `-std=c99 -pedantic-errors -fsyntax-only`. THE `#include` IS
 * THE LOAD-BEARING LINE: it parses every declaration in the header, which is the whole of the
 * conformance claim. The usage below is deliberately small and confined to symbols frozen by the
 * binary-compatibility contract — it proves the declarations are usable and not merely parseable,
 * without becoming a second, weaker copy of naudio_c_abi_smoke (which covers C-callability
 * properly, at C11, and links). Do not grow it to chase coverage; a broad TU here would break on
 * legitimate ABI additions while adding nothing to what the include already checks.
 *
 * Nothing is executed and nothing is linked: -fsyntax-only stops at the front end, which is
 * sufficient because the question is what the HEADER's own constructs demand of a consumer's
 * language level. Keep this file free of POSIX and of anything past C99 — a C11-ism HERE would
 * fail the gate and read exactly like a C11-ism in the header.
 */
#include <naudio.h>

#include <stddef.h>

/* Never called. Present so the header's declarations are exercised as declarations. */
static int na_c99_consumer(void)
{
    na_context* ctx;
    na_device dev;
    na_client_stats stats;
    na_client_callbacks cbs;
    na_transport transport = NA_TRANSPORT_UDP;
    na_reliability_profile profile = NA_RELIABILITY_DEFAULT;
    const char* version;
    int n;

    ctx = na_context_create();
    if (ctx == NULL) {
        return -1;
    }

    /* The two structs the library WRITES take their size as an explicit parameter; the one it
     * READS carries it in-band. Both idioms have to be expressible at C99. */
    n = na_enumerate(ctx, &dev, 1, sizeof dev);
    stats.connected = 0;
    cbs.struct_size = sizeof cbs;

    version = na_version_string();
    na_context_destroy(ctx);

    return (version != NULL && n >= 0 && transport == NA_TRANSPORT_UDP
            && profile == NA_RELIABILITY_DEFAULT && stats.connected == 0
            && cbs.struct_size >= NA_CLIENT_CALLBACKS_SIZE_V1)
               ? 0
               : -1;
}

/* Silence "defined but not used" without needing a linker: take its address. */
int (*na_c99_consumer_ref)(void) = na_c99_consumer;
