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
 * DATA PATH (both directions are straight S16LE byte copies ON THIS SIDE — this file never
 * resamples and never converts a sample format):
 *   RX: rig_stream_read(AUDIO_RX, S16@48k) ---> na_server_inject_audio()  [fan-out + FEC]
 *   TX: na_server_tx_audio_cb() --ring--> rig_stream_write(AUDIO_TX, S16@48k)
 *
 * That is a claim about THIS PROCESS, not about the whole path. Since Hamlib PR #2116 commit
 * 961093f2, libhamlib itself may convert underneath these calls: backends now advertise only
 * their hardware-native capability, and the streaming core serves anything else through a
 * frontend conversion pipeline. S16@48k is not native on every backend — the dummy now
 * advertises PCM_F32|OPUS natively — so the request above is commonly served through an
 * F32->S16 conversion, and on hardware whose native rate is not 48 kHz it adds libsamplerate
 * resampling inside the very hop this bridge exists to keep short. That is a deliberate
 * trade: converting reaches far more hardware than demanding native would, and demanding it
 * (rig_stream_config.require_native = 1) would fail outright against the dummy backend. What
 * is NOT acceptable is doing it silently, so open_stream() reports the active stages at open.
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
static volatile sig_atomic_t g_failed = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* A worker that stops for any reason other than an operator-requested shutdown must take the
 * process down with it. The bridge runs unattended at the radio site, so its expected failure —
 * the rig dropping off — has to surface as a non-zero exit rather than as a live process with a
 * healthy-looking console, an open port, and no audio moving. Called only from the workers; the
 * g_stop check keeps a stream teardown that races a SIGINT from being reported as a failure. */
