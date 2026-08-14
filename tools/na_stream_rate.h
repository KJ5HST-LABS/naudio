/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * na_stream_rate — pick the sample rate na_hamlib_bridge opens its Hamlib streams at
 * (issue #84).
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * WHY THIS IS A SEPARATE HEADER, AND WHY IT TAKES PLAIN int ARRAYS. The policy below is
 * the whole of the decision #84 asked for, and it is the part that can be wrong in ways a
 * running bridge would not show: an off-by-one on a 0-terminated list, a tie broken
 * differently on two platforms, a direction silently dropped from the intersection. So it
 * is written against `const int *` + a bound rather than against `struct rig_stream_caps`,
 * which buys the one thing the bridge itself cannot have — it compiles and runs on the
 * WHOLE CI matrix, Windows included, with no libhamlib anywhere near it. na_hamlib_bridge
 * is opt-in, POSIX-only, and needs a hand-built streaming prefix; a defect reachable only
 * there is a defect nothing routinely runs. Same split, and the same reasoning, as
 * naudio_bridge_probe (CMakeLists.txt): prove the portable half everywhere, and let the
 * platform half be the thin part that is left.
 *
 * It is deliberately NOT in include/naudio/. That tree is installed and ships upstream;
 * this is a policy belonging to one tool, not to the naudio library or its ABI.
 *
 * THE POLICY. Given the hardware-native rate list of the RX stream, and of the TX stream
 * when TX is in use:
 *
 *   1. If the preferred rate (48 kHz — what naudio's presets are tuned at, and what this
 *      bridge has always asked for) is native on every direction in use, take it. This is
 *      the case that matters most, because it is the one that must NOT change: the Hamlib
 *      dummy backend advertises native {8000,16000,24000,48000,96000} on both audio types
 *      (measured at b538567b), so every existing bridge arm keeps the behaviour it had.
 *   2. Otherwise take the native rate CLOSEST to preferred, ties to the higher.
 *   3. With no native information at all — a libhamlib older than PR #2116 commit
 *      961093f2, where the native_* fields do not exist, or a caps block that leaves them
 *      zero — return preferred and change nothing.
 *
 * Closest-to-48k rather than highest-native is a bandwidth judgement, not an aesthetic
 * one: this bridge exists to cross a lossy WAN hop, and the wire cost scales linearly with
 * the rate while a rig's demodulated audio is bandlimited to a few kHz either way. Taking
 * the highest native rate would double or quadruple the bytes on the hop the tool is
 * supposed to protect, to carry content the radio never produced. Ties resolve upward
 * because that is the side that discards nothing.
 *
 * BOTH DIRECTIONS, NOT JUST RX — the constraint #84 does not state. naudio carries ONE
 * audio format for the whole server (na_server_set_audio_format is server-wide, and the
 * TX callback hands back audio in exactly that layout), so the rate chosen here is the
 * rate the operator's TX audio arrives in as well. Choosing it from the RX caps alone
 * would open the TX stream at a rate the TX side may not be native at, moving the
 * resample rather than removing it — and doing so invisibly, since nothing in the TX path
 * reports a conversion. When the two directions share no native rate at all, RX wins and
 * TX resamples: RX is the mandatory stream and runs continuously, TX is optional and only
 * carries audio while an operator is actually keyed.
 */
#ifndef NA_STREAM_RATE_H
#define NA_STREAM_RATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* What the bridge has always asked for, and still asks for whenever it can be had. */
#define NA_STREAM_RATE_PREFERRED 48000

/* Length of a 0-terminated rate list that is ALSO bounded by the array it lives in.
 *
 * Both bounds are load-bearing and neither is redundant: Hamlib's caps arrays are
 * 0-terminated by contract, but a backend that fills every slot leaves no terminator to
 * find, and the array bound is the only thing standing between this loop and a read off
 * the end. `cap` must be the bound of the array being passed — the rate lists and the
 * channel-count lists are sized independently upstream (32 vs 16). */
static inline int na_rate_list_len(const int *rates, int cap) {
    int n = 0;
    if (!rates) return 0;
    while (n < cap && rates[n] > 0) n++;
    return n;
}

/* Is `want` an entry of the 0-terminated, `cap`-bounded list? A non-positive `want` is
 * never present: 0 is the terminator and a negative rate is not a rate. */
static inline int na_rate_list_has(const int *rates, int cap, int want) {
    int n = na_rate_list_len(rates, cap);
    if (want <= 0) return 0;
    for (int i = 0; i < n; i++)
        if (rates[i] == want) return 1;
    return 0;
}

/* Choose the rate to open both Hamlib streams at and to run the naudio server at.
 *
 *   rx_rates/rx_cap  the RX stream's hardware-native rate list, and its array bound
 *   tx_rates/tx_cap  the same for TX, or NULL when TX is not in use (-x, or the TX
 *                    stream failed to open and the bridge continued RX-only)
 *   preferred        the rate to keep if it can be kept — NA_STREAM_RATE_PREFERRED
 *
 * Returns the chosen rate; returns `preferred` unchanged whenever there is nothing to
 * choose from, so a caller with no native information behaves exactly as it did before
 * this function existed. Returns 0 only if `preferred` is itself not a usable rate. */
static inline int na_choose_stream_rate(const int *rx_rates, int rx_cap,
                                        const int *tx_rates, int tx_cap,
                                        int preferred) {
    if (preferred <= 0) return 0;

    const int rx_n = na_rate_list_len(rx_rates, rx_cap);
    if (rx_n == 0) return preferred;    /* no native view of the mandatory stream */

    /* Two passes at most. The first honours TX; the second runs only when TX shares no
     * native rate with RX at all, and drops the TX constraint rather than the choice. */
    const int use_tx = (na_rate_list_len(tx_rates, tx_cap) > 0);
    for (int pass = 0; pass < 2; pass++) {
        const int with_tx = (pass == 0) && use_tx;

        if ((!with_tx || na_rate_list_has(tx_rates, tx_cap, preferred))
                && na_rate_list_has(rx_rates, rx_cap, preferred))
            return preferred;

        int best = 0, best_delta = 0;
        for (int i = 0; i < rx_n; i++) {
            const int r = rx_rates[i];
            if (with_tx && !na_rate_list_has(tx_rates, tx_cap, r)) continue;
            const int delta = (r > preferred) ? (r - preferred) : (preferred - r);
            /* The `delta == best_delta && r > best` clause is what makes an exact tie
             * resolve to the HIGHER rate, and it is written as an explicit comparison
             * rather than left to the order the list arrives in. Hamlib documents the
             * CHANNEL-count list as ascending and says no such thing about the rate
             * lists, so a policy that leaned on iteration order would be relying on
             * something the caps contract does not promise. */
            if (best == 0 || delta < best_delta || (delta == best_delta && r > best)) {
                best = r;
                best_delta = delta;
            }
        }
        if (best > 0) return best;
        if (!with_tx) break;            /* pass 1 was already unconstrained — nothing left */
    }
    return preferred;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NA_STREAM_RATE_H */
