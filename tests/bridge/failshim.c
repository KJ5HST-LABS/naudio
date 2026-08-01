/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — failshim: force rig_stream_read / rig_stream_write to fail, from outside.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * A preloaded interposition library that sits between na_hamlib_bridge and libhamlib and makes the
 * two stream calls misbehave on demand. It exists because the faults issues #5 and #6 describe
 * CANNOT be provoked from outside the bridge's process: the Hamlib dummy has no error-injection
 * knob, and it routes reads through its own caps->stream_read hook, so libhamlib's own error and
 * `closing` paths are unreachable however the backend is configured. Without this file, #5 and #6
 * are verifiable only by hand-building a shim in a scratchpad — which six sessions did, and which
 * evaporated six times (issue #15, then #35).
 *
 * WHAT IT IS NOT. It does not test naudio. It is a fixture for the bridge's ERROR paths, and every
 * assertion made with it is made against the bridge's own observable behaviour — its exit status,
 * its stderr, and the TX loss counters it already prints. Nothing here is asserted on directly:
 * a probe that reports its own opinion of a fault is testing itself (Learning 43).
 *
 * TWO MARKERS, AND THEY PROVE DIFFERENT THINGS (issue #39).
 *
 *   `na_failshim: armed ...`        printed from the constructor — the library LOADED
 *   `na_failshim: interposed <fn>`  printed from inside shim_read / shim_write — that call BOUND
 *
 * The first is load-bearing because a shim that fails to load produces EXACTLY the output of a
 * healthy run: no fault, no error, no diagnostic. "The bridge did not report a short write" and
 * "interposition never happened" are otherwise the same observation, and the second one silently
 * passes the control arm (CLAUDE.md Learning 63).
 *
 * The first is also NOT ENOUGH, which is why the second exists. Loading and binding are different
 * failures with the same symptom: the ELF visibility trap below left this file loaded, its
 * constructor printing `armed` on every run, and both interposers unreachable dead code — so the
 * arms failed as though the BRIDGE had regressed and reported issues #5 and #6 as live bugs. A
 * marker emitted from inside the interposed call cannot be printed by a shim that never bound, so
 * it separates the two and lets the driver report a harness fault instead of blaming the bridge.
 * It is emitted on the PASS-THROUGH path as well as the failing one, because the control arm arms
 * no fault at all and would otherwise have nothing from inside the call to grep.
 *
 * macOS SIP TRAP (CLAUDE.md Learning 6): DYLD_INSERT_LIBRARIES is stripped from the environment the
 * instant a SIP-protected binary is exec'd — /usr/bin/timeout, /bin/zsh and /usr/bin/perl included.
 * The driver script therefore launches the bridge DIRECTLY, never through a wrapper. Setting the
 * variable as a prefix on the bridge's own exec is fine; putting any protected binary in between is
 * not, and it fails silently rather than with an error.
 *
 * KNOBS — all environment, all off by default, so an unconfigured load is a pure pass-through and
 * the control arm measures the shim's presence rather than its effect:
 *
 *   NA_FAIL_RX_AFTER=N   after N calls, every later rig_stream_read returns -NA_FAIL_CODE
 *   NA_FAIL_TX_AFTER=N   after N calls, every later rig_stream_write returns -NA_FAIL_CODE
 *   NA_FAIL_CODE=E       the Hamlib error to return, as a POSITIVE number; returned negated,
 *                        matching Hamlib's -RIG_Exxx convention. Default RIG_EIO.
 *   NA_FAIL_TX_SHORT=D   offer only n/D of each write to the real rig_stream_write (D >= 2), so
 *                        it accepts less than the bridge offered — issue #6's short write, which
 *                        is a NORMAL-operation case under backpressure, not an error path.
 */
#include <hamlib/rig.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef __APPLE__
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#  endif
#  include <dlfcn.h>
#endif

/* Counted per call, not per delivered byte: the bridge's RX loop polls on a timeout, so most reads
 * return no data, and a "successful reads" counter would make the trip point depend on how chatty
 * the backend happens to be. A plain call count is what the driver script can predict. */
static atomic_long g_rx_calls = 0;
static atomic_long g_tx_calls = 0;

static long g_rx_after   = -1;   /* < 0 disables */
static long g_tx_after   = -1;
static long g_tx_short   = 0;    /* 0 disables; >= 2 is the divisor */
static int  g_fail_code  = RIG_EIO;

static long env_long(const char *name, long dflt) {
    const char *s = getenv(name);
    if (s == NULL || *s == '\0') return dflt;
    return strtol(s, NULL, 10);
}

__attribute__((constructor)) static void na_failshim_init(void) {
    g_rx_after  = env_long("NA_FAIL_RX_AFTER", -1);
    g_tx_after  = env_long("NA_FAIL_TX_AFTER", -1);
    g_tx_short  = env_long("NA_FAIL_TX_SHORT", 0);
    g_fail_code = (int)env_long("NA_FAIL_CODE", RIG_EIO);
    if (g_tx_short == 1) g_tx_short = 0;   /* n/1 is the whole buffer — not a short write */

    /* The marker every arm greps for. stderr is unbuffered, but flush anyway: the bridge's output
     * is redirected to a file by the driver, and this line has to be there before the first
     * assertion reads it. */
    fprintf(stderr, "na_failshim: armed rx_after=%ld tx_after=%ld tx_short=%ld code=%d\n",
            g_rx_after, g_tx_after, g_tx_short, g_fail_code);
    fflush(stderr);
}

/* ---- the fault logic, shared by both platforms' interposition mechanisms ---------------------- */

typedef int (*read_fn)(RIG *, rig_stream_t *, void *, size_t, size_t *, int,
                       struct rig_stream_read_info *);
typedef int (*write_fn)(RIG *, rig_stream_t *, const void *, size_t, size_t *, int,
                        const struct rig_stream_write_info *);

/* The BOUND marker (issue #39). Printed from inside the interposed call, so a shim that loaded
 * without binding — the ELF visibility trap, a missing __DATA,__interpose entry — cannot produce it
 * however healthy the run looks. Exactly once per function per process: atomic_fetch_add returns the
 * previous value, so precisely one thread ever sees n == 1, and the log the arms grep gains two
 * lines rather than one per call. Called before any fault branch, so the pass-through path announces
 * itself too; the control arm arms nothing and would otherwise have nothing from inside the call to
 * assert on. */
static void announce_bound(const char *fn) {
    fprintf(stderr, "na_failshim: interposed %s\n", fn);
    fflush(stderr);
}

static int shim_read(read_fn real, RIG *rig, rig_stream_t *s, void *buf, size_t bufsize,
                     size_t *got, int timeout_ms, struct rig_stream_read_info *info) {
    const long n = atomic_fetch_add(&g_rx_calls, 1) + 1;
    if (n == 1) announce_bound("rig_stream_read");
    if (g_rx_after >= 0 && n > g_rx_after) {
        /* Zero the out-parameter: the bridge reads `got` on every path, and leaving a stale count
         * here would inject audio the fault is supposed to have prevented. */
        if (got != NULL) *got = 0;
        if (n == g_rx_after + 1) {
            fprintf(stderr, "na_failshim: rig_stream_read #%ld -> -%d (forced)\n", n, g_fail_code);
            fflush(stderr);
        }
        return -g_fail_code;
    }
    return real(rig, s, buf, bufsize, got, timeout_ms, info);
}

static int shim_write(write_fn real, RIG *rig, rig_stream_t *s, const void *buf, size_t n_bytes,
                      size_t *written, int timeout_ms, const struct rig_stream_write_info *info) {
    const long n = atomic_fetch_add(&g_tx_calls, 1) + 1;
    if (n == 1) announce_bound("rig_stream_write");
    if (g_tx_after >= 0 && n > g_tx_after) {
        if (written != NULL) *written = 0;
        if (n == g_tx_after + 1) {
            fprintf(stderr, "na_failshim: rig_stream_write #%ld -> -%d (forced)\n", n, g_fail_code);
            fflush(stderr);
        }
        return -g_fail_code;
    }
    if (g_tx_short >= 2 && n_bytes > 1) {
        /* A REAL short write, not a faked count: the truncated buffer is genuinely handed to
         * libhamlib, so *written comes back from the library and the bytes the bridge has to
         * requeue are exactly the ones the radio never saw. Reporting a short count while writing
         * the whole buffer would test the bridge's arithmetic against a lie. */
        size_t offer = n_bytes / (size_t)g_tx_short;
        if (offer == 0) offer = 1;
        return real(rig, s, buf, offer, written, timeout_ms, info);
    }
    return real(rig, s, buf, n_bytes, written, timeout_ms, info);
}

/* ---- platform interposition ------------------------------------------------------------------- */

#ifdef __APPLE__

/* Mach-O two-level namespace: an inserted library replaces a symbol through a __DATA,__interpose
 * table of {replacement, original} pairs, and the replacement calls the original by its own name.
 * This is why there is no dlsym here — the linker resolves rig_stream_read for us. */
#define DYLD_INTERPOSE(_repl, _orig)                                                               \
    __attribute__((used)) static struct {                                                          \
        const void *repl;                                                                          \
        const void *orig;                                                                          \
    } _interpose_##_orig __attribute__((section("__DATA,__interpose"))) = {                        \
        (const void *)(unsigned long)&_repl, (const void *)(unsigned long)&_orig                   \
    };

static int na_failshim_read(RIG *rig, rig_stream_t *s, void *buf, size_t bufsize, size_t *got,
                            int timeout_ms, struct rig_stream_read_info *info) {
    return shim_read(rig_stream_read, rig, s, buf, bufsize, got, timeout_ms, info);
}
DYLD_INTERPOSE(na_failshim_read, rig_stream_read)

static int na_failshim_write(RIG *rig, rig_stream_t *s, const void *buf, size_t n_bytes,
                             size_t *written, int timeout_ms,
                             const struct rig_stream_write_info *info) {
    return shim_write(rig_stream_write, rig, s, buf, n_bytes, written, timeout_ms, info);
}
DYLD_INTERPOSE(na_failshim_write, rig_stream_write)

#else

/* ELF: LD_PRELOAD puts these definitions ahead of libhamlib's in the global lookup order, so the
 * bridge binds to them and the originals are reachable only through RTLD_NEXT. Resolved lazily —
 * a constructor running before libhamlib is relocated would find nothing. */
int rig_stream_read(RIG *rig, rig_stream_t *s, void *buf, size_t bufsize, size_t *got,
                    int timeout_ms, struct rig_stream_read_info *info) {
    static read_fn real = NULL;
    if (real == NULL) real = (read_fn)dlsym(RTLD_NEXT, "rig_stream_read");
    if (real == NULL) {
        fprintf(stderr, "na_failshim: dlsym(rig_stream_read) failed — not interposing\n");
        fflush(stderr);
        return -RIG_EINTERNAL;
    }
    return shim_read(real, rig, s, buf, bufsize, got, timeout_ms, info);
}

int rig_stream_write(RIG *rig, rig_stream_t *s, const void *buf, size_t n_bytes, size_t *written,
                     int timeout_ms, const struct rig_stream_write_info *info) {
    static write_fn real = NULL;
    if (real == NULL) real = (write_fn)dlsym(RTLD_NEXT, "rig_stream_write");
    if (real == NULL) {
        fprintf(stderr, "na_failshim: dlsym(rig_stream_write) failed — not interposing\n");
        fflush(stderr);
        return -RIG_EINTERNAL;
    }
    return shim_write(real, rig, s, buf, n_bytes, written, timeout_ms, info);
}

#endif
