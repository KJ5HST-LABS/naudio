/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — C-ABI contract smoke.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * C-ABI contract smoke. Compiled as C and linked against the C++ naudio library:
 * it proves naudio.h is C-compilable and C-callable, and asserts:
 *   (0) the library-version contract — the na_version_* accessors are exported and C-callable, the
 *       loaded library agrees with the header it was built from, and both accessors are infallible
 *       (they leave a NON-OK na_last_error() untouched, unlike every fallible call);
 *   (1) the text-function contract — buf == NULL returns the needed length WITHOUT writing (this
 *       previously segfaulted), and a short buffer is NUL-terminated while the full length is
 *       still returned;
 *   (2) the na_error_t model — na_strerror is total, invalid-argument paths return NA_ERR_INVALID
 *       and set na_last_error(), and a successful call leaves na_last_error() == NA_OK;
 *   (3) the na_context lifecycle — create/enumerate/destroy on the real backend.
 * Returns non-zero (failing the ctest) on any contract violation. The contract checks (1)+(2)
 * touch no hardware; (3) only exercises hardware paths when na_context_create() succeeds.
 *
 * TWO SILENT-EVIDENCE GAPS WERE CLOSED HERE (issue #88 item 3, from #60). Both had the same root:
 * this test reported its own degradation on stdout, and ctest is invoked everywhere with
 * `--output-on-failure`, which suppresses the output of PASSING tests. So the discriminating line
 * appeared in no CI log, ever — the report existed and nothing could read it.
 *
 *   (a) A NULL na_context_create() used to print and `return 0`. src/naudio_c_api.cpp maps ANY
 *       Pa_Initialize/backend-ctor failure to NULL, so a FetchContent PortAudio that compiled but
 *       could not initialise took the identical green path as a healthy one. It now FAILS. An
 *       environment that genuinely has no initialisable backend sets NAUDIO_ALLOW_NO_AUDIO_BACKEND,
 *       which downgrades it to a ctest SKIP — deliberately not back to a silent pass, so the
 *       concession is visible in the summary of every run that takes it.
 *   (b) The stride arm is VACUOUS below 2 devices — with fewer, every stride collapses onto element
 *       0 and the placement check cannot fail — and ubuntu CI was measured at 0 devices. The
 *       precondition is now asserted by the separate `naudio_c_abi_stride` ctest arm
 *       (`--require-stride`), which SKIPS rather than passing when it cannot be met. A skip is
 *       reported in the ctest summary whether or not the run failed, which is exactly the property
 *       the printf lacked.
 *
 * Exit codes: 0 pass, 1 contract violation, 77 skipped (ctest SKIP_RETURN_CODE, the autotools
 * convention). 77 must stay in step with the SKIP_RETURN_CODE property in the root CMakeLists.txt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "naudio.h"

/* NA_VERSION_ENCODE must be MONOTONIC, or every `>=` a consumer writes against it is wrong at a
 * carry boundary — and that is the one comparison the whole accessor exists to support. Both are
 * compile-time facts about the macro, so they are asserted at compile time rather than run time.
 *
 * The witnesses are NA_VERSION_MAX_COMPONENT rather than a spelled number ON PURPOSE. A witness
 * chosen by hand tests the number you picked, not the bound the macro documents: `patch = 99` here
 * survived scaling minor by 100 (199 < 200) and reported the encoding monotonic when it was not.
 * Derived from the bound, the same mutation fails — 1*100 + 999 > 2*100. */
_Static_assert(NA_VERSION_ENCODE(0, 1, NA_VERSION_MAX_COMPONENT) < NA_VERSION_ENCODE(0, 2, 0),
               "NA_VERSION_ENCODE: patch must not carry into minor");
_Static_assert(NA_VERSION_ENCODE(0, NA_VERSION_MAX_COMPONENT, NA_VERSION_MAX_COMPONENT)
                   < NA_VERSION_ENCODE(1, 0, 0),
               "NA_VERSION_ENCODE: minor.patch must not carry into major");

/* ctest's SKIP_RETURN_CODE. Kept as a named constant because it appears in three places here and
 * once in CMakeLists.txt, and a bare 77 in any of them reads as a magic number. */
#define NA_SKIP_RC 77

int main(int argc, char** argv) {
    /* --require-stride runs the identical body and changes only the EXIT-CODE POLICY at the stride
     * arm: it turns "fewer than 2 devices, so the placement check proved nothing" into a ctest SKIP
     * instead of a pass. It is a second registered arm rather than a flag on the first because the
     * contract checks below are device-independent and must keep their full weight on a machine
     * with no devices — reporting the whole test as skipped there would throw away real coverage
     * to report the loss of some. The run costs ~0.03 s, so doing it twice is free. */
    const int require_stride = (argc > 1 && strcmp(argv[1], "--require-stride") == 0);

    /* ---- (0) library version ----
     * These run FIRST, before any context exists, because that is part of the contract: both
     * accessors are infallible and callable before na_context_create. */

    const char* const vstr = na_version_string();
    if (vstr == NULL || vstr[0] == '\0') {
        fprintf(stderr, "FAIL: na_version_string() returned %s\n", vstr ? "an empty string" : "NULL");
        return 1;
    }
    /* In-tree the header and the library are the same build, so these MUST agree. Cross-version
     * skew is the case this cannot reach from one build (that is #32's separately-compiled
     * consumer gate); what this proves is that the accessors report the header they were compiled
     * from rather than a hand-maintained constant that drifted. */
    if (na_version_number() != NAUDIO_VERSION_NUMBER) {
        fprintf(stderr, "FAIL: na_version_number()=%d != NAUDIO_VERSION_NUMBER=%d\n",
                na_version_number(), NAUDIO_VERSION_NUMBER);
        return 1;
    }
    /* Its own arm, not folded into the one above: the string is derived from the three macros by
     * stringification and the number by arithmetic, so they fail independently. */
    if (strcmp(vstr, NAUDIO_VERSION_STRING) != 0) {
        fprintf(stderr, "FAIL: na_version_string()=\"%s\" != NAUDIO_VERSION_STRING=\"%s\"\n",
                vstr, NAUDIO_VERSION_STRING);
        return 1;
    }

    /* The pre-release tag is what separates a release from the builds leading up to it, and it
     * is the half of the version a consumer CAN rely on for that: v1.0.0rc5 and the main branch
     * that added na_discover both encode to 1000000, so the NUMBER cannot tell them apart and
     * the STRING must. Two properties, checked separately because they fail for different
     * reasons.
     *
     * (a) Whatever the tag is, the string starts with the bare M.N.P — a tag must be a SUFFIX,
     *     never a replacement, or a consumer parsing the version loses the release it belongs to. */
    {
        char bare[64];
        snprintf(bare, sizeof bare, "%d.%d.%d",
                 NAUDIO_VERSION_MAJOR, NAUDIO_VERSION_MINOR, NAUDIO_VERSION_PATCH);
        if (strncmp(vstr, bare, strlen(bare)) != 0) {
            fprintf(stderr, "FAIL: na_version_string()=\"%s\" does not start with \"%s\"\n",
                    vstr, bare);
            return 1;
        }
        /* (b) The tag is empty (a release) or begins with '-' (SemVer). A tag missing its dash
         *     would make "1.0.0rc5" sort and read as a different release entirely. */
        if (NAUDIO_VERSION_PRERELEASE[0] != '\0' && NAUDIO_VERSION_PRERELEASE[0] != '-') {
            fprintf(stderr, "FAIL: NAUDIO_VERSION_PRERELEASE=\"%s\" must be empty or start with '-'\n",
                    NAUDIO_VERSION_PRERELEASE);
            return 1;
        }
        /* (c) And the string is exactly the concatenation — so a tag can never go missing from
         *     na_version_string() while remaining defined in the header. */
        if (strlen(vstr) != strlen(bare) + strlen(NAUDIO_VERSION_PRERELEASE)) {
            fprintf(stderr, "FAIL: na_version_string()=\"%s\" is not \"%s\" + \"%s\"\n",
                    vstr, bare, NAUDIO_VERSION_PRERELEASE);
            return 1;
        }
        printf("c_abi_smoke: version %s (pre-release tag \"%s\")\n", vstr, NAUDIO_VERSION_PRERELEASE);
    }

    /* Infallible means they do NOT touch the thread's last-error. Establish a NON-OK state first:
     * asserted against NA_OK this would pass even if the accessors cleared it, which is the
     * reading that makes such an assertion worthless. */
    (void)na_probe_format(NULL, 0, 48000, 16, 2, 1); /* a known NA_ERR_INVALID path, no ctx needed */
    if (na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: could not establish a non-OK last-error for the version arm\n");
        return 1;
    }
    (void)na_version_number();
    (void)na_version_string();
    if (na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_version_* cleared na_last_error() (now %d) -- they are "
                        "infallible and must leave it alone\n", na_last_error());
        return 1;
    }

    /* ---- (1) text-function NULL-buffer / truncation contract ---- */

    /* buf == NULL must return the needed length without writing (the documented length probe).
     * NOTE this now runs with last-error at NA_ERR_INVALID from arm (0), which STRENGTHENS the
     * NA_OK assertion below: it can no longer pass by inheriting an already-clear state. */
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
    /* A VALID struct_size here on purpose: na_enumerate now rejects a short one too, so passing a
     * bad size would let that guard satisfy this arm and the NULL check could rot undetected. The
     * fault this arm names must be the only invalid thing present. */
    if (na_enumerate(NULL, devs, 8, sizeof devs[0]) != NA_ERR_INVALID ||
        na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_enumerate(NULL,...) ret=%d last=%d (want NA_ERR_INVALID)\n",
                na_enumerate(NULL, devs, 8, sizeof devs[0]), na_last_error());
        return 1;
    }
    /* The struct_size floor gets its own arm below, once a real ctx exists -- testing it here
     * with ctx == NULL would let the NULL guard satisfy it, which is the trap this arm's own
     * comment above describes, pointed the other way. */
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
        /* FAIL-CLOSED (was `return 0`). na_context_create returns NULL for a genuinely headless
         * box AND for a PortAudio that built but cannot initialise — src/naudio_c_api.cpp funnels
         * every Pa_Initialize/backend-ctor throw into the same NULL — and the second is a defect
         * this suite otherwise reports as success. The evidence that the first case is rare rather
         * than routine: #60 measured ubuntu CI at 0 DEVICES, which it could only do by having
         * enumerated, which requires a context. A backend that initialises and offers nothing is
         * the normal headless shape; a backend that will not initialise at all is not.
         *
         * An environment that really cannot initialise one declares it, and gets a SKIP rather than
         * a pass: the concession then shows up in the ctest summary of every run that takes it,
         * which is the whole complaint against the line this replaces. */
        if (getenv("NAUDIO_ALLOW_NO_AUDIO_BACKEND") != NULL) {
            printf("c_abi_smoke SKIP (no audio backend, allowed by NAUDIO_ALLOW_NO_AUDIO_BACKEND; "
                   "error=%s, len=%d)\n",
                   na_strerror(na_last_error()), len);
            return NA_SKIP_RC;
        }
        fprintf(stderr,
                "FAIL: na_context_create() returned NULL (error=%s).\n"
                "  PortAudio did not initialise, so nothing below this line ran — including the\n"
                "  only hardware-free proof in the suite that the linked PortAudio WORKS rather\n"
                "  than merely having compiled.\n"
                "  If this environment genuinely has no initialisable audio backend, set\n"
                "  NAUDIO_ALLOW_NO_AUDIO_BACKEND=1 for it: the arm then reports SKIPPED, which is\n"
                "  visible in the ctest summary, instead of passing silently as it used to.\n",
                na_strerror(na_last_error()));
        return 1;
    }
    /* The struct_size floor, with ctx/out/max all VALID so only the size can reject. Zero is the
     * case that matters: it is what a caller who forgets the parameter's meaning passes. */
    if (na_enumerate(ctx, devs, 8, 0) != NA_ERR_INVALID || na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_enumerate(..., struct_size=0) did not return NA_ERR_INVALID\n");
        na_context_destroy(ctx);
        return 1;
    }
    if (na_enumerate(ctx, devs, 8, NA_DEVICE_SIZE_V1 - 1) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_enumerate(..., struct_size below V1) not NA_ERR_INVALID\n");
        na_context_destroy(ctx);
        return 1;
    }
    /* Enumerate must succeed (>= 0; zero devices is fine) and leave last-error NA_OK. */
    const int n = na_enumerate(ctx, devs, 8, sizeof devs[0]);
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

    /* ---- (3b) the caller's stride, which is the whole point of struct_size ----
     * Stand in for a consumer compiled against a LATER header: elements are sizeof(na_device)+PAD
     * apart. The library must place element k at k*(sizeof+PAD) -- its OWN sizeof is the wrong
     * stride and would pack the elements, mis-parsing every one after the first. Backed by 0xA5
     * so "written" and "untouched" are distinguishable, exactly as the na_client_stats arm does. */
