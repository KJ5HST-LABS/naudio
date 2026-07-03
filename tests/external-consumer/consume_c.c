/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — external consumer of the installed C ABI.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * External consumer of the INSTALLED naudio C ABI (installed-library check).
 *
 * Proves both C-ABI surfaces — the server (na_server_*) and the client
 * (na_client_*) — link and run from the install prefix via
 * find_package(naudio) -> naudio::naudio (the shared library). Hardware-free:
 * NULL backends touch no audio device, so this runs on a headless CI host.
 *
 * Includes ONLY <naudio.h> (the public C ABI header) — the proof the C surface is
 * self-sufficient from pure C against the installed prefix.
 */
#include <naudio.h>
#include <stdio.h>

int main(void) {
    /* Server C ABI. NULL backend = inject/extract, no PortAudio. */
    na_audio_server* srv = na_server_create(NA_SERVER_BACKEND_NULL, 4533);
    /* Client C ABI. NULL backend = no PortAudio init. */
    na_stream_client* cli =
        na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", 4533, "ext-consumer");

    if (srv == NULL || cli == NULL) {
        fprintf(stderr, "consume_c FAIL: create returned NULL (na_last_error=%d)\n",
                (int)na_last_error());
        na_client_destroy(cli);
        na_server_destroy(srv);
        return 1;
    }

    /* Exercise a getter on each surface (no server is running, so both are 0/false). */
    if (na_server_is_running(srv) != 0 || na_client_is_connected(cli) != 0) {
        fprintf(stderr, "consume_c FAIL: fresh handles report active state\n");
        na_client_destroy(cli);
        na_server_destroy(srv);
        return 1;
    }

    na_client_destroy(cli);
    na_server_destroy(srv);
    printf("consume_c OK: na_server_* + na_client_* C ABI linked + ran from the installed prefix\n");
    return 0;
}
