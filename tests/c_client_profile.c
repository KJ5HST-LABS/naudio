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
 * is the only way to turn that layer on, and this file asserts it does. It also asserts the
 * reverse, which is a separate claim: NA_RELIABILITY_DEFAULT turns that layer back OFF (6), not
 * merely the transport back to TCP (5).
 *
 * WHAT IT DOES NOT COVER — read before adding a claim here. Every path below is loss-free
 * loopback, so it CANNOT distinguish FEC-on from FEC-off: with nothing dropped, the recovering and
 * the non-recovering configuration deliver byte-identical output. That distinction is measurable
 * only by inducing loss, which needs a proxy this suite has no place for. What the loopback CAN
 * prove, and does, is that the profile writes the transport (a client configured with the profile
 * ALONE reaches a UDP-only server, which a client left on the TCP default cannot) and that a client
 * running the whole reliability pipeline still delivers real audio end to end.
 *
 * SO SECTION (6) IS NOT A WHOLE-LAYER CLAIM, and must not be read as one. Of the four knobs
 * NA_RELIABILITY_DEFAULT resets, exactly two have an observable on this side of the C ABI, and
 * that is measured rather than assumed — the same two clients, one per configuration:
 *
 *   reorder buffer  — OBSERVED. sequence_gaps 0 with UDP_WAN against -1 after the reset, and the
 *                     three pre-reorder loss counters flip the opposite way (unmeasured against
 *                     0). Either side of that complement detects the knob.
 *   adaptive jitter — OBSERVED. buffer_target_ms 60 with UDP_WAN against -1 after the reset; only
 *                     an estimator that exists publishes a target.
 *   FEC             — NO OBSERVABLE HERE. packets_recovered_by_fec and fec_blocks_unreconciled
 *                     both read 0 in BOTH configurations, because on a loss-free path there is
 *                     nothing to recover and no block to decline. A decoder that exists and a
 *                     decoder that does not are indistinguishable, so asserting on them here
 *                     would be decoration.
 *   control-ARQ     — NO OBSERVABLE ON A CLIENT AT ALL, by design rather than by fixture:
 *                     control_retransmits is documented ALWAYS 0 on a client (naudio.h), since
 *                     DISCONNECT is its only critical control message and it is sent after the
 *                     thread that pumps the retransmit sweep has exited. Measured 0 in both
 *                     configurations here, exactly as that says.
 *
 * A preset wrongly left in the DEFAULT case is therefore caught by this file iff it differs in
 * reordering or adaptive jitter. One differing only in FEC or control-ARQ is NOT caught.
 *
 * SECTION (7) APPLIES THE SAME TWO-KNOB INSTRUMENT to a different question (issue #11): not
 * "did DEFAULT turn the layer off" but "did UDP_IQ apply its own preset rather than a neighbouring
 * one". Same reach, so the same limits: it separates udpIq() from udpWan() and from any TCP-shaped
 * config, and CANNOT separate it from udpLan() or udpFt8(). Section (5) gains the client-side half of
 * NA_RELIABILITY_DUAL over the same period — that value aliases to TCP here, which the existing
 * refusal probe pins directly.
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

/* RX is re-framed into bytesPerFrame() chunks on the way out, so what the callback sees is
 * frame-aligned rather than inject-aligned. Assert on a repeating signature — two consecutive
 * periods — which survives any framing and which silence cannot produce. A byte count would not:
 * zeros are well-formed audio arriving at exactly the right rate. */
#define SIG_PERIOD 4
static const unsigned char SIG_UNIT[SIG_PERIOD] = {0x5A, 0xA5, 0x3C, 0xC3};
static unsigned char RXBUF[8192];

static NA_TEST_ATOMIC int g_rx_ok = 0;

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
            na_client_set_reliability_profile(probe, NA_RELIABILITY_UDP_FT8) != NA_OK ||
            na_client_set_reliability_profile(probe, NA_RELIABILITY_UDP_IQ)  != NA_OK ||
            na_client_set_reliability_profile(probe, NA_RELIABILITY_DUAL)    != NA_OK) {
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

    /* The ON side of the pair that section (6) asserts the OFF side of, and it is not optional.
     * A negative claim — "the reliability layer is off" — is worth exactly as much as the proof
     * that its instrument can see the layer when it IS on. So the same three fields are read
     * here, on a client running the whole pipeline, and must read the opposite way. Without this
     * arm, a field that returned "off" unconditionally would satisfy (6) perfectly. */
    {
        na_client_stats on;
        if (na_client_get_stats(c, &on, sizeof on) != NA_OK) {
            return fail("na_client_get_stats on the profile-only client", c, srv);
        }
        /* A reorder buffer IS engaged here, so the post-reorder gap counter carries a reading and
         * the three pre-reorder counters are unmeasured. naudio.h specifies the two as exact
         * complements — never both, never neither — so this reads both sides of that. */
        if (on.sequence_gaps < 0) {
            return fail("sequence_gaps unmeasured on UDP_WAN, which engages a reorder buffer",
                        c, srv);
        }
        if (on.packets_lost >= 0 || on.packets_out_of_order >= 0 || on.packet_loss_rate >= 0.0) {
            return fail("a pre-reorder loss counter is measured alongside sequence_gaps", c, srv);
        }
        /* A second, independent knob: adaptive jitter builds its own estimator, and only an
         * existing estimator publishes a target. */
        if (on.buffer_target_ms < 0) {
            return fail("buffer_target_ms unmeasured on UDP_WAN, which enables adaptive jitter",
                        c, srv);
        }
        printf("c_client_profile: UDP_WAN pipeline live — sequence_gaps %lld, loss counters "
               "unmeasured, buffer_target_ms %d\n", on.sequence_gaps, on.buffer_target_ms);
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
    /* NA_RELIABILITY_DUAL aliases to TCP on the client (issue #11; the promise is stated on
     * na_client_set_reliability_profile and on na_transport), so it cannot reach a UDP-only server
     * either. READ THE NEXT SENTENCE BEFORE REUSING THIS TECHNIQUE: section (6) below records that
     * the refusal probe is the WRONG instrument for a reliability claim, precisely because a preset
     * with the transport wrong and every knob on is refused exactly like a correct reset. That
     * warning does not apply here — the claim being pinned IS a transport claim, and nothing else.
     * The DUAL preset's reliability half is inert on this side by the same aliasing, so there is no
     * second half for this arm to under-report. What it catches is DUAL wired to a UDP preset, or
     * to nothing; what it cannot catch is DUAL wired to plain TCP defaults, since both refuse. */
    if (!assert_cannot_reach_udp_server(port, "UDP_WAN-then-DUAL",
                                        NA_TRANSPORT_TCP, 0, NA_RELIABILITY_DUAL)) {
        return fail("NA_RELIABILITY_DUAL did not alias to TCP on the client", NULL, srv);
    }

    /* ---- (6) DEFAULT resets the RELIABILITY LAYER too, not merely the transport ---- */

    /* NA_RELIABILITY_DEFAULT's contract (naudio.h, na_reliability_profile) is TWO assertions —
     * "plain TCP" AND "with the whole reliability layer off". Everything above pins only the
     * first, and the refusal probe (5) uses CANNOT be extended to the second: NA_TRANSPORT_DUAL
     * is aliased to TCP on the client side, so a preset that leaves the transport wrong AND every
     * reliability knob on is refused by this UDP-only server in exactly the way a correct reset
     * is, and (5) still passes. Measured: replacing the DEFAULT case with dualDefault() left the
     * whole suite green before this section existed.
     *
     * The technique that does work is to stop varying the transport at all. set_transport(UDP)
     * runs AFTER the profile and overwrites whichever transport it chose — last-writer-wins,
     * which (5) itself pins — so this client reaches the server whatever DEFAULT did to the
     * transport, and the reliability half is then the only thing left varying. The counters read
     * it directly.
     *
     * This is also the only public path that builds a UDP connection with NO reorder buffer: all
     * four NA_RELIABILITY_UDP_* profiles configure one (UDP_IQ included — see section (7), which
     * depends on that), so a C consumer reaches the reorder-free branch only by resetting with
     * DEFAULT and re-selecting UDP. */

    g_rx_ok = 0;  /* section (3) left it set; this arm needs a reading of its own */
    na_stream_client *reset =
        na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", port, "reset-cli");
    if (reset == NULL) return fail("na_client_create (reset)", NULL, srv);
    na_client_set_playback_device(reset, 0);
    na_client_set_audio_cb(reset, on_rx_audio, NULL);
    na_client_set_auto_reconnect(reset, 0);
    if (na_client_set_reliability_profile(reset, NA_RELIABILITY_UDP_WAN) != NA_OK ||
        na_client_set_reliability_profile(reset, NA_RELIABILITY_DEFAULT) != NA_OK ||
        na_client_set_transport(reset, NA_TRANSPORT_UDP) != NA_OK) {
        return fail("UDP_WAN -> DEFAULT -> set_transport(UDP) rejected", reset, srv);
    }
    if (na_client_connect(reset, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "  (%s)\n", err);
        return fail("the reset client could not reach the UDP server", reset, srv);
    }

    /* Gate on RX ALONE, never on the roster: the server has not yet reaped the client section (4)
     * disconnected, so na_client_server_client_count reads 2 here and a `== 1` gate burns the
     * entire budget waiting for a number that never arrives. Measured: 20 ms with this gate
     * against the full 3000 ms with the roster one. */
    int rwaited = 0;
    while (rwaited < 3000 && !g_rx_ok) {
        na_server_inject_audio(srv, RXBUF, (int)sizeof RXBUF);
        sleep_ms(20);
        rwaited += 20;
    }
    if (!g_rx_ok) return fail("no RX reached the reset client within budget", reset, srv);

    {
        na_client_stats off;
        if (na_client_get_stats(reset, &off, sizeof off) != NA_OK) {
            return fail("na_client_get_stats on the reset client", reset, srv);
        }
        /* The exact mirror of section (3): with no reorder buffer it is the post-reorder counter
         * that reads -1 and the three pre-reorder ones that carry the reading. */
        if (off.sequence_gaps != -1) {
            fprintf(stderr, "  (sequence_gaps %lld — a reorder buffer survived the reset)\n",
                    off.sequence_gaps);
            return fail("DEFAULT left the reorder buffer engaged on a UDP client", reset, srv);
        }
        if (off.packets_lost < 0 || off.packets_out_of_order < 0 || off.packet_loss_rate < 0.0) {
            return fail("the gap tracker is not running, so no reorder-free path was taken",
                        reset, srv);
        }
        /* A separate knob builds a separate estimator, so this is a SECOND detector rather than a
         * restatement of the first. It is MASKED by the reorder assertions above, which are
         * evaluated first and which most wrong presets also trip — dualDefault() dies there and
         * never reaches this line. Isolating it needs the reorder pair neutralised and a preset
         * that enables adaptive jitter ALONE; done that way it fails here with
         * buffer_target_ms 40, and the same neutralisation with no mutation stays green. Keep
         * that in mind before trusting a red run to mean this assertion is live. */
        if (off.buffer_target_ms != -1) {
            fprintf(stderr, "  (buffer_target_ms %d — an adaptive-jitter estimator survived)\n",
                    off.buffer_target_ms);
            return fail("DEFAULT left adaptive jitter enabled on a UDP client", reset, srv);
        }
        if (off.jitter_ms != 0.0) {
            return fail("DEFAULT left a live jitter estimate on a UDP client", reset, srv);
        }
        printf("c_client_profile: UDP_WAN -> DEFAULT -> UDP — reliability layer off "
               "(sequence_gaps -1, loss counters measured, buffer_target_ms -1) (%d ms)\n",
               rwaited);
    }
    na_client_disconnect(reset);
    na_client_destroy(reset);

    /* ---- (7) NA_RELIABILITY_UDP_IQ applies its PRESET, not merely its transport (issue #11) ----
     *
     * Section (5)'s refusal probe and c_server_smoke.c's arm F both pin the transport a profile
     * selects, and neither can say more: all four NA_RELIABILITY_UDP_* values select UDP, so a
     * UDP_IQ case mis-wired to udpLan() or udpWan() satisfies every transport assertion in the
     * suite. This arm is the one that separates them, and it uses the only two knobs of the four
     * that have a client-side observable — the same two section (6) uses, for the same reason.
     *
     * THE FINGERPRINT IS THE PAIR, not either half:
     *
     *   sequence_gaps >= 0   a reorder buffer exists (udpIq sets reorderBufferSize = 8). Rules out
     *                        a case wired to a TCP preset or to a default-constructed config, both
     *                        of which build none — section (6) measures that as -1.
     *   buffer_target_ms -1  NO adaptive-jitter estimator (udpIq sets adaptiveJitterEnabled=false).
     *                        Rules out udpWan(), the one preset that turns it on. MEASURED both
     *                        ways this session against this same UDP_WAN server: a UDP_WAN client
     *                        reads buffer_target_ms 61, a UDP_IQ client reads -1.
     *
     * Neither half alone discriminates — udpLan() and udpFt8() would pass both — so this arm
     * deliberately does NOT claim to identify udpIq() uniquely. WHAT IT CANNOT SEE, stated so that
     * nobody reads more into a green run than is there:
     *
     *   - UDP_IQ vs UDP_LAN vs UDP_FT8 is INVISIBLE here. All three set reorderBufferSize = 8 and
     *     leave adaptiveJitterEnabled false, so all three produce this arm's exact pair; udpWan is
     *     the only one of the four that differs. MEASURED for UDP_FT8 (a UDP_FT8 client reads
     *     sequence_gaps 0, buffer_target_ms -1 — identical); derived from the preset bodies for
     *     UDP_LAN. The presets differ in buffer targets and
     *     hold times, and the server overwrites the buffer values at handshake anyway
     *     (ControlMessage::applyAudioConfigTo, src/ControlMessage.cpp:314) — so those fields say
     *     nothing about which preset the CLIENT chose. adaptiveJitterEnabled and reorderBufferSize
     *     are what survive that overwrite, which is exactly why the fingerprint is built on them.
     *   - IQ'S DEFINING 192 kHz IS PINNED NOWHERE, on either side of the ABI. There is no public
     *     accessor for the negotiated rate; issue #36 declined adding one, on the grounds that a
     *     new na_* symbol on a Hamlib-bound ABI is too high a price for a test detector. Two
     *     candidate observables were re-measured this session and BOTH are dead: total byte volume
     *     (already recorded dead at c_server_smoke.c's section-6 comment) and the per-callback RX
     *     chunk size, which is MTU-derived on UDP (1376 bytes at 48 kHz and at 192 kHz alike) and
     *     inject-derived on TCP. Do not add a rate assertion here without first re-measuring that
     *     it can fail; as of this session none can.
     *
     * The client is configured with the PROFILE ALONE — no na_client_set_transport — so reaching
     * this UDP-only server is itself the transport half of the claim, exactly as in section (2). */

    g_rx_ok = 0;
    na_stream_client *iq = na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", port, "iq-cli");
    if (iq == NULL) return fail("na_client_create (iq)", NULL, srv);
    na_client_set_playback_device(iq, 0);
    na_client_set_audio_cb(iq, on_rx_audio, NULL);
    na_client_set_auto_reconnect(iq, 0);
    if (na_client_set_reliability_profile(iq, NA_RELIABILITY_UDP_IQ) != NA_OK) {
        return fail("na_client_set_reliability_profile(UDP_IQ) rejected", iq, srv);
    }
    if (na_client_connect(iq, err, (int)sizeof err) != NA_OK) {
        fprintf(stderr, "  (%s)\n", err);
        return fail("UDP_IQ alone did not select UDP — connect refused", iq, srv);
    }

    /* Gate on RX alone, for section (6)'s reason: earlier clients may not be reaped yet. Note the
     * server here is UDP_WAN and this client is UDP_IQ, so the server sends parity packets this
     * client discards (naudio.h states that mismatch cost on na_reliability_profile). Loopback is
     * loss-free, so nothing needs recovering and audio flows regardless — which is the point being
     * made: a profile mismatch degrades recovery silently rather than breaking the stream. */
    int iqwaited = 0;
    while (iqwaited < 3000 && !g_rx_ok) {
        na_server_inject_audio(srv, RXBUF, (int)sizeof RXBUF);
        sleep_ms(20);
        iqwaited += 20;
    }
    if (!g_rx_ok) return fail("no RX reached the UDP_IQ client within budget", iq, srv);

    {
        na_client_stats iqs;
        if (na_client_get_stats(iq, &iqs, sizeof iqs) != NA_OK) {
            return fail("na_client_get_stats on the UDP_IQ client", iq, srv);
        }
        if (iqs.sequence_gaps < 0) {
            fprintf(stderr, "  (sequence_gaps %lld — no reorder buffer)\n", iqs.sequence_gaps);
            return fail("UDP_IQ built no reorder buffer — the preset was not applied", iq, srv);
        }
        if (iqs.buffer_target_ms != -1) {
            fprintf(stderr, "  (buffer_target_ms %d — an adaptive-jitter estimator exists)\n",
                    iqs.buffer_target_ms);
            return fail("UDP_IQ enabled adaptive jitter — that is udpWan's preset, not udpIq's",
                        iq, srv);
        }
        printf("c_client_profile: UDP_IQ preset applied — reorder on (sequence_gaps %lld), "
               "adaptive jitter off (buffer_target_ms -1) (%d ms)\n", iqs.sequence_gaps, iqwaited);
    }
    na_client_disconnect(iq);
    na_client_destroy(iq);

    na_server_stop(srv);
    na_server_destroy(srv);
    printf("c_client_profile: PASS\n");
    return 0;
}
