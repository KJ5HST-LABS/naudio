#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio tests — decide whether a bridge health log shows SUSTAINED counter drift.
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# ISSUE #12, acceptance item 3: "a multi-hour run shows no unbounded drift in overruns/underruns."
# That is the one acceptance item in #12 that cannot be settled by looking at it. The other three
# are a human listening (RX intelligible), a human watching the rig key (TX + -k), and a line the
# bridge already prints at startup (the negotiated format). This one is a claim about a TREND across
# hours of a log nobody will read line by line, and "I scrolled it and it looked fine" is not a
# measurement.
#
# WHY A TREND AND NOT A THRESHOLD. Every counter on the health line is CUMULATIVE since start
# (tools/na_hamlib_bridge.c, the tick % 25 block). So a total is not a verdict: a rig that hiccups
# four times in the first ten seconds while PortAudio settles and then runs clean for six hours ends
# with overruns=4, and a rig whose sample clock is genuinely 40 ppm off ALSO ends with a small
# number after an hour — but it never stops climbing. Those are opposite outcomes with similar
# totals, and only the SHAPE separates them. Hence:
#
#   sustained drift  ==  the counter is STILL climbing in the final quarter of the run
#
# A startup burst flattens and passes. A persistent producer/consumer rate mismatch does not, and
# cannot, because the mismatch is what the counter is counting. There is NO TUNED CONSTANT in that
# test — the discriminator is zero-versus-nonzero growth in the tail, not a rate someone has to
# keep re-tuning per rig. That is the same discipline tests/bridge/netrigctl_arm.sh applies to the
# gap signature, and for the same reason: a tuned bound on a hardware path is a bound that will be
# wrong on the next radio.
#
# WHAT IS ASSERTED, AND WHAT IS ONLY REPORTED
#
# ASSERTED: overruns, underruns (RX side) and rig_overruns, rig_underruns (TX side). These are
# #12's named counters and they are the ones a clock mismatch moves.
#
# REPORTED, NEVER ASSERTED: gaps, link_loss, reads, and the TX ring's short/lost_queue/lost_requeue.
# They are on the health line and they matter, but they are not this script's question. `gaps`
# already has a detector with a sharper test (the bridge's own alarm, and netrigctl_arm.sh's
# complement of it); `link_loss` is a property of the network hop, which on-air is a variable being
# introduced deliberately; and the TX ring counters move when the OPERATOR produces faster than the
# radio drains, which is an input to the test rather than a fault in it. Printing them next to the
# verdict lets a reader apply their own test — the reason the bridge prints `reads` beside `gaps`.
#
# FAIL-CLOSED ON TOO LITTLE DATA. A log with fewer than --min-ticks health ticks exits 2 (harness
# fault), NOT 0. A quartile of three samples is not a trend, and the failure mode this guards is the
# obvious one: a soak that died after ninety seconds must not report "no drift detected" — that is
# an absence that was never a measurement (CLAUDE.md L222). Same reason the empty-input case is a
# fault and not a pass.
#
# ELAPSED TIME. Health lines carry no timestamp, so per-hour figures need one of two sources:
#   1. Preferred — the capture prefixes each line with an epoch second, which is what the runbook in
#      docs/hamlib-streaming-bridge.md does. Rates are then real wall clock, and a STALLED bridge
#      (ticks stop arriving) is visible as a widening gap.
#   2. Fallback — no timestamps, so elapsed is inferred as tick_index * --nominal-tick-s (5 s, the
#      bridge's 25 x 200 ms cadence). This is LABELLED "inferred" in the output, because it assumes
#      the very liveness a soak is meant to test: if the loop stalled, inferred time keeps counting
#      and a stall reads as healthy. Prefer route 1 on the day.
#
# The verdict itself (climbing in the tail, or not) does not depend on either — it is computed from
# sample ORDER, so it survives a missing or wrong clock. Only the per-hour figures need the clock.
#
# USAGE
#   health_drift.sh [--min-ticks N] [--nominal-tick-s S] LOGFILE
#   health_drift.sh --selftest
#
# EXIT: 0 = no sustained drift    1 = sustained drift detected    2 = harness fault
#
# --selftest proves the detector can SPEAK before any radio is attached, and is registered as
# naudio_health_drift so it runs on every build. It is the positive control: a detector for a fault
# that has never been shown to fire is indistinguishable from a detector that is broken, and this
# one will spend most of its life reporting "no drift". It drives three synthetic logs through the
# real code path -- flat (must pass), sustained (must be caught, and must NAME the right counter),
# truncated (must be a fault, not a pass) -- so all three verdicts are exercised, not just the
# happy one.
set -uo pipefail

