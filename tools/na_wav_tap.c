/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * na_wav_tap — record what a naudio CLIENT actually receives, as a WAV another program can analyse.
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
 * FORMAT. The rate and channel count come from na_client_get_audio_format AFTER connect — the
 * negotiated format the server declared in AUDIO_CONFIG, i.e. the format of the PCM this client's
 * callback actually receives. (This tool used to hardcode 48000/16/2, which mislabels the WAV the
 * day the server is provisioned lower — e.g. na_audio_source --rate 12000 --channels 1 for a
 * constrained link. Measured before the fix: against a 12 kHz mono server it exited 0 while
 * writing a mangled file and blaming the network.) Channel 0 is taken, and when the negotiated
 * rate is an integer multiple of 12000 it is decimated to the 12 kHz narrow-band analysis programs commonly want,
 * behind an N-sample box average — crude, but the receiver's own filter is ~3 kHz wide so there
 * is nothing near Nyquist to alias down. Any other rate is written as-is at the negotiated rate.
 * The delivered byte rate is still measured against what the negotiated format implies: a stream
 * that disagrees with its own AUDIO_CONFIG says so loudly rather than producing a silently
 * wrong-pitched WAV.
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

/* The output rate narrow-band analysis programs commonly want. When the negotiated rate divides by it, output goes there. */
#define TAP_RATE 12000

typedef struct {
    short *out;
    size_t n, cap;
    unsigned long long bytes;
    int acc_n;
    long acc;
    int armed;
    int src_channels;   /* negotiated, set before arming; 2 * this = bytes per sample-frame */
    int decim;          /* box-average factor; 1 = passthrough */
} tap;

