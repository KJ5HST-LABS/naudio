// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio examples — demo audio source.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// A tiny streaming server so you can run one of the example clients and actually
// HEAR something — no radio, no second machine. It speaks the same network-audio
// protocol the clients expect, over the public `naudio.h` C ABI only (it never
// names a C++ type and links only the shared `naudio` library).
//
// Two ways to run it:
//
//   1. Capture mode (default) — broadcast a LOCAL audio input.
//        na_audio_source --capture-id 3 --port 4533
//      naudio opens the chosen PortAudio input device, captures live audio, and
//      broadcasts it to every connected client. Point a client at it and the
//      captured audio plays on the client's speakers. The input can be a real
//      microphone / line-in, or a virtual loopback (BlackHole on macOS, a
//      PulseAudio/PipeWire null sink on Linux) so you can stream whatever the
//      machine is already playing. Run `--list-devices` first to find the id;
//      with no `--capture-id` the first capture-capable device is used.
//
//   2. Test-tone mode — a hardware-free deterministic tone.
//        na_audio_source --test-tone --port 4533
//      No audio device is opened at all. The server emits a fixed sawtooth tone,
//      so a connected client hears a steady buzz and automated checks get a
//      byte-for-byte reproducible stream. Use this when there is no capture
//      device (CI, a headless box) or to verify a client end to end.
//
// Output: the machine-readable `LISTENING port=N` line goes to STDOUT (handy when
// `--port 0` asks the OS for an ephemeral port); all human logs go to STDERR.

#define _POSIX_C_SOURCE 200809L  // nanosleep + clock_gettime(CLOCK_MONOTONIC) on glibc

#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "naudio.h"  // the only audio header — pure C, no C++

// ---- Audio format ------------------------------------------------------------------------
// Defaults match AudioStreamConfig's (48 kHz / 16-bit / stereo). --rate/--channels declare a
// lower server-wide format via na_server_set_audio_format — 12 kHz mono is 192 kbps on the
// wire against 1.536 Mbps for the default, which is what fits a constrained link (issue #91).
// naudio does not resample: in capture mode the device is opened at exactly this format (the
// start fails loudly if it cannot be), and in test-tone mode the frames are generated in it.
#define DEFAULT_RATE  48000
#define BITS          16
#define DEFAULT_CHANNELS 2
#define FRAME_MS      20
// Frame geometry is computed from the effective rate/channels in main(): at the defaults,
// 960 sample-frames -> 1920 int16 values -> 3840 bytes per 20 ms frame.

// ---- Test-tone sawtooth ------------------------------------------------------------------
// A deterministic 16-bit sawtooth, regenerated identically for every frame so the stream is
// byte-for-byte reproducible no matter when a client connects: each frame's first samples are
// always -15000, -14743, -14486, -14229, ... which is `68 c5 69 c6 6a c7 6b c8` as little-
// endian PCM. An interop client can assert that exact first frame. The phase resets to 0 at
// every frame, so those first 8 bytes are the same at EVERY rate/channel setting — the
// fingerprint identifies the tone, not the format (the minimum --rate keeps a frame >= 8 bytes).
#define SAW_STEP  257
#define SAW_MOD   30000
#define SAW_BIAS  15000

// Fill `frame` (n_int16 * 2 bytes) with one period-reset sawtooth frame, little-endian.
static void fill_tone_frame(unsigned char* frame, int n_int16) {
    long phase = 0;  // (j * SAW_STEP) % SAW_MOD, advanced without overflow
    for (int j = 0; j < n_int16; j++) {
        int16_t  sample = (int16_t)(phase - SAW_BIAS);
        uint16_t u      = (uint16_t)sample;
        frame[j * 2]     = (unsigned char)(u & 0xFF);          // low byte first (LE)
        frame[j * 2 + 1] = (unsigned char)((u >> 8) & 0xFF);
        phase += SAW_STEP;
        if (phase >= SAW_MOD) phase -= SAW_MOD;
    }
}

