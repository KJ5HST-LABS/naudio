/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — the client-side reliability counters (na_client_get_stats).
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * WHAT THIS COVERS. na_client_set_reliability_profile turns the loss-recovery layer ON;
 * na_client_get_stats is how a C consumer sees it work. Before it, corroborating FEC recovery from
 * outside the library meant inferring it from delivered-byte parity against a separate no-loss
 * control run — which shows that delivery survived loss, not how many packets were repaired.
 *
 * THE HEADLINE ARM IS LOSSY ON PURPOSE. A counter that has never been observed to increment is an
 * unverified claim, not a feature, and loopback drops nothing: with no loss, the recovering and the
 * non-recovering configuration deliver byte-identical output and packets_recovered_by_fec can only
 * ever read 0. So arm (3) relays the audio through a fixture that discards ONE AUDIO PACKET PER FEC
 * BLOCK (client_stats_proxy.cpp) — exactly the damage single-parity XOR FEC is specified to repair —
 * and asserts the counter moves. Arm (4) is the control: the SAME relay with nothing dropped, where
 * the counter must stay at 0. A detector is not finished when it fires on the fault; it is finished
 * when it also stays quiet on the healthy neighbour.
 *
 * Hardware-free: NULL backends on both ends, loopback UDP, no PortAudio, no radio.
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

/* The lossy relay fixture (client_stats_proxy.cpp — a C++ TU behind extern "C", because the C side
 * has no portable socket layer of its own and naudio::net::Socket already is one). */
void* naproxy_start(int server_port, int block_size, int drop_ordinal, int* out_port);
long long naproxy_audio_seen(void* handle);
long long naproxy_audio_dropped(void* handle);
long long naproxy_parity_forwarded(void* handle);
void naproxy_stop(void* handle);

/* NA_RELIABILITY_UDP_WAN's FEC block size (AudioStreamConfig::udpWan). The relay must drop by AUDIO
 * ORDINAL against this, not by sequence number: the sender's sequence counter is shared with control
 * and heartbeat traffic and the parity consumes one, so a modulo of the sequence would spoil some
 * blocks with two losses and leave others untouched. */
#define WAN_FEC_BLOCK 5
#define DROP_ORDINAL  2   /* the 3rd audio packet of each block */
#define NO_DROP       (-1)
#define ARM_MS        3000  /* both UDP arms run this long, so their counts are comparable */

#define SIG_PERIOD 4
static const unsigned char SIG_UNIT[SIG_PERIOD] = {0x5A, 0xA5, 0x3C, 0xC3};
static unsigned char RXBUF[4096];

static volatile int g_rx_ok = 0;

