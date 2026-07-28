/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * na_hamlib_bridge — bridge a Hamlib streaming source (PR #2116, rig_stream_*)
 * onto a naudio server so the audio crosses a lossy/WAN hop with naudio's FEC +
 * adaptive jitter + reorder, which Hamlib's own transport (trusted-LAN only) lacks.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * WHERE THIS RUNS: at the RADIO SITE, upstream of the lossy hop. Hamlib carries the
 * reliable local hop (rig -> here); naudio carries the internet hop (here -> operator).
 * FEC only protects the link it runs on, so placing this box on the far side of the
 * Internet from the rig would defeat the point.
 *
 * DATA PATH (both directions are straight S16LE byte copies — no resample, no convert):
 *   RX: rig_stream_read(AUDIO_RX, S16@48k) ---> na_server_inject_audio()  [fan-out + FEC]
 *   TX: na_server_tx_audio_cb() --ring--> rig_stream_write(AUDIO_TX, S16@48k)
 *
 * Pure C: links the naudio C ABI (naudio.h) + libhamlib (<hamlib/rig.h>). The C ABI's
 * na_server_set_reliability_profile()/_set_audio_format() expose the FEC profile + mono
 * format that used to be C++-only. Hardware-free with the dummy backend (-m 1).
 */

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <hamlib/rig.h>
#include <hamlib/riglist.h>

#include "naudio.h"

/* ------------------------------------------------------------------ config + globals */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* A mutex-only byte FIFO for TX frames: the na_server_tx_audio_cb (mixer thread, MUST NOT
 * block) pushes; the TX drain thread pops. Non-blocking both ways — push drops the oldest
 * bytes on overflow so a stalled radio write can never wedge the naudio mixer. */
typedef struct {
    unsigned char *buf;
    size_t cap, head, count;
    pthread_mutex_t m;
} byte_ring;

static int ring_init(byte_ring *r, size_t cap) {
    r->buf = (unsigned char *)malloc(cap);
    if (!r->buf) return -1;
    r->cap = cap; r->head = r->count = 0;
    return pthread_mutex_init(&r->m, NULL);
}
static void ring_free(byte_ring *r) {
    if (r->buf) { free(r->buf); r->buf = NULL; }
    pthread_mutex_destroy(&r->m);
}
static void ring_push(byte_ring *r, const unsigned char *src, size_t n) {
    pthread_mutex_lock(&r->m);
    if (n > r->cap) { src += n - r->cap; n = r->cap; }  /* keep only the newest cap bytes */
    if (r->count + n > r->cap) {                        /* drop oldest to make room */
        size_t drop = r->count + n - r->cap;
        r->head = (r->head + drop) % r->cap;
        r->count -= drop;
    }
    size_t tail = (r->head + r->count) % r->cap;
    size_t first = n < (r->cap - tail) ? n : (r->cap - tail);
    memcpy(r->buf + tail, src, first);
    memcpy(r->buf, src + first, n - first);
    r->count += n;
    pthread_mutex_unlock(&r->m);
}
static size_t ring_pop(byte_ring *r, unsigned char *dst, size_t max) {
    pthread_mutex_lock(&r->m);
    size_t n = r->count < max ? r->count : max;
    size_t first = n < (r->cap - r->head) ? n : (r->cap - r->head);
    memcpy(dst, r->buf + r->head, first);
    memcpy(dst + first, r->buf, n - first);
    r->head = (r->head + n) % r->cap;
    r->count -= n;
    pthread_mutex_unlock(&r->m);
    return n;
}

/* Shared context for the worker threads + the TX callback. */
typedef struct {
    RIG *rig;
    rig_stream_t *rx;
    rig_stream_t *tx;               /* NULL if TX not available */
    na_audio_server *srv;
    byte_ring txring;
    int use_ptt;
} bridge;

/* ------------------------------------------------------------------ TX: naudio -> ring */

static void on_tx_frame(const unsigned char *pcm, size_t n_bytes, void *user) {
    bridge *b = (bridge *)user;
    ring_push(&b->txring, pcm, n_bytes);   /* non-blocking; mixer thread must not stall */
}

/* ------------------------------------------------------------------ RX feeder thread */

static void *rx_thread(void *arg) {
    bridge *b = (bridge *)arg;
    unsigned char buf[16384];
    struct rig_stream_read_info info;
    for (;;) {
        if (g_stop) break;
        size_t got = 0;
        int r = rig_stream_read(b->rig, b->rx, buf, sizeof buf, &got, 200, &info);
        if (r == RIG_OK && got > 0) {
            na_server_inject_audio(b->srv, buf, (int)got);   /* copy-and-return; fans out + FEC */
        } else if (r == -RIG_ETIMEOUT || (r == RIG_OK && got == 0)) {
            continue;                                         /* no data this tick */
        } else if (r == -RIG_ENAVAIL) {
            break;                                            /* stream closing */
        } else {
            fprintf(stderr, "na_hamlib_bridge: rig_stream_read: %s\n", rigerror(r));
            break;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ TX drain thread */

static void *tx_thread(void *arg) {
    bridge *b = (bridge *)arg;
    unsigned char tmp[16384];
    int ptt_on = 0;
    for (;;) {
        if (g_stop) break;
        size_t n = ring_pop(&b->txring, tmp, sizeof tmp);
        /* Is a client actually transmitting? (len-return convention: 0 == no owner). */
        int has_owner = na_server_tx_owner(b->srv, NULL, 0) > 0;
        if (b->use_ptt) {
            if (has_owner && !ptt_on) { rig_set_ptt(b->rig, RIG_VFO_CURR, RIG_PTT_ON);  ptt_on = 1; }
            else if (!has_owner && ptt_on) { rig_set_ptt(b->rig, RIG_VFO_CURR, RIG_PTT_OFF); ptt_on = 0; }
        }
        if (n > 0 && has_owner) {
            size_t written = 0;
            int r = rig_stream_write(b->rig, b->tx, tmp, n, &written, 200, NULL);
            if (r != RIG_OK && r != -RIG_ETIMEOUT) {
                fprintf(stderr, "na_hamlib_bridge: rig_stream_write: %s\n", rigerror(r));
            }
        } else if (n == 0) {
            sleep_ms(5);   /* idle: nothing queued — poll gently */
        }
        /* n > 0 && !has_owner: silence/stale frame, already popped — discard. */
    }
    if (ptt_on) rig_set_ptt(b->rig, RIG_VFO_CURR, RIG_PTT_OFF);
    return NULL;
}

/* ------------------------------------------------------------------ helpers */

static na_reliability_profile parse_profile(const char *s) {
    if (strcmp(s, "lan") == 0) return NA_RELIABILITY_UDP_LAN;
    if (strcmp(s, "ft8") == 0) return NA_RELIABILITY_UDP_FT8;
    return NA_RELIABILITY_UDP_WAN;   /* default */
}

/* Open one stream of `type` as S16 @ 48k / `channels`. On failure, dump the backend's caps
 * so the user can see what it actually offers. Returns RIG_OK / negative. */
static int open_stream(RIG *rig, rig_stream_type_t type, int channels, rig_stream_t **out) {
    struct rig_stream_config *cfg = rig_stream_config_alloc();
    if (!cfg) return -RIG_ENOMEM;
    cfg->type = type;
    cfg->format = RIG_STREAM_FORMAT_PCM_S16;
    cfg->sample_rate = 48000;
    cfg->channels = channels;
    int r = rig_stream_open(rig, cfg, out);
    rig_stream_config_free(cfg);
    if (r != RIG_OK) {
        fprintf(stderr, "na_hamlib_bridge: rig_stream_open(type=%d, S16@48k/%dch): %s\n",
                (int)type, channels, rigerror(r));
        int n = rig_stream_caps_count(rig);
        for (int i = 0; i < n; i++) {
            const struct rig_stream_caps *c = rig_stream_caps_at(rig, i);
            if (c) fprintf(stderr, "  caps[%d]: type=%d formats=0x%x channels=%d..%d\n",
                           i, (int)c->type, (unsigned)c->formats, c->channels_min, c->channels_max);
        }
    }
    return r;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [-m model] [-r host:port] [-p naudio_port] [-c channels]\n"
        "          [-R lan|wan|ft8] [-S stream_mode] [-k] [-x]\n"
        "  -m  Hamlib model (default 1=DUMMY hardware-free; 2=NETRIGCTL)\n"
        "  -r  netrigctl target host:port (model 2), e.g. localhost:5555\n"
        "  -S  backend stream_mode conf (dummy: tone|silence|loopback|counter).\n"
        "      'loopback' feeds TX back out of RX — a hardware-free TX round-trip.\n"
        "  -p  naudio server port (default 4533)\n"
        "  -c  channels 1 or 2 (default 1)\n"
        "  -R  reliability profile: wan (default, FEC), lan, ft8\n"
        "  -k  key rig PTT around client TX bursts\n"
        "  -x  disable the TX (operator->rig) direction; RX only\n"
        "Runs at the radio site, upstream of the lossy hop.\n", argv0);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv) {
    int model = 1;                     /* RIG_MODEL_DUMMY */
    const char *rig_file = NULL;
    const char *stream_mode = NULL;    /* unset => leave the backend's own default alone */
    int na_port = 4533;
    int channels = 1;
    na_reliability_profile profile = NA_RELIABILITY_UDP_WAN;
    int use_ptt = 0;
    int want_tx = 1;

    rig_set_debug(RIG_DEBUG_WARN);   /* a bridge daemon, not a trace tool */

    int opt;
    while ((opt = getopt(argc, argv, "m:r:p:c:R:S:kxh")) != -1) {
        switch (opt) {
            case 'm': model = atoi(optarg); break;
            case 'r': rig_file = optarg; break;
            case 'S': stream_mode = optarg; break;
            case 'p': na_port = atoi(optarg); break;
            case 'c': channels = atoi(optarg); break;
            case 'R': profile = parse_profile(optarg); break;
            case 'k': use_ptt = 1; break;
            case 'x': want_tx = 0; break;
            case 'h': default: usage(argv[0]); return (opt == 'h') ? 0 : 2;
        }
    }
    if (channels != 1 && channels != 2) {
        fprintf(stderr, "na_hamlib_bridge: channels must be 1 or 2\n");
        return 2;
    }

    bridge b;
    memset(&b, 0, sizeof b);
    b.use_ptt = use_ptt;
    if (ring_init(&b.txring, 1u << 18) != 0) {   /* 256 KB TX FIFO */
        fprintf(stderr, "na_hamlib_bridge: ring_init failed\n");
        return 1;
    }

    /* ---- Hamlib source ---- */
    b.rig = rig_init((rig_model_t)model);
    if (!b.rig) {
        fprintf(stderr, "na_hamlib_bridge: rig_init(%d) failed\n", model);
        ring_free(&b.txring);
        return 1;
    }
    if (rig_file) {
        /* Public route to the port path (the RIG struct's port pointer is HL_PRIVATE):
         * the "rig_pathname" frontend conf token. For NETRIGCTL this is "host:port". */
        int rc = rig_set_conf(b.rig, rig_token_lookup(b.rig, "rig_pathname"), rig_file);
        if (rc != RIG_OK)
            fprintf(stderr, "na_hamlib_bridge: warning: set rig_pathname(%s): %s\n",
                    rig_file, rigerror(rc));
    }
    if (stream_mode) {
        /* Backend-specific conf, read when the stream opens — so it must be set before
         * rig_stream_open. The dummy backend's "loopback" mode feeds written TX audio back out of
         * the RX stream, which is what makes a hardware-free TX round-trip observable. */
        int rc = rig_set_conf(b.rig, rig_token_lookup(b.rig, "stream_mode"), stream_mode);
        if (rc != RIG_OK)
            fprintf(stderr, "na_hamlib_bridge: warning: set stream_mode(%s): %s\n",
                    stream_mode, rigerror(rc));
    }
    int r = rig_open(b.rig);
    if (r != RIG_OK) {
        fprintf(stderr, "na_hamlib_bridge: rig_open: %s\n", rigerror(r));
        rig_cleanup(b.rig);
        ring_free(&b.txring);
        return 1;
    }
    if (open_stream(b.rig, RIG_STREAM_TYPE_AUDIO_RX, channels, &b.rx) != RIG_OK) {
        rig_close(b.rig); rig_cleanup(b.rig); ring_free(&b.txring);
        return 1;
    }
    if (want_tx) {
        if (open_stream(b.rig, RIG_STREAM_TYPE_AUDIO_TX, channels, &b.tx) != RIG_OK) {
            fprintf(stderr, "na_hamlib_bridge: TX stream unavailable — continuing RX-only\n");
            b.tx = NULL;
        }
    }

    /* ---- naudio sink (fan-out + FEC) ---- */
    b.srv = na_server_create(NA_SERVER_BACKEND_NULL, na_port);
    if (!b.srv) {
        fprintf(stderr, "na_hamlib_bridge: na_server_create: %s\n", na_strerror(na_last_error()));
        goto teardown_rig;
    }
    if (na_server_set_reliability_profile(b.srv, profile) != NA_OK ||
        na_server_set_audio_format(b.srv, 48000, 16, channels) != NA_OK) {
        fprintf(stderr, "na_hamlib_bridge: server config: %s\n", na_strerror(na_last_error()));
        goto teardown_all;
    }
    if (b.tx) na_server_set_tx_audio_cb(b.srv, on_tx_frame, &b);

    char err[256];
    if (na_server_start(b.srv, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "na_hamlib_bridge: na_server_start: %s\n", err);
        goto teardown_all;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("na_hamlib_bridge: model=%d %s -> naudio :%d  profile=%s  channels=%d  tx=%s\n",
           model, rig_file ? rig_file : "(local)", na_server_port(b.srv),
           profile == NA_RELIABILITY_UDP_WAN ? "wan" :
           profile == NA_RELIABILITY_UDP_LAN ? "lan" : "ft8",
           channels, b.tx ? (use_ptt ? "on+ptt" : "on") : "off");

    /* ---- run ---- */
    pthread_t rxt, txt;
    pthread_create(&rxt, NULL, rx_thread, &b);
    if (b.tx) pthread_create(&txt, NULL, tx_thread, &b);

    int tick = 0;
    while (!g_stop) {
        sleep_ms(200);
        if (++tick % 25 == 0) {   /* ~every 5s: RX health + roster */
            struct rig_stream_stats st;
            if (rig_stream_get_stats(b.rig, b.rx, &st) == RIG_OK) {
                printf("  rx: clients=%d gaps=%u link_loss=%u overruns=%u underruns=%u\n",
                       na_server_client_count(b.srv), st.gaps, st.link_loss,
                       st.overruns, st.underruns);
                fflush(stdout);
            }
        }
    }

    /* ---- teardown ---- */
    printf("\nna_hamlib_bridge: stopping...\n");
    pthread_join(rxt, NULL);
    if (b.tx) pthread_join(txt, NULL);
teardown_all:
    na_server_stop(b.srv);
    na_server_destroy(b.srv);
teardown_rig:
    if (b.rx) rig_stream_close(b.rig, b.rx);
    if (b.tx) rig_stream_close(b.rig, b.tx);
    rig_close(b.rig);
    rig_cleanup(b.rig);
    ring_free(&b.txring);
    return 0;
}