static void worker_failed(const char *who, const char *why) {
    if (g_stop) return;                                     /* already stopping by request */
    size_t n = strlen(why);
    while (n > 0 && (why[n - 1] == '\n' || why[n - 1] == '\r')) n--;   /* rigerror2() appends \n */
    fprintf(stderr, "na_hamlib_bridge: %s worker stopped: %.*s — shutting down\n",
            who, (int)n, why);
    g_failed = 1;
    g_stop = 1;
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* A mutex-only byte FIFO for TX frames: the na_server_tx_audio_cb (mixer thread, which must not
 * stall) pushes; the TX drain thread pops. Neither side ever waits on the radio — push drops the
 * oldest bytes on overflow, so a stalled rig_stream_write can never wedge the naudio mixer.
 *
 * Both sides DO take r->m, so this is not lock-free and the comment used to overclaim by calling
 * it "non-blocking both ways" (issue #8 item 4). The guarantee it actually gives: a push never
 * waits on I/O, and contends only with the drain thread's bounded memcpy — never for the duration
 * of a radio write, which is the stall that would matter. The mutex is the right call at this
 * contention window; a lock-free SPSC ring would make the stronger claim literally true and is a
 * bigger change than this warrants.
 *
 * Every way this bridge can lose TX audio passes through a ring operation, so the loss
 * counters live here under the ring's own mutex rather than in free-standing globals that
 * two threads would race on. ring_stats() reads them as one consistent snapshot. */
typedef struct {
    unsigned char *buf;
    size_t cap, head, count;
    unsigned long long lost_queue;    /* bytes overwritten on push: radio not draining fast enough */
    unsigned long long lost_requeue;  /* bytes of a short write's tail that no longer fit back */
    unsigned long long short_writes;  /* rig_stream_write calls that accepted less than offered */
    pthread_mutex_t m;
} byte_ring;

/* Snapshot of the above, for the periodic health line. */
typedef struct {
    unsigned long long lost_queue, lost_requeue, short_writes;
} tx_loss;

static int ring_init(byte_ring *r, size_t cap) {
    r->buf = (unsigned char *)malloc(cap);
    if (!r->buf) return -1;
    r->cap = cap; r->head = r->count = 0;
    /* Free the buffer if the mutex will not init: main treats a non-zero return as fatal and
     * returns WITHOUT calling ring_free, so nothing else would ever release it (issue #8 item 1).
     * NULLing it keeps ring_free idempotent for any future caller that does clean up. */
    int rc = pthread_mutex_init(&r->m, NULL);
    if (rc != 0) { free(r->buf); r->buf = NULL; }
    return rc;
}
static void ring_free(byte_ring *r) {
    if (r->buf) { free(r->buf); r->buf = NULL; }
    pthread_mutex_destroy(&r->m);
}
static void ring_push(byte_ring *r, const unsigned char *src, size_t n) {
    pthread_mutex_lock(&r->m);
    if (n > r->cap) {                                   /* keep only the newest cap bytes */
        r->lost_queue += n - r->cap;
        src += n - r->cap;
        n = r->cap;
    }
    if (r->count + n > r->cap) {                        /* drop oldest to make room */
        size_t drop = r->count + n - r->cap;
        r->lost_queue += drop;
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
/* Put the unconsumed tail of a short write back at the FRONT of the ring, so the next drain pass
 * retries it in order instead of dropping it. Those bytes are already off the ring, so whatever
 * this does not hand back is transmitted audio that disappears — hence the accounting.
 *
 * When the mixer has filled the ring behind us, the leading (oldest) bytes of the tail go. That is
 * the same "keep the newest" policy ring_push applies on overflow, and it is what keeps the bytes
 * that survive contiguous with the newer frames already queued behind them. */
static void ring_requeue(byte_ring *r, const unsigned char *src, size_t n) {
    pthread_mutex_lock(&r->m);
    r->short_writes++;
    size_t space = r->cap - r->count;
    if (n > space) {
        r->lost_requeue += n - space;
        src += n - space;
        n = space;
    }
    r->head = (r->head + r->cap - n) % r->cap;          /* walk head back over the returned tail */
    size_t first = n < (r->cap - r->head) ? n : (r->cap - r->head);
    memcpy(r->buf + r->head, src, first);
    memcpy(r->buf, src + first, n - first);
    r->count += n;
    pthread_mutex_unlock(&r->m);
}
static void ring_stats(byte_ring *r, tx_loss *out) {
    pthread_mutex_lock(&r->m);
    out->lost_queue   = r->lost_queue;
    out->lost_requeue = r->lost_requeue;
    out->short_writes = r->short_writes;
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

/* How much RX audio actually reached naudio, over how long, and in how many reads. A stream that
 * opens successfully is not a stream that delivers what it agreed to: the peer may accept a
 * channel count and then send a different one, and nothing in rig_stream_* reports that back — the
 * negotiated count is write-only from the caller's side. Same policy as the TX loss counters: a
 * shared counter lives under a mutex, not in a free-standing global two threads race on.
 *
 * The read COUNT is what makes the check work. Delivered bytes/second is not a usable alarm on its
 * own, because it says as much about the producer as about the framing — a dummy in `loopback`
 * with no TX peer paces itself off nanosleep and lands near 70% of nominal at a perfectly correct
 * channels=1. Rate is reported because it is useful to see; the alarm is the gap signature. */
typedef struct {
    pthread_mutex_t m;
    unsigned long long bytes;
    unsigned long long reads;
    struct timespec t0;
} rx_meter;

static int rx_meter_init(rx_meter *r) {
    r->bytes = 0;
    r->reads = 0;
    clock_gettime(CLOCK_MONOTONIC, &r->t0);
    return pthread_mutex_init(&r->m, NULL);
}
/* Restart the clock once the stream is actually open. rig_init + stream open + na_server_start
 * take real time and deliver no audio, so timing from rx_meter_init would understate the rate and
 * report a shortfall that is only startup cost. */
static void rx_meter_start(rx_meter *r) {
    pthread_mutex_lock(&r->m);
    r->bytes = 0;
    r->reads = 0;
    clock_gettime(CLOCK_MONOTONIC, &r->t0);
    pthread_mutex_unlock(&r->m);
}
static void rx_meter_add(rx_meter *r, size_t n) {
    pthread_mutex_lock(&r->m);
    r->bytes += n;
    r->reads++;
    pthread_mutex_unlock(&r->m);
}
/* Snapshot bytes, reads and elapsed seconds together, so the rate they form is self-consistent. */
static void rx_meter_read(rx_meter *r, unsigned long long *bytes, unsigned long long *reads,
                          double *secs) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&r->m);
    *bytes = r->bytes;
    *reads = r->reads;
    *secs = (double)(now.tv_sec - r->t0.tv_sec)
            + (double)(now.tv_nsec - r->t0.tv_nsec) / 1e9;
    pthread_mutex_unlock(&r->m);
}
static void rx_meter_free(rx_meter *r) { pthread_mutex_destroy(&r->m); }

/* Shared context for the worker threads + the TX callback. */
typedef struct {
    RIG *rig;
    rig_stream_t *rx;
    rig_stream_t *tx;               /* NULL if TX not available */
    na_audio_server *srv;
    byte_ring txring;
    rx_meter rxm;
    int use_ptt;
} bridge;

/* ------------------------------------------------------------------ naudio server events */

/* The naudio server names every fault it hits — a refused send, an outbound backlog, a heartbeat
 * that could not go out, a connection that timed out — through notifyError. Until now this bridge
 * registered no callbacks table at all, so all of it fanned out to nobody: the C ABI forwards
 * onError only when cbs.on_error is set. That is the open half of issue #17.
 *
 * The one that motivated this is worth naming, because it is the reason a TX lapse looked causeless.
 * writerLoop closes a session whose send failed, and closing it unregisters it from the mixer, which
 * releases any TX channel it held — an operator mid-transmission goes off the air. The library has
 * reported the direction and the size of that failed send since the #17 investigation added it
 * ("Send error: RX audio frame (16407 bytes)"), precisely so an oversized-frame kill is
 * distinguishable from a client that genuinely went away. On this path the message was constructed,
 * posted to the dispatch thread, and dropped for want of a listener.
 *
 * Errors go to stderr, roster changes to stdout, matching the rest of this file: stderr is what a
 * supervisor's journal shows first, and the roster lines belong beside the periodic health block. */
static void on_server_error(const char *client_id, const char *message, void *user) {
    (void)user;
    /* An empty client_id means the fault is the server's own rather than one session's — an accept
     * error or a lost device (AudioStreamServer.cpp notifyError call sites). Do not print an empty
     * bracket for it. */
    if (client_id && *client_id)
        fprintf(stderr, "na_hamlib_bridge: server error [%s]: %s\n", client_id, message);
    else
        fprintf(stderr, "na_hamlib_bridge: server error: %s\n", message);
}

static void on_client_connected(const char *client_id, const char *addr, void *user) {
    (void)user;
    printf("na_hamlib_bridge: client %s connected from %s\n", client_id, addr);
    fflush(stdout);
}

/* The other half of a TX lapse caused by a dropped session: the error above says WHY, this says the
 * client is gone. Either alone is ambiguous — a disconnect with no error is an ordinary client
 * leaving, and #17's whole complaint was that the operator got neither. */
static void on_client_disconnected(const char *client_id, void *user) {
    (void)user;
    printf("na_hamlib_bridge: client %s disconnected\n", client_id);
    fflush(stdout);
}

/* ------------------------------------------------------------------ TX: naudio -> ring */

static void on_tx_frame(const unsigned char *pcm, size_t n_bytes, void *user) {
    bridge *b = (bridge *)user;
    /* Never waits on I/O; contends only with the drain thread's bounded memcpy. See the byte_ring
     * comment — this is not lock-free, and the mixer thread must not stall on the radio. */
    ring_push(&b->txring, pcm, n_bytes);
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
            rx_meter_add(&b->rxm, got);
            na_server_inject_audio(b->srv, buf, (int)got);   /* copy-and-return; fans out + FEC */
        } else if (r == -RIG_ETIMEOUT || (r == RIG_OK && got == 0)) {
            continue;                                         /* no data this tick */
        } else if (r == -RIG_ENAVAIL) {
            worker_failed("rx", "rig_stream_read: stream closed");
            break;
        } else {
            worker_failed("rx", rigerror2(r));
            break;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ TX drain thread */

static void *tx_thread(void *arg) {
    bridge *b = (bridge *)arg;
    unsigned char tmp[16384];
    /* Size each write to the STREAM's negotiated budget, not to this local buffer. Over netrigctl
     * that budget is one UDP datagram's worth of samples (1420 B at the default 1500 MTU), and an
     * over-budget write is rejected OUTRIGHT with -RIG_EIO rather than partially accepted — which,
     * since a dead worker now stops the bridge, means the operator's first keyup over -m 2 shuts the
     * whole thing down. The value is frame-aligned by construction and fixed for the life of the
     * stream, so read it once. A non-positive answer means the backend publishes no budget: fall
     * back to the buffer, which is the pre-existing behaviour. */
    int budget = rig_stream_get_max_payload(b->tx);
    size_t chunk = (budget > 0 && (size_t)budget < sizeof tmp) ? (size_t)budget : sizeof tmp;
    printf("na_hamlib_bridge: tx write budget %zu bytes/call\n", chunk);
    fflush(stdout);
    int ptt_on = 0;
    /* TX-ownership episodes. Issue #17's signature is ownership claimed at the start of a run, held
     * for about a second, and then gone for the rest of it — while the mixer keeps handing this
     * thread silence frames that the `has_owner` test below correctly discards. Nothing on this path
     * said so, which is what made the operator's audio disappear quietly.
     *
     * A line per transition, carrying how long the episode lasted and how many bytes actually
     * reached the radio, makes that shape readable in the log on its own — whether or not the cause
     * announces itself through on_server_error above. The counters are local to this loop and read
     * by nobody else, so unlike the ring's loss counters and the RX meter they need no mutex.
     *
     * A run that is still transmitting when SIGINT arrives leaves its final episode unclosed and
     * unprinted. That is deliberate: it is the operator's own shutdown, not a lapse. */
    int owned = 0;
    struct timespec owned_since = {0, 0};
    unsigned long long owned_written = 0;
    for (;;) {
        if (g_stop) break;
        size_t n = ring_pop(&b->txring, tmp, chunk);
        /* Is a client actually transmitting? The len-return convention makes 0 mean "no owner"
         * (naudio.h: `"" (length 0) if no owner`), so this tests the owner STRING's length rather
         * than an explicit has-owner flag. That is only correct while the server cannot report an
         * owner whose id is empty — checked in the tree rather than assumed (issue #8 item 3):
         * session ids are minted as "audio-" + a counter at AudioStreamServer.cpp's handleNewClient,
         * the only site that builds one, and AudioMixer::submitTxAudio rejects an id with no
         * matching session before any claim can be recorded. An empty-string owner is therefore
         * unrepresentable and "length 0 == nobody is transmitting" holds. */
        int has_owner = na_server_tx_owner(b->srv, NULL, 0) > 0;
        if (has_owner != owned) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (has_owner) {
                owned_since = now;
                owned_written = 0;
                printf("na_hamlib_bridge: tx owner acquired\n");
            } else {
                double held = (double)(now.tv_sec - owned_since.tv_sec)
                            + (double)(now.tv_nsec - owned_since.tv_nsec) / 1e9;
                printf("na_hamlib_bridge: tx owner released after %.2fs, %llu B to the rig\n",
                       held, owned_written);
            }
            fflush(stdout);
            owned = has_owner;
        }
        if (b->use_ptt) {
            if (has_owner && !ptt_on) { rig_set_ptt(b->rig, RIG_VFO_CURR, RIG_PTT_ON);  ptt_on = 1; }
            else if (!has_owner && ptt_on) { rig_set_ptt(b->rig, RIG_VFO_CURR, RIG_PTT_OFF); ptt_on = 0; }
        }
        if (n > 0 && has_owner) {
            /* Init matters: the early-return error paths in rig_stream_write leave *bytes_written
             * untouched, so 0 is what "the radio took nothing" has to read as. */
            size_t written = 0;
            int r = rig_stream_write(b->rig, b->tx, tmp, n, &written, 200, NULL);
            if (r == -RIG_ENAVAIL) {
                worker_failed("tx", "rig_stream_write: stream closed");
                break;
            } else if (r != RIG_OK && r != -RIG_ETIMEOUT) {
                worker_failed("tx", rigerror2(r));
                break;
            }
            if (written > n) written = n;   /* never trust a count past what we offered */
            owned_written += written;       /* what this episode actually delivered to the radio */
            if (written < n) {
                /* Short write — including the -RIG_ETIMEOUT case, where the popped bytes are just
                 * as gone as on the success path. Hand the tail back and retry it next pass, which
                 * re-checks PTT and TX ownership first; that is why this requeues rather than
                 * looping here. */
                ring_requeue(&b->txring, tmp + written, n - written);
                /* Back off ONLY when the radio took nothing. Partial progress usually means a small
                 * per-call payload budget rather than a radio that needs time — note that netrigctl
                 * is NOT an example: it accepts a write whole or rejects it whole, which is why the
                 * pop above is sized to its budget instead of being left to short-write here.
                 * rig_stream_write already blocks up to timeout_ms when a slow radio really is the
                 * problem, so a sleep here would just double-pace it. Retrying at once costs
                 * nothing lasting either: the loop spins only while a backlog exists, and
                 * draining the backlog is what ends the spin. Measured, not assumed — pacing every
                 * short write instead of only the stalled ones cost 206 KB of TX audio on a
                 * 16-bytes-per-call backend that the unpaced loop carried without a single drop. */
                if (written == 0) sleep_ms(2);
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

/* Print a 0-terminated rate list from struct rig_stream_caps, bounded by the array size so a
 * caps block that fills every slot without a terminator cannot run off the end. */
static void print_rates(FILE *f, const int *rates) {
    for (int i = 0; i < HAMLIB_MAX_STREAM_RATES && rates[i]; i++)
        fprintf(f, "%s%d", i ? "," : "", rates[i]);
}

/* Name the conversion stages libhamlib is running between the hardware and an open stream.
 *
 * The bridge asks for S16@48k because that is what naudio carries. Since PR #2116 commit
 * 961093f2 that request is served whether or not the hardware speaks it, so silence here is
 * ambiguous — it could mean a native stream or an undisclosed resample sitting in the local
 * hop. One line at open removes the ambiguity. Reported, never enforced: require_native is
 * left at 0 deliberately (see the DATA PATH note in this file's header).
 *
 * Compiled out against a streaming libhamlib built before 961093f2, which has no such call. */
static void report_conversions(rig_stream_t *stream, rig_stream_type_t type) {
#ifdef NAUDIO_HAMLIB_HAS_STREAM_CONV
    int conv = rig_stream_get_conversions(stream);
    if (conv < 0) {
        fprintf(stderr, "na_hamlib_bridge: stream type=%d: cannot read conversion state: %s\n",
                (int)type, rigerror(conv));
        return;
    }
    if (conv == RIG_STREAM_CONV_NONE) {
        printf("na_hamlib_bridge: stream type=%d S16@48k is NATIVE (libhamlib converts nothing)\n",
               (int)type);
    } else {
        /* Rate conversion is called out first and by name: it is the one stage that adds
         * latency and libsamplerate cost to the hop this bridge exists to keep short. */
        printf("na_hamlib_bridge: stream type=%d S16@48k is CONVERTED by libhamlib:%s%s%s "
               "(conv=0x%x)\n",
               (int)type,
               (conv & RIG_STREAM_CONV_RATE)     ? " resample" : "",
               (conv & RIG_STREAM_CONV_FORMAT)   ? " sample-format" : "",
               (conv & RIG_STREAM_CONV_CHANNELS) ? " channel-map" : "",
               (unsigned)conv);
    }
    fflush(stdout);
#else
    (void)stream; (void)type;
#endif
}

/* Open one stream of `type` as S16 @ 48k / `channels`. On success, report what libhamlib is
 * converting. On failure, dump the backend's caps so the user can see what it actually offers.
 * Returns RIG_OK / negative. */
static int open_stream(RIG *rig, rig_stream_type_t type, int channels, rig_stream_t **out) {
    struct rig_stream_config *cfg = rig_stream_config_alloc();
    if (!cfg) return -RIG_ENOMEM;
    cfg->type = type;
    cfg->format = RIG_STREAM_FORMAT_PCM_S16;
    cfg->sample_rate = 48000;
    cfg->channels = channels;
    int r = rig_stream_open(rig, cfg, out);
    rig_stream_config_free(cfg);
    if (r == RIG_OK) {
        report_conversions(*out, type);
        return r;
    }
    fprintf(stderr, "na_hamlib_bridge: rig_stream_open(type=%d, S16@48k/%dch): %s\n",
            (int)type, channels, rigerror(r));
    int n = rig_stream_caps_count(rig);
    for (int i = 0; i < n; i++) {
        const struct rig_stream_caps *c = rig_stream_caps_at(rig, i);
        if (!c) continue;
        /* Both views. The classic fields are the EFFECTIVE set — everything rig_stream_open
         * would accept, conversions included — so on their own they no longer tell a reader
         * what the hardware does, which is the question a failed open raises. */
        fprintf(stderr, "  caps[%d]: type=%d formats=0x%x channels=%d..%d rates=",
                i, (int)c->type, (unsigned)c->formats, c->channels_min, c->channels_max);
        print_rates(stderr, c->sample_rates);
#ifdef NAUDIO_HAMLIB_HAS_STREAM_CONV
        fprintf(stderr, "\n            native: formats=0x%x channels=%d..%d rates=",
                (unsigned)c->native_formats, c->native_channels_min, c->native_channels_max);
        print_rates(stderr, c->native_sample_rates);
#endif
        fputc('\n', stderr);
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

/* What to tell an operator who is staring at the gap signature below. The diagnosis is the same
 * either way — the two ends disagree about frame size — but the ACTION depends on which libhamlib
 * this bridge is linked against, so it is chosen at compile time rather than guessed at runtime.
 *
 * NAUDIO_HAMLIB_HAS_STREAM_CONV is a proxy for "at or after Hamlib PR #2116 commit 961093f2": that
 * commit added rig_stream_get_conversions() (which the gate detects) and, in the same change, made
 * netrigctl_stream_open forward `channels=` on the wire (which is what fixes -c 2 over -m 2).
 *
 * The dependency binds on THIS side, not the peer's — measured, not assumed. With a bridge linked
 * against 961093f2, -c 2 over -m 2 held 0 gaps at 83% of nominal against BOTH an old rigctld
 * (9f412fe, which still opens its rig mono) and a new one, and a naudio client read 429112/430080
 * non-zero samples off the old-peer run: fully populated stereo, not mono padded with silence.
 * So the fix an operator needs is a newer libhamlib HERE; upgrading the remote rigctld alone does
 * nothing for this, and an old bridge against a new rigctld fails to open the stream at all. */
static const char rx_gap_advice[] =
#ifndef NAUDIO_HAMLIB_HAS_STREAM_CONV
    "  This bridge is linked against a libhamlib older than Hamlib 961093f2, whose \\stream_open\n"
    "  carries no channels field — so over netrigctl (-m 2) rigctld opens the rig MONO however its\n"
    "  caps advertise. Rebuild THIS bridge against a libhamlib at or after that commit (the remote\n"
    "  rigctld's own version does not matter), or run -c 1 here.\n"
#else
    "  This bridge's libhamlib is at or after Hamlib 961093f2, which forwards channels= on\n"
    "  \\stream_open — and -c 2 over netrigctl measures clean against that build, with an old peer\n"
    "  as well as a new one. So the known netrigctl channels limit is NOT the explanation here.\n"
#endif
    "  See docs/hamlib-streaming-bridge.md.\n";

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
    /* -p goes through atoi like -c, and without this check an out-of-range or non-numeric value
     * reached na_server_create with a less specific complaint than the tool can give (issue #8
     * item 2). Port 0 is the case worth naming: atoi("abc") yields it, and 0 means "bind any free
     * port" to the socket layer — so a typo would silently bring the bridge up somewhere the
     * operator never asked for, and every client pointed at the intended port would simply fail
     * to find it. Rejecting it costs one comparison. */
    if (na_port < 1 || na_port > 65535) {
        fprintf(stderr, "na_hamlib_bridge: port must be 1..65535\n");
        return 2;
    }

    bridge b;
    memset(&b, 0, sizeof b);
    b.use_ptt = use_ptt;
    if (ring_init(&b.txring, 1u << 18) != 0) {   /* 256 KB TX FIFO */
        fprintf(stderr, "na_hamlib_bridge: ring_init failed\n");
        return 1;
    }
    if (rx_meter_init(&b.rxm) != 0) {
        fprintf(stderr, "na_hamlib_bridge: rx_meter_init failed\n");
        ring_free(&b.txring);
        return 1;
    }

    /* ---- Hamlib source ---- */
    /* Handlers go in BEFORE the first slow call, not after the server is up. Everything below —
     * rig_init, rig_set_conf, rig_open, both rig_stream_open calls, na_server_create and
     * na_server_start — is setup, and over -m 2 (netrigctl) several of those are real network
     * round-trips against rigctld. Installing the handlers afterwards left that whole window
     * unprotected, so a Ctrl-C anywhere in it took the DEFAULT action: the process died with the
     * rig open and streams allocated, with no rig_close and no rig_stream_close (issue #7).
     *
     * Nothing in the setup path reads g_stop, so a signal arriving here is simply absorbed: setup
     * runs to completion, both workers see g_stop on their first check and return at once, the
     * main loop is skipped, and teardown runs normally — an orderly shutdown that exits 0.
     * worker_failed() early-returns while g_stop is set, so that shutdown is not misreported as a
     * worker death. If a signal does interrupt a setup call badly enough to fail it, the failure is
     * reported and exits non-zero on its own path, which is still an orderly teardown. */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    b.rig = rig_init((rig_model_t)model);
    if (!b.rig) {
        fprintf(stderr, "na_hamlib_bridge: rig_init(%d) failed\n", model);
        ring_free(&b.txring);
        rx_meter_free(&b.rxm);
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
        rx_meter_free(&b.rxm);
        return 1;
    }
    if (open_stream(b.rig, RIG_STREAM_TYPE_AUDIO_RX, channels, &b.rx) != RIG_OK) {
        rig_close(b.rig); rig_cleanup(b.rig); ring_free(&b.txring); rx_meter_free(&b.rxm);
        return 1;
    }
    if (want_tx) {
        if (open_stream(b.rig, RIG_STREAM_TYPE_AUDIO_TX, channels, &b.tx) != RIG_OK) {
            fprintf(stderr, "na_hamlib_bridge: TX stream unavailable — continuing RX-only\n");
            b.tx = NULL;
        }
    }

    /* ---- naudio sink (fan-out + FEC) ---- */
    /* Every failure from here down reaches the shared teardown by goto and then falls through to
     * the single return at the bottom of main(). That return reports g_failed, which ONLY a dead
     * worker sets (worker_failed) — so these three startup failures each printed a diagnostic to
     * stderr and then exited 0. That is invisible interactively and not at all invisible to a
     * supervisor: systemd, launchd, a wrapper script or a container restart policy reads 0 as "the
     * process completed its work and shut down cleanly" and so does NOT restart. The bridge never
     * comes up, and its exit status actively argues against intervention.
     *
     * Tracked separately from g_failed rather than folded into it: g_failed means "a worker died
     * mid-run", this means "we never started", and the two reach the same teardown from opposite
     * ends of the process lifetime. */
    int startup_failed = 0;
    b.srv = na_server_create(NA_SERVER_BACKEND_NULL, na_port);
    if (!b.srv) {
        fprintf(stderr, "na_hamlib_bridge: na_server_create: %s\n", na_strerror(na_last_error()));
        startup_failed = 1;
        goto teardown_rig;
    }
    if (na_server_set_reliability_profile(b.srv, profile) != NA_OK ||
        na_server_set_audio_format(b.srv, 48000, 16, channels) != NA_OK) {
        fprintf(stderr, "na_hamlib_bridge: server config: %s\n", na_strerror(na_last_error()));
        startup_failed = 1;
        goto teardown_all;
    }
    /* Roster + fault reporting. This MUST precede na_server_start — na_server_set_callbacks refuses
     * with NA_ERR_INVALID once the server has been started, because the dispatch worker reads the
     * table with no lock. The struct is caller-allocated and carries its own size, so the memset +
     * struct_size pair is the documented way to stay forward-compatible (naudio.h).
     *
     * A failure here is fatal for the same reason the config calls above are: it can only mean the
     * ABI contract was broken (a NULL server, a short struct, or a server already started), and a
     * bridge that comes up unable to report its own faults is the exact condition issue #17 is
     * about. Better to refuse to start than to run blind. */
    na_server_callbacks cbs;
    memset(&cbs, 0, sizeof cbs);
    cbs.struct_size = sizeof cbs;
    cbs.on_client_connected = on_client_connected;
    cbs.on_client_disconnected = on_client_disconnected;
    cbs.on_error = on_server_error;
    if (na_server_set_callbacks(b.srv, &cbs, NULL) != NA_OK) {
        fprintf(stderr, "na_hamlib_bridge: server callbacks: %s\n", na_strerror(na_last_error()));
        startup_failed = 1;
        goto teardown_all;
    }
    if (b.tx) na_server_set_tx_audio_cb(b.srv, on_tx_frame, &b);

    char err[256];
    /* The likeliest of the three in real deployment: the port is already taken, usually by a
     * previous instance of this bridge that has not fully exited. */
    if (na_server_start(b.srv, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "na_hamlib_bridge: na_server_start: %s\n", err);
        startup_failed = 1;
        goto teardown_all;
    }

    printf("na_hamlib_bridge: model=%d %s -> naudio :%d  profile=%s  channels=%d  tx=%s\n",
           model, rig_file ? rig_file : "(local)", na_server_port(b.srv),
           profile == NA_RELIABILITY_UDP_WAN ? "wan" :
           profile == NA_RELIABILITY_UDP_LAN ? "lan" : "ft8",
           channels, b.tx ? (use_ptt ? "on+ptt" : "on") : "off");
    fflush(stdout);   /* redirected to a file this sits in the buffer until the first health tick,
                       * landing AFTER any stderr diagnostic it is supposed to precede */

    /* ---- run ---- */
    pthread_t rxt, txt;
    rx_meter_start(&b.rxm);
    pthread_create(&rxt, NULL, rx_thread, &b);
    if (b.tx) pthread_create(&txt, NULL, tx_thread, &b);

    int tick = 0;
    int rx_short_reported = 0;
    /* Bytes per second the format handed to na_server_set_audio_format implies. Every byte the
     * bridge injects is charged against this, because naudio re-frames what it is given using
     * exactly this layout — it does not resample or convert (naudio.h, na_server_set_audio_format). */
    const double rx_nominal_bps = 48000.0 * 2.0 * (double)channels;
    while (!g_stop) {
        sleep_ms(200);
        if (++tick % 25 == 0) {   /* ~every 5s: RX health + roster, then TX loss */
            unsigned long long rx_got = 0, rx_reads = 0;
            double rx_secs = 0.0;
            rx_meter_read(&b.rxm, &rx_got, &rx_reads, &rx_secs);

            struct rig_stream_stats st;
            int have_st = (rig_stream_get_stats(b.rig, b.rx, &st) == RIG_OK);
            if (have_st) {
                printf("  rx: clients=%d gaps=%u link_loss=%u overruns=%u underruns=%u\n",
                       na_server_client_count(b.srv), st.gaps, st.link_loss,
                       st.overruns, st.underruns);
            }
            /* Delivered rate, reported but never used as the alarm: it reflects how fast the
             * producer runs as much as whether the framing is right (a `loopback` dummy with no TX
             * peer sits near 70% of nominal while being perfectly correct). */
            if (rx_secs >= 1.0) {
                double bps = (double)rx_got / rx_secs;
                printf("  rx: audio %.0f B/s of %.0f expected (%.0f%%)\n",
                       bps, rx_nominal_bps, 100.0 * bps / rx_nominal_bps);
            }
            /* A gap on nearly every read, with no link loss, is not packet loss — it is the two
             * ends disagreeing about how many bytes make a frame. The sender advances the wire
             * timestamp by its own frame size; the receiver expects payload_len / OUR frame size.
             * When the peer honours a different channel count than it accepted, every packet's
             * timestamp appears to jump and the audio is mis-framed rather than merely thinned.
             * Genuine loss looks nothing like this: it lands far below one gap per read and
             * normally moves link_loss too. */
            if (!rx_short_reported && have_st && rx_reads >= 50
                    && st.link_loss == 0 && (unsigned long long)st.gaps * 2 > rx_reads) {
                rx_short_reported = 1;
                fprintf(stderr,
                    "na_hamlib_bridge: %u gaps over %llu RX reads with link_loss=0 — the peer is\n"
                    "  framing this stream differently than the channels=%d we opened, so naudio is\n"
                    "  re-framing its bytes wrongly and clients get mis-framed audio, not just less.\n"
                    "%s",
                    st.gaps, rx_reads, channels, rx_gap_advice);
                fflush(stderr);
            }
            if (b.tx) {
                /* Three separate ways TX audio goes missing, kept apart because they have
                 * different causes: our FIFO overwritten (the operator is producing faster than
                 * the radio drains), a short write's tail that no longer fit back, and the rig
                 * stream's OWN ring overwriting unread audio — that last one is counted inside
                 * libhamlib and was invisible from here until now. */
                tx_loss L;
                struct rig_stream_stats ts;
                ring_stats(&b.txring, &L);
                if (rig_stream_get_stats(b.rig, b.tx, &ts) == RIG_OK)
                    printf("  tx: short=%llu lost_queue=%lluB lost_requeue=%lluB"
                           " rig_overruns=%u rig_underruns=%u\n",
                           L.short_writes, L.lost_queue, L.lost_requeue,
                           ts.overruns, ts.underruns);
                else
                    printf("  tx: short=%llu lost_queue=%lluB lost_requeue=%lluB\n",
                           L.short_writes, L.lost_queue, L.lost_requeue);
            }
            fflush(stdout);
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
    rx_meter_free(&b.rxm);
    /* Two independent reasons to report failure: a worker that died mid-run, and a startup that
     * never completed. An operator-requested SIGINT/SIGTERM sets neither and still exits 0. */
    return (g_failed || startup_failed) ? 1 : 0;
}
