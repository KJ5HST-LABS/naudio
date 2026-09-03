// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio examples — inject a tone as TX audio.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// The transmitting client the demo pair lacked: na_audio_source serves RX and, with
// --playback-id, plays its clients' TX on a device; this program SENDS TX — a sawtooth at the
// server's format, from the hardware-free NULL backend, for N seconds. With it the TX path is
// proved with no digital-mode application in the loop, and a silent bench splits into a
// naudio-side fault or an application-side one. It is tests/c_tx_sustained.c with the
// assertions taken out and the arguments put in.
//
//   na_c_inject_tone --host 192.168.1.10 --port 4533 --seconds 5
//
// Hardware-free on this end: no capture device (na_client_set_tx_inject is set before connect,
// the only way a NULL-backend client transmits) and the NULL backend plays nothing. TCP, the
// server's default transport. The server decides the format; this reads it back after connect
// (na_client_get_audio_format) and builds the tone in it — nothing here resamples. Sending is
// gated by NA_DUPLEX_TALK, which is NOT PTT (read the note on na_client_set_duplex): whether a
// carrier goes up is rig control's job, and this program never keys anything.
//
// Output: the human log goes to STDERR; one machine-readable RESULT line goes to STDOUT.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

#include "naudio.h"  // the only audio header — pure C, no C++

#define FRAME_MS   20    // one inject per 20 ms, the server's own frame cadence
#define TONE_HZ    1000  // the sawtooth's pitch, at any server rate
#define AMPLITUDE  8192  // a quarter of full scale: audible on a desk, kind to a TX input

static void usage(void) {
    fprintf(stderr,
        "usage: na_c_inject_tone [--host H] [--port N] [--seconds N] [--name S]\n\n"
        "  --host H       server host (default 127.0.0.1)\n"
        "  --port N       server port (default 4533)\n"
        "  --seconds N    how long to send the tone (default 5)\n"
        "  --name S       this client's display name (default na-c-inject-tone); the owner id\n"
                 "                 the server logs is its own, audio-N\n"
        "  -h, --help     print this message\n");
}

static int fail(const char* what, na_stream_client* c) {
    fprintf(stderr, "error: %s: %s\n", what, na_strerror(na_last_error()));
    if (c) na_client_destroy(c);
    return 1;
}

int main(int argc, char** argv) {
    const char* host    = "127.0.0.1";
    int         port    = 4533;
    long long   seconds = 5;
    const char* name    = "na-c-inject-tone";
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        #define NEED_VAL(opt) (i + 1 < argc ? argv[++i] : (fprintf(stderr, "error: %s needs a value\n", opt), exit(2), ""))
        if      (strcmp(a, "--host") == 0)    host = NEED_VAL("--host");
        else if (strcmp(a, "--port") == 0)    port = atoi(NEED_VAL("--port"));
        else if (strcmp(a, "--seconds") == 0) seconds = atoll(NEED_VAL("--seconds"));
        else if (strcmp(a, "--name") == 0)    name = NEED_VAL("--name");
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", a); usage(); return 2; }
        #undef NEED_VAL
    }

    na_stream_client* c = na_client_create(NA_CLIENT_BACKEND_NULL, host, port, name);
    if (c == NULL) return fail("na_client_create", NULL);
    na_client_set_auto_reconnect(c, 0);  // a reconnect mid-tone would hide a dropped link
    if (na_client_set_tx_inject(c, 1) != NA_OK) return fail("na_client_set_tx_inject", c);

    char err[256];
    if (na_client_connect(c, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "error: connect to %s:%d failed: %s\n", host, port, err);
        na_client_destroy(c);
        return 1;
    }

    // The server's format, read back after the handshake; the tone is built in it.
    int rate = 0, bits = 0, channels = 0;
    na_client_get_audio_format(c, &rate, &bits, &channels);
    const int samples_per_frame = rate * FRAME_MS / 1000;
    const int int16_per_frame   = samples_per_frame * channels;
    unsigned char* frame = (unsigned char*)malloc((size_t)int16_per_frame * 2);
    if (frame == NULL || bits != 16) {
        fprintf(stderr, "error: cannot build the tone (%d Hz / %d-bit / %d ch)\n", rate, bits, channels);
        free(frame);
        na_client_destroy(c);
        return 1;
    }

    na_client_set_duplex(c, NA_DUPLEX_TALK);  // sending gated on; this keys nothing
    fprintf(stderr, "[tone] connected to %s:%d as %s (%d Hz / %d ch); sending a %d Hz sawtooth "
                    "for %lld s\n", host, port, name, rate, channels, TONE_HZ, seconds);

    const int  period = rate / TONE_HZ;  // samples per cycle; the phase runs on across frames
    int        phase  = 0;
    long long  injected = 0, frames = 0;
    char       owner[128] = "";
    for (long long tick = 0; tick < seconds * 1000 / FRAME_MS; tick++) {
        for (int s = 0; s < samples_per_frame; s++) {
            const int16_t  v = (int16_t)(phase * (2 * AMPLITUDE) / period - AMPLITUDE);
            const uint16_t u = (uint16_t)v;
            for (int ch = 0; ch < channels; ch++) {
                frame[(s * channels + ch) * 2]     = (unsigned char)(u & 0xFF);
                frame[(s * channels + ch) * 2 + 1] = (unsigned char)((u >> 8) & 0xFF);
            }
            if (++phase >= period) phase = 0;
        }
        const int n = na_client_inject_tx_audio(c, frame, int16_per_frame * 2);
        if (n > 0) { injected += n; frames++; }
        // The roster's view of the TX owner (CLIENTS_UPDATE), logged on change: "us" is the proof
        // the server granted this client the channel, from the client's side of the wire.
        char now_owner[128] = "";
        if (na_client_server_tx_owner(c, now_owner, (int)sizeof now_owner) <= 0) now_owner[0] = '\0';
        if (strcmp(now_owner, owner) != 0) {
            fprintf(stderr, "[tone] server tx owner=%s\n", now_owner[0] ? now_owner : "none");
            snprintf(owner, sizeof owner, "%s", now_owner);
        }
        sleep_ms(FRAME_MS);
    }

    na_client_set_duplex(c, NA_DUPLEX_LISTEN);
    na_client_disconnect(c);
    na_client_destroy(c);
    free(frame);
    printf("RESULT injected_bytes=%lld frames=%lld rate=%d channels=%d\n", injected, frames, rate,
           channels);
    return injected > 0 ? 0 : 1;
}