/* Fires on the receive worker thread. Keep it allocation-free and non-blocking. */
static void on_rx(const unsigned char *pcm, size_t n, void *user) {
    tap *t = (tap *)user;
    t->bytes += n;
    if (!t->armed) return;
    size_t step = (size_t)t->src_channels * 2;       /* S16LE frames: take channel 0 */
    for (size_t i = 0; i + step <= n; i += step) {
        short l = (short)((unsigned short)pcm[i] | ((unsigned short)pcm[i + 1] << 8));
        t->acc += l;
        if (++t->acc_n == t->decim) {
            if (t->n < t->cap) t->out[t->n++] = (short)(t->acc / t->decim);
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

static int write_wav(const char *path, const short *s, size_t n, unsigned rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    unsigned data = (unsigned)(n * 2);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + data); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 1);
    put32(f, rate); put32(f, rate * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, data);
    fwrite(s, 2, n, f);
    return fclose(f) == 0 ? 0 : -1;
}

static void usage(void) {
    fprintf(stderr,
        "usage: na_wav_tap [--host H] [--port N] [--seconds S] [--out F] [--align15]\n"
        "  --host H     naudio server (default 127.0.0.1)\n"
        "  --port N     server port (default 4533)\n"
        "  --seconds S  record length in seconds (default 15)\n"
        "  --out F      output WAV, mono at %d Hz when the negotiated rate divides by it\n"
        "               (48k/24k/12k do), else at the negotiated rate (default tap.wav)\n"
        "  --align15    wait for a wall-clock 15 s boundary before recording\n"
        "  --tcp        use TCP instead of the UDP_WAN profile — the discriminating run when\n"
        "               UDP is short: TCP retransmits, so loss shows up as delay, not absence\n",
        TAP_RATE);
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1", *out = "tap.wav";
    int port = 4533, seconds = 15, align = 0, use_tcp = 0;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--host")    && i + 1 < argc) host    = argv[++i];
        else if (!strcmp(argv[i], "--port")    && i + 1 < argc) port    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out")     && i + 1 < argc) out     = argv[++i];
        else if (!strcmp(argv[i], "--align15")) align = 1;
        else if (!strcmp(argv[i], "--tcp")) use_tcp = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(); return 2; }
    }
    if (seconds <= 0 || port <= 0 || port > 65535) { usage(); return 2; }

    /* The record buffer is sized from the NEGOTIATED format, which exists only after connect —
     * so allocation moves below. Until t.armed is set the callback only counts bytes, and it
     * never touches t.out while unarmed, so connecting first is safe. */
    tap t;
    memset(&t, 0, sizeof t);

    na_stream_client *c = na_client_create(NA_CLIENT_BACKEND_NULL, host, port, "na_wav_tap");
    if (!c) { fprintf(stderr, "na_wav_tap: na_client_create failed\n"); return 1; }
    na_client_set_audio_cb(c, on_rx, &t);
    /* NOT na_client_set_transport(NA_TRANSPORT_UDP): that sets the transport and NOTHING else,
     * leaving FEC, reordering, adaptive jitter and control-ARQ off — a trap documented in
     * docs/hamlib-streaming-bridge.md and one this tool walked straight into. On loopback the
     * difference is invisible. Across a real LAN it was measured at 33.7 / 48.3 / 33.1 % of the
     * audio arriving, against 100.1 % for a loopback client on the same server at the same time.
     * The profile selects the transport as part of itself, so no separate call is needed. */
    /* --tcp is the DISCRIMINATING experiment, not a feature. TCP retransmits, so if a link is
     * merely lossy TCP arrives complete (late, but complete) while UDP does not. Same audio, same
     * server, same tap: TCP ~100% and UDP ~33% means the path is dropping datagrams; BOTH at ~33%
     * means the shortfall is not loss at all and the search moves elsewhere entirely. */
    if (use_tcp) na_client_set_reliability_profile(c, NA_RELIABILITY_DEFAULT);
    else         na_client_set_reliability_profile(c, NA_RELIABILITY_UDP_WAN);
    fprintf(stderr, "na_wav_tap: profile %s\n", use_tcp ? "DEFAULT (TCP)" : "UDP_WAN (FEC+reorder+jitter)");
    /* Read the components BACK rather than trusting the call above — the assertion this tool
     * lacked when it ran a whole on-air session with every component silently off (@since 0.5.0).
     * On TCP all four read 0: the transport itself orders and retransmits. */
    {
        int comps = 0;
        na_client_get_reliability(c, &comps);
        fprintf(stderr, "na_wav_tap: reliability components: fec=%d reorder=%d jitter=%d arq=%d\n",
                (comps & NA_RELIABILITY_COMPONENT_FEC) != 0,
                (comps & NA_RELIABILITY_COMPONENT_REORDER) != 0,
                (comps & NA_RELIABILITY_COMPONENT_ADAPTIVE_JITTER) != 0,
                (comps & NA_RELIABILITY_COMPONENT_CONTROL_ARQ) != 0);
    }
    /* No na_client_set_playback_device: optional on the NULL backend since 0.5.0. */

    char err[256] = {0};
    if (na_client_connect(c, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "na_wav_tap: connect failed: %s\n", err);
        na_client_destroy(c); free(t.out); return 1;
    }
    fprintf(stderr, "na_wav_tap: connected to %s:%d\n", host, port);

    /* The negotiated format — what the server's AUDIO_CONFIG declared and what the callback's
     * PCM is actually in. Post-connect by contract (before connect this getter reports only the
     * local placeholder). @since 0.5.0; the hardcoded 48000/16/2 this replaces is the reason a
     * 12 kHz-provisioned server used to produce a mangled file with exit code 0. */
    int src_rate = 0, src_bits = 0, src_channels = 0;
    if (na_client_get_audio_format(c, &src_rate, &src_bits, &src_channels) != NA_OK ||
        src_rate <= 0 || src_bits != 16 || (src_channels != 1 && src_channels != 2)) {
        fprintf(stderr, "na_wav_tap: unusable negotiated format %d Hz / %d-bit / %d ch\n",
                src_rate, src_bits, src_channels);
        na_client_disconnect(c); na_client_destroy(c); return 1;
    }
    int decim    = (src_rate % TAP_RATE == 0) ? src_rate / TAP_RATE : 1;
    int out_rate = src_rate / decim;
    fprintf(stderr, "na_wav_tap: negotiated %d Hz / 16-bit / %d ch -> %d Hz mono WAV "
                    "(channel 0, %d:1 box decimation)\n",
            src_rate, src_channels, out_rate, decim);
    if (out_rate != TAP_RATE)
        fprintf(stderr, "na_wav_tap: note: %d Hz does not divide by %d — the WAV is correct at "
                        "%d Hz rather than the %d Hz narrow-band analysis programs commonly expect\n",
                src_rate, TAP_RATE, out_rate, TAP_RATE);

    t.cap = (size_t)out_rate * (size_t)(seconds + 2);
    t.out = (short *)calloc(t.cap, sizeof(short));
    if (!t.out) {
        fprintf(stderr, "na_wav_tap: out of memory\n");
        na_client_disconnect(c); na_client_destroy(c); return 2;
    }
    t.src_channels = src_channels;   /* both set before t.armed — on_rx reads them only armed */
    t.decim = decim;

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

    /* Read the client's own accounting BEFORE disconnecting — `connected` goes 0 after, and every
     * field with it. A delivered-byte percentage says audio went missing; only these say WHERE.
     * The distinctions that matter on a lossy link:
     *   packets_recovered_by_fec  the reliability layer earning its place
     *   socket_rx_drops           the network delivered it and the KERNEL buffer dropped it —
     *                             a local receive problem, not a path problem
     *   crc_errors                arrived corrupted rather than late
     *   jitter_ms / buffer_target_ms  whether the adaptive buffer is tracking the link
     * Note -1 means NOT MEASURED, never zero: packets_lost / packet_loss_rate / sequence_gaps are
     * unmeasured on every UDP profile, because the gap tracker only runs with no reorder buffer. */
    na_client_stats st;
    memset(&st, 0, sizeof st);
    int have_stats = (na_client_get_stats(c, &st, sizeof st) == NA_OK);

    na_client_disconnect(c);   /* joins the worker: no callback can be in flight after this */
    na_client_destroy(c);

    if (have_stats) {
        fprintf(stderr,
            "na_wav_tap: client stats — connected=%d\n"
            "  packets_received      %lld        bytes_received  %lld\n"
            "  packets_recovered_fec %lld        fec_unreconciled %lld\n"
            "  packets_reordered     %lld        crc_errors      %d\n"
            "  socket_rx_drops       %lld        queue_drops     %lld\n"
            "  jitter_ms             %.1f        buffer_target_ms %d\n"
            "  packets_lost          %lld (-1 = not measured on a UDP profile)\n",
            st.connected,
            st.packets_received, st.bytes_received,
            st.packets_recovered_by_fec, st.fec_blocks_unreconciled,
            st.packets_reordered, st.crc_errors,
            st.socket_rx_drops, st.queue_drops,
            st.jitter_ms, st.buffer_target_ms,
            st.packets_lost);
    } else {
        fprintf(stderr, "na_wav_tap: na_client_get_stats failed — no diagnosis available\n");
    }

    double bps = (double)got / (double)seconds,
           want = (double)src_rate * 2.0 * (double)src_channels;
    fprintf(stderr, "na_wav_tap: %llu bytes in %d s = %.0f B/s (expect %.0f, %.1f%%)\n",
            got, seconds, bps, want, 100.0 * bps / want);
    if (got == 0) {
        fprintf(stderr, "na_wav_tap: NO AUDIO RECEIVED — nothing written\n");
        free(t.out); return 1;
    }
    /* Two different faults land here and they need different reactions, so name both rather than
     * assert one. Nominal is now what the NEGOTIATED format implies, so a rate far OVER it means
     * the stream disagrees with the server's own AUDIO_CONFIG — a protocol violation, and the
     * file is wrong-pitched. A rate UNDER nominal usually means the
     * producer is not keeping up (this project's synthetic sources habitually do not: the Hamlib
     * dummy runs ~83 %% of nominal, a loopback dummy ~70 %%, na_audio_source --test-tone 75.8 %%
     * measured) — pitch is then correct and the recording is simply short of wall time, which may
     * still decode. A real radio should sit at ~100 %%: the 2026-08-21 on-air runs measured
     * 100.0 %% exactly, so a shortfall against a RIG is a genuine finding, not background. */
    /* Print ONLY the branch that applies. The first version of this listed both possibilities in
     * one message, and the first person to hit it read the wrong half as the verdict — a diagnostic
     * that makes the reader choose between two explanations has not diagnosed anything. */
    if (bps > want * 1.1) {
        fprintf(stderr, "na_wav_tap: WARNING delivered %.1f%% of nominal — ABOVE it, so this stream "
                        "disagrees with the %d Hz / %d ch its own AUDIO_CONFIG declared.\n"
                        "  The %d Hz WAV header is therefore wrong and the file will not decode.\n",
                100.0 * bps / want, src_rate, src_channels, out_rate);
    } else if (bps < want * 0.9) {
        fprintf(stderr, "na_wav_tap: WARNING delivered %.1f%% of nominal — BELOW it, so audio went "
                        "missing in transit.\n"
                        "  Pitch is still correct and the file may decode; it is short of wall time.\n"
                        "  Across a network this is loss on the hop. On loopback it means the "
                        "producer is not keeping up (this project's synthetic sources habitually do "
                        "not: --test-tone ~75%%, the Hamlib dummy ~83%%). Off a real capture device at ~100%%, "
                        "a shortfall is a genuine finding.\n",
                100.0 * bps / want);
    }

    if (write_wav(out, t.out, t.n, (unsigned)out_rate) != 0) {
        fprintf(stderr, "na_wav_tap: writing %s failed\n", out);
        free(t.out); return 2;
    }
    fprintf(stderr, "na_wav_tap: wrote %s — %zu samples @ %d Hz mono (%.1f s)\n",
            out, t.n, out_rate, (double)t.n / out_rate);
    free(t.out);
    return 0;
}
