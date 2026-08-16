/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — version-skew consumer: the LONG-caller direction.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * The sibling of consume_skew.c, for the other half of the promise in
 * README.md § "Binary compatibility":
 *
 *   consume_skew.c    library NEWER than caller -> writes only the caller's prefix
 *   consume_zerofill.c  caller declares MORE than the library knows -> the library
 *                       ZERO-fills the remainder, so the tail is defined rather
 *                       than indeterminate
 *
 * This one compiles against the CURRENT installed header on purpose. A consumer
 * built against a *future* header is not something a test can produce — there is
 * no future library to link — but the behaviour it depends on is exactly what a
 * caller declaring a larger size observes, and that is expressible today: declare
 * more than the library knows and require the excess to come back zeroed.
 *
 * Still a separately-compiled consumer of the INSTALLED library, which is what
 * distinguishes this from the in-tree arms in tests/c_abi_smoke.c.
 */
#include <naudio.h>

#include <stdio.h>
#include <string.h>

#define GUARD 0xA5u

/* Slack the caller DECLARES (must come back zero-filled) and slack it merely
 * owns but does not declare (must come back untouched). Two distinct regions —
 * conflating them is how a zero-fill bug hides behind an overrun check. */
#define DECLARED_SLACK 128u
#define UNDECLARED_SLACK 128u

union guarded_stats {
    na_client_stats st;
    unsigned char raw[sizeof(na_client_stats) + DECLARED_SLACK + UNDECLARED_SLACK];
};

int main(void) {
    na_stream_client* cli =
        na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", 4533, "zerofill-consumer");
    if (cli == NULL) {
        fprintf(stderr, "consume_zerofill FAIL: na_client_create returned NULL (na_last_error=%d)\n",
                (int)na_last_error());
        return 1;
    }

    union guarded_stats b;
    memset(b.raw, GUARD, sizeof b.raw);

    /* Stand in for a consumer compiled against a LATER header: it believes the
     * struct is bigger than this library's own sizeof. */
    const size_t known = sizeof(na_client_stats);
    const size_t declared = known + DECLARED_SLACK;

    if (na_client_get_stats(cli, &b.st, declared) != NA_OK) {
        fprintf(stderr,
                "consume_zerofill FAIL: na_client_get_stats rejected a struct_size LARGER than its "
                "own (%zu vs %zu) — a newer consumer must be accepted, not refused "
                "(na_last_error=%d)\n",
                declared, known, (int)na_last_error());
        na_client_destroy(cli);
        return 1;
    }

    /* The declared-but-unknown region must be ZERO, not left as the caller's
     * fill. This is the half of the promise that makes an older library's answer
     * *defined* for a newer consumer. */
    for (size_t i = known; i < declared; ++i) {
        if (b.raw[i] != 0) {
            fprintf(stderr,
                    "consume_zerofill FAIL: declared-but-unknown byte %zu (offset %zu past the "
                    "library's own %zu-byte struct) is 0x%02X, expected 0x00. The library left the "
                    "caller's declared tail INDETERMINATE instead of zero-filling it.\n",
                    i, i - known, known, b.raw[i]);
            na_client_destroy(cli);
            return 1;
        }
    }

    /* ...and it must stop at the size the caller declared. */
    for (size_t i = declared; i < sizeof b.raw; ++i) {
        if (b.raw[i] != GUARD) {
            fprintf(stderr,
                    "consume_zerofill FAIL: the library wrote past the DECLARED size — byte %zu "
                    "(offset %zu past a declared %zu) is 0x%02X, expected 0x%02X\n",
                    i, i - declared, declared, b.raw[i], GUARD);
            na_client_destroy(cli);
            return 1;
        }
    }

    na_client_destroy(cli);
    printf("consume_zerofill OK: declared %zu vs library's %zu; %u byte(s) zero-filled, %u "
           "byte(s) past the declaration untouched\n",
           declared, known, DECLARED_SLACK, UNDECLARED_SLACK);
    return 0;
}
