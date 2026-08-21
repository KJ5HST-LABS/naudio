/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * na_wav_tap — record what a naudio CLIENT actually receives, as a WAV a decoder can read.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * WHY THIS EXISTS. Issue #12's first acceptance item is "RX audio from a real rig arrives at a
 * remote naudio client and is INTELLIGIBLE". Intelligibility judged by ear leaves no artifact, is
 * not comparable between runs, and cannot be checked by anyone who was not in the room. For a
 * digital mode it can be made objective instead: if audio carried end to end through naudio still
 * DECODES, the pipeline preserved the information, and the decode names callsigns that either are
 * or are not really on the air.
 *
 * Measured on 2026-08-21 against a Yaesu on 14.074 MHz (20 m FT8), this tap plus `jt9 -8` produced
 * six decodes across five consecutive periods, from +15 dB down to -25 dB — e.g.
 * `W9IKE VE3GLN FN25` and `RA9J KD4SN EM77` — on audio that had crossed a naudio UDP wire.
 *
 * WHAT IT TAPS, AND WHY THAT POINT. It is a normal naudio client driving ONLY the public C ABI
 * (na_client_create / na_client_set_audio_cb / na_client_connect), so what it writes is what a
 * third-party consumer of the shipped contract would get — not an internal buffer. It runs on
 * NA_CLIENT_BACKEND_NULL: RX is still delivered to the audio callback but no device is opened, so
 * the file is what naudio DELIVERED rather than what some sound card then did to it.
 *
 *   NOTE the NULL backend still requires na_client_set_playback_device(). connect() checks the id
 *   unconditionally (src/net/AudioStreamClient.cpp), which the "hardware-free" wording on the enum
 *   does not say. The id is never opened as a device on this backend.
 *
 * FORMAT. naudio carries S16LE stereo at 48 kHz here; WSJT-X decoders want 12 kHz mono. This takes
 * the LEFT channel and decimates 4:1 behind a 4-sample box average — crude, but the receiver's own
 * filter is ~3 kHz wide so there is nothing near the 6 kHz Nyquist to alias down. That 48k/16/2
 * assumption is ASSERTED, not trusted: the delivered byte rate is measured and a run that is not
 * within 10 % of 192000 B/s says so loudly, because the failure mode otherwise is a WAV that is
 * silently wrong-pitched and simply never decodes.
 *
 * ALIGNMENT. --align15 waits for a wall-clock 15 s boundary before recording, which is what FT8
 * periods start on. Without it the decoder must find the signal by its own time search and a file
 * that straddles two periods decodes nothing.
 *
 *   usage: na_wav_tap [--host H] [--port N] [--seconds S] [--out F] [--align15]
 *
 * Exit 0 = wrote a file. 1 = connect failed or no audio arrived. 2 = usage / write error.
 *
 * See docs/on-air-verification.md for the full procedure, including the negative control (decode
 * digital silence and require zero decodes) without which "it decoded" is not yet evidence.
 */
#include <naudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Portability. This tool is useful on Windows precisely because it needs no audio device: it is
 * the CLIENT end of an on-air check whose server is the machine with the radio, so the remote
 * half of issue #12 item 1 ("a REMOTE naudio client") is often a Windows box. Only three POSIX
 * facilities were in the way — a millisecond sleep, a whole-second sleep, and a wall clock with
 * sub-second resolution — so they are wrapped rather than the tool being kept POSIX-only. */
#if defined(_WIN32)
#  include <windows.h>
   static void na_sleep_ms(unsigned ms) { Sleep(ms); }
   /* Wall clock as seconds + nanoseconds. FILETIME counts 100 ns ticks from 1601-01-01; the
    * constant below is the offset to the Unix epoch. Only the SECONDS are used for the 15 s
    * alignment and only the sub-second part gates the boundary, so precision here is ample. */
   static void na_wall_now(long long *sec, long *nsec) {
       FILETIME ft; ULARGE_INTEGER u;
       GetSystemTimeAsFileTime(&ft);
       u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
       unsigned long long t = u.QuadPart - 116444736000000000ULL;   /* -> 100 ns since 1970 */
       *sec  = (long long)(t / 10000000ULL);
       *nsec = (long)((t % 10000000ULL) * 100ULL);
   }
