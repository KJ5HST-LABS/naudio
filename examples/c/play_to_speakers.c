// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio examples — play a network audio stream to your speakers (C).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// A dead-simple client: connect to a network-audio server and play whatever it
// is streaming on your local speakers. It uses only the public `naudio.h` C ABI
// (it never names a C++ type and links only the shared `naudio` library), so it
// is also a worked example of writing a full streaming client from plain C.
//
// The playback is done by naudio itself: you pick a local output device and the
// library renders the received audio to it. By default the client picks your
// first output device and starts playing — no flags required:
//
//     play_to_speakers --host 127.0.0.1 --port 4533
//
// Pair it with the demo source to hear something without a radio or a second
// machine (see examples/server):
//
//     na_audio_source --test-tone --port 4533      # in one terminal
//     play_to_speakers --port 4533                 # in another — you hear a tone
//
// Use `--list-devices` to see the output-device ids, then `--playback-id N` to
// choose one. `--backend null` runs hardware-free (the audio is still received,
// it just is not played) — handy on a headless box, in CI, or to verify a server
// end to end without a sound card.
//
// Output: the machine-readable `RESULT ...` line goes to STDOUT; all human logs
// (events, the throughput meter, the summary) go to STDERR.

#define _POSIX_C_SOURCE 200809L  // nanosleep + clock_gettime(CLOCK_MONOTONIC) on glibc

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "naudio.h"  // the only audio header — pure C, no C++

// ---- RX stats: written on the receive worker thread, read from main ----------------------
// The hot-path audio callback fires on naudio's internal receive worker thread (one call per
// RX frame); main polls these counters for a live meter. The counters are atomic so the meter
// reads them without a data race. Only ONE worker thread invokes the RX sink, so first_hex is
// written by a single thread, and main reads it only after na_client_disconnect() has joined
// that thread — so there is no race on the string either.
struct rx_stats {
    atomic_llong frames;
    atomic_llong bytes;
    atomic_llong nonzero_bytes;  // a frame of underrun silence is all-zero; this proves content arrived
    atomic_int   have_first;
    char         first_hex[24];  // first up-to-8 bytes of the first frame, hex (a wire fingerprint)
};

// The hot-path RX PCM sink (na_audio_cb). `user` is the rx_stats passed at registration.
static void on_rx_audio(const unsigned char* pcm, size_t n, void* user) {
    struct rx_stats* s = (struct rx_stats*)user;
    atomic_fetch_add(&s->frames, 1);
    atomic_fetch_add(&s->bytes, (long long)n);
    long long nz = 0;
    for (size_t i = 0; i < n; i++)
        if (pcm[i] != 0) nz++;
    atomic_fetch_add(&s->nonzero_bytes, nz);
    if (!atomic_load(&s->have_first) && n > 0) {
        size_t k = n < 8 ? n : 8;
        for (size_t i = 0; i < k; i++)
            sprintf(s->first_hex + i * 2, "%02x", pcm[i]);
        s->first_hex[k * 2] = '\0';
        atomic_store(&s->have_first, 1);
    }
}

// ---- lifecycle / roster events (na_client_callbacks) -------------------------------------
// All fire on naudio's dispatch thread; we only print (short + non-blocking, per the contract).
static void ev_connected(const char* id, const char* addr, void* u) {
    (void)u;
    fprintf(stderr, "[client] connected id=%s addr=%s\n", id ? id : "", addr ? addr : "");
}
static void ev_disconnected(const char* id, void* u) {
    (void)u;
    fprintf(stderr, "[client] disconnected id=%s\n", id ? id : "");
}
static void ev_stream_started(const char* id, void* u) {
    (void)u;
    fprintf(stderr, "[client] stream started id=%s\n", id ? id : "");
}
static void ev_stream_stopped(const char* id, void* u) {
    (void)u;
    fprintf(stderr, "[client] stream stopped id=%s\n", id ? id : "");
}
static void ev_error(const char* id, const char* msg, void* u) {
    (void)u;
    fprintf(stderr, "[client] ERROR id=%s: %s\n", id ? id : "", msg ? msg : "");
}
static void ev_reconnecting(const char* id, int attempt, int max_attempts, void* u) {
    (void)id; (void)u;
    fprintf(stderr, "[client] reconnecting attempt %d/%d\n", attempt, max_attempts);
}
static void ev_reconnected(const char* id, void* u) {
    (void)id; (void)u;
    fprintf(stderr, "[client] reconnected\n");
}
static void ev_clients_update(int count, int max_clients, const char* tx_owner,
                              const char* const* ids, int n_ids, void* u) {
    (void)ids; (void)n_ids; (void)u;
    fprintf(stderr, "[roster] clients=%d/%d txOwner=%s\n", count, max_clients,
            (tx_owner && tx_owner[0]) ? tx_owner : "(none)");
}

// ---- Ctrl-C: flip a flag the monitor loop polls (na_client_disconnect joins workers) -----
static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---- device listing / default-playback selection (SYSTEM backend) ------------------------
#define MAX_DEVICES 128

