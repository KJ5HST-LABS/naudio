/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — sustained UDP TX delivery.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * Regression test for a silent, permanent loss of client TX audio.
 *
 * WHY THIS IS SEPARATE FROM c_net_smoke's TX round-trip: that test asserts injected TX
 * audio reaches the server's callback AT ALL, and passes on the first burst alone. The
 * defect this file covers let TX flow for about a second and then stop forever, so an
 * arrived-at-all assertion could not see it. The load-bearing assertion here is that TX
 * is still flowing in the SECOND HALF of the run.
 *
 * The trigger is not on the TX path. The server fans RX audio out to each client from a
 * writer thread, and a single failed send there tore the whole session down — which
 * unregisters the client from the mixer and so releases the TX channel it held. A legal
 * maximum-payload audio packet (19 + 16384 + 4 = 16407 bytes) exceeds the default UDP
 * send buffer on macOS (9216, net.inet.udp.maxdgram), so sendto() refused it with
 * EMSGSIZE and the operator went off the air with nothing logged. Hence the RX injection
 * below uses 16384-byte frames — the size na_hamlib_bridge's rx_thread actually emits.
 *
 * Hardware-free: NULL backends on both ends, loopback UDP, no PortAudio, no radio.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include "naudio.h"

#define RX_CHUNK   16384   /* na_hamlib_bridge rx_thread's buffer size, and == MAX_PAYLOAD */
#define TX_FRAME    1920   /* 20 ms of 48 kHz S16 mono — the bridge's TX shape             */
#define RUN_MS      3000
#define TICK_MS       20

static unsigned char g_rx[RX_CHUNK];
static unsigned char g_tx[TX_FRAME];

/* Non-silent bytes only. The mixer re-frames TX and pads with silence when the ring runs
 * dry, and keeps emitting frames even with NO TX owner — so total callback bytes climb
 * steadily while nothing the client sent is getting through. Counting raw callback bytes
 * is precisely the metric that made this defect look healthy. */
static _Atomic long long g_audible = 0;

static void on_tx_audio(const unsigned char *pcm, size_t n, void *user) {
    (void)user;
    if (pcm == NULL) return;
    long long audible = 0;
    for (size_t i = 0; i < n; i++) {
        if (pcm[i] != 0) audible++;
    }
    g_audible += audible;
}

static _Atomic int g_errors = 0;
static char g_last_error[256];

static void on_error(const char *client_id, const char *message, void *user) {
    (void)user;
    g_errors++;
    snprintf(g_last_error, sizeof g_last_error, "%s: %s", client_id ? client_id : "(null)",
             message ? message : "(null)");
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static int fail(const char *msg, na_stream_client *c, na_audio_server *s) {
    fprintf(stderr, "FAIL: %s\n", msg);
    if (c) na_client_destroy(c);
    if (s) { na_server_stop(s); na_server_destroy(s); }
    return 1;
}

int main(void) {
    for (size_t i = 0; i < sizeof g_rx; i++) g_rx[i] = (unsigned char)(1 + (i % 255));
    /* Every byte non-zero, so "audible bytes" cannot be confused with silence padding. */
    for (size_t i = 0; i < sizeof g_tx; i++) g_tx[i] = (unsigned char)(0x11 + (i % 4) * 0x11);

    na_audio_server *srv = na_server_create(NA_SERVER_BACKEND_NULL, 0);
    if (srv == NULL) return fail("na_server_create", NULL, NULL);

    /* The bridge's default profile, and the one that exercises FEC + reorder + ARQ. */
    if (na_server_set_reliability_profile(srv, NA_RELIABILITY_UDP_WAN) != NA_OK) {
        return fail("na_server_set_reliability_profile", NULL, srv);
    }
    na_server_set_tx_audio_cb(srv, on_tx_audio, NULL);

    na_server_callbacks scbs;
    memset(&scbs, 0, sizeof scbs);
    scbs.struct_size = sizeof scbs;
    scbs.on_error = on_error;
    na_server_set_callbacks(srv, &scbs, NULL);

    char err[256];
    if (na_server_start(srv, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "  (%s)\n", err);
        return fail("na_server_start", NULL, srv);
    }

    na_stream_client *c =
        na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", na_server_port(srv), "tx-sustained");
    if (c == NULL) return fail("na_client_create", NULL, srv);

    /* The server selected a UDP profile, so the client MUST match — a client left on the
     * default TCP transport is refused at connect. */
    if (na_client_set_transport(c, NA_TRANSPORT_UDP) != NA_OK) {
        return fail("na_client_set_transport(UDP)", c, srv);
    }
    na_client_set_playback_device(c, 0);
    na_client_set_auto_reconnect(c, 0);  /* a reconnect would mask the very defect under test */
    if (na_client_set_tx_inject(c, 1) != NA_OK) {
        return fail("na_client_set_tx_inject", c, srv);
    }
    if (na_client_connect(c, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "  (%s)\n", err);
        return fail("na_client_connect", c, srv);
    }
    na_client_set_ptt(c, 1);

    long long injected_first = 0, injected_second = 0;
    long long audible_at_half = 0;
    const int half_tick = (RUN_MS / TICK_MS) / 2;
    const int total_ticks = RUN_MS / TICK_MS;

    for (int tick = 0; tick < total_ticks; tick++) {
        /* Fan RX out to the client — the path whose failure used to kill the session. */
        na_server_inject_audio(srv, g_rx, (int)sizeof g_rx);

        const int a = na_client_inject_tx_audio(c, g_tx, (int)sizeof g_tx);
        if (a > 0) {
            if (tick < half_tick) injected_first += a; else injected_second += a;
        }
        if (tick == half_tick) audible_at_half = g_audible;
        sleep_ms(TICK_MS);
    }

    const long long audible_total = g_audible;
    const long long audible_second = audible_total - audible_at_half;
    const int clients = na_server_client_count(srv);
    char owner[128];
    memset(owner, 0, sizeof owner);
    const int owner_len = na_server_tx_owner(srv, owner, (int)sizeof owner);

    printf("c_tx_sustained: injected=%lld+%lld audible=%lld (2nd half %lld) clients=%d owner='%s'\n",
           injected_first, injected_second, audible_total, audible_second, clients, owner);
    if (g_errors > 0) printf("c_tx_sustained: server errors=%d last='%s'\n", g_errors, g_last_error);

    /* (1) The weak assertion every previous TX test made — it passes even when broken. */
    if (audible_at_half <= 0) {
        return fail("no TX audio reached the server in the first half", c, srv);
    }
    /* (2) The assertion that actually covers this defect. */
    if (injected_second <= 0) {
        return fail("client stopped accepting TX injection in the second half", c, srv);
    }
    if (audible_second * 2 < injected_second) {
        fprintf(stderr, "FAIL: TX delivery collapsed after the first half: %lld audible bytes "
                        "for %lld injected (need >= 50%%)\n", audible_second, injected_second);
        na_client_destroy(c);
        na_server_stop(srv);
        na_server_destroy(srv);
        return 1;
    }
    /* (3) The session must still be alive and still hold the TX channel. */
    if (clients != 1) return fail("server dropped the client during a sustained TX run", c, srv);
    if (owner_len <= 0) return fail("TX ownership lapsed during continuous injection", c, srv);

    na_client_set_ptt(c, 0);
    na_client_destroy(c);
    na_server_stop(srv);
    na_server_destroy(srv);

    printf("c_tx_sustained OK (sustained TX over UDP with %d-byte RX fan-out)\n", RX_CHUNK);
    return 0;
}