#else
#  include <unistd.h>
   static void na_sleep_ms(unsigned ms) { usleep(ms * 1000u); }
   static void na_wall_now(long long *sec, long *nsec) {
       struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
       *sec = (long long)ts.tv_sec; *nsec = (long)ts.tv_nsec;
   }
#endif

#define SRC_RATE 48000
#define DECIM    4
#define OUT_RATE (SRC_RATE / DECIM)

typedef struct {
    short *out;
    size_t n, cap;
    unsigned long long bytes;
    int acc_n;
    long acc;
    int armed;
} tap;

/* Fires on the receive worker thread. Keep it allocation-free and non-blocking. */
static void on_rx(const unsigned char *pcm, size_t n, void *user) {
    tap *t = (tap *)user;
    t->bytes += n;
    if (!t->armed) return;
    for (size_t i = 0; i + 3 < n; i += 4) {          /* S16LE stereo: take channel 0 */
        short l = (short)((unsigned short)pcm[i] | ((unsigned short)pcm[i + 1] << 8));
        t->acc += l;
        if (++t->acc_n == DECIM) {
            if (t->n < t->cap) t->out[t->n++] = (short)(t->acc / DECIM);
            t->acc = 0;
            t->acc_n = 0;
        }
    }
}

static void put32(FILE *f, unsigned v) {
    fputc((int)(v & 255), f); fputc((int)((v >> 8) & 255), f);
    fputc((int)((v >> 16) & 255), f); fputc((int)((v >> 24) & 255), f);
}
static void put16(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)((v >> 8) & 255), f); }

static int write_wav(const char *path, const short *s, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    unsigned data = (unsigned)(n * 2);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + data); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 1);
    put32(f, OUT_RATE); put32(f, OUT_RATE * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, data);
    fwrite(s, 2, n, f);
    return fclose(f) == 0 ? 0 : -1;
}