// First-frame fingerprint as lowercase hex (the first 8 bytes), for a self-check log.
static void tone_first_frame_hex(const unsigned char* frame, char* out /* >= 17 bytes */) {
    for (int i = 0; i < 8; i++) sprintf(out + i * 2, "%02x", frame[i]);
    out[16] = '\0';
}

// ---- roster tracking ---------------------------------------------------------------------
// Lifecycle callbacks fire on the server's dispatch thread; main only reads these counters.
static atomic_int g_clients = 0;       // currently connected
static atomic_int g_peak_clients = 0;  // high-water mark, for the summary

static void on_started(int port, void* user) {
    (void)user;
    fprintf(stderr, "[source] listening on port %d\n", port);
}
static void on_client_connected(const char* id, const char* addr, void* user) {
    (void)user;
    int n = atomic_fetch_add(&g_clients, 1) + 1;
    int peak = atomic_load(&g_peak_clients);
    while (n > peak && !atomic_compare_exchange_weak(&g_peak_clients, &peak, n)) { /* retry */ }
    fprintf(stderr, "[source] client connected id=%s addr=%s (now %d)\n",
            id ? id : "", addr ? addr : "", n);
}
static void on_client_disconnected(const char* id, void* user) {
    (void)user;
    int n = atomic_fetch_sub(&g_clients, 1) - 1;
    fprintf(stderr, "[source] client disconnected id=%s (now %d)\n", id ? id : "", n);
}
static void on_error(const char* id, const char* msg, void* user) {
    (void)id; (void)user;
    fprintf(stderr, "[source] ERROR: %s\n", msg ? msg : "");
}

// ---- Ctrl-C: flip a flag the run loop polls (na_server_destroy joins all workers) --------
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

// ---- device listing / default-capture selection (capture mode) ---------------------------
#define MAX_DEVICES 128

static const char* cap_name(int capability) {
    switch (capability) {
        case NA_CAP_CAPTURE:  return "capture";
        case NA_CAP_PLAYBACK: return "playback";
        case NA_CAP_DUPLEX:   return "duplex";
        default:              return "unknown";
    }
}

// Print every capture-capable device; returns 0 on success, 1 on failure.
static int list_devices(void) {
    na_context* ctx = na_context_create();
    if (ctx == NULL) {
        fprintf(stderr, "error: na_context_create: %s\n", na_strerror(na_last_error()));
        return 1;
    }
    na_device devs[MAX_DEVICES];
    int n = na_enumerate(ctx, devs, MAX_DEVICES, sizeof devs[0]);
    if (n < 0) {
        fprintf(stderr, "error: na_enumerate: %s\n", na_strerror(na_last_error()));
        na_context_destroy(ctx);
        return 1;
    }
    printf("Capture-capable devices (use the id with --capture-id):\n");
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (devs[i].capability == NA_CAP_CAPTURE || devs[i].capability == NA_CAP_DUPLEX) {
            printf("  [%2d] %-40s %-12s %s%s\n", devs[i].capture_backend_id, devs[i].name,
                   cap_name(devs[i].capability), devs[i].host_api,
                   devs[i].is_virtual ? "  (virtual loopback)" : "");
            shown++;
        }
    }
    if (shown == 0)
        printf("  (none — use --test-tone for a hardware-free source)\n");
    na_context_destroy(ctx);
    return 0;
}

// Return the first capture-capable device's id, or -1 if there is none.
static int default_capture_id(void) {
    na_context* ctx = na_context_create();
    if (ctx == NULL) return -1;
    na_device devs[MAX_DEVICES];
    int n = na_enumerate(ctx, devs, MAX_DEVICES, sizeof devs[0]);
    int id = -1;
    for (int i = 0; i < n; i++) {
        if (devs[i].capability == NA_CAP_CAPTURE || devs[i].capability == NA_CAP_DUPLEX) {
            id = devs[i].capture_backend_id;
            break;
        }
    }
    na_context_destroy(ctx);
    return id;
}

