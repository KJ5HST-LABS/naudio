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
 * THE HEADLINE ARMS INJECT FAULTS ON PURPOSE. A counter that has never been observed to increment is
 * an unverified claim, not a feature, and loopback is both loss-free and corruption-free: with no
 * fault, the recovering and the non-recovering configuration deliver byte-identical output and the
 * counters can only ever read 0. So the arms relay audio through client_stats_proxy.cpp, which
 * injects one fault per FEC block, and each fault arm is paired with the SAME relay injecting
 * nothing. A detector is not finished when it fires on the fault; it is finished when it also stays
 * quiet on the healthy neighbour.
 *
 *   (1) argument contract      (2) TCP: transport counters live, reliability counters off
 *   (3) UDP_WAN, one audio packet DROPPED per block  -> packets_recovered_by_fec moves
 *   (4) UDP_WAN, the same relay dropping nothing     -> it stays 0
 *   (5) UDP_WAN, one audio packet CORRUPTED per block -> crc_errors moves
 *   (6) UDP_WAN, a deliberately stalled audio callback -> queue_drops does NOT move
 *
 * SINCE 0.2.0, arm (1) also covers the struct_size guard in BOTH directions — a caller declaring
 * MORE than the library writes (the tail is zero-filled, nothing past it touched) and a caller
 * declaring the V1 size against this longer library (sequence_gaps is not written at all). The
 * second is the case appending a field created, and only a shorter-than-ours declaration reaches
 * the guard, so neither block substitutes for the other. Arms (2)-(6) carry sequence_gaps through
 * the same fault matrix: -1 on TCP, 0 on the lossless control, bounded on both sides by the
 * relay's own drop count under loss, and — deliberately — 0 on the stalled consumer, because a
 * tail-dropping socket buffer leaves no hole for a gap counter to find.
 *
 * DROPPING AND CORRUPTING ARE DIFFERENT FAULTS, and the distinction is the whole reason crc_errors
 * sat at 0 through fourteen sessions of loss testing: a dropped datagram never arrives, so nothing
 * fails a checksum. Arm (5) is the first arm on this project to make a datagram ARRIVE broken.
 *
 * ARM (6) ASSERTS A NEGATIVE, and does so deliberately. queue_drops and control_retransmits are
 * live code with real increments that no client-side path can reach — see the na_client_stats
 * contract in include/naudio.h, which documents why. Arm (6) runs the provocation that looks like it
 * should work and shows that it does not, so the claim in the header is executable rather than
 * merely asserted. If either counter ever does move here, this test fails and that header contract
 * is what needs updating.
 *
 * Hardware-free: NULL backends on both ends, loopback UDP, no PortAudio, no radio.
 */
#include "c_atomic_compat.h"
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
void* naproxy_start(int server_port, int block_size, int drop_ordinal, int corrupt_ordinal,
                    int* out_port);
long long naproxy_audio_seen(void* handle);
long long naproxy_audio_dropped(void* handle);
long long naproxy_audio_corrupted(void* handle);
long long naproxy_parity_forwarded(void* handle);
void naproxy_stop(void* handle);

/* NA_RELIABILITY_UDP_WAN's FEC block size (AudioStreamConfig::udpWan). The relay must drop by AUDIO
 * ORDINAL against this, not by sequence number: the sender's sequence counter is shared with control
 * and heartbeat traffic and the parity consumes one, so a modulo of the sequence would spoil some
 * blocks with two losses and leave others untouched. */
#define WAN_FEC_BLOCK 5
#define DROP_ORDINAL  2   /* the 3rd audio packet of each block */
#define NO_DROP       (-1)
#define CORRUPT_ORDINAL 2 /* the 3rd audio packet of each block, in the corrupting arm */
#define NO_CORRUPT    (-1)

/* EVERY ARM IS BOUNDED BY INJECTED PACKETS, NEVER BY WALL CLOCK. A duration budget silently encodes
 * the calibrating machine's speed as a hidden constant in whatever the arm asserts; a packet count is
 * the same number on every machine. This arm's predecessor ran "3000 ms at one packet per 20 ms" and
 * asserted a 3/4 recovery ratio against it — which held at 29/30 here and read 5/30 and then 0/30 on
 * the macOS CI runner. See the LOSSY_* note below for why, and issue #43. */
