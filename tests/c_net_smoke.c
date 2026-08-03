/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — C-ABI networking smoke.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * C-ABI networking smoke. Compiled as C and linked against the C++ naudio library
 * (mirroring c_abi_smoke.c): it proves the na_client_* surface of naudio.h is
 * C-compilable and C-callable, and drives a FULL connect -> RX-frame-via-callback -> disconnect
 * against an in-process C++ AudioStreamServer over loopback — hardware-free (NULL client backend
 * + inject-only server, no PortAudio). It asserts:
 *   (1) the invalid-argument contract — na_client_create(NULL host) returns NULL/NA_ERR_INVALID,
 *       and NULL-client setters/connect return NA_ERR_INVALID (no server / hardware needed);
 *   (2) a NULL-backend client connects to the loopback server, the roster reports one client, and
 *       an injected payload arrives byte-identically at the C RX audio callback;
 *   (3) disconnect leaves the client not-connected and the server back at zero clients, and
 *       na_client_destroy() is clean (it joins workers first, so no callback is in flight after).
 * Returns non-zero (failing the ctest) on any contract violation.
 *
 * The loopback server is provided by net_smoke_server.cpp (a C++ test fixture exposing an
 * extern "C" surface) so this translation unit stays pure C and never names a C++ type.
 */
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

/* ---- test-only server fixture (net_smoke_server.cpp) ---- */
extern void* nasmoke_server_start(int* out_port);
extern void  nasmoke_server_inject(void* handle, const unsigned char* data, int len);
extern int   nasmoke_server_client_count(void* handle);
extern void  nasmoke_server_stop(void* handle);

/* A known RX payload — injected by the server, expected byte-identically at the audio callback. */
static const unsigned char KNOWN[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};

/* The TX counterpart (section 4). A whole-payload compare is not usable in this direction: the
 * server's mixer re-frames TX into bytesPerFrame() chunks and pads with silence when the ring runs
 * dry, so what the callback sees is frame-aligned, not inject-aligned. Instead the payload is a
 * repeating 4-byte signature, and the callback scans for two consecutive periods — a match that
 * silence (all zero) cannot produce and that survives any framing. Sized well over one 20ms stereo
 * frame (3840 bytes) so every read lands inside the pattern. */
#define TXSIG_PERIOD 4
static const unsigned char TXSIG_UNIT[TXSIG_PERIOD] = {0x11, 0x22, 0x33, 0x44};
static unsigned char TXSIG[8192];
static volatile int g_tx_ok = 0;  /* set when the signature reaches the server's TX callback */

static void on_tx_audio(const unsigned char* pcm, size_t n_bytes, void* user) {
    (void)user;
    if (pcm == NULL || n_bytes < TXSIG_PERIOD * 2) return;
    for (size_t i = 0; i + (TXSIG_PERIOD * 2) <= n_bytes; i++) {
        if (memcmp(pcm + i, TXSIG_UNIT, TXSIG_PERIOD) == 0 &&
            memcmp(pcm + i + TXSIG_PERIOD, TXSIG_UNIT, TXSIG_PERIOD) == 0) {
            g_tx_ok = 1;
            return;
        }
    }
}

/* Cross-thread flags: callbacks fire on the client's internal worker threads; main polls these.
 * `volatile` + the 20ms nanosleep barriers in the poll loop suffice for a smoke. The load-bearing
 * roster/connection gates additionally use the C++-mutex/atomic-guarded accessors below. */
static volatile int g_rx_ok = 0;          /* set when KNOWN arrives at the RX audio callback   */
static volatile int g_connected = 0;      /* set by on_connected                               */
static volatile int g_roster_count = -1;  /* last count seen via on_clients_update             */