#define PAD 16
    const size_t elem = sizeof(na_device) + PAD;
    /* _Alignas so the library writes through a properly-aligned na_device*, as a real caller's
     * array would be -- a bare unsigned char[] is only byte-aligned. */
    _Alignas(na_device) unsigned char wide[8 * (sizeof(na_device) + PAD)];
    memset(wide, 0xA5, sizeof wide);
    const int m = na_enumerate(ctx, (na_device*)wide, 8, elem);
    if (m < 0) {
        fprintf(stderr, "FAIL: na_enumerate rejected a struct_size LARGER than its own (%d)\n", m);
        na_context_destroy(ctx);
        return 1;
    }
    /* PLACEMENT first, in its own pass, and the TAIL second in another. The two must not share a
     * pass: a wrong stride also tramples the pads (element k+1 lands on element k's tail), so a
     * combined loop reports the tail failure and the placement assertion below is never evaluated
     * -- it would sit here dead, passing every mutation. Separated, each is provable on its own. */
    for (int i = 0; i < m; i++) {
        const na_device* slot = (const na_device*)(wide + (size_t)i * elem);
        /* The element must be AT the caller's stride. A library striding by its own sizeof leaves
         * this offset holding some other element's interior (or untouched 0xA5). */
        if (slot->backend_id != devs[i].backend_id) {
            fprintf(stderr, "FAIL: element %d not at the caller's stride (backend_id %d != %d)\n",
                    i, slot->backend_id, devs[i].backend_id);
            na_context_destroy(ctx);
            return 1;
        }
    }
    for (int i = 0; i < m; i++) {
        const unsigned char* slot = wide + (size_t)i * elem;
        /* The declared tail past the library's own struct must be ZERO-filled, not left 0xA5. */
        for (size_t b = sizeof(na_device); b < elem; b++) {
            if (slot[b] != 0x00) {
                fprintf(stderr, "FAIL: element %d's declared tail not zero-filled at +%zu\n", i, b);
                na_context_destroy(ctx);
                return 1;
            }
        }
    }
    /* Nothing past the elements it reported writing. */
    for (size_t b = (size_t)m * elem; b < sizeof wide; b++) {
        if (wide[b] != 0xA5) {
            fprintf(stderr, "FAIL: na_enumerate wrote past element %d at +%zu\n", m, b);
            na_context_destroy(ctx);
            return 1;
        }
    }
    /* Say which half ran. With fewer than 2 devices every stride collapses onto element 0, so the
     * placement check above is vacuous and only the tail/overrun checks carry weight. */
    printf("c_abi_smoke: stride arm %s (%d device record(s), elem=%zu, sizeof=%zu)\n",
           m >= 2 ? "EXERCISED" : "NOT exercised -- needs 2+ devices", m, elem, sizeof(na_device));
    /* ...and under --require-stride, ASSERT that precondition instead of narrating it. This printf
     * has been correct since the arm was written and has never been readable: ctest runs with
     * --output-on-failure everywhere, which prints nothing for a passing test. A SKIP is carried in
     * the test RESULT, so it survives that. */
    if (require_stride && m < 2) {
        printf("c_abi_smoke SKIP (--require-stride: %d device record(s), need 2+ before the "
               "placement check can distinguish the caller's stride from the library's)\n",
               m);
        na_context_destroy(ctx);
        return NA_SKIP_RC;
    }
#undef PAD

    na_context_destroy(ctx);  /* must not crash; Pa_Terminate balances the create's Pa_Initialize */
    na_context_destroy(NULL); /* safe on NULL */

    printf("c_abi_smoke OK (install-instructions length=%d, %d device(s) enumerated)\n", len, n);
    return 0;
}