#define ARM_PACKETS   150   /* the fault arms and their lossless control: 30 FEC blocks of 5 */
#define STALL_ARM_PACKETS 75 /* the queue_drops arm: shorter, because it asserts a negative */
/* One corrupted packet per 5 leaves 4 good ones between faults, and each good packet resets
 * UdpClientConnection's consecutive-error run — so the 20-error teardown threshold
 * (MAX_CONSECUTIVE_CRC_ERRORS) is never approached and the arm measures counting, not teardown. */
#define STALL_CB_MS   100   /* how long the audio callback blocks in that arm */

/* The injection cadence is the PROFILE'S OWN frame period (AudioStreamConfig::UDP_FRAME_MS = 10),
 * not a number chosen here. It used to be 20 ms, and that single arbitrary constant is most of #43:
 * FEC recovery needs a block's surviving audio to still be held by the decoder when the block's
 * parity arrives, and the decoder used to discard a pending block once it was 120 ms old MEASURED
 * FROM THE BLOCK'S CREATION — so a wide cadence killed blocks whose own packets were still arriving.
 * The erase was SILENT (in this pipeline the reorder buffer never forwards gap markers, so the
 * discarded block emitted nothing and moved no counter), which is why #43 presented as an
 * unexplained zero.
 *
 * That is now history: #47 made the bound IDLE (measured from the last insert) and derived it from
 * the negotiated stream shape, and FecDecoder::pendingPacketsDiscarded counts the discard. The
 * cadence stays at the profile's 10 ms because a test should drive the subsystem at the rate the
 * profile declares, not because the margin is thin. */
#define INJECT_PERIOD_MS 10

/* The lossy arm's bound: keep injecting until this many packets have actually been REPAIRED, capped
 * by a packet budget. That is a completion count, so it measures recovery; the old ratio measured how
 * much recovery happened to fit inside a fixed 3 s, which is a property of the machine.
 *
 * Both numbers are derived from a measured degradation curve, not picked. Widening this file's own
 * injection cadence is the only knob that reproduces the CI signature on this laptop, and at 48 ms it
 * reproduces it exactly — `recovered 0, unreconciled 0, cb_calls 120, received 156`, the macOS
 * runner's line to the field. Sweeping the OLD arm gives: <=40 ms -> 30/30 recovered, 42 -> 25,
 * 44 -> 18, 46 -> 8, 48 -> 7, 50 -> 0. CI's two readings, 5/30 and 0/30, sit at the 48-50 ms end.
 *
 * DO NOT read that curve as a constant yield to multiply by the budget: it is not linear, and past
 * ~46 ms it collapses rather than thins (120 blocks at 48 ms returned 9 repairs, not the ~27 a
 * constant 23% would predict). What the budget buys is measured directly instead — the arm's cliff,
 * swept end to end:
 *
 *     old arm (150 packets, 3/4 ratio):          passes to 42 ms, fails from 44 ms
 *     this arm, before #47 (600 pkts, 20 target): passes to 46 ms, fails from 48 ms
 *     this arm, after  #47:                       passes to 246 ms, fails at 250 ms
 *
 * Re-swept after #47 widened the decoder's pending bound (10/48/100/150/200/230/240/245/246 ms all
 * reach the 20-repair target; 250 ms returns 11). 248 ms is UNMEASURED — the sweep was cut short —
 * so the cliff is somewhere in (246, 250].
 *
 * Against the profile's nominal 10 ms cadence that is the machine-degradation factor the arm
 * tolerates: ~4.6x before #47, ~24x after. The runner that filed #43 was measured at ~2.4x (it
 * behaved like a uniform 48 ms at a nominal 20 ms).
 *
 * The cliff is NOT simply "2 x cadence exceeds the 490 ms bound". That arithmetic predicts a cliff
 * just past 245 ms and 246 ms still passes, so the edge is set by the packet budget and by blocks
 * lost to interleaved control traffic (`unreconciled` climbs from 0 at 48 ms to 10 by 245 ms) as
 * well as by the bound. Stated as a measurement, not as a derivation.
 *
 * On a healthy machine the target is met after ~21 blocks and the arm exits in ~1 s, FASTER than the
 * fixed 3 s it replaces. The budget is only spent when the machine is genuinely struggling. */
