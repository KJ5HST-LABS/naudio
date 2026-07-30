/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * The GROUND TRUTH for the FFI example-client gate (tests/ffi-clients/run.sh).
 *
 * examples/python, examples/java and examples/rust each mirror na_device
 * BYTE-FOR-BYTE in a hand-written declaration, because none of them has a C
 * compiler in the loop. Nothing detects it when the C side moves and one of them
 * does not: measured on this repo, shifting na_device by 8 bytes leaves all three
 * clients exiting 0 while printing wrong device records (see run.sh's header).
 *
 * This program is the reference the three are compared against. It #includes
 * naudio.h and links the same libnaudio they load, so it is the layout by
 * definition, and it emits it as sorted `key=value` lines that a plain diff can
 * pinpoint a single wrong field in.
 *
 * It prints TWO independent things:
 *
 *   1. The static layout — sizeof / alignof / every field offset. Deterministic,
 *      needs no audio hardware, and is what catches a struct whose fields moved.
 *
 *   2. The devices na_enumerate actually writes. This exercises the runtime path
 *      the layout exists to serve — the library writing records and the caller
 *      striding through them — and it is the half that would catch a change to
 *      na_enumerate's CALL CONVENTION (an added parameter, say) that left every
 *      offset intact. On a host with no audio devices the count is 0 for all four
 *      programs and only that half is weakened; run.sh says so out loud rather
 *      than reporting a vacuous match.
 *
 * na_client_callbacks is emitted too — only the Python client hand-declares it,
 * so run.sh diffs that section against Python alone.
 *
 * Output goes to stdout, one key per line, and nothing else may: run.sh diffs it
 * verbatim.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "naudio.h"

#define OFF(s, f) printf("%s.offset.%s=%zu\n", #s, #f, offsetof(s, f))

int main(void) {
    /* ---- 1. static layout ------------------------------------------------------- */
    printf("na_device.sizeof=%zu\n", sizeof(na_device));
    printf("na_device.alignof=%zu\n", (size_t)_Alignof(na_device));
    OFF(na_device, backend_id);
    OFF(na_device, capture_backend_id);
    OFF(na_device, playback_backend_id);
    OFF(na_device, name);
    OFF(na_device, host_api);
    OFF(na_device, type);
    OFF(na_device, capability);
    OFF(na_device, is_virtual);
    /* The two char arrays are the only fields whose SIZE a mirror can get wrong
     * without moving a later offset — a shorter `name` and a longer `host_api`
     * cancel out. Emit both sizes so that cancellation cannot hide. */
    printf("na_device.fieldsizeof.name=%zu\n", sizeof(((na_device*)0)->name));
    printf("na_device.fieldsizeof.host_api=%zu\n", sizeof(((na_device*)0)->host_api));

    printf("na_client_callbacks.sizeof=%zu\n", sizeof(na_client_callbacks));
    printf("na_client_callbacks.alignof=%zu\n", (size_t)_Alignof(na_client_callbacks));
    OFF(na_client_callbacks, struct_size);
    OFF(na_client_callbacks, on_connected);
    OFF(na_client_callbacks, on_disconnected);
    OFF(na_client_callbacks, on_stream_started);
    OFF(na_client_callbacks, on_stream_stopped);
    OFF(na_client_callbacks, on_error);
    OFF(na_client_callbacks, on_reconnecting);
    OFF(na_client_callbacks, on_reconnected);
    OFF(na_client_callbacks, on_clients_update);
    OFF(na_client_callbacks, on_tx_granted);
    OFF(na_client_callbacks, on_tx_denied);
    OFF(na_client_callbacks, on_tx_preempted);
    OFF(na_client_callbacks, on_tx_released);

    /* ---- 2. the device records na_enumerate writes ------------------------------- */
    na_context* ctx = na_context_create();
    if (ctx == NULL) {
        fprintf(stderr, "na_device_truth: na_context_create failed: %s\n",
                na_strerror(na_last_error()));
        return 1;
    }

    /* 128 matches the Java and Rust clients' MAX_DEVICES; Python asks for 64. The
     * count is compared across all four, so the cap only has to exceed the host's
     * device count -- which it does by a wide margin on any real machine. */
    na_device devs[128];
    int n = na_enumerate(ctx, devs, 128);
    if (n < 0) {
        fprintf(stderr, "na_device_truth: na_enumerate failed: %s\n", na_strerror(n));
        na_context_destroy(ctx);
        return 1;
    }

    printf("device.count=%d\n", n);
    for (int i = 0; i < n; i++) {
        printf("device.%d.backend_id=%d\n", i, devs[i].backend_id);
        printf("device.%d.capture_backend_id=%d\n", i, devs[i].capture_backend_id);
        printf("device.%d.playback_backend_id=%d\n", i, devs[i].playback_backend_id);
        printf("device.%d.type=%d\n", i, devs[i].type);
        printf("device.%d.capability=%d\n", i, devs[i].capability);
        printf("device.%d.is_virtual=%d\n", i, devs[i].is_virtual);
        printf("device.%d.name=%s\n", i, devs[i].name);
        printf("device.%d.host_api=%s\n", i, devs[i].host_api);
    }

    na_context_destroy(ctx);
    return 0;
}
