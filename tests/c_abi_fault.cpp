/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — C-ABI fault-injection test.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * C-ABI fault-injection test. It proves the na_client_* extern "C" boundary is
 * exception-tight under failure — the contract naudio.h promises: "No C++ exception ever crosses
 * this boundary." A C++ translation unit (operator new cannot be overridden from C) that links the
 * public C ABI and arms allocation failure around exactly one C-ABI call at a time, asserting:
 *
 *   a setter whose C++ body allocates surfaces a forced std::bad_alloc as NA_ERR_NOMEM and a
 *   graceful return — never an exception crossing into the caller;
 *   a read-only getter clears na_last_error() to NA_OK on entry (it was left stale before);
 *   na_client_set_callbacks / na_client_set_audio_cb are REJECTED with NA_ERR_INVALID once
 *   na_client_connect has been attempted (the documented "set callbacks before connect" rule,
 *   now enforced so a setter can't race a worker reading the pointers);
 *   na_client_destroy under total allocation failure does NOT std::terminate — teardown
 *   (disconnect + the noexcept destructor's join/drain) swallows the failure and the process
 *   survives.
 *
 * Returns non-zero (failing the ctest) on any contract violation. This is the C++ companion to the
 * pure-C c_abi_smoke.c / c_net_smoke.c (which exercise the happy paths); here we exercise the
 * failure paths that only a C++ allocation hook can force.
 */
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "naudio.h"

namespace {
// When armed (on the calling thread), the next allocation throws std::bad_alloc. Armed only around
// a single C-ABI call and disarmed immediately, so the harness's own allocations are unaffected.
thread_local bool g_failAlloc = false;
}  // namespace

// Global throwing operator new/new[] with the fault hook. delete frees via std::free to match.
void* operator new(std::size_t n) {
    if (g_failAlloc) throw std::bad_alloc();
    if (void* p = std::malloc(n != 0 ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (g_failAlloc) throw std::bad_alloc();
    if (void* p = std::malloc(n != 0 ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(void) {
    int failures = 0;

    na_stream_client* c =
        na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", 4533, "c-abi-fault");
    if (c == NULL) {
        fprintf(stderr, "FAIL: na_client_create (%s)\n", na_strerror(na_last_error()));
        return 1;
    }
    /* Deterministic teardown later: no reconnect worker churn. */
    na_client_set_auto_reconnect(c, 0);

    /* A long (> SSO) callsign forces a real heap allocation inside setCallsign's std::string, so the
     * armed hook fires inside the C++ body rather than being optimized into the stack. */
    const char* longCall = "VERYLONGCALLSIGN-WELL-PAST-SHORT-STRING-OPTIMIZATION-BUFFER";

    /* ---- A1: forced bad_alloc in a setter surfaces as NA_ERR_NOMEM, not a thrown exception ---- */
    g_failAlloc = true;
    na_error_t r = na_client_set_identity(c, longCall, NULL, NULL);
    g_failAlloc = false;
    if (r != NA_ERR_NOMEM || na_last_error() != NA_ERR_NOMEM) {
        fprintf(stderr, "FAIL: A1 setter under OOM: ret=%d last=%d (want NA_ERR_NOMEM=%d)\n", r,
                na_last_error(), NA_ERR_NOMEM);
        ++failures;
    }

    /* The client is uncorrupted: the same setter succeeds and clears the error when allocation works. */
    if (na_client_set_identity(c, longCall, "Operator", "Grid") != NA_OK ||
        na_last_error() != NA_OK) {
        fprintf(stderr, "FAIL: A1 setter post-recovery not NA_OK (last=%d)\n", na_last_error());
        ++failures;
    }

    /* ---- A4: a read-only getter clears na_last_error() to NA_OK on entry ---- */
    g_failAlloc = true;
    (void)na_client_set_identity(c, longCall, NULL, NULL);  /* leaves NA_ERR_NOMEM on the thread */
    g_failAlloc = false;
    if (na_last_error() != NA_ERR_NOMEM) {
        fprintf(stderr, "FAIL: A4 precondition (expected NA_ERR_NOMEM, got %d)\n", na_last_error());
        ++failures;
    }
    (void)na_client_is_connected(c);  /* a getter: must clear the stale error on entry */
    if (na_last_error() != NA_OK) {
        fprintf(stderr, "FAIL: A4 getter did not clear na_last_error (last=%d)\n", na_last_error());
        ++failures;
    }

    /* ---- A3: callbacks may not be set after connect() is attempted ---- */
    char errbuf[128];
    (void)na_client_connect(c, errbuf, (int)sizeof errbuf);  /* attempt (fails: no server) — locks cbs */
    na_client_callbacks cbs;
    memset(&cbs, 0, sizeof cbs);
    if (na_client_set_callbacks(c, &cbs, NULL) != NA_ERR_INVALID ||
        na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: A3 set_callbacks after connect not rejected (last=%d)\n",
                na_last_error());
        ++failures;
    }
    if (na_client_set_audio_cb(c, NULL, NULL) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: A3 set_audio_cb after connect not rejected\n");
        ++failures;
    }

    /* ---- A2: na_client_destroy under total allocation failure must not std::terminate ---- */
    g_failAlloc = true;
    na_client_destroy(c);  /* teardown allocations fail; the boundary must swallow, not abort */
    g_failAlloc = false;
    /* Reaching this line proves no std::terminate occurred during teardown. */

    na_client_destroy(NULL);  /* still NULL-safe */

    if (failures == 0) {
        printf("c_abi_fault OK (A1 NOMEM, A4 clears, A3 rejects-after-connect, A2 no-terminate)\n");
    }
    return failures != 0 ? 1 : 0;
}