static void on_connected(const char* id, const char* addr, void* user) {
    (void)id; (void)addr; (void)user;
    g_connected = 1;
}
static void on_clients_update(int count, int max_clients, const char* tx_owner,
                              const char* const* ids, int n_ids, void* user) {
    (void)max_clients; (void)tx_owner; (void)ids; (void)n_ids; (void)user;
    g_roster_count = count;
}
static void on_rx_audio(const unsigned char* pcm, size_t n_bytes, void* user) {
    (void)user;
    if (n_bytes == sizeof KNOWN && memcmp(pcm, KNOWN, sizeof KNOWN) == 0) g_rx_ok = 1;
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    /* The UCRT has no nanosleep; Sleep() is the C-callable equivalent here. */
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

int main(void) {
    for (size_t i = 0; i < sizeof TXSIG; i++) TXSIG[i] = TXSIG_UNIT[i % TXSIG_PERIOD];

    /* ---- (1) invalid-argument contract (no server / hardware) ---- */

    if (na_client_create(NA_CLIENT_BACKEND_NULL, NULL, 4533, "x") != NULL ||
        na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_client_create(NULL host) not NULL/NA_ERR_INVALID\n");
        return 1;
    }
    if (na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", 0, "x") != NULL ||
        na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_client_create(port 0) not NULL/NA_ERR_INVALID\n");
        return 1;
    }
    if (na_client_set_playback_device(NULL, 0) != NA_ERR_INVALID ||
        na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_client_set_playback_device(NULL) not NA_ERR_INVALID\n");
        return 1;
    }
    if (na_client_connect(NULL, NULL, 0) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_client_connect(NULL) not NA_ERR_INVALID\n");
        return 1;
    }
    if (na_client_set_tx_inject(NULL, 1) != NA_ERR_INVALID ||
        na_client_inject_tx_audio(NULL, KNOWN, (int)sizeof KNOWN) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: NULL-client TX-inject setters not NA_ERR_INVALID\n");
        return 1;
    }
    /* NULL-safe no-ops must not crash. */
    na_client_disconnect(NULL);
    na_client_destroy(NULL);
    if (na_client_is_connected(NULL) != 0 || na_client_server_client_count(NULL) != -1) {
        fprintf(stderr, "FAIL: NULL-client query accessors wrong\n");
        return 1;
    }

    /* ---- (2) loopback server + NULL-backend C client ---- */

    int port = 0;
    void* server = nasmoke_server_start(&port);
    if (server == NULL || port <= 0) {
        fprintf(stderr, "FAIL: loopback server start (port=%d)\n", port);
        return 1;
    }

    na_stream_client* client =
        na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", port, "c-net-smoke");
    if (client == NULL) {
        fprintf(stderr, "FAIL: na_client_create (%s)\n", na_strerror(na_last_error()));
        nasmoke_server_stop(server);
        return 1;
    }

    na_client_callbacks cbs;
    memset(&cbs, 0, sizeof cbs);
    cbs.struct_size = sizeof cbs;
    cbs.on_connected = on_connected;
    cbs.on_clients_update = on_clients_update;
    na_client_set_callbacks(client, &cbs, NULL);
    na_client_set_audio_cb(client, on_rx_audio, NULL);
    na_client_set_playback_device(client, 0);  /* REQUIRED for RX; NULL backend accepts any id */
    na_client_set_auto_reconnect(client, 0);    /* deterministic: no backoff churn in the smoke */

    char err[256];
    if (na_client_connect(client, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "FAIL: na_client_connect (%s)\n", err);
        na_client_destroy(client);
        nasmoke_server_stop(server);
        return 1;
    }
    if (!na_client_is_connected(client)) {
        fprintf(stderr, "FAIL: na_client_is_connected()==0 after a successful connect\n");
        na_client_destroy(client);
        nasmoke_server_stop(server);
        return 1;
    }

    /* Inject the known payload until it arrives at the RX callback AND on_connected has
     * fired AND the roster shows us (≤3s). g_connected is set on the dispatch thread
     * slightly after the connect returns — poll for it here rather than asserting it
     * immediately after the loop, or a busy scheduler loses that race.
     * serverClientCount() is C++-mutex-guarded (thread-safe to poll from here). */
    int waited = 0;
    while (waited < 3000 &&
           !(g_rx_ok && g_connected && na_client_server_client_count(client) == 1)) {
        nasmoke_server_inject(server, KNOWN, (int)sizeof KNOWN);
        sleep_ms(20);
        waited += 20;
    }
    if (!g_rx_ok) {
        fprintf(stderr, "FAIL: no RX frame via the C audio callback within budget\n");
        na_client_destroy(client);
        nasmoke_server_stop(server);
        return 1;
    }
    if (na_client_server_client_count(client) != 1) {
        fprintf(stderr, "FAIL: server roster never reached 1 (poll=%d, cb=%d)\n",
                na_client_server_client_count(client), g_roster_count);
        na_client_destroy(client);
        nasmoke_server_stop(server);
        return 1;
    }
    if (!g_connected) {
        fprintf(stderr, "FAIL: on_connected callback never fired\n");
        na_client_destroy(client);
        nasmoke_server_stop(server);
        return 1;
    }
    /* TX inject is frozen on connect (it decides whether the send worker spawns, and connect
     * spawns the workers once) — flipping it now must be rejected, not silently ignored. */
    if (na_client_set_tx_inject(client, 1) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_client_set_tx_inject not frozen after connect\n");
        na_client_destroy(client);
        nasmoke_server_stop(server);
        return 1;
    }
    /* This client never enabled TX inject, so it has no send worker. The TX ring is allocated for
     * every connection regardless, so the write would otherwise succeed and be silently dropped —
     * inject must report 0 rather than bytes that can never leave the process. Keyed up first, to
     * prove it is the inject flag being tested here and not the PTT gate. */
    na_client_set_ptt(client, 1);
    if (na_client_inject_tx_audio(client, KNOWN, (int)sizeof KNOWN) != 0) {
        fprintf(stderr, "FAIL: TX inject accepted bytes without na_client_set_tx_inject\n");
        na_client_destroy(client);
        nasmoke_server_stop(server);
        return 1;
    }
    na_client_set_ptt(client, 0);

    /* ---- (3) clean disconnect ---- */

    na_client_disconnect(client);
    if (na_client_is_connected(client)) {
        fprintf(stderr, "FAIL: still connected after na_client_disconnect()\n");
        na_client_destroy(client);
        nasmoke_server_stop(server);
        return 1;
    }
    int gone = 0;
    for (int i = 0; i < 100; i++) {  /* up to ~2s for the server to drop the session */
        if (nasmoke_server_client_count(server) == 0) {
            gone = 1;
            break;
        }
        sleep_ms(20);
    }

    na_client_destroy(client);  /* idempotent disconnect + join + free */
    nasmoke_server_stop(server);

    if (!gone) {
        fprintf(stderr, "FAIL: server still reports clients after disconnect\n");
        return 1;
    }

    /* ---- (4) TX round-trip: client inject -> server mixer -> server tx-audio callback ----
     *
     * The mirror of section (2): there the server injected RX and the client's callback saw it;
     * here the client injects TX and the SERVER's callback sees it. This proves the TX direction
     * end-to-end hardware-free, which was impossible before na_client_inject_tx_audio — a NULL
     * backend cannot capture (openCaptureStream throws), so a headless client had no TX source.
     *
     * This section uses a C-ABI server (na_server_*) rather than the C++ fixture above, because
     * only the C-ABI NULL backend exposes mixed TX to C, via na_server_set_tx_audio_cb. */

    na_audio_server* txsrv = na_server_create(NA_SERVER_BACKEND_NULL, 0);
    if (txsrv == NULL) {
        fprintf(stderr, "FAIL: na_server_create (tx round-trip) (%s)\n",
                na_strerror(na_last_error()));
        return 1;
    }
    na_server_set_tx_audio_cb(txsrv, on_tx_audio, NULL);
    char txerr[256];
    if (na_server_start(txsrv, txerr, (int)sizeof txerr) != NA_OK) {
        fprintf(stderr, "FAIL: na_server_start (tx round-trip) (%s)\n", txerr);
        na_server_destroy(txsrv);
        return 1;
    }

    na_stream_client* txc =
        na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", na_server_port(txsrv), "c-tx-smoke");
    if (txc == NULL) {
        fprintf(stderr, "FAIL: na_client_create (tx) (%s)\n", na_strerror(na_last_error()));
        na_server_stop(txsrv);
        na_server_destroy(txsrv);
        return 1;
    }
    na_client_set_playback_device(txc, 0);
    na_client_set_auto_reconnect(txc, 0);
    if (na_client_set_tx_inject(txc, 1) != NA_OK) {
        fprintf(stderr, "FAIL: na_client_set_tx_inject rejected before connect\n");
        na_client_destroy(txc);
        na_server_stop(txsrv);
        na_server_destroy(txsrv);
        return 1;
    }
    if (na_client_connect(txc, txerr, (int)sizeof txerr) != NA_OK) {
        fprintf(stderr, "FAIL: na_client_connect (tx) (%s)\n", txerr);
        na_client_destroy(txc);
        na_server_stop(txsrv);
        na_server_destroy(txsrv);
        return 1;
    }

    /* Bad arguments on a live client: NULL pcm / non-positive length. */
    if (na_client_inject_tx_audio(txc, NULL, 16) != NA_ERR_INVALID ||
        na_client_inject_tx_audio(txc, TXSIG, 0) != NA_ERR_INVALID ||
        na_client_inject_tx_audio(txc, TXSIG, -1) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_client_inject_tx_audio bad args not NA_ERR_INVALID\n");
        na_client_destroy(txc);
        na_server_stop(txsrv);
        na_server_destroy(txsrv);
        return 1;
    }

    /* PTT gate: injected audio obeys the same keying rule as captured audio, so a client that has
     * not asserted PTT must accept 0 bytes (documented as "not an error"). */
    if (na_client_inject_tx_audio(txc, TXSIG, (int)sizeof TXSIG) != 0) {
        fprintf(stderr, "FAIL: TX inject accepted bytes with PTT inactive\n");
        na_client_destroy(txc);
        na_server_stop(txsrv);
        na_server_destroy(txsrv);
        return 1;
    }

    na_client_set_ptt(txc, 1);  /* key up: capture unmuted => inject accepted */
    int accepted = na_client_inject_tx_audio(txc, TXSIG, (int)sizeof TXSIG);
    if (accepted <= 0) {
        fprintf(stderr, "FAIL: TX inject accepted %d bytes with PTT active\n", accepted);
        na_client_destroy(txc);
        na_server_stop(txsrv);
        na_server_destroy(txsrv);
        return 1;
    }

    /* Keep feeding until the signature reaches the server's TX callback (≤3s). Continuous feed
     * also holds the mixer's TX grant, which the idle timeout would otherwise release. */
    waited = 0;
    while (waited < 3000 && !g_tx_ok) {
        na_client_inject_tx_audio(txc, TXSIG, (int)sizeof TXSIG);
        sleep_ms(20);
        waited += 20;
    }

    na_client_destroy(txc);
    na_server_stop(txsrv);
    na_server_destroy(txsrv);

    if (!g_tx_ok) {
        fprintf(stderr, "FAIL: injected TX audio never reached the server tx-audio callback\n");
        return 1;
    }

    printf("c_net_smoke OK (port=%d, RX frame received via C callback, roster=1, clean disconnect, "
           "TX inject round-trip)\n",
           port);
    return 0;
}