static void usage(void) {
    fprintf(stderr,
        "usage: na_audio_source [--port N] [--capture-id N] [--transport tcp|udp]\n"
        "                       [--rate HZ] [--channels 1|2]\n"
        "                       [--test-tone] [--max-clients N] [--seconds N]\n"
        "       na_audio_source --list-devices\n\n"
        "  --port N          listen port; 0 = OS-assigned ephemeral (default 4533)\n"
        "  --capture-id N    capture device id to broadcast (default: first capture device)\n"
        "  --transport T     tcp (default) | udp\n"
        "  --reliability P   lan | wan  (UDP profiles: FEC/reorder/jitter). UDP/DUAL needs\n"
        "                    one: a bare start is refused since 0.5.0\n"
        "  --rate HZ         server-wide sample rate, 8000..192000, divisible by 50 for an\n"
        "                    exact 20 ms frame (default 48000). naudio does NOT resample: in\n"
        "                    capture mode the device must support this rate or the start\n"
        "                    fails. 12000 mono = 192 kbps on the wire vs 1536 kbps default\n"
        "  --channels N      1 (mono) | 2 (stereo, default). Server-wide, like --rate\n"
        "  --test-tone       hardware-free: broadcast a deterministic sawtooth (no device)\n"
        "  --max-clients N   maximum simultaneous clients (default 4)\n"
        "  --seconds N       run time; 0 = until Ctrl-C (default 0)\n"
        "  --list-devices    print capture-capable device ids, then exit\n"
        "  -h, --help        print this message\n");
}