static void usage(void) {
    fprintf(stderr,
        "usage: na_wav_tap [--host H] [--port N] [--seconds S] [--out F] [--align15]\n"
        "  --host H     naudio server (default 127.0.0.1)\n"
        "  --port N     server port (default 4533)\n"
        "  --seconds S  record length (default 15, one FT8 period)\n"
        "  --out F      output WAV, %d Hz mono (default tap.wav)\n"
        "  --align15    wait for a wall-clock 15 s boundary before recording (FT8/FT4 periods)\n",
        OUT_RATE);
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1", *out = "tap.wav";
    int port = 4533, seconds = 15, align = 0;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--host")    && i + 1 < argc) host    = argv[++i];
        else if (!strcmp(argv[i], "--port")    && i + 1 < argc) port    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out")     && i + 1 < argc) out     = argv[++i];
        else if (!strcmp(argv[i], "--align15")) align = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(); return 2; }
    }
    if (seconds <= 0 || port <= 0 || port > 65535) { usage(); return 2; }

    tap t;
    memset(&t, 0, sizeof t);
    t.cap = (size_t)OUT_RATE * (size_t)(seconds + 2);
    t.out = (short *)calloc(t.cap, sizeof(short));
    if (!t.out) { fprintf(stderr, "na_wav_tap: out of memory\n"); return 2; }

    na_stream_client *c = na_client_create(NA_CLIENT_BACKEND_NULL, host, port, "na_wav_tap");
    if (!c) { fprintf(stderr, "na_wav_tap: na_client_create failed\n"); free(t.out); return 1; }
    na_client_set_audio_cb(c, on_rx, &t);
    na_client_set_transport(c, NA_TRANSPORT_UDP);
    na_client_set_playback_device(c, 0);   /* required even on NULL — see the header note */

    char err[256] = {0};
    if (na_client_connect(c, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "na_wav_tap: connect failed: %s\n", err);
        na_client_destroy(c); free(t.out); return 1;
    }
    fprintf(stderr, "na_wav_tap: connected to %s:%d\n", host, port);

    /* Wait for audio to actually flow before timing anything. A client that has just connected
     * spends up to a couple of seconds before the first datagram arrives, and counting that dead
     * time into the measured window makes the byte-rate assertion below fire on a healthy stream
     * (measured: 75.8 %% of nominal on an 8 s run armed immediately after connect). */
    {
        int waited = 0;
        while (t.bytes == 0 && waited < 5000) { na_sleep_ms(20); waited += 20; }
        if (t.bytes == 0) {
            fprintf(stderr, "na_wav_tap: no audio within 5 s of connecting — nothing written\n");
            na_client_disconnect(c); na_client_destroy(c); free(t.out); return 1;
        }
        fprintf(stderr, "na_wav_tap: audio flowing after %d ms\n", waited);
    }

    if (align) {
        for (;;) {
            long long sec; long nsec;
            na_wall_now(&sec, &nsec);
            if (sec % 15 == 0 && nsec < 60000000L) break;
            na_sleep_ms(20);
        }
        fprintf(stderr, "na_wav_tap: 15 s boundary reached, recording\n");
    }

    unsigned long long before = t.bytes;
    t.armed = 1;
    na_sleep_ms((unsigned)seconds * 1000u);
    t.armed = 0;
    unsigned long long got = t.bytes - before;

    na_client_disconnect(c);   /* joins the worker: no callback can be in flight after this */
    na_client_destroy(c);

    double bps = (double)got / (double)seconds, want = (double)SRC_RATE * 2.0 * 2.0;
    fprintf(stderr, "na_wav_tap: %llu bytes in %d s = %.0f B/s (expect %.0f, %.1f%%)\n",
            got, seconds, bps, want, 100.0 * bps / want);
    if (got == 0) {
        fprintf(stderr, "na_wav_tap: NO AUDIO RECEIVED — nothing written\n");
        free(t.out); return 1;
    }
    /* Two different faults land here and they need different reactions, so name both rather than
     * assert one. A rate far OVER or a non-multiple pattern means the stream is not 48000/16/2 and
     * the file is wrong-pitched — it will never decode. A rate UNDER nominal usually means the
     * producer is not keeping up (this project's synthetic sources habitually do not: the Hamlib
     * dummy runs ~83 %% of nominal, a loopback dummy ~70 %%, na_audio_source --test-tone 75.8 %%
     * measured) — pitch is then correct and the recording is simply short of wall time, which may
     * still decode. A real radio should sit at ~100 %%: the 2026-08-21 on-air runs measured
     * 100.0 %% exactly, so a shortfall against a RIG is a genuine finding, not background. */
    if (bps < want * 0.9 || bps > want * 1.1)
        fprintf(stderr, "na_wav_tap: WARNING delivered %.1f%% of the 48000/16/2 nominal rate.\n"
                        "  Over nominal or wildly off => not S16LE stereo @48k, and this file's "
                        "%d Hz header is wrong (it will not decode).\n"
                        "  Under nominal => the producer is not keeping up; pitch is right but the "
                        "recording is short of wall time. Off a real rig, investigate.\n",
                100.0 * bps / want, OUT_RATE);

    if (write_wav(out, t.out, t.n) != 0) {
        fprintf(stderr, "na_wav_tap: writing %s failed\n", out);
        free(t.out); return 2;
    }
    fprintf(stderr, "na_wav_tap: wrote %s — %zu samples @ %d Hz mono (%.1f s)\n",
            out, t.n, OUT_RATE, (double)t.n / OUT_RATE);
    free(t.out);
    return 0;
}
