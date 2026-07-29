/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — the client-side reliability profile (na_client_set_reliability_profile).
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * WHAT THIS COVERS. na_client_set_transport writes the transport and NOTHING else, so a C-ABI
 * client configured with it alone runs UDP with FEC, reordering, adaptive jitter and control-ARQ
 * all off — it receives every parity packet the server sends and discards it. The profile setter
 * is the only way to turn that layer on, and this file asserts it does.
 *
 * WHAT IT DOES NOT COVER — read before adding a claim here. Every path below is loss-free
 * loopback, so it CANNOT distinguish FEC-on from FEC-off: with nothing dropped, the recovering and
 * the non-recovering configuration deliver byte-identical output. That distinction is measurable
 * only by inducing loss, which needs a proxy this suite has no place for. What the loopback CAN
 * prove, and does, is that the profile writes the transport (a client configured with the profile
 * ALONE reaches a UDP-only server, which a client left on the TCP default cannot) and that a client
 * running the whole reliability pipeline still delivers real audio end to end.
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

/* RX is re-framed into bytesPerFrame() chunks on the way out, so what the callback sees is
 * frame-aligned rather than inject-aligned. Assert on a repeating signature — two consecutive
 * periods — which survives any framing and which silence cannot produce. A byte count would not:
 * zeros are well-formed audio arriving at exactly the right rate. */
#define SIG_PERIOD 4
static const unsigned char SIG_UNIT[SIG_PERIOD] = {0x5A, 0xA5, 0x3C, 0xC3};
static unsigned char RXBUF[8192];

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

static int fail(const char *msg, na_stream_client *c, na_audio_server *s) {
    fprintf(stderr, "FAIL: %s\n", msg);
    if (c) na_client_destroy(c);
    if (s) { na_server_stop(s); na_server_destroy(s); }
    return 1;
}

/* A throwaway client that must NOT be able to reach `port` — used to prove that the transport a
 * profile selected was genuinely overwritten by whatever ran after it. Returns 1 on success (the
 * connect was refused), 0 if it connected anyway. */
static int assert_cannot_reach_udp_server(int port, const char *what,
                                          na_transport later_transport, int use_later_transport,
                                          na_reliability_profile later_profile) {
    char err[256];
    na_stream_client *c = na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", port, what);
    if (c == NULL) {
        fprintf(stderr, "FAIL: na_client_create (%s)\n", what);
        return 0;
    }
    na_client_set_playback_device(c, 0);
    na_client_set_auto_reconnect(c, 0);  /* a retry storm would only slow the refusal down */
    if (na_client_set_reliability_profile(c, NA_RELIABILITY_UDP_WAN) != NA_OK) {
        fprintf(stderr, "FAIL: profile rejected on the %s probe\n", what);
        na_client_destroy(c);
        return 0;
    }
    if (use_later_transport) {
        if (na_client_set_transport(c, later_transport) != NA_OK) {
            fprintf(stderr, "FAIL: na_client_set_transport rejected on the %s probe\n", what);
            na_client_destroy(c);
            return 0;
        }
    } else {
        if (na_client_set_reliability_profile(c, later_profile) != NA_OK) {
            fprintf(stderr, "FAIL: second profile rejected on the %s probe\n", what);
            na_client_destroy(c);
            return 0;
        }
    }
    const na_error_t rc = na_client_connect(c, err, (int)sizeof err);
    na_client_destroy(c);
    if (rc == NA_OK) {
        fprintf(stderr, "FAIL: %s still reached the UDP-only server — the later write did not "
                        "take effect\n", what);
        return 0;
    }
    printf("c_client_profile: %s correctly refused (%s)\n", what, err);
    return 1;
}