MIN_TICKS=8
NOMINAL_TICK_S=5
LOG=""

die_fault() { printf 'health_drift: %s\n' "$1" >&2; exit 2; }

while [ $# -gt 0 ]; do
    case "$1" in
        --selftest)        SELFTEST=1; shift ;;
        --min-ticks)       MIN_TICKS="${2:-}"; shift 2 ;;
        --nominal-tick-s)  NOMINAL_TICK_S="${2:-}"; shift 2 ;;
        -h|--help)         sed -n '/^# USAGE/,/^# EXIT/p' "$0"; exit 0 ;;
        -*)                die_fault "unknown option: $1" ;;
        *)                 LOG="$1"; shift ;;
    esac
done

# ---------------------------------------------------------------------------
# analyse LOGFILE -> prints the report, returns 0 clean / 1 drift / 2 fault
# ---------------------------------------------------------------------------
analyse() {
    local log="$1"
    [ -n "$log" ]  || die_fault "no logfile given (try --help)"
    [ -r "$log" ]  || die_fault "cannot read $log"

    awk -v min_ticks="$MIN_TICKS" -v nominal="$NOMINAL_TICK_S" '
    function rate_tail(name, arr, n,   q, span) {
        # Growth across the final quarter of the samples. Index 1..n.
        q = int(n * 3 / 4); if (q < 1) q = 1
        return arr[n] - arr[q]
    }
    {
        line = $0
        # Optional leading epoch second from the runbook capture.
        ts = ""
        if (match(line, /^[0-9]{9,}[ \t]/)) { ts = substr(line, 1, RLENGTH - 1) + 0; line = substr(line, RLENGTH + 1) }
    }
    /rx: clients=/ {
        n_rx++
        if (ts != "") { if (!t0_seen) { t0 = ts; t0_seen = 1 } ; t_last = ts; have_clock = 1 }
        # Field-name driven, not positional: the health line has grown fields before (link_loss was
        # added later) and a positional parse would silently read the wrong column after the next one.
        for (i = 1; i <= NF; i++) {
            if (split($i, kv, "=") == 2) {
                k = kv[1]; sub(/^.*:/, "", k)          # strip the "rx:" prefix on the first field
                v = kv[2] + 0
                if (k == "gaps")       gaps[n_rx]      = v
                if (k == "reads")      reads[n_rx]     = v
                if (k == "link_loss")  link[n_rx]      = v
                if (k == "overruns")   over[n_rx]      = v
                if (k == "underruns")  under[n_rx]     = v
            }
        }
        next
    }
    /tx: short=/ {
        n_tx++
        for (i = 1; i <= NF; i++) {
            if (split($i, kv, "=") == 2) {
                k = kv[1]; sub(/^.*:/, "", k)
                v = kv[2] + 0
                if (k == "short")          tshort[n_tx]  = v
                if (k == "rig_overruns")   tover[n_tx]   = v
                if (k == "rig_underruns")  tunder[n_tx]  = v
            }
        }
        next
    }
    END {
        if (n_rx < min_ticks) {
            printf "health_drift: FAULT — %d RX health tick(s), need at least %d for a tail quartile\n", n_rx, min_ticks > "/dev/stderr"
            printf "  a soak that ended early must not read as \"no drift\" — this is fail-closed by design\n" > "/dev/stderr"
            exit 2
        }

        if (have_clock) { elapsed = t_last - t0; src = "measured" }
        else            { elapsed = (n_rx - 1) * nominal; src = "INFERRED from " nominal "s cadence — a stalled bridge is invisible to it" }
        hours = (elapsed > 0) ? elapsed / 3600.0 : 0

        printf "health_drift: %d RX ticks, %d TX ticks, elapsed %ds (%s)\n", n_rx, n_tx, elapsed, src
        printf "\n  %-16s %10s %10s %12s   %s\n", "counter", "first", "last", "tail-growth", "verdict"
        printf "  %-16s %10s %10s %12s   %s\n", "----------------", "-------", "-------", "-----------", "-------"

        # ---- asserted ----
        drift = 0
        n = split("overruns underruns rig_overruns rig_underruns", names, " ")
        for (j = 1; j <= n; j++) {
            nm = names[j]
            if (nm == "overruns")           { cnt = n_rx; f = over[1];   l = over[cnt];   g = rate_tail(nm, over, cnt) }
            else if (nm == "underruns")     { cnt = n_rx; f = under[1];  l = under[cnt];  g = rate_tail(nm, under, cnt) }
            else if (nm == "rig_overruns")  { cnt = n_tx; if (cnt == 0) continue; f = tover[1];  l = tover[cnt];  g = rate_tail(nm, tover, cnt) }
            else                            { cnt = n_tx; if (cnt == 0) continue; f = tunder[1]; l = tunder[cnt]; g = rate_tail(nm, tunder, cnt) }

            if (g > 0) { v = "SUSTAINED — still climbing in the final quarter"; drift = 1 }
            else if (l > f) { v = "settled (grew early, flat in the tail)" }
            else { v = "clean" }
            printf "  %-16s %10d %10d %12d   %s\n", nm, f, l, g, v
            if (hours > 0 && l > f)
                printf "  %-16s %s%.1f/hour over the whole run\n", "", "        ~", (l - f) / hours
        }

        # ---- reported only ----
        printf "\n  reported, not asserted (see the header for why each one is out of scope):\n"
        printf "    gaps        %d -> %d      reads %d -> %d      link_loss %d -> %d\n",
               gaps[1], gaps[n_rx], reads[1], reads[n_rx], link[1], link[n_rx]
        if (n_tx > 0)
            printf "    tx short    %d -> %d\n", tshort[1], tshort[n_tx]

        if (drift) {
            printf "\nhealth_drift: SUSTAINED DRIFT — at least one asserted counter is still climbing at the end.\n"
            printf "  This is the #12 item-3 failure: not a transient, a rate mismatch that does not resolve.\n"
            exit 1
        }
        printf "\nhealth_drift: no sustained drift — every asserted counter is flat in the final quarter.\n"
        exit 0
    }' "$log"
}

