/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — version-skew consumer: OLD header, CURRENT library.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * This translation unit is compiled against a PINNED OLDER <naudio.h> and linked
 * against the CURRENTLY installed shared library. That combination is the whole
 * point: every other test in this repo compiles against the same header as the
 * library it links, so none of them can observe the one hazard `struct_size`
 * exists to prevent — a newer library writing past the end of a struct that an
 * already-compiled consumer allocated.
 *
 * `sizeof(na_client_stats)` below is therefore the size AS THE OLD HEADER
 * DECLARES IT, baked into this object file at compile time. The library cannot
 * see it and must honour the size passed as a parameter.
 *
 * Hardware-free: NULL backend, no PortAudio, no connect. na_client_get_stats is
 * documented as safe before connect (it yields connected == 0), which is all
 * this needs — the assertion is about MEMORY, not about counters.
 */
#include <naudio.h>

#include <stdio.h>
#include <string.h>

/* Fill byte for memory the library must not touch. Chosen to be neither 0 (which
 * a zero-fill would legitimately produce) nor 0xFF (this ABI's "not measured"). */
#define GUARD 0xA5u

/* Bytes of guarded slack past the caller's struct. Must comfortably exceed any
 * plausible single-release append so an overrun lands inside it rather than past. */
#define TAIL 256u

/* Alignment matters: na_client_stats holds long long / double, so the buffer has
 * to be at least as aligned as the struct. A union gets that for free and keeps
 * the raw view legal to read byte-wise. */
union guarded_stats {
    na_client_stats st;
    unsigned char raw[sizeof(na_client_stats) + TAIL];
};

int main(void) {
    /* ---- Is the skew REAL? -------------------------------------------------
     * A gate that silently degrades to "old header == new header" passes while
     * testing nothing, which is this repo's recorded failure mode (Learning 9).
     * The header's version is a compile-time constant baked in from the PIN; the
     * library's comes from the .so actually loaded. Report both, always, and let
     * the harness decide — this program never silently claims coverage it lacks. */
    const unsigned long hdr_v = (unsigned long)NAUDIO_VERSION_NUMBER;
    const unsigned long lib_v = (unsigned long)na_version_number();

    printf("consume_skew: pinned-header version=%lu  linked-library version=%lu  %s\n", hdr_v,
           lib_v,
           lib_v > hdr_v    ? "SKEW REAL (library is newer)"
           : lib_v == hdr_v ? "NO SKEW (same version) — this arm proves nothing today"
                            : "INVERTED (library is OLDER than the header)");

    if (lib_v < hdr_v) {
        fprintf(stderr, "consume_skew FAIL: linked a library OLDER than the pinned header; "
                        "the harness pinned something wrong\n");
        return 1;
    }

    na_stream_client* cli =
        na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", 4533, "skew-consumer");
    if (cli == NULL) {
        fprintf(stderr, "consume_skew FAIL: na_client_create returned NULL (na_last_error=%d)\n",
                (int)na_last_error());
        return 1;
    }

    union guarded_stats b;
    memset(b.raw, GUARD, sizeof b.raw);

    /* The declared size is this TU's own sizeof — exactly what a real consumer
     * compiled against the pinned header would pass. */
    const size_t declared = sizeof(na_client_stats);

    if (na_client_get_stats(cli, &b.st, declared) != NA_OK) {
        fprintf(stderr,
                "consume_skew FAIL: na_client_get_stats rejected a struct_size of %zu, which is "
                "sizeof(na_client_stats) as the PINNED header declares it (na_last_error=%d)\n",
                declared, (int)na_last_error());
        na_client_destroy(cli);
        return 1;
    }

    /* ---- Positive control: the call must have WRITTEN something -------------
     * Without this, a library that did nothing at all would sail through the
     * overrun check below — an untouched buffer trivially has an untouched tail.
     * An absence is only a measurement once a presence has been shown. */
    size_t written = 0;
    for (size_t i = 0; i < declared; ++i) {
        if (b.raw[i] != GUARD) {
            written++;
        }
    }
    if (written == 0) {
        fprintf(stderr, "consume_skew FAIL: na_client_get_stats returned NA_OK but wrote NOTHING "
                        "into the caller's %zu-byte struct — the overrun check below would have "
                        "passed vacuously\n",
                declared);
        na_client_destroy(cli);
        return 1;
    }

    /* ---- THE ASSERTION -----------------------------------------------------
     * Every byte at or past the caller's declared size must be exactly as the
     * caller left it. A library that strides by its OWN sizeof — i.e. that
     * ignores struct_size — writes the fields appended after this header was
     * frozen, and lands here. */
    for (size_t i = declared; i < sizeof b.raw; ++i) {
        if (b.raw[i] != GUARD) {
            fprintf(stderr,
                    "consume_skew FAIL: the library wrote past the caller's declared size. "
                    "byte %zu (offset %zu past a declared %zu) is 0x%02X, expected 0x%02X.\n"
                    "This is precisely the binary-compatibility break struct_size exists to "
                    "prevent: a consumer compiled against the pinned header allocated %zu bytes "
                    "and the library scribbled beyond them.\n",
                    i, i - declared, declared, b.raw[i], GUARD, declared);
            na_client_destroy(cli);
            return 1;
        }
    }

    /* A field the pinned header DOES know must still read sensibly — proof the
     * prefix carries real data rather than merely having been touched. */
    if (b.st.connected != 0) {
        fprintf(stderr, "consume_skew FAIL: a never-connected client reports connected=%d\n",
                (int)b.st.connected);
        na_client_destroy(cli);
        return 1;
    }

    na_client_destroy(cli);
    printf("consume_skew OK: library honoured a %zu-byte declared size; %zu byte(s) written in "
           "the prefix, %u guarded byte(s) past it untouched\n",
           declared, written, TAIL);
    return 0;
}