#define LOSSY_TARGET_RECOVERED 20
#define LOSSY_MAX_PACKETS      600
#define RECOVERY_POLL_EVERY    5    /* poll the counter once per FEC block, not per packet */

#define SIG_PERIOD 4
static const unsigned char SIG_UNIT[SIG_PERIOD] = {0x5A, 0xA5, 0x3C, 0xC3};
static unsigned char RXBUF[4096];

static NA_TEST_ATOMIC int g_rx_ok = 0;
/* Non-zero makes the audio callback block for that many ms — the stalled-consumer arm. */
static NA_TEST_ATOMIC int g_stall_cb_ms = 0;
static NA_TEST_ATOMIC long g_cb_calls = 0;

static void sleep_ms(int ms);

static void on_rx_audio(const unsigned char *pcm, size_t n_bytes, void *user) {
    (void)user;
    g_cb_calls++;
    if (g_stall_cb_ms > 0) sleep_ms(g_stall_cb_ms);
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

/* Run one UDP_WAN arm through the relay for a FIXED NUMBER OF INJECTED PACKETS. `drop_ordinal` < 0
 * relays losslessly (the control).
 *
 * `stop_after_recovered` > 0 additionally stops the arm early once the client reports that many FEC
 * repairs. That early exit is deliberate and is NOT the mistake of stopping at the first sign of what
 * you are looking for: the arm's assertion is "this many repairs happened within `max_packets`", so
 * the budget — not the break — is what the result is measured against, and an arm that never reaches
 * the target burns the whole budget and fails with real numbers. The arms that assert a NEGATIVE
 * (nothing recovered, nothing corrupted, nothing dropped) pass 0 and always run their full budget,
 * because a negative has nothing to complete.
 *
 * Fills *out with the client's final counters. Returns 1 on success. */
static int run_wan_arm(na_audio_server *srv, int server_port, int drop_ordinal, int corrupt_ordinal,
                       const char *what, int max_packets, int stop_after_recovered,
                       na_client_stats *out, long long *out_dropped, long long *out_parity,
                       long long *out_corrupted) {
    char err[256];
    int proxy_port = 0;
    void *proxy =
        naproxy_start(server_port, WAN_FEC_BLOCK, drop_ordinal, corrupt_ordinal, &proxy_port);
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
    g_cb_calls = 0;
    if (na_client_connect(c, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "FAIL: connect through the relay (%s): %s\n", what, err);
        na_client_destroy(c);
        naproxy_stop(proxy);
        return 0;
    }

    /* Drive audio at the profile's own frame cadence, bounded by a packet count. */
    int injected = 0;
    na_client_stats st;
    memset(&st, 0, sizeof st);
    while (injected < max_packets) {
        na_server_inject_audio(srv, RXBUF, (int)sizeof RXBUF);
        ++injected;
        sleep_ms(INJECT_PERIOD_MS);
        if (stop_after_recovered > 0 && (injected % RECOVERY_POLL_EVERY) == 0) {
            na_client_stats probe;
            memset(&probe, 0, sizeof probe);
            if (na_client_get_stats(c, &probe, sizeof probe) == NA_OK &&
                probe.packets_recovered_by_fec >= stop_after_recovered) {
                break;
            }
        }
    }

    if (na_client_get_stats(c, &st, sizeof st) != NA_OK) {
        fprintf(stderr, "FAIL: na_client_get_stats final (%s)\n", what);
        na_client_destroy(c);
        naproxy_stop(proxy);
        return 0;
    }
    *out = st;
    *out_dropped = naproxy_audio_dropped(proxy);
    *out_parity = naproxy_parity_forwarded(proxy);
    *out_corrupted = naproxy_audio_corrupted(proxy);

    /* Echo the fault parameters this arm actually ran with, next to the results. An arm that prints
     * only its results cannot be told apart from a differently-parameterised one that silently fell
     * back to a default. */
    printf("c_client_stats: %s — drop_ordinal=%d corrupt_ordinal=%d stall_cb_ms=%d; relay saw %lld "
           "audio, dropped %lld, corrupted %lld, forwarded %lld parity; client recovered %lld, "
           "unreconciled %lld, reordered %lld, crc_errors %d, control_retransmits %lld, "
           "queue_drops %lld, sequence_gaps %lld, received %lld pkts / %lld B, cb_calls %ld, "
           "rx_signature=%d (%d injected of %d budget)\n",
           what, drop_ordinal, corrupt_ordinal, g_stall_cb_ms, naproxy_audio_seen(proxy),
           *out_dropped, *out_corrupted, *out_parity, st.packets_recovered_by_fec,
           st.fec_blocks_unreconciled, st.packets_reordered, st.crc_errors, st.control_retransmits,
           st.queue_drops, st.sequence_gaps, st.packets_received, st.bytes_received, g_cb_calls,
           g_rx_ok, injected, max_packets);

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

    if (na_client_get_stats(NULL, &st, sizeof st) != NA_ERR_INVALID || na_last_error() != NA_ERR_INVALID) {
        return fail("na_client_get_stats(NULL, &st, sizeof st) not NA_ERR_INVALID", NULL, NULL, NULL);
    }
    {
        na_stream_client *probe =
            na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", 4533, "stats-probe");
        if (probe == NULL) return fail("na_client_create (probe)", NULL, NULL, NULL);
        if (na_client_get_stats(probe, NULL, sizeof st) != NA_ERR_INVALID) {
            return fail("na_client_get_stats(client, NULL) not NA_ERR_INVALID", probe, NULL, NULL);
        }
        /* Never connected: NOT an error — connected == 0 with defaults, and the unmeasured
         * counters already reading -1 rather than 0. */
        if (na_client_get_stats(probe, &st, sizeof st) != NA_OK) {
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

        /* ---- struct_size is the write bound, not a formality ----
         * Below the v1 floor is refused outright — including the 0 a caller who forgot the
         * argument would pass through an implicit conversion. */
        if (na_client_get_stats(probe, &st, 0) != NA_ERR_INVALID ||
            na_last_error() != NA_ERR_INVALID) {
            return fail("struct_size 0 not NA_ERR_INVALID", probe, NULL, NULL);
        }
        if (na_client_get_stats(probe, &st, NA_CLIENT_STATS_SIZE_V1 - 1) != NA_ERR_INVALID) {
            return fail("struct_size below the v1 floor not NA_ERR_INVALID", probe, NULL, NULL);
        }
        /* A caller compiled against a LONGER future header. Two things must hold, and they are
         * the whole point of the parameter: the bytes it DECLARED beyond our struct are zeroed
         * (defined, not indeterminate), and not one byte past what it declared is touched.
         * The second is the out-of-bounds write this migration exists to prevent, observed
         * directly rather than inferred. */
        {
            struct { na_client_stats base; unsigned char tail[64]; } future;
            unsigned char *raw = (unsigned char *)&future;
            const size_t declared = sizeof(na_client_stats) + 32;  /* < sizeof future: 32 spare */
            size_t i;
            memset(&future, 0xA5, sizeof future);
            if (na_client_get_stats(probe, &future.base, declared) != NA_OK) {
                return fail("an over-sized (future-header) struct_size was rejected", probe, NULL,
                            NULL);
            }
            for (i = sizeof(na_client_stats); i < declared; i++) {
                if (raw[i] != 0x00) {
                    return fail("the declared tail beyond our struct was not zero-filled", probe,
                                NULL, NULL);
                }
            }
            for (i = declared; i < sizeof future; i++) {
                if (raw[i] != 0xA5) {
                    return fail("na_client_get_stats wrote PAST the caller's declared struct_size",
                                probe, NULL, NULL);
                }
            }
        }
        /* ---- the mirror, and the case 0.2.0 actually created ----
         * A caller compiled against v1 calling this 0.2.0 library. It is entitled to: the floor
         * check accepts NA_CLIENT_STATS_SIZE_V1 and always will, because a v1 consumer is exactly
         * who the struct_size scheme exists to keep working. sequence_gaps lives BEYOND that floor,
         * so writing it unconditionally — the obvious way to add a field — scribbles past the end
         * of that consumer's struct. That is the #26 hazard reintroduced by the first field to use
         * the #26 mechanism, and it is observed here directly rather than reasoned about: every
         * byte from the v1 floor onward must come back untouched.
         *
         * The over-sized block above cannot catch this. It declares MORE than we write, so it
         * exercises the zero-fill; this one declares LESS, which is the only way to reach the
         * guard. Two directions, two blocks — a single block would leave one of them untested. */
        {
            struct { na_client_stats base; unsigned char tail[32]; } v1c;
            unsigned char *raw = (unsigned char *)&v1c;
            size_t i;
            memset(&v1c, 0xA5, sizeof v1c);
            if (na_client_get_stats(probe, &v1c.base, NA_CLIENT_STATS_SIZE_V1) != NA_OK) {
                return fail("a v1-sized struct_size was rejected — v1 consumers must keep working",
                            probe, NULL, NULL);
            }
            for (i = NA_CLIENT_STATS_SIZE_V1; i < sizeof v1c; i++) {
                if (raw[i] != 0xA5) {
                    return fail("na_client_get_stats wrote past a V1 caller's struct — the post-v1 "
                                "field is not guarded on the caller's declared size",
                                probe, NULL, NULL);
                }
            }
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
        if (na_client_get_stats(tc, &st, sizeof st) != NA_OK) {
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
        /* The one place BOTH sides of the complement are unmeasured, and the reason sequence_gaps
         * defaults to -1 rather than to 0 like its Transport neighbours: TCP builds no reorder
         * buffer, so there is nothing counting, and a 0 would read as "nothing was lost" from a
         * field that measured nothing at all. */
        if (st.sequence_gaps != -1) {
            fprintf(stderr, "  (sequence_gaps %lld on TCP, which builds no reorder buffer)\n",
                    st.sequence_gaps);
            return fail("sequence_gaps does not read -1 on TCP", tc, tsrv, NULL);
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
    long long lossy_dropped = 0, lossy_parity = 0, lossy_corrupted = 0;
    memset(&lossy, 0, sizeof lossy);
    if (!run_wan_arm(srv, port, DROP_ORDINAL, NO_CORRUPT,
                     "lossy (1 audio packet dropped per FEC block)", LOSSY_MAX_PACKETS,
                     LOSSY_TARGET_RECOVERED, &lossy, &lossy_dropped,
                     &lossy_parity, &lossy_corrupted)) {
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
     * than merely being non-zero.
     *
     * This is a COMPLETION COUNT, not a ratio of what fit inside a fixed window. Its predecessor
     * asserted `recovered >= 3/4 of dropped` over a 3000 ms budget, with a comment recording "29 of
     * 30 recovered, identical across five runs". Every word of that was true and it still failed on
     * the macOS CI runner at 5/30 and then, unchanged, at 0/30 — because five runs on one machine
     * calibrate a number, they do not bound one. A ratio taken over a fixed duration is a measurement
     * of the machine; "did N repairs complete, given a budget of injected packets" is not. See #43.
     *
     * The floor is NOT simply relaxed to fit the observation: a threshold anywhere between 0 and 22
     * would have been passable and meaningless on the readings above. Instead the arm is given enough
     * blocks to reach a fixed target, so a client that repairs nothing still fails. */
    if (lossy.packets_recovered_by_fec < LOSSY_TARGET_RECOVERED) {
        fprintf(stderr, "  (recovered %lld of %lld dropped in %d injected packets — short of the "
                        "%d-repair target; FEC is not recovering, or is recovering far too little "
                        "to be the layer this profile promises)\n",
                lossy.packets_recovered_by_fec, lossy_dropped, LOSSY_MAX_PACKETS,
                LOSSY_TARGET_RECOVERED);
        return fail("packets_recovered_by_fec does not track the induced loss", NULL, srv, NULL);
    }
    /* Upper bound: FEC cannot repair more than was lost. This is what catches a field wired to the
     * wrong source — packets_reordered reads 81 and packets_received 108 on this same arm, so
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
    /* sequence_gaps (@since 0.2.0) is the COMPLEMENT of those three: measured precisely where they
     * are not. -1 here would mean no reorder buffer, which UDP_WAN always has. */
    if (lossy.sequence_gaps < 0) {
        return fail("sequence_gaps reads unmeasured on UDP_WAN, which always engages a reorder "
                    "buffer — it and the three gap counters must never both be -1",
                    NULL, srv, NULL);
    }
    /* Two-sided, both bounds anchored to a quantity known INDEPENDENTLY of the field (Learning 31):
     *   lower — every FEC recovery implies the slot was gapped first, since the pipeline is
     *           reorder -> FEC and the decoder only ever sees what the buffer already gave up on;
     *   upper — the relay's own drop count, which the library never sees.
     * Measured 21 gaps against 21 dropped and 21 recovered, so the window is tight — and note it is
     * tight WITHOUT being calibrated: all three move together with the budget, because each is
     * anchored to the relay rather than to a wall clock. A field cross-wired to a sibling blows one
     * of these: packets_reordered reads 81 and packets_received 108 on this same arm. */
    if (lossy.sequence_gaps < lossy.packets_recovered_by_fec) {
        fprintf(stderr, "  (sequence_gaps %lld < recovered %lld — a slot was repaired that was "
                        "never gapped)\n", lossy.sequence_gaps, lossy.packets_recovered_by_fec);
        return fail("sequence_gaps is below the packets FEC recovered from those same gaps",
                    NULL, srv, NULL);
    }
    if (lossy.sequence_gaps > lossy_dropped) {
        fprintf(stderr, "  (sequence_gaps %lld > %lld actually dropped)\n",
                lossy.sequence_gaps, lossy_dropped);
        return fail("sequence_gaps exceeds the packets the relay actually dropped", NULL, srv,
                    NULL);
    }
    if (lossy.buffer_target_ms <= 0) {
        return fail("buffer_target_ms is not live on UDP_WAN, which enables adaptive jitter",
                    NULL, srv, NULL);
    }

    /* A dropped packet must NOT be counted as a CRC error: it never arrived, so there was nothing to
     * fail a checksum. This separates the two faults the relay can inject — without it, an arm that
     * corrupts could be satisfied by a counter that actually tracks loss. */
    if (lossy.crc_errors != 0) {
        fprintf(stderr, "  (crc_errors %d on an arm that only DROPPED %lld packets)\n",
                lossy.crc_errors, lossy_dropped);
        return fail("crc_errors counted dropped packets, which never arrived", NULL, srv, NULL);
    }

    na_client_stats clean;
    long long clean_dropped = 0, clean_parity = 0, clean_corrupted = 0;
    memset(&clean, 0, sizeof clean);
    if (!run_wan_arm(srv, port, NO_DROP, NO_CORRUPT, "control (same relay, nothing dropped)",
                     ARM_PACKETS, 0, &clean, &clean_dropped, &clean_parity, &clean_corrupted)) {
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
    /* crc_errors silent on the healthy neighbour, so arm (5)'s non-zero cannot just mean "UDP". */
    if (clean.crc_errors != 0) {
        return fail("crc_errors moved with nothing corrupted", NULL, srv, NULL);
    }
    /* Silent here too — and note this is 0, not -1: the reorder buffer IS engaged and IS counting,
     * it simply had nothing to count. That distinction is the entire point of the two conventions,
     * and this is the arm where the difference between them is observable. */
    if (clean.sequence_gaps != 0) {
        fprintf(stderr, "  (sequence_gaps %lld with nothing dropped)\n", clean.sequence_gaps);
        return fail("sequence_gaps moved on a lossless relay", NULL, srv, NULL);
    }

    /* ---- (5) crc_errors: the SAME relay corrupting instead of dropping ----
     *
     * A corrupted datagram ARRIVES and fails its CRC; a dropped one never arrives. Only the first can
     * move crc_errors, at any loss rate — which is why fourteen sessions of loss testing left this
     * counter at 0. The relay flips one payload byte and leaves the header intact, so the datagram
     * clears the expected-sender and truncation gates that return early WITHOUT counting
     * (UdpClientConnection.cpp:257) and reaches the CRC check that does. */

    na_client_stats corrupt;
    long long corrupt_dropped = 0, corrupt_parity = 0, corrupt_corrupted = 0;
    memset(&corrupt, 0, sizeof corrupt);
    if (!run_wan_arm(srv, port, NO_DROP, CORRUPT_ORDINAL,
                     "corrupting (1 audio packet per FEC block arrives with a bad CRC)", ARM_PACKETS,
                     0, &corrupt, &corrupt_dropped, &corrupt_parity, &corrupt_corrupted)) {
        return fail("the corrupting UDP_WAN arm did not complete", NULL, srv, NULL);
    }
    if (corrupt_corrupted <= 0) {
        return fail("the relay corrupted nothing — the arm was not a CRC arm", NULL, srv, NULL);
    }
    if (corrupt_dropped != 0) {
        return fail("the corrupting relay also dropped something", NULL, srv, NULL);
    }
    /* THE HEADLINE: crc_errors moving under real induced corruption, through the real client, by an
     * amount that TRACKS the corruption rather than merely being non-zero. The shortfall the 3/4
     * floor allows for is the last datagram or two still in flight when the arm ends. */
    if (corrupt.crc_errors < (int)((corrupt_corrupted * 3) / 4)) {
        fprintf(stderr, "  (crc_errors %d of %lld corrupted — below the 3/4 floor)\n",
                corrupt.crc_errors, corrupt_corrupted);
        return fail("crc_errors does not track the induced corruption", NULL, srv, NULL);
    }
    /* Upper bound anchored to a quantity known INDEPENDENTLY of the library: the relay is the only
     * source of undeserializable datagrams on this path, so the client cannot legitimately count
     * more than it corrupted. This is what catches a field wired to a sibling — packets_received and
     * packets_reordered both run far above corrupt_corrupted on this same arm and would sail past a
     * bare "> 0" while blowing this bound. */
    if ((long long)corrupt.crc_errors > corrupt_corrupted) {
        fprintf(stderr, "  (crc_errors %d exceeds the %lld datagrams the relay corrupted)\n",
                corrupt.crc_errors, corrupt_corrupted);
        return fail("crc_errors exceeds the datagrams actually corrupted", NULL, srv, NULL);
    }
    if (!g_rx_ok) {
        return fail("no RX signature survived the corrupting arm", NULL, srv, NULL);
    }

    /* ---- (6) queue_drops: #25's proposed provocation, RUN, and it does not move ----
     *
     * #25 proposed "a listener whose na_audio_cb sleeps longer than the reorder window" for
     * queue_drops. That cannot work, and this arm is the executable refutation rather than an
     * argument. The queue's cap is BlockingPacketQueue::kDefaultMaxSize = 2048 packets (~20 s of
     * audio), not the 30 ms reorder window; and on a client-owned connection — the only kind a
     * na_client_* consumer ever gets (UdpClientTransport.hpp:50) — receiveFromSocket DRAINS the
     * queue before it reads the socket, so the producer and the consumer are the same thread. A
     * blocking callback stalls the producer with the consumer and the depth never exceeds one
     * reorder burst (~8).
     *
     * So this asserts a NEGATIVE, deliberately: queue_drops stays 0 while the consumer is stalled
     * hard enough to overflow the socket buffer. If a future change makes it move, this arm fails —
     * and that failure is the signal to update the na_client_stats contract in include/naudio.h,
     * which documents the field as unreachable on a client connection. */

    na_client_stats stalled;
    long long stalled_dropped = 0, stalled_parity = 0, stalled_corrupted = 0;
    memset(&stalled, 0, sizeof stalled);
    g_stall_cb_ms = STALL_CB_MS;
    const int stall_ok =
        run_wan_arm(srv, port, NO_DROP, NO_CORRUPT, "stalled consumer (audio cb blocks 100 ms)",
                    STALL_ARM_PACKETS, 0, &stalled, &stalled_dropped, &stalled_parity,
                    &stalled_corrupted);
    g_stall_cb_ms = 0;
    if (!stall_ok) {
        return fail("the stalled-consumer arm did not complete", NULL, srv, NULL);
    }
    /* The stall has to have actually happened, or the negative below proves nothing. */
    if (g_cb_calls <= 0) {
        return fail("the audio callback never ran, so the consumer was never stalled", NULL, srv,
                    NULL);
    }
    if (stalled.queue_drops != 0) {
        fprintf(stderr, "  (queue_drops %lld — reachable after all; update the na_client_stats "
                        "contract in include/naudio.h)\n", stalled.queue_drops);
        return fail("queue_drops moved on a client-owned connection", NULL, srv, NULL);
    }
    /* sequence_gaps ALSO stays 0 here, and this is the second negative this arm asserts — added
     * with the field in 0.2.0 because the obvious expectation is the opposite one.
     *
     * A gap counter looks like it should report slow-consumer loss: the consumer falls behind, the
     * socket buffer overflows, datagrams die, and surely a hole appears. It does not. The buffer
     * TAIL-drops — it discards the newest arrivals, not the oldest — so a stalled consumer reads an
     * unbroken PREFIX of the stream and simply stops early. Nothing it read has a hole in it, so
     * there is nothing for any gap-based counter to count.
     *
     * Measured while writing this arm, by running it out to 8 s: the relay forwarded 400 audio
     * packets, the client read 127, and sequence_gaps and packets_reordered were both 0. The loss
     * was real and total silence from every counter was the correct reading.
     *
     * This is why the field's header contract says local loss remains unreported by this struct,
     * rather than claiming sequence_gaps closes that gap — issue #29's premise was that a
     * post-reorder gap counter would report it, and this arm is the executable refutation. If it
     * ever DOES move here, the tail-drop assumption has changed and that contract needs revisiting. */
    if (stalled.sequence_gaps != 0) {
        fprintf(stderr, "  (sequence_gaps %lld on a stalled consumer — the socket buffer is no "
                        "longer tail-dropping; revisit the contract in include/naudio.h)\n",
                stalled.sequence_gaps);
        return fail("sequence_gaps moved on a stalled consumer, which tail-drop makes invisible",
                    NULL, srv, NULL);
    }
    /* Same shape for control_retransmits, and for the same reason it is documented as unreachable:
     * ControlReliability::isCriticalType does not list ConnectRequest, HeartbeatAck or LatencyProbe,
     * so the only tracked control message a client ever sends is Disconnect — and by the time it is
     * sent, closed_ is set and the heartbeat loop that pumps the retransmit sweep is already down,
     * which AudioStreamClient.cpp:742-744 states in its own comment. Nothing is ever pending when a
     * sweep runs. */
    if (stalled.control_retransmits != 0 || corrupt.control_retransmits != 0 ||
        lossy.control_retransmits != 0) {
        fprintf(stderr, "  (control_retransmits %lld/%lld/%lld — reachable after all; update the "
                        "na_client_stats contract in include/naudio.h)\n",
                lossy.control_retransmits, corrupt.control_retransmits,
                stalled.control_retransmits);
        return fail("control_retransmits moved on a client connection", NULL, srv, NULL);
    }

    na_server_stop(srv);
    na_server_destroy(srv);
    printf("c_client_stats: PASS — %lld recovered under induced loss (0 with none), %d crc_errors "
           "under induced corruption of %lld datagrams (0 with none); queue_drops and "
           "control_retransmits confirmed unreachable on a client connection\n",
           lossy.packets_recovered_by_fec, corrupt.crc_errors, corrupt_corrupted);
    return 0;
}
