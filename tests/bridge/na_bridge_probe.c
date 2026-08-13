/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — na_bridge_probe: a content-aware RX probe for na_hamlib_bridge.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * A headless naudio client that attaches to a running server, samples its RX audio for a fixed
 * wall clock, and reports what ARRIVED rather than merely how much. It links the public C ABI and
 * NOTHING ELSE — no libhamlib, no PortAudio, no C++ fixture — which is the whole point of where it
 * lives in the build: six sessions hand-built a bridge harness in a scratchpad and all six
 * evaporated (issue #15), and every one of them was mostly this file. Splitting the probe from the
 * thing it probes is what lets the probe be compiled and RUN on every platform while the
 * bridge-driving arm, which needs a hand-built streaming libhamlib, self-skips.
 *
 * WHY IT MEASURES CONTENT AND NOT VOLUME. A byte count proves transport, not content. Measured
 * against the Hamlib dummy on 2026-07-31, `-S tone` and `-S silence` are indistinguishable on every
 * volume-shaped metric — 956160 vs 954240 bytes, 996 vs 994 callbacks, 79680 vs 79520 B/s, and the
 * bridge's own meter reports 83% of nominal for BOTH. Peak |sample| is the only discriminator:
 * 16383 against 0. Any harness that gates on rate or byte count passes a totally silent bridge.
 *
 * WHY ABSOLUTE RATE IS REPORTED BUT NEVER ASSERTED. 83% of nominal is what a CORRECT run looks
 * like here: a dummy backend paces itself off nanosleep and simply runs slow while being perfectly
 * right. A rate threshold tight enough to catch a fault would fire on that healthy run, so rate is
 * printed for a human and --min-rate is opt-in, defaulting to off.
 *
 * MODES
 *   (default) attach — connect to --host/--port, sample --seconds, print RESULT, and assert
 *             whichever of --expect / --min-rate were given.
 *   --selftest      — stand up an in-process NULL-backend server, inject KNOWN content and then
 *                     silence, and assert this probe's own detector tells them apart. Needs no
 *                     bridge and no libhamlib, so it runs in ctest on every platform. Without it,
 *                     "the probe is built in CI" would mean compiled-but-never-executed.
 *
 * EXITS 0 when every requested expectation held, 1 when one did not (the assertion failure the
 * caller wants to see), and 2 when the probe ITSELF could not run — a tri-state, so a broken
 * harness can never be read as a clean negative result.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

/* Cross-thread flag primitives — same rationale and same shape as tests/c_server_smoke.c: C11
 * <stdatomic.h> everywhere except MSVC, whose stdatomic support is gated behind toolset-specific
 * switches, where Interlocked* gives the same seq-cst int semantics. */
#if defined(_MSC_VER) && !defined(__clang__)
typedef volatile LONG atomic_int;
#  define atomic_store(p, v)     InterlockedExchange((p), (LONG)(v))
#  define atomic_load(p)         ((int)InterlockedCompareExchange((p), 0, 0))
#  define atomic_fetch_add(p, v) InterlockedExchangeAdd((p), (LONG)(v))
#else
#  include <stdatomic.h>
#endif

#include "naudio.h"

#define EXIT_MET      0
#define EXIT_UNMET    1
#define EXIT_HARNESS  2

/* ---- the detector -----------------------------------------------------------------------------
 *
 * Every field is derived from the bytes that actually arrived; nothing here restates a value the
 * caller supplied (Learning 43 — a probe that spells a value is testing itself). n_bytes in
 * particular is NOT assumed: na_server_inject_audio broadcasts the caller's buffer verbatim, so a
 * probe that reported its own inject size would report it unchanged no matter what the wire did. */
static atomic_int g_calls   = 0;
static atomic_int g_bytes   = 0;
static atomic_int g_peak    = 0;   /* max |sample| over every S16 frame seen                     */
static atomic_int g_nonzero = 0;   /* count of samples != 0 — separates "silent" from "tiny"     */
static atomic_int g_samples = 0;
static atomic_int g_odd     = 0;   /* callbacks whose length is not a whole number of S16 samples */

static void on_rx_audio(const unsigned char* pcm, size_t n_bytes, void* user) {
    (void)user;
    if ((n_bytes % 2u) != 0u) atomic_fetch_add(&g_odd, 1);

    const size_t n = n_bytes / 2u;
    int peak = 0, nz = 0;
    for (size_t i = 0; i < n; i++) {
        /* Assembled byte-wise rather than through a short* cast: the callback buffer carries no
         * alignment guarantee, and a misaligned short load is UB the sanitizer gate would catch. */
        const int lo = pcm[2u * i], hi = pcm[2u * i + 1u];
        int v = (int)(short)((unsigned)lo | ((unsigned)hi << 8));
        if (v != 0) nz++;
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    atomic_fetch_add(&g_calls, 1);
    atomic_fetch_add(&g_bytes, (int)n_bytes);
    atomic_fetch_add(&g_samples, (int)n);
    atomic_fetch_add(&g_nonzero, nz);
    /* Peak is a max, not a sum: read-compare-store is safe here because this callback is the only
     * writer and naudio delivers RX audio on ONE dispatch thread. */
    if (peak > atomic_load(&g_peak)) atomic_store(&g_peak, peak);
}

static void reset_detector(void) {
    atomic_store(&g_calls, 0);
    atomic_store(&g_bytes, 0);
    atomic_store(&g_peak, 0);
    atomic_store(&g_nonzero, 0);
    atomic_store(&g_samples, 0);
    atomic_store(&g_odd, 0);
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);   /* the UCRT has no nanosleep */
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* ---- the transmit half -------------------------------------------------------------------------
 *
 * Issue #6's fault — a short rig_stream_write silently dropping TX audio — cannot be provoked
 * unless a client is ACTUALLY transmitting: na_hamlib_bridge's tx_thread calls rig_stream_write
 * only while na_server_tx_owner reports an owner (tools/na_hamlib_bridge.c:366-397). A pure RX
 * probe leaves that whole path unreached, so --tx exists to key up and feed it.
 *
 * The route is public C ABI end to end: na_client_set_tx_inject BEFORE connect (it decides whether
 * the send worker starts, and connect starts the workers once), na_client_set_ptt to key up
 * (injected audio obeys exactly the same keying rules as captured audio), then
 * na_client_inject_tx_audio. No capture device is involved — the NULL backend cannot capture, and
 * this is the reason na_client_set_tx_inject exists. */
#define TX_FRAME_SAMPLES 480    /* 10 ms at 48 kHz mono S16 — the format the bridge negotiates */
/* Naudio-internal injection only: this tone is generated here and consumed by this probe, and
 * never crosses Hamlib. It therefore does NOT have to match the dummy backend, and no longer
 * does — since PR #2116 commit 961093f2 the dummy's tone reaches the bridge through an F32->S16
 * conversion and arrives at 16384. Keep this equal to SELFTEST_TONE_PEAK, not to the dummy. */
#define TX_TONE_PEAK     16383

/* A square wave, so |sample| is exactly `peak` on every sample and the expected value downstream is
 * a derived constant rather than something to approximate. */
static void fill_tone(unsigned char* frame, int samples, int peak) {
    for (int i = 0; i < samples; i++) {
        const int v = (i / 24) % 2 ? -peak : peak;
        frame[2 * i]     = (unsigned char)((unsigned)v & 0xFFu);
        frame[2 * i + 1] = (unsigned char)(((unsigned)v >> 8) & 0xFFu);
    }
}

/* What the SERVER's TX sink actually received. Used only by --selftest: it is the far end of the
 * client's transmit path, and it is what makes the TX half portable-testable without a bridge.
 * Peak, not volume — the server delivers continuous SILENCE frames whenever nobody is transmitting
 * (naudio.h, na_server_tx_audio_cb), so byte count cannot tell a keyed-up client from an idle
 * one and every arm here would pass on a client that transmits nothing. */
static atomic_int g_txsrv_calls = 0;
static atomic_int g_txsrv_bytes = 0;
static atomic_int g_txsrv_peak  = 0;

/* The selftest's inject barrier (issue #48). Set from on_stream_started, which fires only after
 * the session has been registered as a broadcast target — so once this is 1, na_server_inject_audio
 * actually reaches the selftest client. na_server_client_count() cannot say that: it reports the
 * ROSTER, which a client joins at accept, before its handshake has even been read. */
static atomic_int g_srv_streaming = 0;

static void on_selftest_stream_started(const char* client_id, void* user) {
    (void)client_id;
    (void)user;
    atomic_store(&g_srv_streaming, 1);
}

static void on_server_tx_audio(const unsigned char* pcm, size_t n_bytes, void* user) {
    (void)user;
    const size_t n = n_bytes / 2u;
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        const int lo = pcm[2u * i], hi = pcm[2u * i + 1u];
        int v = (int)(short)((unsigned)lo | ((unsigned)hi << 8));
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    atomic_fetch_add(&g_txsrv_calls, 1);
    atomic_fetch_add(&g_txsrv_bytes, (int)n_bytes);
    /* Single dispatch thread (the mixer playback thread), so read-compare-store is safe — the same
     * argument as on_rx_audio's peak. */
    if (peak > atomic_load(&g_txsrv_peak)) atomic_store(&g_txsrv_peak, peak);
}

static void reset_tx_detector(void) {
    atomic_store(&g_txsrv_calls, 0);
    atomic_store(&g_txsrv_bytes, 0);
    atomic_store(&g_txsrv_peak, 0);
}

/* Key up and stream `ms` milliseconds of tone, 10 ms per frame. Returns bytes accepted, or -1 if
 * the client refused to key up. The LEN-RETURN convention means a 0 from inject is not an error —
 * it means not connected, TX inject not enabled, or PTT inactive — so a caller that wants "audio
 * was really sent" must look at the total, which is why it is returned rather than logged. */
static long transmit_tone(na_stream_client* c, int ms, int peak) {
    unsigned char frame[TX_FRAME_SAMPLES * 2];
    fill_tone(frame, TX_FRAME_SAMPLES, peak);
    if (na_client_set_ptt(c, 1) != NA_OK) return -1;
    long total = 0;
    for (int t = 0; t < ms / 10; t++) {
        const int w = na_client_inject_tx_audio(c, frame, (int)sizeof frame);
        if (w < 0) return -1;
        total += w;
        sleep_ms(10);
    }
    return total;
}

/* Build a configured, connected UDP client. The bridge always selects a UDP reliability profile,
 * so a client left on the ABI-default TCP transport is refused at connect with SO_ERROR=61 — that
 * is not a bridge fault, it is the documented default (CLAUDE.md Learning 7). */
static na_stream_client* attach(const char* host, int port, const char* who, int tx_inject) {
    char err[256];
    err[0] = '\0';
    na_stream_client* c = na_client_create(NA_CLIENT_BACKEND_NULL, host, port, who);
    if (c == NULL) {
        fprintf(stderr, "na_bridge_probe: na_client_create failed (%s)\n",
                na_strerror(na_last_error()));
        return NULL;
    }
    /* Before connect, without exception: this setter decides whether the send worker is started,
     * and connect starts the workers exactly once (naudio.h, na_client_set_tx_inject). */
    if (tx_inject && na_client_set_tx_inject(c, 1) != NA_OK) {
        fprintf(stderr, "na_bridge_probe: na_client_set_tx_inject rejected (%s)\n",
                na_strerror(na_last_error()));
        na_client_destroy(c);
        return NULL;
    }
    /* The profile selects the transport too, so it is NOT paired with na_client_set_transport —
     * pairing them would make this probe an ordering test by accident (c_client_profile.c owns
     * that claim). UDP_WAN matches the bridge's own default (-R wan). */
    if (na_client_set_reliability_profile(c, NA_RELIABILITY_UDP_WAN) != NA_OK) {
        fprintf(stderr, "na_bridge_probe: reliability profile rejected\n");
        na_client_destroy(c);
        return NULL;
    }
    na_client_set_playback_device(c, 0);   /* REQUIRED for RX even on the NULL backend */
    na_client_set_auto_reconnect(c, 0);    /* a reconnect mid-sample would reset the counters */
    na_client_set_audio_cb(c, on_rx_audio, NULL);
    if (na_client_connect(c, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "na_bridge_probe: connect to %s:%d failed (%s)\n", host, port, err);
        na_client_destroy(c);
        return NULL;
    }
    return c;
}

/* One machine-readable line, greppable by a driver script. */
static void report(const char* label, double secs) {
    const int bytes = atomic_load(&g_bytes), calls = atomic_load(&g_calls);
    const int samples = atomic_load(&g_samples), nz = atomic_load(&g_nonzero);
    printf("RESULT %s bytes=%d calls=%d rate=%.0f peak_abs_sample=%d nonzero=%d/%d odd_calls=%d\n",
           label, bytes, calls, secs > 0.0 ? (double)bytes / secs : 0.0,
           atomic_load(&g_peak), nz, samples, atomic_load(&g_odd));
    fflush(stdout);
}

/* ---- self-test ---------------------------------------------------------------------------------
 *
 * Proves the detector DISCRIMINATES, which is the only property the bridge arm relies on. Two arms
 * that must disagree: no constant-returning detector can satisfy both a content arm demanding a
 * non-zero peak and a silence arm demanding exactly zero, so a dead instrument fails one of them
 * instead of agreeing with both and reading as corroboration (Learning 58).
 *
 * The silence arm is the one that matters. It is the committed form of the negative control that
 * issue #15's own comment calls the single highest-value line in the whole harness, and it is
 * cheap here precisely because a server can be told to inject zeros. */
#define SELFTEST_FRAME_SAMPLES 480              /* 10 ms of 48 kHz mono S16 */
#define SELFTEST_FRAMES        60
/* Self-injected and self-checked; never crosses Hamlib, so this is not the dummy backend's
 * amplitude and must not be "corrected" to it. Through the bridge the dummy now arrives at
 * 16384 (F32->S16 conversion, PR #2116 commit 961093f2) — which is exactly why --expect content
 * asserts peak > 0 rather than an exact value. */
#define SELFTEST_TONE_PEAK     16383

static int selftest_arm(na_audio_server* srv, na_stream_client* cli, int tone, const char* label) {
    unsigned char frame[SELFTEST_FRAME_SAMPLES * 2];
    for (int i = 0; i < SELFTEST_FRAME_SAMPLES; i++) {
        /* A square wave, not a sine: its |sample| is exactly SELFTEST_TONE_PEAK on every sample,
         * so the expected peak is a derived constant rather than something to approximate. */
        const int v = tone ? ((i / 24) % 2 ? -SELFTEST_TONE_PEAK : SELFTEST_TONE_PEAK) : 0;
        frame[2 * i]     = (unsigned char)((unsigned)v & 0xFFu);
        frame[2 * i + 1] = (unsigned char)(((unsigned)v >> 8) & 0xFFu);
    }
    (void)cli;

    reset_detector();
    for (int f = 0; f < SELFTEST_FRAMES; f++) {
        if (na_server_inject_audio(srv, frame, (int)sizeof frame) != NA_OK) {
            fprintf(stderr, "na_bridge_probe: selftest inject failed on arm %s\n", label);
            return EXIT_HARNESS;
        }
        sleep_ms(10);
    }
    sleep_ms(200);   /* let the tail drain through the jitter buffer before reading the detector */
    report(label, (double)SELFTEST_FRAMES * 0.01);

    const int peak = atomic_load(&g_peak);
    const int calls = atomic_load(&g_calls);
    const int odd = atomic_load(&g_odd);
    if (calls == 0) {
        fprintf(stderr, "FAIL selftest/%s: no audio reached the probe at all\n", label);
        return EXIT_HARNESS;   /* nothing arrived: the harness failed, not the detector */
    }
    if (odd != 0) {
        fprintf(stderr, "FAIL selftest/%s: %d callbacks were not whole S16 samples\n", label, odd);
        return EXIT_UNMET;
    }
    if (tone && peak != SELFTEST_TONE_PEAK) {
        fprintf(stderr, "FAIL selftest/%s: peak %d, expected exactly %d — the detector does not "
                        "see content it is being sent\n", label, peak, SELFTEST_TONE_PEAK);
        return EXIT_UNMET;
    }
    if (!tone && peak != 0) {
        fprintf(stderr, "FAIL selftest/%s: peak %d on injected silence — the detector reports "
                        "content that was never sent\n", label, peak);
        return EXIT_UNMET;
    }
    return EXIT_MET;
}

/* The TX half of the self-test, and the reason --tx is not a compiled-but-never-executed mode.
 * --tx is exercised for real only by the bridge fault arm, which needs a hand-built streaming
 * libhamlib and so runs on ONE developer machine; without this, every other platform in CI would
 * compile the transmit path and never run a line of it — exactly the gap this file's --selftest was
 * written to close for the RX half.
 *
 * Two arms that must DISAGREE, for the same reason the RX pair must (Learning 58): the server hands
 * its TX sink continuous silence whenever nobody is transmitting, so an arm that only checked
 * "frames arrived" would pass identically on a client that never keys up. PTT off must give peak 0
 * and PTT on must give exactly TX_TONE_PEAK. */
static int selftest_tx(na_audio_server* srv, na_stream_client* cli) {
    /* Keyed DOWN first. na_client_inject_tx_audio is gated on PTT, so this must accept nothing and
     * the server's sink must stay silent — the control that stops a constant-returning detector, or
     * a server that simply echoes anything offered, from satisfying the arm below. */
    reset_tx_detector();
    unsigned char frame[TX_FRAME_SAMPLES * 2];
    fill_tone(frame, TX_FRAME_SAMPLES, TX_TONE_PEAK);
    na_client_set_ptt(cli, 0);
    long unkeyed = 0;
    for (int t = 0; t < 30; t++) {
        const int w = na_client_inject_tx_audio(cli, frame, (int)sizeof frame);
        if (w > 0) unkeyed += w;
        sleep_ms(10);
    }
    sleep_ms(200);
    printf("RESULT tx-unkeyed accepted=%ld srv_calls=%d srv_bytes=%d srv_peak=%d\n",
           unkeyed, atomic_load(&g_txsrv_calls), atomic_load(&g_txsrv_bytes),
           atomic_load(&g_txsrv_peak));
    fflush(stdout);
    if (atomic_load(&g_txsrv_peak) != 0) {
        fprintf(stderr, "FAIL selftest/tx-unkeyed: the server's TX sink saw peak %d from a client "
                        "with PTT OFF — injected audio is not obeying the keying gate\n",
                atomic_load(&g_txsrv_peak));
        return EXIT_UNMET;
    }

    /* Keyed UP: the same frames must now reach the server's TX sink at full amplitude. */
    reset_tx_detector();
    const long sent = transmit_tone(cli, 500, TX_TONE_PEAK);
    if (sent < 0) {
        fprintf(stderr, "na_bridge_probe: selftest could not key up or inject TX audio\n");
        return EXIT_HARNESS;
    }
    sleep_ms(300);   /* let the last frames cross the wire and the mixer */
    const int peak = atomic_load(&g_txsrv_peak), calls = atomic_load(&g_txsrv_calls);
    printf("RESULT tx-keyed accepted=%ld srv_calls=%d srv_bytes=%d srv_peak=%d\n",
           sent, calls, atomic_load(&g_txsrv_bytes), peak);
    fflush(stdout);
    if (sent == 0) {
        fprintf(stderr, "FAIL selftest/tx-keyed: the client accepted 0 TX bytes with PTT on — "
                        "na_client_set_tx_inject or the keying gate is not doing what it says\n");
        return EXIT_UNMET;
    }
    if (calls == 0) {
        fprintf(stderr, "na_bridge_probe: selftest TX sink never fired — the server delivered no "
                        "TX frames at all, so nothing can be concluded about keying\n");
        return EXIT_HARNESS;
    }
    if (peak != TX_TONE_PEAK) {
        fprintf(stderr, "FAIL selftest/tx-keyed: the server's TX sink peaked at %d, expected "
                        "exactly %d — TX audio is not arriving intact\n", peak, TX_TONE_PEAK);
        return EXIT_UNMET;
    }
    na_client_set_ptt(cli, 0);
    return EXIT_MET;
}

static int run_selftest(void) {
    char err[256];
    printf("na_bridge_probe --selftest: proving the detector tells content from silence\n");

    na_audio_server* srv = na_server_create(NA_SERVER_BACKEND_NULL, 0);
    if (srv == NULL) {
        fprintf(stderr, "na_bridge_probe: na_server_create failed (%s)\n",
                na_strerror(na_last_error()));
        return EXIT_HARNESS;
    }
    /* Mirror what na_hamlib_bridge configures: mono 48 kHz S16 over the UDP WAN profile. */
    if (na_server_set_audio_format(srv, 48000, 16, 1) != NA_OK ||
        na_server_set_reliability_profile(srv, NA_RELIABILITY_UDP_WAN) != NA_OK) {
        fprintf(stderr, "na_bridge_probe: selftest server config rejected\n");
        na_server_destroy(srv);
        return EXIT_HARNESS;
    }
    /* The far end of the client's transmit path. NULL-backend only, and before start. */
    if (na_server_set_tx_audio_cb(srv, on_server_tx_audio, NULL) != NA_OK) {
        fprintf(stderr, "na_bridge_probe: selftest TX sink rejected\n");
        na_server_destroy(srv);
        return EXIT_HARNESS;
    }
    /* The inject barrier — also before start, like every other server callback. */
    na_server_callbacks scbs;
    memset(&scbs, 0, sizeof scbs);
    scbs.struct_size = sizeof scbs;
    scbs.on_stream_started = on_selftest_stream_started;
    if (na_server_set_callbacks(srv, &scbs, NULL) != NA_OK) {
        fprintf(stderr, "na_bridge_probe: selftest server callbacks rejected\n");
        na_server_destroy(srv);
        return EXIT_HARNESS;
    }
    if (na_server_start(srv, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "na_bridge_probe: selftest server start failed (%s)\n", err);
        na_server_destroy(srv);
        return EXIT_HARNESS;
    }

    const int port = na_server_port(srv);
    na_stream_client* cli = attach("127.0.0.1", port, "bridge-probe-selftest", 1);
    if (cli == NULL) {
        na_server_stop(srv);
        na_server_destroy(srv);
        return EXIT_HARNESS;
    }
    /* This client has to be a BROADCAST TARGET before injecting, or the first frames are broadcast
     * to nobody and the content arm fails for a reason that is not the detector's.
     *
     * This gate used to poll na_server_client_count() >= 1 and its comment claimed exactly the
     * guarantee above — which that call does not give (issue #48): it reports the ROSTER, which a
     * client joins at accept, before the handshake. on_stream_started is the barrier that means
     * what the comment says.
     *
     * MEASURED, because the wrong gate costs nothing until it does. On this loopback the old gate
     * delivered 60 of 60 frames in 5 runs — the window is too narrow to lose anything, which is
     * exactly why it went unnoticed. Forcing it open with a 300 ms sleep before addTarget (the
     * technique tests/net/test_server.cpp already uses for issue #46):
     *
     *     old gate (na_server_client_count), 300 ms window:  36, 37, 37 frames of 60 — and the
     *                                                        selftest still reported OK
     *     new gate (on_stream_started),      300 ms window:  60, 60, 60
     *
     * So the arm was passing while silently dropping 40% of the audio it claims to verify: it
     * asserts calls > 0, not calls == 60, and was carried by selftest_arm's 60-frame retry loop
     * underneath the gate rather than by the gate. */
    for (int i = 0; i < 200 && atomic_load(&g_srv_streaming) == 0; i++) sleep_ms(10);
    int rc = EXIT_MET;
    if (atomic_load(&g_srv_streaming) == 0) {
        fprintf(stderr, "na_bridge_probe: selftest client never became a broadcast target\n");
        rc = EXIT_HARNESS;
    } else {
        rc = selftest_arm(srv, cli, 1, "content");
        if (rc == EXIT_MET) rc = selftest_arm(srv, cli, 0, "silence");
        if (rc == EXIT_MET) rc = selftest_tx(srv, cli);
    }

    na_client_disconnect(cli);
    na_client_destroy(cli);
    na_server_stop(srv);
    na_server_destroy(srv);
    if (rc == EXIT_MET)
        printf("na_bridge_probe --selftest: OK (RX content/silence separated, TX keying honoured)\n");
    return rc;
}

/* ---- attach mode ------------------------------------------------------------------------------ */

static void usage(void) {
    fprintf(stderr,
        "usage: na_bridge_probe [--host H] [--port N] [--seconds S]\n"
        "                       [--expect content|silence] [--min-rate BYTES_PER_SEC] [--tx]\n"
        "       na_bridge_probe --selftest\n"
        "\n"
        "  --expect content   require peak |sample| > 0   (a silent stream FAILS)\n"
        "  --expect silence   require peak |sample| == 0  (the negative control)\n"
        "  --min-rate N       also require the delivered byte rate >= N. OFF by default:\n"
        "                     a correct run against a dummy backend sits near 83%% of nominal,\n"
        "                     so any threshold tight enough to be useful fires on healthy runs.\n"
        "  --tx               ALSO key up and inject TX tone for the sampling window, making this\n"
        "                     client the server's TX owner. Required to reach na_hamlib_bridge's\n"
        "                     rig_stream_write path at all — it writes only while an owner exists.\n"
        "\n"
        "exit: 0 expectation met, 1 expectation NOT met, 2 the probe could not run\n");
}

int main(int argc, char** argv) {
    const char* host = "127.0.0.1";
    int port = 4533, seconds = 8;
    const char* expect = NULL;
    long min_rate = 0;
    int tx = 0;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--selftest") == 0) return run_selftest();
        if (strcmp(a, "--tx") == 0) { tx = 1; continue; }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(); return EXIT_MET; }
        if (i + 1 >= argc) { usage(); return EXIT_HARNESS; }
        if      (strcmp(a, "--host") == 0)     host = argv[++i];
        else if (strcmp(a, "--port") == 0)     port = atoi(argv[++i]);
        else if (strcmp(a, "--seconds") == 0)  seconds = atoi(argv[++i]);
        else if (strcmp(a, "--expect") == 0)   expect = argv[++i];
        else if (strcmp(a, "--min-rate") == 0) min_rate = strtol(argv[++i], NULL, 10);
        else { usage(); return EXIT_HARNESS; }
    }
    if (expect != NULL && strcmp(expect, "content") != 0 && strcmp(expect, "silence") != 0) {
        usage();
        return EXIT_HARNESS;
    }
    if (seconds <= 0 || port <= 0 || port > 65535) { usage(); return EXIT_HARNESS; }

    reset_detector();
    na_stream_client* c = attach(host, port, "bridge-probe", tx);
    if (c == NULL) return EXIT_HARNESS;
    printf("na_bridge_probe: attached to %s:%d, sampling %d s%s\n", host, port, seconds,
           tx ? " (transmitting)" : "");
    fflush(stdout);
    if (tx) {
        /* Transmitting IS the sampling window here — transmit_tone paces itself at 10 ms a frame,
         * so it occupies the same wall clock the RX detector is accumulating over. */
        const long sent = transmit_tone(c, seconds * 1000, TX_TONE_PEAK);
        if (sent <= 0) {
            fprintf(stderr, "na_bridge_probe: --tx could not key up or the client accepted 0 bytes "
                            "(%ld) — nothing was transmitted, so the TX path was never exercised\n",
                    sent);
            na_client_disconnect(c);
            na_client_destroy(c);
            return EXIT_HARNESS;
        }
        printf("RESULT tx-injected bytes=%ld\n", sent);
        fflush(stdout);
    } else {
        sleep_ms(seconds * 1000);
    }

    report(expect != NULL ? expect : "sample", (double)seconds);
    const int peak = atomic_load(&g_peak);
    const int calls = atomic_load(&g_calls);
    const int bytes = atomic_load(&g_bytes);
    na_client_disconnect(c);
    na_client_destroy(c);

    if (calls == 0) {
        fprintf(stderr, "FAIL: no RX audio callbacks in %d s — nothing is being delivered\n",
                seconds);
        return EXIT_UNMET;
    }
    if (expect != NULL && strcmp(expect, "content") == 0 && peak == 0) {
        fprintf(stderr, "FAIL: %d bytes over %d callbacks and every sample is zero. This is what a\n"
                        "  silent source looks like, and no byte count can tell it from a healthy\n"
                        "  one — see this file's header for the measurement.\n", bytes, calls);
        return EXIT_UNMET;
    }
    if (expect != NULL && strcmp(expect, "silence") == 0 && peak != 0) {
        fprintf(stderr, "FAIL: expected silence, saw peak |sample| %d\n", peak);
        return EXIT_UNMET;
    }
    if (min_rate > 0 && (double)bytes / (double)seconds < (double)min_rate) {
        fprintf(stderr, "FAIL: %.0f B/s is below the requested floor of %ld B/s\n",
                (double)bytes / (double)seconds, min_rate);
        return EXIT_UNMET;
    }
    return EXIT_MET;
}