int main(int argc, char** argv) {
    int          port        = 4533;  // AudioStreamConfig default audio port
    int          capture_id  = -1;    // -1 => auto-select the first capture device
    na_transport transport   = NA_TRANSPORT_TCP;
    int          test_tone   = 0;
    int          max_clients = 4;
    long long    seconds     = 0;     // 0 = until Ctrl-C
    int          reliability = -1;    // -1 => bare transport (unchanged default)
    int          rate        = DEFAULT_RATE;
    int          channels    = DEFAULT_CHANNELS;
    int          format_set  = 0;     // only call na_server_set_audio_format when asked

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        #define NEED_VAL(opt) (i + 1 < argc ? argv[++i] : (fprintf(stderr, "error: %s needs a value\n", opt), exit(2), ""))
        if      (strcmp(a, "--port") == 0)        port = atoi(NEED_VAL("--port"));
        else if (strcmp(a, "--capture-id") == 0)  capture_id = atoi(NEED_VAL("--capture-id"));
        else if (strcmp(a, "--max-clients") == 0) max_clients = atoi(NEED_VAL("--max-clients"));
        else if (strcmp(a, "--seconds") == 0)     seconds = atoll(NEED_VAL("--seconds"));
        else if (strcmp(a, "--rate") == 0)        { rate = atoi(NEED_VAL("--rate")); format_set = 1; }
        else if (strcmp(a, "--channels") == 0)    { channels = atoi(NEED_VAL("--channels")); format_set = 1; }
        else if (strcmp(a, "--test-tone") == 0)   test_tone = 1;
        else if (strcmp(a, "--list-devices") == 0) return list_devices();
        else if (strcmp(a, "--reliability") == 0) {
            const char* r = NEED_VAL("--reliability");
            if      (strcmp(r, "lan") == 0) reliability = NA_RELIABILITY_UDP_LAN;
            else if (strcmp(r, "wan") == 0) reliability = NA_RELIABILITY_UDP_WAN;
            else { fprintf(stderr, "error: invalid --reliability '%s' (lan|wan)\n", r); return 2; }
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

    // The bounds keep a 20 ms frame exact ((rate * FRAME_MS) % 1000 == 0 <=> divisible by 50)
    // and at least 8 bytes long (the fingerprint's read); na_server_set_audio_format re-validates
    // rate > 0 and channels 1|2 but knows nothing about this tool's frame cadence.
    if (rate < 8000 || rate > 192000 || (rate * FRAME_MS) % 1000 != 0) {
        fprintf(stderr, "error: invalid --rate %d (8000..192000, divisible by 50)\n", rate);
        return 2;
    }
    if (channels != 1 && channels != 2) {
        fprintf(stderr, "error: invalid --channels %d (1|2)\n", channels);
        return 2;
    }

    // Frame geometry for the effective format (at the defaults: 960 / 1920 / 3840).
    const int samples_per_frame = rate * FRAME_MS / 1000;
    const int int16_per_frame   = samples_per_frame * channels;
    const int bytes_per_frame   = int16_per_frame * (BITS / 8);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // Test-tone uses the hardware-free NULL backend (inject); capture uses the SYSTEM backend.
    na_server_backend backend = test_tone ? NA_SERVER_BACKEND_NULL : NA_SERVER_BACKEND_SYSTEM;

    // In capture mode, resolve the capture device up front so we can fail early with guidance.
    if (!test_tone && capture_id < 0) {
        capture_id = default_capture_id();
        if (capture_id < 0) {
            fprintf(stderr, "error: no capture device found. Run --list-devices, pass "
                            "--capture-id N, or use --test-tone for a hardware-free source.\n");
            return 1;
        }
        fprintf(stderr, "[source] auto-selected capture device id=%d "
                        "(run --list-devices to choose another)\n", capture_id);
    }

    na_audio_server* server = na_server_create(backend, port);
    if (server == NULL) {
        fprintf(stderr, "error: na_server_create: %s\n", na_strerror(na_last_error()));
        return 1;
    }

    // Configure BEFORE start — every setter is frozen once the server is running.
    na_server_callbacks cbs;
    memset(&cbs, 0, sizeof cbs);
    cbs.struct_size = sizeof cbs;
    cbs.on_started             = on_started;
    cbs.on_client_connected    = on_client_connected;
    cbs.on_client_disconnected = on_client_disconnected;
    cbs.on_error               = on_error;
    na_server_set_callbacks(server, &cbs, NULL);
    /* FEC parity only goes on the wire if the SERVER selects a profile that emits it, so a client
     * asking for UDP_WAN against a bare-transport server still recovers nothing. Both ends must
     * agree — see docs/hamlib-streaming-bridge.md, "A client built on the C ABI must select the
     * profile too". Default stays plain transport so this example's existing behaviour is
     * unchanged; --reliability opts in. */
    if (reliability >= 0)
        na_server_set_reliability_profile(server, (na_reliability_profile)reliability);
    else
        na_server_set_transport(server, transport);
    na_server_set_max_clients(server, max_clients);
    /* Server-wide: EVERY client gets this format (per-client negotiation does not exist yet —
     * issue #91 Phase B). It survives a profile call in either order, so the placement relative
     * to na_server_set_reliability_profile above is not load-bearing. */
    if (format_set) {
        if (na_server_set_audio_format(server, rate, BITS, channels) != NA_OK) {
            fprintf(stderr, "error: na_server_set_audio_format(%d, %d, %d): %s\n",
                    rate, BITS, channels, na_strerror(na_last_error()));
            na_server_destroy(server);
            return 1;
        }
    }
    if (!test_tone) {
        if (na_server_set_capture_device(server, capture_id) != NA_OK) {
            fprintf(stderr, "error: na_server_set_capture_device(%d): %s\n",
                    capture_id, na_strerror(na_last_error()));
            na_server_destroy(server);
            return 1;
        }
    }

    char err[256];
    if (na_server_start(server, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "error: na_server_start failed: %s\n", err);
        if (format_set && !test_tone)
            fprintf(stderr, "hint: the capture device may not support %d Hz / %d ch — naudio "
                            "does not resample. Pick a rate the device supports, or --test-tone.\n",
                    rate, channels);
        na_server_destroy(server);
        return 1;
    }

    const int bound_port = na_server_port(server);
    // The EFFECTIVE transport: a --reliability profile selects UDP as part of itself, so echoing
    // the --transport flag would misreport exactly the runs the profile flag exists for (the same
    // latent misreport the 0.5.0 example sweep fixed in the clients).
    const char* transport_name =
        reliability >= 0 ? "udp" : (transport == NA_TRANSPORT_TCP ? "tcp" : "udp");

    // Machine-readable on STDOUT so a harness with --port 0 can learn the ephemeral port.
    printf("LISTENING port=%d\n", bound_port);
    fflush(stdout);

    // The tone frame is identical every frame — generate once, sized for the effective format.
    unsigned char* frame = NULL;
    if (test_tone) {
        frame = (unsigned char*)malloc((size_t)bytes_per_frame);
        if (frame == NULL) {
            fprintf(stderr, "error: out of memory\n");
            na_server_destroy(server);
            return 1;
        }
        fill_tone_frame(frame, int16_per_frame);
    }

    if (test_tone) {
        char hex[17];
        tone_first_frame_hex(frame, hex);
        fprintf(stderr, "[source] test-tone source on %s:%d (%d Hz / %d ch, max %d clients); "
                        "tone_first_frame_hex=%s\n", transport_name, bound_port, rate, channels,
                max_clients, hex);
        fprintf(stderr, "[source] connect a client, e.g. "
                        "na_c_play_to_speakers --backend system --playback-id N --host 127.0.0.1 --port %d\n",
                bound_port);
    } else {
        fprintf(stderr, "[source] capturing device id=%d (%d Hz / %d ch) -> broadcasting on "
                        "%s:%d (max %d clients)\n",
                capture_id, rate, channels, transport_name, bound_port, max_clients);
    }
    fprintf(stderr, "[source] running %s ...\n",
            seconds > 0 ? "for a fixed time" : "until Ctrl-C");

    // Run loop. Test-tone injects one frame every FRAME_MS (the library captures + broadcasts
    // automatically in capture mode, so there we just wait). A periodic status line shows life.

    const long long start    = now_ms();
    const long long deadline = seconds > 0 ? start + seconds * 1000 : 0;
    long long frames_injected = 0;
    long long next_status = start + 2000;

    while (!g_stop && (deadline == 0 || now_ms() < deadline)) {
        if (test_tone) {
            if (na_server_inject_audio(server, frame, bytes_per_frame) != NA_OK) {
                fprintf(stderr, "[source] inject failed: %s\n", na_strerror(na_last_error()));
                break;
            }
            frames_injected++;
            sleep_ms(FRAME_MS);
        } else {
            sleep_ms(100);
        }
        const long long t = now_ms();
        if (t >= next_status) {
            fprintf(stderr, "  t=%2llds  clients=%d  %s\n", (t - start) / 1000,
                    atomic_load(&g_clients),
                    test_tone ? "(test tone)" : "(capturing)");
            next_status = t + 2000;
        }
    }

    const long long elapsed = now_ms() - start;
    fprintf(stderr, "\n[source] stopping after %lld ms (peak clients=%d%s)\n",
            elapsed, atomic_load(&g_peak_clients),
            test_tone ? "" : ", capture");

    // SUMMARY on STDOUT (mirrors the clients' machine-readable line).
    printf("SUMMARY port=%d peak_clients=%d frames_injected=%lld mode=%s\n",
           bound_port, atomic_load(&g_peak_clients), frames_injected,
           test_tone ? "test-tone" : "capture");

    na_server_destroy(server);  // stop + join workers + drain dispatch thread, then free
    free(frame);
    return 0;
}