int main(void) {
    for (size_t i = 0; i < sizeof RXBUF; i++) RXBUF[i] = SIG_UNIT[i % SIG_PERIOD];

    /* ---- (1) argument contract, no server needed ---- */

    if (na_client_set_reliability_profile(NULL, NA_RELIABILITY_UDP_WAN) != NA_ERR_INVALID ||
        na_last_error() != NA_ERR_INVALID) {
        return fail("na_client_set_reliability_profile(NULL) not NA_ERR_INVALID", NULL, NULL);
    }
    {
        na_stream_client *probe =
            na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", 4533, "profile-probe");
        if (probe == NULL) return fail("na_client_create (probe)", NULL, NULL);
        if (na_client_set_reliability_profile(probe, NA_RELIABILITY_DEFAULT) != NA_OK ||
            na_client_set_reliability_profile(probe, NA_RELIABILITY_UDP_LAN) != NA_OK ||
            na_client_set_reliability_profile(probe, NA_RELIABILITY_UDP_WAN) != NA_OK ||
            na_client_set_reliability_profile(probe, NA_RELIABILITY_UDP_FT8) != NA_OK) {
            return fail("a documented profile was rejected", probe, NULL);
        }
        if (na_client_set_reliability_profile(probe, (na_reliability_profile)99) != NA_ERR_INVALID) {
            return fail("unknown profile not rejected", probe, NULL);
        }
        na_client_destroy(probe);  /* never connected — clean create/destroy */
    }

    /* ---- (2) a UDP_WAN server, and a client configured with the PROFILE ALONE ---- */

    na_audio_server *srv = na_server_create(NA_SERVER_BACKEND_NULL, 0);
    if (srv == NULL) return fail("na_server_create", NULL, NULL);
    /* The bridge's default profile: UDP, FEC on, reorder + adaptive jitter + control-ARQ. */
    if (na_server_set_reliability_profile(srv, NA_RELIABILITY_UDP_WAN) != NA_OK) {
        return fail("na_server_set_reliability_profile", NULL, srv);
    }
    char err[256];
    if (na_server_start(srv, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "  (%s)\n", err);
        return fail("na_server_start", NULL, srv);
    }
    const int port = na_server_port(srv);

    na_stream_client *c = na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", port, "profile-cli");
    if (c == NULL) return fail("na_client_create", NULL, srv);

    /* NOTE: na_client_set_transport is deliberately NOT called. The server is UDP-only, so a client
     * left on the default TCP transport is refused at connect — reaching the server at all is the
     * assertion that the profile wrote the transport, not just the reliability knobs. */
    if (na_client_set_reliability_profile(c, NA_RELIABILITY_UDP_WAN) != NA_OK) {
        return fail("na_client_set_reliability_profile(UDP_WAN)", c, srv);
    }
    na_client_set_playback_device(c, 0);  /* REQUIRED for RX even on the NULL backend */
    na_client_set_audio_cb(c, on_rx_audio, NULL);
    na_client_set_auto_reconnect(c, 0);
    if (na_client_connect(c, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "  (%s)\n", err);
        return fail("the profile alone did not select UDP — connect refused", c, srv);
    }

    /* ---- (3) real audio flows with the whole reliability pipeline in place ---- */

    int waited = 0;
    while (waited < 3000 && !(g_rx_ok && na_client_server_client_count(c) == 1)) {
        na_server_inject_audio(srv, RXBUF, (int)sizeof RXBUF);
        sleep_ms(20);
        waited += 20;
    }
    if (!g_rx_ok) {
        return fail("no RX signature reached the audio callback within budget", c, srv);
    }
    if (na_client_server_client_count(c) != 1) {
        return fail("server roster never reached 1", c, srv);
    }

    /* ---- (4) frozen once connected, like every other config setter ---- */

    if (na_client_set_reliability_profile(c, NA_RELIABILITY_UDP_LAN) != NA_ERR_INVALID ||
        na_client_set_transport(c, NA_TRANSPORT_TCP) != NA_ERR_INVALID) {
        return fail("config setters not frozen after connect", c, srv);
    }

    printf("c_client_profile: profile-only client connected over UDP, RX signature seen (%d ms)\n",
           waited);
    na_client_disconnect(c);
    na_client_destroy(c);

    /* ---- (5) last-writer-wins, in both directions ---- */

    /* set_transport after the profile changes the transport and leaves the rest in force. */
    if (!assert_cannot_reach_udp_server(port, "profile-then-set_transport(TCP)",
                                        NA_TRANSPORT_TCP, 1, NA_RELIABILITY_DEFAULT)) {
        return fail("ordering: set_transport did not override the profile's transport", NULL, srv);
    }
    /* NA_RELIABILITY_DEFAULT really is a reset — it must undo a UDP profile, not merely be a no-op. */
    if (!assert_cannot_reach_udp_server(port, "UDP_WAN-then-DEFAULT",
                                        NA_TRANSPORT_TCP, 0, NA_RELIABILITY_DEFAULT)) {
        return fail("NA_RELIABILITY_DEFAULT did not reset the transport to TCP", NULL, srv);
    }

    na_server_stop(srv);
    na_server_destroy(srv);
    printf("c_client_profile: PASS\n");
    return 0;
}
