/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — C-ABI networking SERVER smoke.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * C-ABI networking SERVER smoke. Compiled as C and linked against the C++
 * naudio library (mirroring c_abi_smoke.c / c_net_smoke.c): it proves the
 * na_server_* surface of naudio.h is C-compilable and C-callable, and drives a FULL server-side
 * lifecycle — create -> start -> a real na_client_* client connects over loopback -> RX broadcast
 * via na_server_inject_audio -> TX extract via the tx-audio callback -> clean teardown — entirely
 * from PURE C, hardware-free (NULL server backend + NULL client backend, no PortAudio, no C++
 * fixture). Unlike c_net_smoke.c (which needed net_smoke_server.cpp to spin up a C++ server), the
 * na_server_* ABI lets the C side own BOTH ends.
 *
 * It asserts:
 *   (1) the invalid-argument contract — na_server_create(bad port) returns NULL/NA_ERR_INVALID;
 *       NULL-server setters/start/inject return NA_ERR_INVALID; the NULL backend rejects the
 *       device setters with NA_ERR_UNSUPPORTED; NULL-safe stop/destroy/getters don't crash;
 *   (2) start brings the server up on an OS-assigned port, on_started fires with that port,
 *       na_server_is_running()==1, and post-start config setters are frozen (NA_ERR_INVALID);
 *   (3) a NULL-backend client connects, the server roster reports one client, on_client_connected
 *       fires, an injected RX payload arrives byte-identically at the client's RX audio callback,
 *       and the server's mixed-TX stream is delivered to the na_server_tx_audio_cb (silence frames
 *       once a client is connected — the extract WIRING, since a NULL-backend client cannot
 *       capture real TX);
 *   (4) clean disconnect leaves the server at zero clients and on_client_disconnected fires;
 *       na_server_stop fires on_stopped and flips is_running to 0; destroy of both is clean.
 * Returns non-zero (failing the ctest) on any contract violation.
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

/* Cross-thread flag primitives: C11 <stdatomic.h> everywhere except MSVC, whose
 * stdatomic support is gated behind toolset-specific switches (VS 17.5 wanted
 * /experimental:c11atomics; VS 18's cl rejects that combination outright) —
 * Interlocked* gives the same seq-cst int semantics there. The _Atomic path is
 * the one the TSan gate exercises (POSIX-only), so nothing is lost on Windows. */
#if defined(_MSC_VER) && !defined(__clang__)
typedef volatile LONG atomic_int;
#  define atomic_store(p, v)     InterlockedExchange((p), (LONG)(v))
#  define atomic_load(p)         ((int)InterlockedCompareExchange((p), 0, 0))
#  define atomic_fetch_add(p, v) InterlockedExchangeAdd((p), (LONG)(v))
#else
#  include <stdatomic.h>
#endif

#include "naudio.h"

/* A known RX payload — injected at the server, expected byte-identically at the client callback. */
static const unsigned char KNOWN[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};

/* Cross-thread flags: callbacks fire on internal worker / dispatch / mixer threads; main polls
 * these. C11 _Atomic (not plain `volatile`) so the smoke is itself data-race-clean under TSan — the
 * point of the TSan gate is to prove the LIBRARY is race-free, which a `volatile`-flag harness would
 * mask with its own benign poll races. The load-bearing roster/run gates additionally use the
 * mutex/atomic-guarded na_server_* accessors. */
static atomic_int g_srv_started = 0;       /* server on_started (records the port)        */
static atomic_int g_srv_started_port = 0;
static atomic_int g_srv_stopped = 0;       /* server on_stopped                           */
static atomic_int g_srv_client_conn = 0;   /* server on_client_connected                  */
static atomic_int g_srv_client_disc = 0;   /* server on_client_disconnected               */
static atomic_int g_tx_frames = 0;         /* mixed-TX frames seen at na_server_tx_audio_cb */
static atomic_int g_tx_bytes = 0;          /* size of the last TX frame                   */

static atomic_int g_cli_connected = 0;     /* client on_connected                         */
static atomic_int g_cli_rx_ok = 0;         /* KNOWN arrived at the client RX audio callback */

/* ---- server-side callbacks ---- */
static void srv_on_started(int port, void* user) {
    (void)user;
    atomic_store(&g_srv_started_port, port);
    atomic_store(&g_srv_started, 1);
}
static void srv_on_stopped(void* user) { (void)user; atomic_store(&g_srv_stopped, 1); }
static void srv_on_client_connected(const char* id, const char* addr, void* user) {
    (void)id; (void)addr; (void)user;
    atomic_store(&g_srv_client_conn, 1);
}
static void srv_on_client_disconnected(const char* id, void* user) {
    (void)id; (void)user;
    atomic_store(&g_srv_client_disc, 1);
}
static void srv_on_tx_audio(const unsigned char* pcm, size_t n_bytes, void* user) {
    (void)pcm; (void)user;
    atomic_store(&g_tx_bytes, (int)n_bytes);
    atomic_fetch_add(&g_tx_frames, 1);
}