# ---------------------------------------------------------------------------
# --selftest — the positive control (see the header)
# ---------------------------------------------------------------------------
selftest() {
    local work rc fails=0
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' RETURN

    # (1) FLAT: a startup burst of 3 overruns in the first two ticks, then nothing. Must PASS.
    {
        echo "na_hamlib_bridge: model=1 (local) -> naudio :4533  profile=wan  rate=48000  channels=1  tx=on"
        for i in $(seq 1 12); do
            o=3; [ "$i" -eq 1 ] && o=1; [ "$i" -eq 2 ] && o=3
            echo "  rx: clients=1 gaps=0 reads=$((i * 429)) link_loss=0 overruns=$o underruns=0"
            echo "  tx: short=0 lost_queue=0B lost_requeue=0B rig_overruns=0 rig_underruns=0"
        done
    } > "$work/flat.log"

    # (2) SUSTAINED: underruns climb by 2 every tick, start to finish — a clock mismatch. Must be CAUGHT,
    #     and must name `underruns` rather than merely failing.
    {
        for i in $(seq 1 12); do
            echo "  rx: clients=1 gaps=0 reads=$((i * 429)) link_loss=0 overruns=0 underruns=$((i * 2))"
            echo "  tx: short=0 lost_queue=0B lost_requeue=0B rig_overruns=0 rig_underruns=0"
        done
    } > "$work/sustained.log"

    # (3) TIMESTAMPED + SUSTAINED: the same drift, captured the way the runbook actually captures it
    #     (epoch-prefixed). This is the path the on-air soak will use, and it was NOT covered by (1)
    #     and (2) — both of which exercise the inferred-clock fallback. A parser that only handles
    #     the fallback would pass this selftest and then mis-read every real capture, so the case
    #     that matters most on the day is the one that has to be gated here. Ticks are 300 s apart,
    #     so the per-hour figure is arithmetic a reader can check by hand: 22 underruns / 55 min.
    {
        t=1755400000
        for i in $(seq 1 12); do
            echo "$t   rx: clients=1 gaps=0 reads=$((i * 429)) link_loss=0 overruns=0 underruns=$((i * 2))"
            echo "$t   tx: short=0 lost_queue=0B lost_requeue=0B rig_overruns=0 rig_underruns=0"
            t=$((t + 300))
        done
    } > "$work/ts.log"

    # (4) TRUNCATED: a soak that died after 3 ticks. Must be a FAULT (2), never a pass.
    {
        for i in 1 2 3; do
            echo "  rx: clients=1 gaps=0 reads=$((i * 429)) link_loss=0 overruns=0 underruns=0"
        done
    } > "$work/short.log"

    echo "== health_drift --selftest =="

    analyse "$work/flat.log" > "$work/flat.out" 2>&1; rc=$?
    if [ "$rc" -eq 0 ]; then echo "  [ok]   flat log      -> 0 (no drift)"
    else echo "  [FAIL] flat log      -> $rc, want 0"; cat "$work/flat.out"; fails=$((fails + 1)); fi

    analyse "$work/sustained.log" > "$work/sust.out" 2>&1; rc=$?
    if [ "$rc" -eq 1 ]; then echo "  [ok]   sustained log -> 1 (drift detected)"
    else echo "  [FAIL] sustained log -> $rc, want 1"; cat "$work/sust.out"; fails=$((fails + 1)); fi
    # Detecting is not enough: it has to point at the counter that moved, or an on-air operator
    # gets "something drifted" and no lead. underruns moved; overruns did not.
    if grep -q '^  underruns .*SUSTAINED' "$work/sust.out"; then
        echo "  [ok]   ...and names underruns as the counter that drifted"
    else
        echo "  [FAIL] drift detected but underruns not flagged"; cat "$work/sust.out"; fails=$((fails + 1))
    fi
    if grep -q '^  overruns .*SUSTAINED' "$work/sust.out"; then
        echo "  [FAIL] overruns flagged as drifting — it never moved; the detector is indiscriminate"
        fails=$((fails + 1))
    else
        echo "  [ok]   ...and does NOT flag overruns, which never moved (negative control)"
    fi

    analyse "$work/ts.log" > "$work/ts.out" 2>&1; rc=$?
    if [ "$rc" -eq 1 ]; then echo "  [ok]   timestamped   -> 1 (drift detected through the runbook's capture format)"
    else echo "  [FAIL] timestamped   -> $rc, want 1"; cat "$work/ts.out"; fails=$((fails + 1)); fi
    # The clock must be READ, not inferred — otherwise the per-hour figures on the day are fiction
    # dressed as measurement, and the "(measured)" label is the only thing that says which it was.
    if grep -q 'elapsed 3300s (measured)' "$work/ts.out"; then
        echo "  [ok]   ...and reads elapsed from the timestamps (3300s measured, not inferred)"
    else
        echo "  [FAIL] timestamps present but elapsed not taken from them"; cat "$work/ts.out"; fails=$((fails + 1))
    fi

    analyse "$work/short.log" > "$work/short.out" 2>&1; rc=$?
    if [ "$rc" -eq 2 ]; then echo "  [ok]   truncated log -> 2 (fault, not a pass)"
    else echo "  [FAIL] truncated log -> $rc, want 2"; cat "$work/short.out"; fails=$((fails + 1)); fi

    if [ "$fails" -ne 0 ]; then echo "== health_drift --selftest: $fails FAILED =="; return 1; fi
    echo "== health_drift --selftest: all cases passed =="
    return 0
}

if [ "${SELFTEST:-0}" = "1" ]; then
    selftest
    exit $?
fi

analyse "$LOG"
