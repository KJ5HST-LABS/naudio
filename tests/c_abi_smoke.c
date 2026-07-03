/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — C-ABI contract smoke.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * C-ABI contract smoke. Compiled as C and linked against the C++ naudio library:
 * it proves naudio.h is C-compilable and C-callable, and asserts:
 *   (1) the text-function contract — buf == NULL returns the needed length WITHOUT writing (this
 *       previously segfaulted), and a short buffer is NUL-terminated while the full length is
 *       still returned;
 *   (2) the na_error_t model — na_strerror is total, invalid-argument paths return NA_ERR_INVALID
 *       and set na_last_error(), and a successful call leaves na_last_error() == NA_OK;
 *   (3) the na_context lifecycle — create/enumerate/destroy on the real backend when one is
 *       available (tolerated-skip in a headless environment so CI without audio still passes).
 * Returns non-zero (failing the ctest) on any contract violation. The contract checks (1)+(2)
 * touch no hardware; (3) only exercises hardware paths when na_context_create() succeeds.
 */
#include <stdio.h>
#include <string.h>

#include "naudio.h"

int main(void) {
    /* ---- (1) text-function NULL-buffer / truncation contract ---- */

    /* buf == NULL must return the needed length without writing (the documented length probe). */
    const int len = na_install_instructions(NULL, 1000);
    if (len <= 0) {
        fprintf(stderr, "FAIL: NULL-buffer length probe returned %d\n", len);
        return 1;
    }
    /* A successful call must leave the thread's last-error cleared to NA_OK. */
    if (na_last_error() != NA_OK) {
        fprintf(stderr, "FAIL: na_last_error() != NA_OK after a successful call (%d)\n",
                na_last_error());
        return 1;
    }

    /* buf == NULL with len 0 returns the same length. */
    const int len0 = na_install_instructions(NULL, 0);
    if (len0 != len) {
        fprintf(stderr, "FAIL: NULL/0 length %d != %d\n", len0, len);
        return 1;
    }

    /* A short buffer is truncated to len-1 chars, NUL-terminated, and the FULL length returned. */
    char buf[16];
    memset(buf, 'X', sizeof buf);
    const int full = na_install_instructions(buf, (int)sizeof buf);
    if (full != len) {
        fprintf(stderr, "FAIL: truncated call returned %d, expected %d\n", full, len);
        return 1;
    }
    if (buf[sizeof buf - 1] != '\0' || strlen(buf) != sizeof buf - 1) {
        fprintf(stderr, "FAIL: short buffer not NUL-terminated at len-1\n");
        return 1;
    }

    /* ---- (2) na_error_t model ---- */

    /* na_strerror is total: every defined code AND an out-of-range code map to a non-NULL string. */
    const na_error_t codes[] = {NA_OK, NA_ERR_BACKEND, NA_ERR_INVALID, NA_ERR_DEVICE_UNAVAILABLE,
                                NA_ERR_INIT, NA_ERR_NOMEM, NA_ERR_UNSUPPORTED};
    for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; ++i) {
        if (na_strerror(codes[i]) == NULL) {
            fprintf(stderr, "FAIL: na_strerror(%d) returned NULL\n", codes[i]);
            return 1;
        }
    }
    if (na_strerror((na_error_t)(-999)) == NULL) {
        fprintf(stderr, "FAIL: na_strerror(unknown) returned NULL\n");
        return 1;
    }

    /* NULL-context argument paths must return NA_ERR_INVALID and record it on the thread. */
    na_device devs[8];
    if (na_enumerate(NULL, devs, 8) != NA_ERR_INVALID || na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_enumerate(NULL,...) ret=%d last=%d (want NA_ERR_INVALID)\n",
                na_enumerate(NULL, devs, 8), na_last_error());
        return 1;
    }
    if (na_probe_format(NULL, 0, 48000, 16, 2, 1) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_probe_format(NULL,...) did not return NA_ERR_INVALID\n");
        return 1;
    }
    if (na_diagnostic_report(NULL, NULL, 0) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_diagnostic_report(NULL,...) did not return NA_ERR_INVALID\n");
        return 1;
    }
    /* Handle-returning calls signal failure with NULL + na_last_error(). */
    if (na_open_capture(NULL, 0, 48000, 16, 2, NULL) != NULL ||
        na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_open_capture(NULL,...) not NULL/NA_ERR_INVALID\n");
        return 1;
    }
    if (na_open_playback(NULL, 0, 48000, 16, 2) != NULL || na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_open_playback(NULL,...) not NULL/NA_ERR_INVALID\n");
        return 1;
    }

    /* ---- (3) na_context lifecycle (real backend when present) ---- */

    na_context* ctx = na_context_create();
    if (ctx == NULL) {
        /* Headless / no audio backend: tolerated. The contract checks above already passed. */
        printf("c_abi_smoke OK (no audio backend; context path skipped, error=%s, len=%d)\n",
               na_strerror(na_last_error()), len);
        return 0;
    }
    /* Enumerate must succeed (>= 0; zero devices is fine) and leave last-error NA_OK. */
    const int n = na_enumerate(ctx, devs, 8);
    if (n < 0) {
        fprintf(stderr, "FAIL: na_enumerate(ctx,...) returned %d (%s)\n", n,
                na_strerror(na_last_error()));
        na_context_destroy(ctx);
        return 1;
    }
    if (na_last_error() != NA_OK) {
        fprintf(stderr, "FAIL: na_last_error() != NA_OK after successful enumerate\n");
        na_context_destroy(ctx);
        return 1;
    }
    na_context_destroy(ctx);  /* must not crash; Pa_Terminate balances the create's Pa_Initialize */
    na_context_destroy(NULL); /* safe on NULL */

    printf("c_abi_smoke OK (install-instructions length=%d, %d device(s) enumerated)\n", len, n);
    return 0;
}