/* ---- client-side callbacks ---- */
static void cli_on_connected(const char* id, const char* addr, void* user) {
    (void)id; (void)addr; (void)user;
    atomic_store(&g_cli_connected, 1);
}
static void cli_on_rx_audio(const unsigned char* pcm, size_t n_bytes, void* user) {
    (void)user;
    if (n_bytes == sizeof KNOWN && memcmp(pcm, KNOWN, sizeof KNOWN) == 0)
        atomic_store(&g_cli_rx_ok, 1);
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
    /* ---- (1) invalid-argument contract (no server / hardware) ---- */

    if (na_server_create(NA_SERVER_BACKEND_NULL, -1) != NULL ||
        na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_server_create(port -1) not NULL/NA_ERR_INVALID\n");
        return 1;
    }
    if (na_server_create(NA_SERVER_BACKEND_NULL, 70000) != NULL ||
        na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_server_create(port 70000) not NULL/NA_ERR_INVALID\n");
        return 1;
    }
    if (na_server_set_max_clients(NULL, 4) != NA_ERR_INVALID ||
        na_server_start(NULL, NULL, 0) != NA_ERR_INVALID ||
        na_server_inject_audio(NULL, KNOWN, (int)sizeof KNOWN) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: NULL-server setters/start/inject not NA_ERR_INVALID\n");
        return 1;
    }
    /* NULL-safe no-ops must not crash, and the NULL-server getters return their sentinels. */
    na_server_stop(NULL);
    na_server_destroy(NULL);
    if (na_server_is_running(NULL) != 0 || na_server_port(NULL) != -1 ||
        na_server_client_count(NULL) != -1 || na_server_max_clients(NULL) != -1) {
        fprintf(stderr, "FAIL: NULL-server query accessors wrong\n");
        return 1;
    }

    /* ---- (2) create + configure + start a NULL-backend server on an ephemeral port ---- */

    na_audio_server* server = na_server_create(NA_SERVER_BACKEND_NULL, 0);
    if (server == NULL) {
        fprintf(stderr, "FAIL: na_server_create (%s)\n", na_strerror(na_last_error()));
        return 1;
    }

    /* The NULL backend has no real devices: the device setters must be refused as UNSUPPORTED. */
    if (na_server_set_capture_device(server, 0) != NA_ERR_UNSUPPORTED ||
        na_server_set_playback_device(server, 0) != NA_ERR_UNSUPPORTED) {
        fprintf(stderr, "FAIL: NULL backend did not reject device setters with NA_ERR_UNSUPPORTED\n");
        na_server_destroy(server);
        return 1;
    }

    na_server_callbacks scbs;
    memset(&scbs, 0, sizeof scbs);
    scbs.on_started = srv_on_started;
    scbs.on_stopped = srv_on_stopped;
    scbs.on_client_connected = srv_on_client_connected;
    scbs.on_client_disconnected = srv_on_client_disconnected;
    if (na_server_set_callbacks(server, &scbs, NULL) != NA_OK ||
        na_server_set_tx_audio_cb(server, srv_on_tx_audio, NULL) != NA_OK ||
        na_server_set_max_clients(server, 4) != NA_OK ||
        na_server_set_transport(server, NA_TRANSPORT_TCP) != NA_OK) {
        fprintf(stderr, "FAIL: server configuration setters\n");
        na_server_destroy(server);
        return 1;
    }
    if (na_server_max_clients(server) != 4) {  /* readable pre-start */
        fprintf(stderr, "FAIL: na_server_max_clients pre-start != 4\n");
        na_server_destroy(server);
        return 1;
    }

    char serr[256];
    if (na_server_start(server, serr, (int)sizeof serr) != NA_OK) {
        fprintf(stderr, "FAIL: na_server_start (%s)\n", serr);
        na_server_destroy(server);
        return 1;
    }
    int port = na_server_port(server);
    if (port <= 0 || !na_server_is_running(server)) {
        fprintf(stderr, "FAIL: server not running on a valid port (port=%d)\n", port);
        na_server_destroy(server);
        return 1;
    }
    /* on_started fired with the bound port (dispatch thread; allow a beat). */
    for (int i = 0; i < 50 && !atomic_load(&g_srv_started); i++) sleep_ms(20);
    if (!atomic_load(&g_srv_started) || atomic_load(&g_srv_started_port) != port) {
        fprintf(stderr, "FAIL: on_started (fired=%d, port=%d vs %d)\n",
                atomic_load(&g_srv_started), atomic_load(&g_srv_started_port), port);
        na_server_destroy(server);
        return 1;
    }
    /* Config is frozen after start. */
    if (na_server_set_max_clients(server, 8) != NA_ERR_INVALID ||
        na_server_set_callbacks(server, &scbs, NULL) != NA_ERR_INVALID ||
        na_server_set_tx_audio_cb(server, srv_on_tx_audio, NULL) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: config setters not frozen after start\n");
        na_server_destroy(server);
        return 1;
    }

    /* ---- (3) a real NULL-backend client connects over loopback ---- */

    na_stream_client* client =
        na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", port, "c-server-smoke");
    if (client == NULL) {
        fprintf(stderr, "FAIL: na_client_create (%s)\n", na_strerror(na_last_error()));
        na_server_destroy(server);
        return 1;
    }
    na_client_callbacks ccbs;
    memset(&ccbs, 0, sizeof ccbs);
    ccbs.on_connected = cli_on_connected;
    na_client_set_callbacks(client, &ccbs, NULL);
    na_client_set_audio_cb(client, cli_on_rx_audio, NULL);
    na_client_set_playback_device(client, 0);  /* REQUIRED for RX; NULL backend accepts any id */
    na_client_set_auto_reconnect(client, 0);    /* deterministic: no backoff churn in the smoke */

    char cerr[256];
    if (na_client_connect(client, cerr, (int)sizeof cerr) != NA_OK) {
        fprintf(stderr, "FAIL: na_client_connect (%s)\n", cerr);
        na_client_destroy(client);
        na_server_destroy(server);
        return 1;
    }

    /* Inject the known RX payload until it arrives at the client callback AND both connect
     * callbacks have fired AND the server roster shows one client (<=3s). The two connect
     * flags land on the server/client dispatch threads slightly after the wire-level connect
     * — poll for them here rather than asserting them immediately after the loop, or a busy
     * scheduler loses that race. na_server_client_count is mutex-guarded (safe to poll). */
    int waited = 0;
    while (waited < 3000 &&
           !(atomic_load(&g_cli_rx_ok) && atomic_load(&g_cli_connected) &&
             atomic_load(&g_srv_client_conn) && na_server_client_count(server) == 1)) {
        na_server_inject_audio(server, KNOWN, (int)sizeof KNOWN);
        sleep_ms(20);
        waited += 20;
    }
    if (!atomic_load(&g_cli_rx_ok)) {
        fprintf(stderr, "FAIL: injected RX frame never reached the client audio callback\n");
        na_client_destroy(client);
        na_server_destroy(server);
        return 1;
    }
    if (na_server_client_count(server) != 1) {
        fprintf(stderr, "FAIL: server roster never reached 1 (count=%d)\n",
                na_server_client_count(server));
        na_client_destroy(client);
        na_server_destroy(server);
        return 1;
    }
    if (!atomic_load(&g_srv_client_conn) || !atomic_load(&g_cli_connected)) {
        fprintf(stderr, "FAIL: connect callbacks (srv=%d, cli=%d)\n",
                atomic_load(&g_srv_client_conn), atomic_load(&g_cli_connected));
        na_client_destroy(client);
        na_server_destroy(server);
        return 1;
    }

    /* TX-extract WIRING: with a client connected, the mixer playback loop drains the (empty) TX
     * buffer as silence frames into the ForwardingPlaybackStream -> the C tx-audio callback. We
     * cannot drive REAL TX from a NULL-backend client (no capture), so we verify the extract path
     * fires with full-size frames. <=3s for the initial-buffering window to elapse. */
    for (int i = 0; i < 150 && atomic_load(&g_tx_frames) == 0; i++) sleep_ms(20);
    if (atomic_load(&g_tx_frames) == 0 || atomic_load(&g_tx_bytes) <= 0) {
        fprintf(stderr, "FAIL: na_server_tx_audio_cb never delivered a mixed-TX frame "
                        "(frames=%d, bytes=%d)\n", atomic_load(&g_tx_frames), atomic_load(&g_tx_bytes));
        na_client_destroy(client);
        na_server_destroy(server);
        return 1;
    }

    /* ---- (4) clean disconnect + stop ---- */

    na_client_disconnect(client);
    int gone = 0;
    for (int i = 0; i < 150; i++) {  /* up to ~3s for the server to drop the session */
        if (na_server_client_count(server) == 0) { gone = 1; break; }
        sleep_ms(20);
    }
    na_client_destroy(client);
    if (!gone) {
        fprintf(stderr, "FAIL: server still reports clients after client disconnect\n");
        na_server_destroy(server);
        return 1;
    }
    if (!atomic_load(&g_srv_client_disc)) {
        fprintf(stderr, "FAIL: server on_client_disconnected never fired\n");
        na_server_destroy(server);
        return 1;
    }

    na_server_stop(server);
    if (na_server_is_running(server)) {
        fprintf(stderr, "FAIL: server still running after na_server_stop\n");
        na_server_destroy(server);
        return 1;
    }
    for (int i = 0; i < 50 && !atomic_load(&g_srv_stopped); i++) sleep_ms(20);
    if (!atomic_load(&g_srv_stopped)) {
        fprintf(stderr, "FAIL: server on_stopped never fired\n");
        na_server_destroy(server);
        return 1;
    }

    na_server_destroy(server);  /* idempotent stop + join + drain dispatcher + free */

    printf("c_server_smoke OK (port=%d, client RX byte-identical, roster=1, TX-extract frames=%d "
           "@ %d bytes, clean disconnect+stop)\n", port, atomic_load(&g_tx_frames),
           atomic_load(&g_tx_bytes));
    return 0;
}
