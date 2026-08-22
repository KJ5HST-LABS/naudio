/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — C-ABI networking SERVER smoke.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * C-ABI networking SERVER smoke. Compiled as C and linked against the C++
 * naudio library (mirroring c_abi_smoke.c / c_net_smoke.c): it proves the
 * na_server_* surface of naudio.h is C-compilable and C-callable, and drives a FULL server-side
 * lifecycle — create -> start -> a real na_client_* client connects over loopback -> RX broadcast
 * via na_server_inject_audio -> TX extract via the tx-audio callback -> clean teardown — entirely
 * from PURE C, hardware-free (NULL server backend + NULL client backend, no PortAudio, no C++
 * fixture). Unlike c_net_smoke.c (which needed net_smoke_server.cpp to spin up a C++ server), the
 * na_server_* ABI lets the C side own BOTH ends.
 *
 * It asserts:
 *   (1) the invalid-argument contract — na_server_create(bad port) returns NULL/NA_ERR_INVALID;
 *       NULL-server setters/start/inject return NA_ERR_INVALID; the NULL backend rejects the
 *       device setters with NA_ERR_UNSUPPORTED; NULL-safe stop/destroy/getters don't crash;
 *   (2) start brings the server up on an OS-assigned port, on_started fires with that port,
 *       na_server_is_running()==1, and post-start config setters are frozen (NA_ERR_INVALID);
 *   (3) a NULL-backend client connects, the server roster reports one client, on_client_connected
 *       fires, an injected RX payload arrives byte-identically at the client's RX audio callback,
 *       and the server's mixed-TX stream is delivered to the na_server_tx_audio_cb (silence frames
 *       once a client is connected — the extract WIRING, since a NULL-backend client cannot
 *       capture real TX);
 *   (4) clean disconnect leaves the server at zero clients and on_client_disconnected fires;
 *       na_server_stop fires on_stopped and flips is_running to 0; destroy of both is clean;
 *   (5) the transport/profile ordering naudio.h promises — na_server_set_transport and
 *       na_server_set_reliability_profile both write the transport and the LAST ONE CALLED WINS —
 *       observed through which client kind can reach the resulting server, plus the reset the
 *       shared profile type promises: NA_RELIABILITY_DEFAULT really undoes a UDP profile; and the
 *       transport each of the two issue-#11 profiles selects — NA_RELIABILITY_DUAL serving BOTH
 *       transports on one port (the only (connect, connect) expectation here) and
 *       NA_RELIABILITY_UDP_IQ selecting UDP;
 *   (6) the other half of that promise — na_server_set_reliability_profile leaves the fields the
 *       other config setters own alone, so they compose in either order. Only max-clients is
 *       observable through the public ABI; the arm's own comment records what is not, and why.
 *       Runs early (before section 2) because it needs no started server.
 * Returns non-zero (failing the ctest) on any contract violation.
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

/* Cross-thread flag primitives: C11 <stdatomic.h> everywhere except MSVC, whose
 * stdatomic support is gated behind toolset-specific switches (VS 17.5 wanted
 * /experimental:c11atomics; VS 18's cl rejects that combination outright) —
 * Interlocked* gives the same seq-cst int semantics there. The _Atomic path is
 * the one the TSan gate exercises (POSIX-only), so nothing is lost on Windows. */
#if defined(_MSC_VER) && !defined(__clang__)
typedef volatile LONG atomic_int;
#  define atomic_store(p, v)     InterlockedExchange((p), (LONG)(v))
#  define atomic_load(p)         ((int)InterlockedCompareExchange((p), 0, 0))
#  define atomic_fetch_add(p, v) InterlockedExchangeAdd((p), (LONG)(v))
#else
#  include <stdatomic.h>
#endif

#include "naudio.h"

/* A known RX payload — injected at the server, expected byte-identically at the client callback. */
static const unsigned char KNOWN[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};

/* Cross-thread flags: callbacks fire on internal worker / dispatch / mixer threads; main polls
 * these. C11 _Atomic (not plain `volatile`) so the smoke is itself data-race-clean under TSan — the
 * point of the TSan gate is to prove the LIBRARY is race-free, which a `volatile`-flag harness would
 * mask with its own benign poll races. The load-bearing roster/run gates additionally use the
 * mutex/atomic-guarded na_server_* accessors. */
static atomic_int g_srv_started = 0;       /* server on_started (records the port)        */
static atomic_int g_srv_started_port = 0;
static atomic_int g_srv_stopped = 0;       /* server on_stopped                           */
static atomic_int g_srv_client_conn = 0;   /* server on_client_connected                  */
static atomic_int g_srv_client_disc = 0;   /* server on_client_disconnected               */
static atomic_int g_tx_frames = 0;         /* mixed-TX frames seen at na_server_tx_audio_cb */
static atomic_int g_tx_bytes = 0;          /* size of the last TX frame                   */

static atomic_int g_cli_connected = 0;     /* client on_connected                         */
static atomic_int g_cli_rx_ok = 0;         /* KNOWN arrived at the client RX audio callback */

/* ---- server-side callbacks ---- */
static void srv_on_started(int port, void* user) {
    (void)user;
    atomic_store(&g_srv_started_port, port);
    atomic_store(&g_srv_started, 1);
}
static void srv_on_stopped(void* user) { (void)user; atomic_store(&g_srv_stopped, 1); }
static void srv_on_client_connected(const char* id, const char* addr, void* user) {
    (void)id; (void)addr; (void)user;
    atomic_store(&g_srv_client_conn, 1);
}
static void srv_on_client_disconnected(const char* id, void* user) {
    (void)id; (void)user;
    atomic_store(&g_srv_client_disc, 1);
}
static void srv_on_tx_audio(const unsigned char* pcm, size_t n_bytes, void* user) {
    (void)pcm; (void)user;
    atomic_store(&g_tx_bytes, (int)n_bytes);
    atomic_fetch_add(&g_tx_frames, 1);
}