// Print every playback-capable device; returns 0 on success, 1 on failure.
static int list_devices(void) {
    na_context* ctx = na_context_create();
    if (ctx == NULL) {
        fprintf(stderr, "error: na_context_create: %s\n", na_strerror(na_last_error()));
        return 1;
    }
    na_device devs[MAX_DEVICES];
    int n = na_enumerate(ctx, devs, MAX_DEVICES);
    if (n < 0) {
        fprintf(stderr, "error: na_enumerate: %s\n", na_strerror(na_last_error()));
        na_context_destroy(ctx);
        return 1;
    }
    printf("Playback-capable devices (use the id with --playback-id):\n");
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (devs[i].capability == NA_CAP_PLAYBACK || devs[i].capability == NA_CAP_DUPLEX) {
            printf("  [%2d] %-40s %s%s\n", devs[i].playback_backend_id, devs[i].name,
                   devs[i].host_api, devs[i].is_virtual ? "  (virtual)" : "");
            shown++;
        }
    }
    if (shown == 0)
        printf("  (none — use --backend null to run without a playback device)\n");
    na_context_destroy(ctx);
    return 0;
}

// Return the first playback-capable device's id, or -1 if there is none.
static int default_playback_id(void) {
    na_context* ctx = na_context_create();
    if (ctx == NULL) return -1;
    na_device devs[MAX_DEVICES];
    int n = na_enumerate(ctx, devs, MAX_DEVICES);
    int id = -1;
    for (int i = 0; i < n; i++) {
        if (devs[i].capability == NA_CAP_PLAYBACK || devs[i].capability == NA_CAP_DUPLEX) {
            id = devs[i].playback_backend_id;
            break;
        }
    }
    na_context_destroy(ctx);
    return id;
}

static void usage(void) {
    fprintf(stderr,
        "usage: play_to_speakers [--host H] [--port N] [--name S] [--playback-id N]\n"
        "                        [--transport tcp|udp] [--seconds N] [--backend system|null]\n"
        "       play_to_speakers --list-devices\n\n"
        "  --host H          server host (default 127.0.0.1)\n"
        "  --port N          server port (default 4533)\n"
        "  --name S          this client's name in the server roster (default na-c-client)\n"
        "  --playback-id N   output device id to play on (default: first output device)\n"
        "  --transport T     tcp (default) | udp\n"
        "  --seconds N       run time; 0 = until Ctrl-C (default 0)\n"
        "  --backend B       system (default; plays RX to the output device) or\n"
        "                    null (hardware-free; RX is received but not played)\n"
        "  --list-devices    print output device ids, then exit\n"
        "  -h, --help        print this message\n");
}