static void on_rx_audio(const unsigned char *pcm, size_t n_bytes, void *user) {
    (void)user;
    if (pcm == NULL || n_bytes < SIG_PERIOD * 2) return;
    for (size_t i = 0; i + (SIG_PERIOD * 2) <= n_bytes; i++) {
        if (memcmp(pcm + i, SIG_UNIT, SIG_PERIOD) == 0 &&
            memcmp(pcm + i + SIG_PERIOD, SIG_UNIT, SIG_PERIOD) == 0) {
            g_rx_ok = 1;
            return;
        }
    }
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static int fail(const char *msg, na_stream_client *c, na_audio_server *s, void *proxy) {
    fprintf(stderr, "FAIL: %s\n", msg);
    if (c) na_client_destroy(c);
    if (proxy) naproxy_stop(proxy);
    if (s) { na_server_stop(s); na_server_destroy(s); }
    return 1;
}

/* The three unmeasured counters must read -1 (not measured), never 0 (nothing was lost). */
static int gap_counters_are_unmeasured(const na_client_stats *st) {
    return st->packets_lost == -1 && st->packets_out_of_order == -1 && st->packet_loss_rate < 0.0;
}

/* Run one UDP_WAN arm through the relay for a FIXED duration. `drop_ordinal` < 0 relays losslessly
 * (the control). Both arms run the same wall clock so their counts are directly comparable — an arm
 * that stopped at the first recovery would report a count that sizes the break condition rather than
 * the effect. Fills *out with the client's final counters. Returns 1 on success. */
static int run_wan_arm(na_audio_server *srv, int server_port, int drop_ordinal,
                       const char *what, int duration_ms, na_client_stats *out,
                       long long *out_dropped, long long *out_parity) {
    char err[256];
    int proxy_port = 0;
    void *proxy = naproxy_start(server_port, WAN_FEC_BLOCK, drop_ordinal, &proxy_port);
    if (proxy == NULL || proxy_port <= 0) {
        fprintf(stderr, "FAIL: naproxy_start (%s)\n", what);
        return 0;
    }

    na_stream_client *c =
        na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", proxy_port, "stats-cli");
    if (c == NULL) {
        naproxy_stop(proxy);
        fprintf(stderr, "FAIL: na_client_create (%s)\n", what);
        return 0;
    }
    /* The profile selects the transport too — pairing it with na_client_set_transport would let the
     * later call silently override it. One call is enough. */
    na_client_set_reliability_profile(c, NA_RELIABILITY_UDP_WAN);
    na_client_set_playback_device(c, 0);  /* REQUIRED for RX even on the NULL backend */
    na_client_set_audio_cb(c, on_rx_audio, NULL);
    na_client_set_auto_reconnect(c, 0);   /* a reconnect would reset the counters mid-arm */

    g_rx_ok = 0;
    if (na_client_connect(c, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "FAIL: connect through the relay (%s): %s\n", what, err);
        na_client_destroy(c);
        naproxy_stop(proxy);
        return 0;
    }

    /* Drive audio for the whole budget — no early exit, in either arm. */
    int waited = 0;
    na_client_stats st;
    memset(&st, 0, sizeof st);
    while (waited < duration_ms) {
        na_server_inject_audio(srv, RXBUF, (int)sizeof RXBUF);
        sleep_ms(20);
        waited += 20;
    }

    if (na_client_get_stats(c, &st) != NA_OK) {
        fprintf(stderr, "FAIL: na_client_get_stats final (%s)\n", what);
        na_client_destroy(c);
        naproxy_stop(proxy);
        return 0;
    }
    *out = st;
    *out_dropped = naproxy_audio_dropped(proxy);
    *out_parity = naproxy_parity_forwarded(proxy);

    printf("c_client_stats: %s — relay saw %lld audio, dropped %lld, forwarded %lld parity; "
           "client recovered %lld, unreconciled %lld, reordered %lld, received %lld pkts / "
           "%lld B, rx_signature=%d (%d ms)\n",
           what, naproxy_audio_seen(proxy), *out_dropped, *out_parity,
           st.packets_recovered_by_fec, st.fec_blocks_unreconciled, st.packets_reordered,
           st.packets_received, st.bytes_received, g_rx_ok, waited);

    na_client_disconnect(c);
    na_client_destroy(c);
    naproxy_stop(proxy);
    return 1;
}

int main(void) {
    for (size_t i = 0; i < sizeof RXBUF; i++) RXBUF[i] = SIG_UNIT[i % SIG_PERIOD];

    na_client_stats st;
    memset(&st, 0, sizeof st);

    /* ---- (1) argument contract, no server needed ---- */

    if (na_client_get_stats(NULL, &st) != NA_ERR_INVALID || na_last_error() != NA_ERR_INVALID) {
        return fail("na_client_get_stats(NULL, &st) not NA_ERR_INVALID", NULL, NULL, NULL);
    }
    {
        na_stream_client *probe =
            na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", 4533, "stats-probe");
        if (probe == NULL) return fail("na_client_create (probe)", NULL, NULL, NULL);
        if (na_client_get_stats(probe, NULL) != NA_ERR_INVALID) {
            return fail("na_client_get_stats(client, NULL) not NA_ERR_INVALID", probe, NULL, NULL);
        }
        /* Never connected: NOT an error — connected == 0 with defaults, and the unmeasured
         * counters already reading -1 rather than 0. */
        if (na_client_get_stats(probe, &st) != NA_OK) {
            return fail("na_client_get_stats on an unconnected client is not NA_OK", probe, NULL,
                        NULL);
        }
        if (st.connected != 0) {
            return fail("connected != 0 before connect", probe, NULL, NULL);
        }
        if (st.packets_received != 0 || st.bytes_received != 0 ||
            st.packets_recovered_by_fec != 0) {
            return fail("counters non-zero before connect", probe, NULL, NULL);
        }
        if (!gap_counters_are_unmeasured(&st)) {
            return fail("the gap counters do not read -1 before connect", probe, NULL, NULL);
        }
        na_client_destroy(probe);
    }

    /* ---- (2) TCP: the transport counters move, every reliability counter stays off ---- */

    {
        na_audio_server *tsrv = na_server_create(NA_SERVER_BACKEND_NULL, 0);
        if (tsrv == NULL) return fail("na_server_create (tcp)", NULL, NULL, NULL);
        char err[256];
        if (na_server_start(tsrv, err, (int)sizeof err) != NA_OK) {
            return fail("na_server_start (tcp)", NULL, tsrv, NULL);
        }
        na_stream_client *tc = na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1",
                                               na_server_port(tsrv), "stats-tcp");
        if (tc == NULL) return fail("na_client_create (tcp)", NULL, tsrv, NULL);
        na_client_set_playback_device(tc, 0);
        na_client_set_audio_cb(tc, on_rx_audio, NULL);
        na_client_set_auto_reconnect(tc, 0);
        g_rx_ok = 0;
        if (na_client_connect(tc, err, (int)sizeof err) != NA_OK) {
            return fail("na_client_connect (tcp)", tc, tsrv, NULL);
        }

        int waited = 0;
        while (waited < 1000) {
            na_server_inject_audio(tsrv, RXBUF, (int)sizeof RXBUF);
            sleep_ms(20);
            waited += 20;
        }
        if (na_client_get_stats(tc, &st) != NA_OK) {
            return fail("na_client_get_stats (tcp)", tc, tsrv, NULL);
        }
        if (st.connected != 1) return fail("connected != 1 while connected (tcp)", tc, tsrv, NULL);
        if (st.packets_received <= 0 || st.bytes_received <= 0) {
            return fail("no packets/bytes counted on a live TCP session", tc, tsrv, NULL);
        }
        /* Distinguishes the byte and packet counters from each other: every audio packet carries a
         * payload, so bytes must exceed the packet count by a wide margin. A field wired to the
         * wrong source would have to survive this. */
        if (st.bytes_received <= st.packets_received) {
            return fail("bytes_received is not larger than packets_received (tcp)", tc, tsrv, NULL);
        }
        /* TCP has none of the reliability subsystems, so each of their counters must be off. */
        if (st.packets_reordered != 0 || st.packets_recovered_by_fec != 0 ||
            st.fec_blocks_unreconciled != 0 || st.control_retransmits != 0 ||
            st.jitter_ms != 0.0 || st.buffer_target_ms != -1) {
            return fail("a reliability counter is live on TCP, which has no such subsystem",
                        tc, tsrv, NULL);
        }
        if (!gap_counters_are_unmeasured(&st)) {
            return fail("the gap counters do not read -1 on TCP", tc, tsrv, NULL);
        }
        if (!g_rx_ok) return fail("no RX signature reached the callback (tcp)", tc, tsrv, NULL);

        printf("c_client_stats: tcp — %lld pkts / %lld B, crc_errors %d, every reliability "
               "counter off, gap counters unmeasured (%d ms)\n",
               st.packets_received, st.bytes_received, st.crc_errors, waited);
        na_client_disconnect(tc);
        na_client_destroy(tc);
        na_server_stop(tsrv);
        na_server_destroy(tsrv);
    }

    /* ---- (3)+(4) UDP_WAN through the relay: lossy, then the lossless control ---- */

    na_audio_server *srv = na_server_create(NA_SERVER_BACKEND_NULL, 0);
    if (srv == NULL) return fail("na_server_create (wan)", NULL, NULL, NULL);
    if (na_server_set_reliability_profile(srv, NA_RELIABILITY_UDP_WAN) != NA_OK) {
        return fail("na_server_set_reliability_profile(UDP_WAN)", NULL, srv, NULL);
    }
    char err[256];
    if (na_server_start(srv, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "  (%s)\n", err);
        return fail("na_server_start (wan)", NULL, srv, NULL);
    }
    const int port = na_server_port(srv);

    na_client_stats lossy;
    long long lossy_dropped = 0, lossy_parity = 0;
    memset(&lossy, 0, sizeof lossy);
    if (!run_wan_arm(srv, port, DROP_ORDINAL, "lossy (1 audio packet dropped per FEC block)",
                     ARM_MS, &lossy, &lossy_dropped, &lossy_parity)) {
        return fail("the lossy UDP_WAN arm did not complete", NULL, srv, NULL);
    }

    /* The relay must actually have done its job, or the arm proves nothing about recovery. */
    if (lossy_dropped <= 0) {
        return fail("the relay dropped nothing — the lossy arm was not lossy", NULL, srv, NULL);
    }
    /* Parity forwarded > 0 separates "FEC could not repair the loss" from "there was no FEC". */
    if (lossy_parity <= 0) {
        return fail("the server sent no FEC parity — the arm cannot test recovery", NULL, srv, NULL);
    }
    /* THE HEADLINE: the counter a C consumer could not read before this change, moving under real
     * induced loss, through the real client — and moving by an amount that TRACKS the loss rather
     * than merely being non-zero. Measured 29 of 30 recovered, identical across five runs; the
     * shortfall is the final block still in flight when the arm ends. The 3/4 floor leaves room for
     * a slower machine to end mid-block without turning a real regression into a pass. */
    if (lossy.packets_recovered_by_fec < (lossy_dropped * 3) / 4) {
        fprintf(stderr, "  (recovered %lld of %lld dropped — below the 3/4 floor)\n",
                lossy.packets_recovered_by_fec, lossy_dropped);
        return fail("packets_recovered_by_fec does not track the induced loss", NULL, srv, NULL);
    }
    /* Upper bound: FEC cannot repair more than was lost. This is what catches a field wired to the
     * wrong source — packets_reordered reads 116 and packets_received 153 on this same arm, so
     * either would blow this bound while sailing past a bare "> 0". */
    if (lossy.packets_recovered_by_fec > lossy_dropped) {
        return fail("packets_recovered_by_fec exceeds the packets actually dropped", NULL, srv,
                    NULL);
    }
    if (!g_rx_ok) {
        return fail("no RX signature survived the lossy arm", NULL, srv, NULL);
    }
    /* Still unmeasured, and this is the arm that proves the -1 is honest rather than lazy: packets
     * genuinely WERE lost here, and reporting 0 would have read as "nothing was lost". */
    if (!gap_counters_are_unmeasured(&lossy)) {
        return fail("the gap counters do not read -1 on UDP_WAN", NULL, srv, NULL);
    }
    if (lossy.buffer_target_ms <= 0) {
        return fail("buffer_target_ms is not live on UDP_WAN, which enables adaptive jitter",
                    NULL, srv, NULL);
    }

    na_client_stats clean;
    long long clean_dropped = 0, clean_parity = 0;
    memset(&clean, 0, sizeof clean);
    if (!run_wan_arm(srv, port, NO_DROP, "control (same relay, nothing dropped)",
                     ARM_MS, &clean, &clean_dropped, &clean_parity)) {
        return fail("the lossless control arm did not complete", NULL, srv, NULL);
    }
    if (clean_dropped != 0) {
        return fail("the control relay dropped something", NULL, srv, NULL);
    }
    /* The counter must be silent on the healthy neighbour. Without this, "recovered > 0" above
     * could just mean "FEC is on", which every UDP_WAN client is. */
    if (clean.packets_recovered_by_fec != 0) {
        return fail("packets_recovered_by_fec moved with nothing dropped", NULL, srv, NULL);
    }
    if (clean.packets_received <= 0) {
        return fail("the control arm carried no audio at all", NULL, srv, NULL);
    }

    na_server_stop(srv);
    na_server_destroy(srv);
    printf("c_client_stats: PASS — %lld recovered under induced loss, 0 with none\n",
           lossy.packets_recovered_by_fec);
    return 0;
}