/* ---- client-side callbacks ---- */
static void cli_on_connected(const char* id, const char* addr, void* user) {
    (void)id; (void)addr; (void)user;
    atomic_store(&g_cli_connected, 1);
}
static void cli_on_rx_audio(const unsigned char* pcm, size_t n_bytes, void* user) {
    (void)user;
    if (n_bytes == sizeof KNOWN && memcmp(pcm, KNOWN, sizeof KNOWN) == 0)
        atomic_store(&g_cli_rx_ok, 1);
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    /* The UCRT has no nanosleep; Sleep() is the C-callable equivalent here. */
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* ---- transport/profile ordering (section 5) --------------------------------------------------
 *
 * naudio.h promises that na_server_set_transport and na_server_set_reliability_profile BOTH write
 * the transport and that the LAST ONE CALLED WINS. Nothing in the C ABI reads the transport back,
 * so it is observed the only way a C consumer can: by which client kind can reach the server. A
 * UDP-only server refuses a client left on the ABI-default TCP transport at connect (SO_ERROR=61),
 * and a TCP-only server refuses a UDP-profile client at handshake. This mirrors
 * tests/c_client_profile.c's assert_cannot_reach_udp_server onto the server's own setter pair.
 *
 * ARM D PINS A SECOND SENTENCE, stated on the shared na_reliability_profile type rather than on
 * either setter: NA_RELIABILITY_DEFAULT "mirrors a default-constructed AudioStreamConfig — plain
 * TCP with the whole reliability layer off, so it is the reset rather than a UDP profile"
 * (include/naudio.h:361-363). The CLIENT half of that claim has been pinned since
 * c_client_profile.c:211-214; the server's profile setter was passed UDP_WAN only, so a DEFAULT
 * case that had quietly become a no-op would have kept the whole suite green while contradicting
 * the shipped contract. Same asymmetry arms A-C closed, one sentence over. */

#define ORD_PROFILE_ONLY           0  /* control: profile(UDP_WAN) alone         -> UDP serves */
#define ORD_PROFILE_THEN_TRANSPORT 1  /* profile(UDP_WAN) -> set_transport(TCP)  -> TCP serves */
#define ORD_TRANSPORT_THEN_PROFILE 2  /* set_transport(TCP) -> profile(UDP_WAN)  -> UDP serves */
#define ORD_PROFILE_THEN_DEFAULT   3  /* profile(UDP_WAN) -> profile(DEFAULT)    -> TCP serves */
#define ORD_PROFILE_DUAL           4  /* profile(DUAL) alone                     -> BOTH serve */
#define ORD_PROFILE_UDP_IQ         5  /* profile(UDP_IQ) alone                   -> UDP serves */

/* Start a NULL-backend server on an ephemeral port with the two setters called in `order`.
 * Returns NULL (having already destroyed the handle) on any failure. */
static na_audio_server* ordering_server(int order, int* out_port) {
    char err[256];
    na_audio_server* s = na_server_create(NA_SERVER_BACKEND_NULL, 0);
    if (s == NULL) {
        fprintf(stderr, "FAIL: na_server_create (ordering arm)\n");
        return NULL;
    }
    int ok = 1;
    /* The two single-profile arms take the early exit: neither composes with set_transport, and
     * starting them from UDP_WAN would let a no-op setter inherit a UDP transport it never chose. */
    if (order == ORD_PROFILE_DUAL || order == ORD_PROFILE_UDP_IQ) {
        ok = na_server_set_reliability_profile(
                 s, order == ORD_PROFILE_DUAL ? NA_RELIABILITY_DUAL : NA_RELIABILITY_UDP_IQ) == NA_OK;
        if (!ok) {
            fprintf(stderr, "FAIL: an ordering-arm config setter was rejected\n");
            na_server_destroy(s);
            return NULL;
        }
        if (na_server_start(s, err, (int)sizeof err) != NA_OK) {
            fprintf(stderr, "FAIL: na_server_start (ordering arm) (%s)\n", err);
            na_server_destroy(s);
            return NULL;
        }
        *out_port = na_server_port(s);
        return s;
    }
    if (order == ORD_TRANSPORT_THEN_PROFILE)
        ok = ok && na_server_set_transport(s, NA_TRANSPORT_TCP) == NA_OK;
    ok = ok && na_server_set_reliability_profile(s, NA_RELIABILITY_UDP_WAN) == NA_OK;
    if (order == ORD_PROFILE_THEN_TRANSPORT)
        ok = ok && na_server_set_transport(s, NA_TRANSPORT_TCP) == NA_OK;
    if (order == ORD_PROFILE_THEN_DEFAULT)  /* the reset, applied THROUGH the profile setter */
        ok = ok && na_server_set_reliability_profile(s, NA_RELIABILITY_DEFAULT) == NA_OK;
    if (!ok) {
        fprintf(stderr, "FAIL: an ordering-arm config setter was rejected\n");
        na_server_destroy(s);
        return NULL;
    }
    if (na_server_start(s, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "FAIL: na_server_start (ordering arm) (%s)\n", err);
        na_server_destroy(s);
        return NULL;
    }
    *out_port = na_server_port(s);
    return s;
}

/* Probe `port` with one client kind: 1 if the connect succeeded, 0 if it was refused, -1 if the
 * harness itself failed. `udp_client` applies the UDP_WAN profile (which selects UDP); otherwise
 * the client is left on the ABI-default transport, exactly as c_client_profile.c's probes are. */
static int reaches(int port, int udp_client, const char* what) {
    char err[256];
    err[0] = '\0';
    na_stream_client* c = na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", port, what);
    if (c == NULL) {
        fprintf(stderr, "FAIL: na_client_create (%s)\n", what);
        return -1;
    }
    na_client_set_playback_device(c, 0);  /* REQUIRED for RX even on the NULL backend */
    na_client_set_auto_reconnect(c, 0);   /* a retry storm would only slow the refusal down */
    if (udp_client && na_client_set_reliability_profile(c, NA_RELIABILITY_UDP_WAN) != NA_OK) {
        fprintf(stderr, "FAIL: profile rejected on the %s probe\n", what);
        na_client_destroy(c);
        return -1;
    }
    const na_error_t rc = na_client_connect(c, err, (int)sizeof err);
    if (rc == NA_OK) na_client_disconnect(c);
    na_client_destroy(c);
    if (rc != NA_OK) printf("    %s refused (%s)\n", what, err);
    return rc == NA_OK ? 1 : 0;
}

/* One ordering arm end to end. BOTH client kinds are probed and BOTH results asserted against the
 * arm's expected pair, which is what makes the control real: no constant-returning probe can
 * satisfy arm A's (connected, refused) and arm B's (refused, connected) at once, so a detector
 * blind to the transport fails whichever expectation it contradicts instead of reporting agreement
 * and reading as corroboration (Learning 58).
 *
 * The client expected to CONNECT is probed first because it is always the fast one: a client whose
 * transport the server does not serve is refused instantly over TCP (SO_ERROR=61) but pays the
 * full 10 s handshake timeout over UDP, since a datagram sent at a port with no UDP listener draws
 * no reply. Probing in this order keeps a regression's report in milliseconds. The TWO slow probes
 * — arm B's UDP one and arm D's — are the whole cost of this section, and neither is redundant with
 * its arm's TCP probe: the TCP probe catches a setter that did nothing (the server stays UDP and
 * refuses it instantly), while only the UDP probe catches one that selected DUAL and left the
 * server answering both, which no TCP probe can see because a DUAL server accepts it happily.
 *
 * MEASURED, so that nobody has to re-derive it before deciding whether to keep them: on a healthy
 * arm D the TCP probe returns in 1.2 ms and the UDP probe in 10000.3 ms — the whole 10 s is
 * src/net/AudioStreamClient.cpp:24's kConnectTimeoutMs, and no public setter shortens it. The cost
 * is therefore paid on GREEN runs only; under either regression the arm reports in milliseconds.
 * Keeping it was a deliberate call, not an oversight — do not "optimise" a UDP probe away without
 * replacing what it catches.
 *
 * IF YOU RE-AUDIT THESE BY MUTATION, note that the two checks below are not independent detectors:
 * a mutation that makes a setter a no-op violates BOTH of an arm's expectations, and the UDP check
 * is evaluated first, so it fires and MASKS the TCP one. Each was confirmed live by neutralising
 * the check that masks it and re-running the same mutation — read WHICH assertion fired, never
 * merely that something went red (Learning 54). Returns 1 on success. */
static int check_ordering_arm(int order, const char* name, int want_udp, int want_tcp) {
    int port = -1;
    na_audio_server* s = ordering_server(order, &port);
    if (s == NULL) return 0;
    printf("  ordering arm %s (port %d)\n", name, port);

    int got_udp, got_tcp;
    if (want_udp) {
        got_udp = reaches(port, 1, "udp-profile");
        got_tcp = reaches(port, 0, "tcp-default");
    } else {
        got_tcp = reaches(port, 0, "tcp-default");
        got_udp = reaches(port, 1, "udp-profile");
    }
    na_server_stop(s);
    na_server_destroy(s);
    if (got_udp < 0 || got_tcp < 0) return 0;

    if (got_udp != want_udp) {
        fprintf(stderr, "FAIL: ordering arm %s — the UDP-profile client %s, want %s\n", name,
                got_udp ? "connected" : "was refused", want_udp ? "connected" : "refused");
        return 0;
    }
    if (got_tcp != want_tcp) {
        fprintf(stderr, "FAIL: ordering arm %s — the TCP-default client %s, want %s\n", name,
                got_tcp ? "connected" : "was refused", want_tcp ? "connected" : "refused");
        return 0;
    }
    return 1;
}

/* ---- profile / config-setter composition (section 6) -----------------------------------------
 *
 * na_server_set_reliability_profile applies a preset but must leave the four fields the OTHER
 * na_server_* config setters own untouched — sample rate, bits, channels and max-clients — so the
 * setters compose in either order. That is the promise at include/naudio.h:786-787.
 *
 * WHAT IS MEASURABLE HERE, AND WHAT IS NOT. Only max-clients is observable through the public C
 * ABI: na_server_max_clients reports the configured value pre-start. The three audio-format fields
 * have NO public accessor, and the obvious byte-volume route is a DEAD detector —
 * na_server_inject_audio neither resamples nor converts, so a client receives the same byte COUNT
 * whatever format the server carries (src/net/AudioStreamServer.cpp:930, injectAudio). Since #20
 * the fan-out may split those bytes across several packets, which changes the frame count but not
 * the total — so this detector is dead for the same reason it always was. Measured, not assumed: deleting `pc.sampleRate = rate;` from the setter
 * leaves all 299 tests GREEN. That is why this section pins one field rather than four, and the
 * unguarded three are tracked as an issue rather than left as folklore.
 *
 * THE THIRD ARM IS WHAT MAKES THE FIRST TWO MEAN ANYTHING. An order-independence claim predicts
 * arm A == arm B, and an accessor stuck on any constant satisfies that perfectly. The profile-only
 * arm must read the DEFAULT instead, so a blind accessor fails here and only here. It is NOT a
 * second preservation check — every preset carries the default max-clients, so a clobbering profile
 * leaves this arm looking correct. Arm A is the clobber detector.
 *
 * Placed before section (2) because it needs no started server — create/destroy only, no port, no
 * measurable suite time — and because section (2) reads the same accessor at :359+13: evaluating
 * the blind-instrument control first keeps a mutation of the accessor landing HERE instead of being
 * masked downstream.
 */
#define COMPOSE_MAX_CLIENTS 7  /* != the documented default (4), so the two are distinguishable */

/* max-clients on a throwaway pre-start server, with na_server_set_max_clients placed on either side
 * of the profile call. Returns -1 on a setup fault, which no arm accepts as an answer. */
static int compose_max_clients(int set_before, int set_after) {
    int seen;
    na_audio_server* s = na_server_create(NA_SERVER_BACKEND_NULL, 0);
    if (s == NULL) return -1;
    if (set_before && na_server_set_max_clients(s, COMPOSE_MAX_CLIENTS) != NA_OK) goto fault;
    if (na_server_set_reliability_profile(s, NA_RELIABILITY_UDP_WAN) != NA_OK) goto fault;
    if (set_after && na_server_set_max_clients(s, COMPOSE_MAX_CLIENTS) != NA_OK) goto fault;
    seen = na_server_max_clients(s);
    na_server_destroy(s);
    return seen;
fault:
    na_server_destroy(s);
    return -1;
}

/* The default the accessor reports on an untouched server — derived from the library rather than
 * spelled here, so this stays a preservation check and not a restatement of the default. */
static int default_max_clients(void) {
    int seen;
    na_audio_server* s = na_server_create(NA_SERVER_BACKEND_NULL, 0);
    if (s == NULL) return -1;
    seen = na_server_max_clients(s);
    na_server_destroy(s);
    return seen;
}

int main(void) {
    /* ---- (1) invalid-argument contract (no server / hardware) ---- */

    if (na_server_create(NA_SERVER_BACKEND_NULL, -1) != NULL ||
        na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_server_create(port -1) not NULL/NA_ERR_INVALID\n");
        return 1;
    }
    if (na_server_create(NA_SERVER_BACKEND_NULL, 70000) != NULL ||
        na_last_error() != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_server_create(port 70000) not NULL/NA_ERR_INVALID\n");
        return 1;
    }
    if (na_server_set_max_clients(NULL, 4) != NA_ERR_INVALID ||
        na_server_start(NULL, NULL, 0) != NA_ERR_INVALID ||
        na_server_inject_audio(NULL, KNOWN, (int)sizeof KNOWN) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: NULL-server setters/start/inject not NA_ERR_INVALID\n");
        return 1;
    }
    /* na_server_get_stats, NULL-server half. The NULL-out and below-the-floor halves need a
     * REAL server handle to be meaningful, so they are asserted at the live-reading site below
     * rather than with a fabricated pointer here. */
    {
        na_server_stats st;
        if (na_server_get_stats(NULL, &st, sizeof st) != NA_ERR_INVALID) {
            fprintf(stderr, "FAIL: na_server_get_stats(NULL) not NA_ERR_INVALID\n");
            return 1;
        }
    }

    /* NULL-safe no-ops must not crash, and the NULL-server getters return their sentinels. */
    na_server_stop(NULL);
    na_server_destroy(NULL);
    if (na_server_is_running(NULL) != 0 || na_server_port(NULL) != -1 ||
        na_server_client_count(NULL) != -1 || na_server_max_clients(NULL) != -1) {
        fprintf(stderr, "FAIL: NULL-server query accessors wrong\n");
        return 1;
    }

    /* New config setters (na_server_set_audio_format / _set_reliability_profile): NULL-server contract
     * plus a throwaway server exercising the exact config the na_hamlib_bridge uses — the UDP WAN
     * reliability profile (FEC on) + mono 48k S16 — and the invalid-value rejections. */
    if (na_server_set_audio_format(NULL, 48000, 16, 2) != NA_ERR_INVALID ||
        na_server_set_reliability_profile(NULL, NA_RELIABILITY_UDP_WAN) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: NULL-server format/reliability setters not NA_ERR_INVALID\n");
        return 1;
    }
    {
        na_audio_server* cfg = na_server_create(NA_SERVER_BACKEND_NULL, 0);
        if (cfg == NULL) {
            fprintf(stderr, "FAIL: na_server_create (cfg probe) (%s)\n", na_strerror(na_last_error()));
            return 1;
        }
        if (na_server_set_reliability_profile(cfg, NA_RELIABILITY_UDP_WAN) != NA_OK ||
            na_server_set_audio_format(cfg, 48000, 16, 1) != NA_OK) {
            fprintf(stderr, "FAIL: UDP_WAN + mono-S16 config setters rejected\n");
            na_server_destroy(cfg);
            return 1;
        }
        /* Every DOCUMENTED value of the enum must be accepted, so a value added to naudio.h without
         * a matching case in the setter's switch is caught here rather than by a consumer. The two
         * added for issue #11 are the reason this list is exhaustive instead of a single specimen. */
        if (na_server_set_reliability_profile(cfg, NA_RELIABILITY_DEFAULT) != NA_OK ||
            na_server_set_reliability_profile(cfg, NA_RELIABILITY_UDP_LAN) != NA_OK ||
            na_server_set_reliability_profile(cfg, NA_RELIABILITY_UDP_FT8) != NA_OK ||
            na_server_set_reliability_profile(cfg, NA_RELIABILITY_UDP_IQ)  != NA_OK ||
            na_server_set_reliability_profile(cfg, NA_RELIABILITY_DUAL)    != NA_OK ||
            na_server_set_reliability_profile(cfg, NA_RELIABILITY_UDP_BARE) != NA_OK ||
            na_server_set_reliability_profile(cfg, NA_RELIABILITY_UDP_WAN) != NA_OK) {
            fprintf(stderr, "FAIL: a documented profile was rejected by the server setter\n");
            na_server_destroy(cfg);
            return 1;
        }
        /* The IQ pairing naudio.h documents on this setter: the two calls compose in EITHER order,
         * which is the property that makes "pair them" honest advice rather than a sequencing trap.
         * Neither the rate nor the profile is readable back through this ABI, so this asserts the
         * calls are ACCEPTED both ways round, not that the rate survived — see arm F's comment. */
        if (na_server_set_audio_format(cfg, 192000, 16, 1) != NA_OK ||
            na_server_set_reliability_profile(cfg, NA_RELIABILITY_UDP_IQ) != NA_OK ||
            na_server_set_reliability_profile(cfg, NA_RELIABILITY_UDP_IQ) != NA_OK ||
            na_server_set_audio_format(cfg, 192000, 16, 1) != NA_OK) {
            fprintf(stderr, "FAIL: the documented UDP_IQ + 192 kHz pairing was rejected\n");
            na_server_destroy(cfg);
            return 1;
        }
        if (na_server_set_audio_format(cfg, 48000, 16, 1) != NA_OK) {  /* restore for what follows */
            fprintf(stderr, "FAIL: could not restore the mono-S16 format\n");
            na_server_destroy(cfg);
            return 1;
        }
        if (na_server_set_audio_format(cfg, 48000, 24, 1) != NA_ERR_INVALID ||   /* bits != 16 */
            na_server_set_audio_format(cfg, 48000, 16, 3) != NA_ERR_INVALID ||   /* channels 3 */
            na_server_set_audio_format(cfg, 0, 16, 2)     != NA_ERR_INVALID ||   /* rate 0     */
            na_server_set_reliability_profile(cfg, (na_reliability_profile)99) != NA_ERR_INVALID) {
            fprintf(stderr, "FAIL: invalid format/profile values not rejected\n");
            na_server_destroy(cfg);
            return 1;
        }
        na_server_destroy(cfg);  /* never started — clean create/destroy */
    }

    /* ---- (6) the profile leaves what the other config setters own alone ----
     * Three arms, three distinct jobs — see the comment block above compose_max_clients. Each
     * assertion stands alone so a mutation lands on the one property it breaks. */
    {
        const int dflt  = default_max_clients();
        const int arm_a = compose_max_clients(1, 0);  /* set_max_clients -> profile */
        const int arm_b = compose_max_clients(0, 1);  /* profile -> set_max_clients */
        const int arm_c = compose_max_clients(0, 0);  /* profile alone -> must read the default */

        if (dflt < 0 || arm_a < 0 || arm_b < 0 || arm_c < 0) {
            fprintf(stderr, "FAIL: a composition arm could not be configured\n");
            return 1;
        }
        if (arm_a != COMPOSE_MAX_CLIENTS) {  /* the clobber detector */
            fprintf(stderr, "FAIL: the profile clobbered max-clients set before it — got %d, "
                            "want %d\n", arm_a, COMPOSE_MAX_CLIENTS);
            return 1;
        }
        if (arm_b != COMPOSE_MAX_CLIENTS) {
            fprintf(stderr, "FAIL: max-clients set after the profile did not stick — got %d, "
                            "want %d\n", arm_b, COMPOSE_MAX_CLIENTS);
            return 1;
        }
        if (arm_c == COMPOSE_MAX_CLIENTS) {  /* the blind-instrument control */
            fprintf(stderr, "FAIL: the profile-only arm reads %d — na_server_max_clients cannot "
                            "discriminate, so arms A and B prove nothing\n", arm_c);
            return 1;
        }
        if (arm_c != dflt) {
            fprintf(stderr, "FAIL: the profile moved max-clients off the default — got %d, want %d\n",
                    arm_c, dflt);
            return 1;
        }
        printf("  (6) profile composition: max-clients %d preserved in both orders; profile-only "
               "reads the default %d\n", COMPOSE_MAX_CLIENTS, dflt);
    }

    /* ---- (2) create + configure + start a NULL-backend server on an ephemeral port ---- */

    na_audio_server* server = na_server_create(NA_SERVER_BACKEND_NULL, 0);
    if (server == NULL) {
        fprintf(stderr, "FAIL: na_server_create (%s)\n", na_strerror(na_last_error()));
        return 1;
    }

    /* The NULL backend has no real devices: the device setters must be refused as UNSUPPORTED. */
    if (na_server_set_capture_device(server, 0) != NA_ERR_UNSUPPORTED ||
        na_server_set_playback_device(server, 0) != NA_ERR_UNSUPPORTED) {
        fprintf(stderr, "FAIL: NULL backend did not reject device setters with NA_ERR_UNSUPPORTED\n");
        na_server_destroy(server);
        return 1;
    }

    /* struct_size is validated on the server side too, and the server is not yet started, so
     * the "set before start" guard cannot be what rejects these. */
    na_server_callbacks sized;
    memset(&sized, 0, sizeof sized);
    if (na_server_set_callbacks(server, &sized, NULL) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: na_server_set_callbacks with struct_size=0 not rejected\n");
        na_server_destroy(server);
        return 1;
    }
    sized.struct_size = sizeof sized;
    if (na_server_set_callbacks(server, &sized, NULL) != NA_OK) {
        fprintf(stderr, "FAIL: na_server_set_callbacks with a correct struct_size rejected\n");
        na_server_destroy(server);
        return 1;
    }

    na_server_callbacks scbs;
    memset(&scbs, 0, sizeof scbs);
    scbs.struct_size = sizeof scbs;
    scbs.on_started = srv_on_started;
    scbs.on_stopped = srv_on_stopped;
    scbs.on_client_connected = srv_on_client_connected;
    scbs.on_client_disconnected = srv_on_client_disconnected;
    if (na_server_set_callbacks(server, &scbs, NULL) != NA_OK ||
        na_server_set_tx_audio_cb(server, srv_on_tx_audio, NULL) != NA_OK ||
        na_server_set_max_clients(server, 4) != NA_OK ||
        na_server_set_transport(server, NA_TRANSPORT_TCP) != NA_OK) {
        fprintf(stderr, "FAIL: server configuration setters\n");
        na_server_destroy(server);
        return 1;
    }
    if (na_server_max_clients(server) != 4) {  /* readable pre-start */
        fprintf(stderr, "FAIL: na_server_max_clients pre-start != 4\n");
        na_server_destroy(server);
        return 1;
    }

    char serr[256];
    if (na_server_start(server, serr, (int)sizeof serr) != NA_OK) {
        fprintf(stderr, "FAIL: na_server_start (%s)\n", serr);
        na_server_destroy(server);
        return 1;
    }
    int port = na_server_port(server);
    if (port <= 0 || !na_server_is_running(server)) {
        fprintf(stderr, "FAIL: server not running on a valid port (port=%d)\n", port);
        na_server_destroy(server);
        return 1;
    }
    /* on_started fired with the bound port (dispatch thread; allow a beat). */
    for (int i = 0; i < 50 && !atomic_load(&g_srv_started); i++) sleep_ms(20);
    if (!atomic_load(&g_srv_started) || atomic_load(&g_srv_started_port) != port) {
        fprintf(stderr, "FAIL: on_started (fired=%d, port=%d vs %d)\n",
                atomic_load(&g_srv_started), atomic_load(&g_srv_started_port), port);
        na_server_destroy(server);
        return 1;
    }
    /* Config is frozen after start. */
    if (na_server_set_max_clients(server, 8) != NA_ERR_INVALID ||
        na_server_set_callbacks(server, &scbs, NULL) != NA_ERR_INVALID ||
        na_server_set_tx_audio_cb(server, srv_on_tx_audio, NULL) != NA_ERR_INVALID ||
        na_server_set_audio_format(server, 48000, 16, 2) != NA_ERR_INVALID ||
        na_server_set_reliability_profile(server, NA_RELIABILITY_UDP_WAN) != NA_ERR_INVALID) {
        fprintf(stderr, "FAIL: config setters not frozen after start\n");
        na_server_destroy(server);
        return 1;
    }

    /* ---- (3) a real NULL-backend client connects over loopback ---- */

    na_stream_client* client =
        na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", port, "c-server-smoke");
    if (client == NULL) {
        fprintf(stderr, "FAIL: na_client_create (%s)\n", na_strerror(na_last_error()));
        na_server_destroy(server);
        return 1;
    }
    na_client_callbacks ccbs;
    memset(&ccbs, 0, sizeof ccbs);
    ccbs.struct_size = sizeof ccbs;
    ccbs.on_connected = cli_on_connected;
    na_client_set_callbacks(client, &ccbs, NULL);
    na_client_set_audio_cb(client, cli_on_rx_audio, NULL);
    na_client_set_playback_device(client, 0);  /* REQUIRED for RX; NULL backend accepts any id */
    na_client_set_auto_reconnect(client, 0);    /* deterministic: no backoff churn in the smoke */

    char cerr[256];
    if (na_client_connect(client, cerr, (int)sizeof cerr) != NA_OK) {
        fprintf(stderr, "FAIL: na_client_connect (%s)\n", cerr);
        na_client_destroy(client);
        na_server_destroy(server);
        return 1;
    }

    /* Inject the known RX payload until it arrives at the client callback AND both connect
     * callbacks have fired AND the server roster shows one client (<=3s). The two connect
     * flags land on the server/client dispatch threads slightly after the wire-level connect
     * — poll for them here rather than asserting them immediately after the loop, or a busy
     * scheduler loses that race. na_server_client_count is mutex-guarded (safe to poll). */
    long long srv_packets_sent_with_client = 0;
    int waited = 0;
    while (waited < 3000 &&
           !(atomic_load(&g_cli_rx_ok) && atomic_load(&g_cli_connected) &&
             atomic_load(&g_srv_client_conn) && na_server_client_count(server) == 1)) {
        na_server_inject_audio(server, KNOWN, (int)sizeof KNOWN);
        sleep_ms(20);
        waited += 20;
    }
    if (!atomic_load(&g_cli_rx_ok)) {
        fprintf(stderr, "FAIL: injected RX frame never reached the client audio callback\n");
        na_client_destroy(client);
        na_server_destroy(server);
        return 1;
    }
    if (na_server_client_count(server) != 1) {
        fprintf(stderr, "FAIL: server roster never reached 1 (count=%d)\n",
                na_server_client_count(server));
        na_client_destroy(client);
        na_server_destroy(server);
        return 1;
    }
    if (!atomic_load(&g_srv_client_conn) || !atomic_load(&g_cli_connected)) {
        fprintf(stderr, "FAIL: connect callbacks (srv=%d, cli=%d)\n",
                atomic_load(&g_srv_client_conn), atomic_load(&g_cli_connected));
        na_client_destroy(client);
        na_server_destroy(server);
        return 1;
    }

    /* ---- na_server_get_stats: a LIVE reading, from pure C ----
     * The two counters this call exists for are control_retransmits and queue_drops, neither of
     * which can carry information on a client. Their VALUES are provoked and bounded in
     * tests/net/test_server.cpp (a server observed at 6 retransmits, and queue_drops pinned at 0
     * while the drain keeps up); what is asserted here is the C-ABI surface itself — that the
     * struct is C-compilable, the call is C-callable, the size parameter behaves, and a live
     * server reports a live roster. */
    {
        na_server_stats st;  /* deliberately NOT pre-zeroed: the library writes every field */

        /* The remaining two thirds of the invalid contract, against a real handle: a NULL out,
         * and a struct_size below the v1 floor. The floor is the guard that stops a caller
         * compiled against a SHORTER header from having its struct overrun — asserted one byte
         * under, which is the only interesting value, since anything smaller is caught by the
         * same comparison. */
        if (na_server_get_stats(server, NULL, sizeof st) != NA_ERR_INVALID ||
            na_server_get_stats(server, &st, NA_SERVER_STATS_SIZE_V1 - 1) != NA_ERR_INVALID) {
            fprintf(stderr, "FAIL: na_server_get_stats NULL-out / below-floor not NA_ERR_INVALID\n");
            na_client_destroy(client);
            na_server_destroy(server);
            return 1;
        }

        if (na_server_get_stats(server, &st, sizeof st) != NA_OK) {
            fprintf(stderr, "FAIL: na_server_get_stats on a running server\n");
            na_client_destroy(client);
            na_server_destroy(server);
            return 1;
        }
        if (st.running != 1 || st.clients_connected != 1) {
            fprintf(stderr, "FAIL: server stats running=%d clients=%d (want 1/1)\n",
                    st.running, st.clients_connected);
            na_client_destroy(client);
            na_server_destroy(server);
            return 1;
        }
        /* Non-vacuity: the connection really is carrying traffic, so the zeros below are
         * measurements of a live server rather than of an idle one. */
        if (st.packets_sent <= 0 || st.bytes_sent <= st.packets_sent) {
            fprintf(stderr, "FAIL: server stats packets_sent=%lld bytes_sent=%lld\n",
                    st.packets_sent, st.bytes_sent);
            na_client_destroy(client);
            na_server_destroy(server);
            return 1;
        }
        /* Documented expectations on a healthy loopback server: no corruption, and no local
         * queue loss because the receive path never blocks (see the na_server_stats contract). */
        if (st.crc_errors != 0 || st.queue_drops != 0) {
            fprintf(stderr, "FAIL: healthy server reported crc_errors=%d queue_drops=%lld\n",
                    st.crc_errors, st.queue_drops);
            na_client_destroy(client);
            na_server_destroy(server);
            return 1;
        }
        srv_packets_sent_with_client = st.packets_sent;  /* for the roster-gauge check below */
    }

    /* The OVER-SIZED caller: a consumer compiled against a FUTURE header that appended fields.
     * The library must fill what it knows and ZERO the tail rather than leave it indeterminate,
     * so an appended field reads as a defined 0 instead of stack garbage. Poisoned first, so
     * "zeroed by the library" and "never written" are distinguishable. */
    {
        unsigned char big[sizeof(na_server_stats) + 32];
        memset(big, 0xAB, sizeof big);
        if (na_server_get_stats(server, (na_server_stats*)big, sizeof big) != NA_OK) {
            fprintf(stderr, "FAIL: na_server_get_stats with an over-sized struct_size\n");
            na_client_destroy(client);
            na_server_destroy(server);
            return 1;
        }
        for (size_t i = sizeof(na_server_stats); i < sizeof big; i++) {
            if (big[i] != 0) {
                fprintf(stderr, "FAIL: over-sized tail not zero-filled at byte %zu (0x%02X)\n",
                        i, big[i]);
                na_client_destroy(client);
                na_server_destroy(server);
                return 1;
            }
        }
    }

    /* TX-extract WIRING: with a client connected, the mixer playback loop drains the (empty) TX
     * buffer as silence frames into the ForwardingPlaybackStream -> the C tx-audio callback. We
     * cannot drive REAL TX from a NULL-backend client (no capture), so we verify the extract path
     * fires with full-size frames. <=3s for the initial-buffering window to elapse. */
    for (int i = 0; i < 150 && atomic_load(&g_tx_frames) == 0; i++) sleep_ms(20);
    if (atomic_load(&g_tx_frames) == 0 || atomic_load(&g_tx_bytes) <= 0) {
        fprintf(stderr, "FAIL: na_server_tx_audio_cb never delivered a mixed-TX frame "
                        "(frames=%d, bytes=%d)\n", atomic_load(&g_tx_frames), atomic_load(&g_tx_bytes));
        na_client_destroy(client);
        na_server_destroy(server);
        return 1;
    }

    /* ---- (4) clean disconnect + stop ---- */

    na_client_disconnect(client);
    int gone = 0;
    for (int i = 0; i < 150; i++) {  /* up to ~3s for the server to drop the session */
        if (na_server_client_count(server) == 0) { gone = 1; break; }
        sleep_ms(20);
    }
    na_client_destroy(client);
    if (!gone) {
        fprintf(stderr, "FAIL: server still reports clients after client disconnect\n");
        na_server_destroy(server);
        return 1;
    }
    if (!atomic_load(&g_srv_client_disc)) {
        fprintf(stderr, "FAIL: server on_client_disconnected never fired\n");
        na_server_destroy(server);
        return 1;
    }

    /* THE ROSTER-GAUGE CONTRACT, asserted from C because it is the one way na_server_stats
     * differs from na_client_stats and the one a consumer will get wrong. These are sums over the
     * clients connected RIGHT NOW, so the departed client's counters left with it and packets_sent
     * must have DROPPED — a C consumer computing a delta across this boundary would read a
     * negative throughput. If this ever stops holding, the header contract is stale. */
    {
        na_server_stats st;
        if (na_server_get_stats(server, &st, sizeof st) != NA_OK) {
            fprintf(stderr, "FAIL: na_server_get_stats after disconnect\n");
            na_server_destroy(server);
            return 1;
        }
        if (st.clients_connected != 0) {
            fprintf(stderr, "FAIL: stats roster %d after disconnect (want 0)\n",
                    st.clients_connected);
            na_server_destroy(server);
            return 1;
        }
        if (st.packets_sent >= srv_packets_sent_with_client) {
            fprintf(stderr, "FAIL: packets_sent did not decrease when the client left "
                    "(%lld -> %lld); na_server_stats promises it is a roster gauge\n",
                    srv_packets_sent_with_client, st.packets_sent);
            na_server_destroy(server);
            return 1;
        }
        printf("  roster gauge: packets_sent %lld (1 client) -> %lld (0 clients)\n",
               srv_packets_sent_with_client, st.packets_sent);
    }

    /* A STOPPED server is not an error — it is running == 0 with defaults, the same rule
     * na_client_stats.connected states. Asserted after na_server_stop below. */

    na_server_stop(server);
    if (na_server_is_running(server)) {
        fprintf(stderr, "FAIL: server still running after na_server_stop\n");
        na_server_destroy(server);
        return 1;
    }
    {
        na_server_stats st;
        if (na_server_get_stats(server, &st, sizeof st) != NA_OK || st.running != 0 ||
            st.clients_connected != 0 || st.packets_sent != 0) {
            fprintf(stderr, "FAIL: stopped server stats running=%d clients=%d packets_sent=%lld "
                    "(want 0/0/0, and NOT an error)\n",
                    st.running, st.clients_connected, st.packets_sent);
            na_server_destroy(server);
            return 1;
        }
    }

    for (int i = 0; i < 50 && !atomic_load(&g_srv_stopped); i++) sleep_ms(20);
    if (!atomic_load(&g_srv_stopped)) {
        fprintf(stderr, "FAIL: server on_stopped never fired\n");
        na_server_destroy(server);
        return 1;
    }

    na_server_destroy(server);  /* idempotent stop + join + drain dispatcher + free */

    /* ---- (5) transport/profile ordering — the last setter to write the transport wins ----
     * The header states this on na_server_set_reliability_profile; before this section nothing
     * asserted it, while the client's identical claim was pinned by c_client_profile.c:203-214.
     * A regression that made either setter a no-op after the other would have kept the whole suite
     * green while contradicting the shipped contract. Arm A is the control the claim requires to
     * DIFFER from arm B; arm C is the same expectation reached in the opposite order, which is
     * what pins the profile as the winner when IT is the later call.
     *
     * Arm D is the profile setter answering for ITSELF rather than against set_transport: it is the
     * only arm in which no na_server_set_transport call appears at all, so it is the one that can
     * tell a real NA_RELIABILITY_DEFAULT reset from a no-op. Its expectation is arm B's, reached
     * without the other setter. */

    if (!check_ordering_arm(ORD_PROFILE_ONLY, "A: profile(UDP_WAN) alone [control]", 1, 0) ||
        !check_ordering_arm(ORD_PROFILE_THEN_TRANSPORT,
                            "B: profile(UDP_WAN) -> set_transport(TCP)", 0, 1) ||
        !check_ordering_arm(ORD_TRANSPORT_THEN_PROFILE,
                            "C: set_transport(TCP) -> profile(UDP_WAN)", 1, 0) ||
        !check_ordering_arm(ORD_PROFILE_THEN_DEFAULT,
                            "D: profile(UDP_WAN) -> profile(DEFAULT) [the reset]", 0, 1) ||
        /* ARMS E AND F — the two profiles added for issue #11. Both are single-profile arms, so
         * like arm D they answer for the profile setter alone with no set_transport in sight.
         *
         * ARM E IS THE ONLY (1, 1) EXPECTATION IN THIS FILE, and that is the whole point: DUAL's
         * defining claim is that ONE port serves BOTH transports, and no other profile can satisfy
         * it. UDP_WAN gives (1, 0) and DEFAULT gives (0, 1) — each fails E on the probe the other
         * passes, so a DUAL case mis-wired to either preset is caught whichever way it went wrong.
         * Note the existing arms already relied on DUAL being distinguishable this way (see the
         * probe-ordering comment above: "only the UDP probe catches one that selected DUAL and left
         * the server answering both") — before this arm, that shape was something the suite guarded
         * AGAINST without anything asserting it is what NA_RELIABILITY_DUAL actually produces.
         *
         * ARM F pins UDP_IQ to the UDP transport. It is deliberately the WEAKER of the two claims:
         * it cannot tell UDP_IQ from any other NA_RELIABILITY_UDP_* value, because all four select
         * UDP and the transport is all this section can see. What it does catch is a case wired to
         * a TCP preset or missing altogether. The preset-was-applied half is pinned on the client
         * side instead, by stats fingerprint, in tests/c_client_profile.c section (7) — and IQ's
         * defining 192 kHz is pinned NOWHERE, because sample rate has no observable through this
         * ABI at all (issue #36, declined; re-measured this session and still true).
         *
         * NEITHER ARM PAYS THE 10 s UDP-REFUSAL COST the comment above warns about: both expect the
         * UDP probe to CONNECT, so both probe it first and both return in milliseconds on green. */
        !check_ordering_arm(ORD_PROFILE_DUAL,
                            "E: profile(DUAL) alone [both transports, one port]", 1, 1) ||
        !check_ordering_arm(ORD_PROFILE_UDP_IQ,
                            "F: profile(UDP_IQ) alone [transport only]", 1, 0)) {
        return 1;
    }

    /* ---- the #92 gate, server side: bare UDP/DUAL start is refused unless explicit ----
     *
     * The server mirror of tests/c_client_profile.c section (6). set_transport writes the
     * transport and nothing else, so UDP (and DUAL, whose UDP half carries the same exposure)
     * with every reliability component off is refused at na_server_start with both remedies in
     * errbuf — before any bind, so the handle survives and the setters unfreeze (the same
     * failed-start recovery the port-busy case documents). NA_RELIABILITY_UDP_BARE is the
     * explicit request that starts: byte-for-byte the old bare composition. */
    {
        static const na_transport bare_transports[] = {NA_TRANSPORT_UDP, NA_TRANSPORT_DUAL};
        char gerr[256];
        size_t t;
        for (t = 0; t < sizeof bare_transports / sizeof bare_transports[0]; t++) {
            na_audio_server *g = na_server_create(NA_SERVER_BACKEND_NULL, 0);
            if (g == NULL) {
                fprintf(stderr, "FAIL: na_server_create (gate probe)\n");
                return 1;
            }
            if (na_server_set_transport(g, bare_transports[t]) != NA_OK) {
                fprintf(stderr, "FAIL: set_transport on the gate probe\n");
                na_server_destroy(g);
                return 1;
            }
            gerr[0] = '\0';
            if (na_server_start(g, gerr, (int)sizeof gerr) == NA_OK) {
                fprintf(stderr, "FAIL: bare transport %d started — the #92 gate is dead\n",
                        (int)bare_transports[t]);
                na_server_stop(g);
                na_server_destroy(g);
                return 1;
            }
            if (strstr(gerr, "NA_RELIABILITY_UDP_BARE") == NULL ||
                strstr(gerr, "na_server_set_reliability_profile") == NULL) {
                fprintf(stderr, "FAIL: gate refusal names neither remedy (%s)\n", gerr);
                na_server_destroy(g);
                return 1;
            }
            /* A failed start unfreezes the setters (documented recovery): the SAME handle, given
             * the explicit BARE request, must start — which is also the consent control proving
             * the gate passes an explicit bare server rather than refusing everything. */
            if (na_server_set_reliability_profile(g, NA_RELIABILITY_UDP_BARE) != NA_OK) {
                fprintf(stderr, "FAIL: setters still frozen after the gate refusal\n");
                na_server_destroy(g);
                return 1;
            }
            if (bare_transports[t] == NA_TRANSPORT_DUAL &&
                na_server_set_transport(g, NA_TRANSPORT_DUAL) != NA_OK) {
                fprintf(stderr, "FAIL: could not restore DUAL after the BARE profile\n");
                na_server_destroy(g);
                return 1;
            }
            if (na_server_start(g, gerr, (int)sizeof gerr) != NA_OK) {
                fprintf(stderr, "FAIL: explicit BARE start refused (%s)\n", gerr);
                na_server_destroy(g);
                return 1;
            }
            if (na_server_port(g) <= 0) {
                fprintf(stderr, "FAIL: BARE server reports no bound port\n");
                na_server_stop(g);
                na_server_destroy(g);
                return 1;
            }
            na_server_stop(g);
            na_server_destroy(g);
        }
        printf("c_server_smoke: bare UDP and DUAL starts refused with both remedies; explicit "
               "BARE starts (#92)\n");
    }

    printf("c_server_smoke OK (port=%d, client RX byte-identical, roster=1, TX-extract frames=%d "
           "@ %d bytes, clean disconnect+stop)\n", port, atomic_load(&g_tx_frames),
           atomic_load(&g_tx_bytes));
    return 0;
}