int main(int argc, char** argv) {
    const char* host        = "127.0.0.1";
    int         port        = 4533;  // AudioStreamConfig default audio port
    const char* name        = "na-c-client";
    na_client_backend backend = NA_CLIENT_BACKEND_SYSTEM;  // default: play to speakers
    int         playback_id = -1;    // -1 => auto-select the first output device (SYSTEM)
    na_transport transport  = NA_TRANSPORT_TCP;
    long long   seconds     = 0;     // 0 = until Ctrl-C

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        #define NEED_VAL(opt) (i + 1 < argc ? argv[++i] : (fprintf(stderr, "error: %s needs a value\n", opt), exit(2), ""))
        if      (strcmp(a, "--host") == 0)        host = NEED_VAL("--host");
        else if (strcmp(a, "--port") == 0)        port = atoi(NEED_VAL("--port"));
        else if (strcmp(a, "--name") == 0)        name = NEED_VAL("--name");
        else if (strcmp(a, "--playback-id") == 0) playback_id = atoi(NEED_VAL("--playback-id"));
        else if (strcmp(a, "--seconds") == 0)     seconds = atoll(NEED_VAL("--seconds"));
        else if (strcmp(a, "--list-devices") == 0) return list_devices();
        else if (strcmp(a, "--backend") == 0) {
            const char* b = NEED_VAL("--backend");
            if      (strcmp(b, "system") == 0) backend = NA_CLIENT_BACKEND_SYSTEM;
            else if (strcmp(b, "null") == 0)   backend = NA_CLIENT_BACKEND_NULL;
            else { fprintf(stderr, "error: invalid --backend '%s' (system|null)\n", b); return 2; }
        }
        else if (strcmp(a, "--transport") == 0) {
            const char* t = NEED_VAL("--transport");
            if      (strcmp(t, "tcp") == 0) transport = NA_TRANSPORT_TCP;
            else if (strcmp(t, "udp") == 0) transport = NA_TRANSPORT_UDP;
            else { fprintf(stderr, "error: invalid --transport '%s' (tcp|udp)\n", t); return 2; }
        }
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", a); usage(); return 2; }
        #undef NEED_VAL
    }

    // Resolve the playback device up front so we can fail early with guidance. The SYSTEM
    // backend needs a real output device; the NULL backend accepts any id (it plays nothing).
    if (backend == NA_CLIENT_BACKEND_SYSTEM && playback_id < 0) {
        playback_id = default_playback_id();
        if (playback_id < 0) {
            fprintf(stderr, "error: no output device found. Run --list-devices, pass "
                            "--playback-id N, or use --backend null to run without one.\n");
            return 1;
        }
        fprintf(stderr, "[client] auto-selected output device id=%d "
                        "(run --list-devices to choose another)\n", playback_id);
    } else if (playback_id < 0) {
        playback_id = 0;  // NULL backend: any id is accepted
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    na_stream_client* client = na_client_create(backend, host, port, name);
    if (client == NULL) {
        fprintf(stderr, "error: na_client_create: %s\n", na_strerror(na_last_error()));
        return 1;
    }

    // Configure BEFORE connect — the worker threads read callbacks/config once streaming starts.
    struct rx_stats stats;
    memset(&stats, 0, sizeof stats);

    na_client_callbacks cbs;
    memset(&cbs, 0, sizeof cbs);
    cbs.on_connected      = ev_connected;
    cbs.on_disconnected   = ev_disconnected;
    cbs.on_stream_started = ev_stream_started;
    cbs.on_stream_stopped = ev_stream_stopped;
    cbs.on_error          = ev_error;
    cbs.on_reconnecting   = ev_reconnecting;
    cbs.on_reconnected    = ev_reconnected;
    cbs.on_clients_update = ev_clients_update;
    na_client_set_callbacks(client, &cbs, NULL);
    na_client_set_audio_cb(client, on_rx_audio, &stats);
    na_client_set_transport(client, transport);
    na_client_set_playback_device(client, playback_id);  // REQUIRED for RX

    const char* backend_name   = backend == NA_CLIENT_BACKEND_SYSTEM ? "system" : "null";
    const char* transport_name = transport == NA_TRANSPORT_TCP ? "tcp" : "udp";

    char err[256];
    if (na_client_connect(client, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "error: connect to %s:%d failed: %s\n", host, port, err);
        na_client_destroy(client);
        return 1;
    }
    fprintf(stderr, "connected to %s:%d (backend=%s, transport=%s); %s",
            host, port, backend_name, transport_name,
            backend == NA_CLIENT_BACKEND_SYSTEM ? "playing" : "receiving");
    if (seconds > 0) fprintf(stderr, " for %llds", seconds);
    else             fprintf(stderr, " until Ctrl-C");
    fprintf(stderr, " ...\n");

    // Monitor loop: a per-second throughput meter so you can see audio arriving.
    const long long start    = now_ms();
    const long long deadline = seconds > 0 ? start + seconds * 1000 : 0;
    long long prev_bytes = 0, prev_ms = start;
    while (!g_stop && (deadline == 0 || now_ms() < deadline)) {
        sleep_ms(200);
        const long long t = now_ms();
        if (t - prev_ms < 1000) continue;
        const long long b   = atomic_load(&stats.bytes);
        const long long bps = (b - prev_bytes) * 1000 / (t - prev_ms);
        fprintf(stderr, "  t=%2llds  rx=%-9lld B  frames=%-7lld  %lld B/s  conn=%d\n",
                (t - start) / 1000, b, atomic_load(&stats.frames), bps,
                na_client_is_connected(client));
        prev_bytes = b;
        prev_ms = t;
    }

    const int connected_end = na_client_is_connected(client);
    na_client_disconnect(client);  // stop reconnection + join workers; no callback after this

    const long long elapsed = now_ms() - start;
    const long long rx_frames = atomic_load(&stats.frames);
    const long long rx_bytes  = atomic_load(&stats.bytes);
    const long long rx_nonzero = atomic_load(&stats.nonzero_bytes);
    const long long avg_bps   = elapsed > 0 ? rx_bytes * 1000 / elapsed : 0;

    // PASS = received real (non-silent) audio. A bounded run (--seconds) that got nothing FAILS
    // (exit 1); an interactive (Ctrl-C) run always exits 0 — you chose when to stop.
    const int gate = seconds > 0;
    const int pass = rx_frames > 0 && rx_nonzero > 0;

    fprintf(stderr,
            "\n=== play to speakers — summary ===\n"
            "  server         : %s:%d (%s)\n"
            "  rx             : %lld frames, %lld bytes (%lld non-zero)\n"
            "  throughput     : avg %lld B/s over %lld ms\n"
            "  first_frame_hex: %s\n"
            "  connected      : %s\n",
            host, port, transport_name, rx_frames, rx_bytes, rx_nonzero,
            avg_bps, elapsed, atomic_load(&stats.have_first) ? stats.first_hex : "(none)",
            connected_end ? "yes" : "no");

    // Machine-readable result on STDOUT (handy for a scripted check).
    printf("RESULT status=%s connected=%s rx_frames=%lld rx_bytes=%lld nonzero_bytes=%lld first_frame_hex=%s avg_bps=%lld\n",
           pass ? "PASS" : "FAIL", connected_end ? "true" : "false",
           rx_frames, rx_bytes, rx_nonzero,
           atomic_load(&stats.have_first) ? stats.first_hex : "", avg_bps);

    na_client_destroy(client);
    return (gate && !pass) ? 1 : 0;
}
