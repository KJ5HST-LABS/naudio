#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio tests — drive na_hamlib_bridge over netrigctl (-m 2) and assert the gap-per-read signature.
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# ISSUE #88 ITEM 8. Every other bridge arm runs `-m 1` — the dummy backend, in-process. Nothing,
# local or in CI, has ever opened a stream over netrigctl, so the entire `\stream_open` round trip
# (the one part of this bridge that talks to another process over a wire) was exercised only by hand.
#
# THAT IS NOT HYPOTHETICAL, AND THE COST IS ON THE RECORD. Issue #21 sat open for a month after its
# cause was gone: Hamlib `961093f2` fixed it on 2026-08-11 as a ride-along inside a commit about
# sample-format conversion, and naudio's CI stayed green before and after because nothing drove the
# path in either direction. A session found it by re-running the reproduction by hand against two
# prefixes. `docs/hamlib-streaming-bridge.md` § "Version floor" states the resulting claim — `-c 2`
# over `-m 2` holds 0 gaps against libhamlib >= 961093f2 — and until this arm existed that claim had
# no backstop at all: an upstream revert would have falsified the doc, and the bridge's compile-time
# advice with it, in silence.
#
# TWO ARMS, AND RUNNING BOTH IS THE POINT:
#   -c 1  the control — documented to hold 0 gaps on EVERY libhamlib that opens the stream at all
#   -c 2  the arm with load — the one that goes wrong below the version floor
# A regression that broke netrigctl outright would redden both. The pre-961093f2 fault reddens ONLY
# -c 2. Keeping the control means the arm reports WHICH of those two happened instead of just "red",
# and it is what stops "this libhamlib is too old for anything" from being read as the channel bug.
#
# WHAT IS ASSERTED, AND WHAT IS ONLY REPORTED
#
# The discriminator is `gaps` per RX read, never a rate. That is #21's own finding and it is load-
# bearing: 83% of nominal is what a CORRECT run looks like against a `nanosleep`-paced dummy, a
# `loopback` dummy with no TX peer sits near 70% while being perfectly correct, and a threshold tight
# enough to catch the half-rate signature false-fires on the healthy `-c 1` control. So the byte-rate
# line is echoed into the log for a human and asserted on by nothing.
#
# The test applied is `gaps * 2 <= reads` — the exact complement of the bridge's own alarm condition
# (tools/na_hamlib_bridge.c, the `st.gaps * 2 > rx_reads` block). It carries no tuned constant: a
# mis-framed stream lands near ONE gap per read and a healthy one at zero, so the two are separated
# by the whole range rather than by a margin someone has to keep re-tuning. Genuine packet loss lands
# far below one per read and normally moves `link_loss` too, which is why `link_loss` is reported on
# every failure — it is how a reader tells "the wire lost packets" from "the two ends disagree about
# what a frame is".
#
# WHY THE ARM DOES ITS OWN ARITHMETIC INSTEAD OF GREPPING FOR THE BRIDGE'S WARNING. Both are checked,
# but only the ratio is primary. Keying solely on the warning line would make this arm's coverage
# exactly co-extensive with the detector it is supposed to be a second opinion about: delete the
# alarm's condition and the arm goes green on a mis-framed stream. `reads=` is on the health line so
# an observer can apply the test independently, and the warning check below is a cross-check that the
# bridge's own detector still agrees with the arithmetic — a disagreement in EITHER direction is
# reported, because a detector that has stopped firing is a defect this arm can see and nothing else
# can.
#
# THE READINESS GATES ARE NOT THE THING UNDER TEST (S84, and issue #17's lapse arm learned it the
# hard way). rigctld is gated on ACCEPTING A TCP CONNECTION and the bridge on its `-> naudio :` line,
# which is printed after na_server_start succeeds and long predates all of this. Gating on the gap
# counter or on the delivered rate would make a real regression report as a harness fault — the two
# observations would be the same one.
#
# AND THE VACUITY GUARD, WHICH IS THE ASSERTION THIS ARM WOULD BE WORTHLESS WITHOUT. `gaps=0` is also
# what a stream that never delivered a byte looks like. So every arm requires, as PREMISES that fail
# as harness faults rather than as bridge defects: two health ticks, a read count above the same
# floor the bridge's own alarm uses, and a naudio client that actually received non-silent audio off
# this path. Without the last one a bridge that opened the stream and delivered silence would satisfy
# every counter here perfectly.
#
# macOS SIP TRAP (CLAUDE.md Learning 6): every child is launched DIRECTLY from this script and never
# wrapped in `timeout`, `perl` or another shell — see deadline.sh for why that matters next door.
set -u

. "$(dirname "${BASH_SOURCE[0]}")/deadline.sh"

BRIDGE="${1:?usage: netrigctl_arm.sh <na_hamlib_bridge> <rigctld> <naudio_bridge_probe>}"
RIGCTLD="${2:?usage: netrigctl_arm.sh <na_hamlib_bridge> <rigctld> <naudio_bridge_probe>}"
PROBE="${3:?usage: netrigctl_arm.sh <na_hamlib_bridge> <rigctld> <naudio_bridge_probe>}"

RIGPORT="${NA_NETRIGCTL_ARM_RIGPORT:-5599}"
NAPORT="${NA_NETRIGCTL_ARM_PORT:-4601}"
SECS="${NA_NETRIGCTL_ARM_SECONDS:-6}"

# 127.0.0.1 AND NOT `localhost`, MEASURED ON A DUAL-STACK RUNNER. `localhost` resolves to ::1 first
# on ubuntu-latest, where nothing is listening: libhamlib's TCP control connection survives it
# ("connect to localhost:5599 failed, (trying next interface)") and goes on to negotiate caps and a
# 1420 B payload budget, so the control plane looks entirely healthy — but the UDP data-plane
# subscribe then never receives its ACK, and `rig_stream_open` fails after three 1666 ms attempts.
# The arm's first real run on Linux is what surfaced it; macOS resolves localhost to 127.0.0.1 and
# never showed it.
#
# It also removes a defect in this harness that was wrong regardless of address family: the
# readiness gate below probed 127.0.0.1 while the bridge was told `localhost`, so the gate was not
# testing the endpoint the bridge would actually use. One name for both is the point.
RIGHOST="${NA_NETRIGCTL_ARM_HOST:-127.0.0.1}"

# The same floor the bridge's own alarm uses before it will believe a ratio. Below this the run is
# too short to have measured anything and `gaps=0` means nothing.
MIN_READS=50

# Learning 9 again: a green build is not evidence any of these exist.
for exe in "$BRIDGE" "$RIGCTLD" "$PROBE"; do
    if [ ! -x "$exe" ]; then
        echo "netrigctl_arm: $exe is not executable — nothing to drive" >&2
        exit 2
    fi
done

workdir=$(mktemp -d)
# na_harness_guard turns "this script died partway" into a harness fault instead of a pass. The
# defect was found HERE — a mutation of this file died in `$(( SECS + NA_PROBE_MARGIN ))` and
# exited 0 — but the measurement and its controls live in deadline.sh rather than being restated
# here (Learning 118).
cleanup() { rm -rf "$workdir"; na_harness_guard "netrigctl_arm"; }
trap cleanup EXIT

echo "netrigctl_arm: $BRIDGE -m 2 -> $(basename "$RIGCTLD") $RIGHOST:$RIGPORT, naudio :$NAPORT, ${SECS}s probe"
"$RIGCTLD" --version 2>&1 | sed -n '1s/^/  peer: /p'

# A TCP connect, not a sleep. rigctld prints nothing on startup worth gating on, and a constant sleep
# here would be a race on a loaded runner — and, worse, one that passes for the wrong reason. Proved
# to answer BOTH ways before it was trusted (Learning 222): it reports ready in ~300 ms against a
# live rigctld and never connects to a port nobody serves.
rigctld_ready () {
    local i
    for i in $(seq 1 100); do
        if ! kill -0 "$1" 2>/dev/null; then return 1; fi
        if (exec 3<>"/dev/tcp/$RIGHOST/$RIGPORT") 2>/dev/null; then
            exec 3<&- 3>&-
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# ------------------------------------------------------------------------------------ one arm
run_arm () {
    channels="$1"
    log="$workdir/bridge.c$channels.log"
    rlog="$workdir/rigctld.c$channels.log"
    plog="$workdir/probe.c$channels.log"
    arc=0

    # `-C stream_mode=tone` goes on RIGCTLD, not on the bridge: the bridge's -S sets the conf on its
    # local netrigctl rig object, which is not where the dummy backend generating audio lives. A -S
    # on an -m 2 run reaches nothing (docs/hamlib-streaming-bridge.md § "Over a remote rigctld").
    "$RIGCTLD" -m 1 -t "$RIGPORT" -C stream_mode=tone >"$rlog" 2>&1 &
    rpid=$!
    if ! rigctld_ready "$rpid"; then
        echo "FAIL netrigctl_arm/c$channels: rigctld never accepted a connection on $RIGHOST:$RIGPORT" >&2
        sed 's/^/    r /' "$rlog" >&2
        na_stop_pid "$rpid" INT "$NA_STOP_DEADLINE" "netrigctl_arm/c$channels rigctld"
        return 2
    fi

    # -x (RX only) deliberately. This arm is about the RX framing signature, and TX over netrigctl
    # has its own hazard that would confound it: an over-budget write is rejected outright with
    # -RIG_EIO, and a dead worker stops the bridge (tools/na_hamlib_bridge.c, tx_thread).
    "$BRIDGE" -m 2 -r "$RIGHOST:$RIGPORT" -p "$NAPORT" -c "$channels" -x >"$log" 2>&1 &
    bpid=$!
    ready=0
    for _ in $(seq 1 150); do
        if ! kill -0 "$bpid" 2>/dev/null; then break; fi
        if grep -q -- '-> naudio :' "$log" 2>/dev/null; then ready=1; break; fi
        sleep 0.1
    done
    if [ "$ready" -eq 0 ]; then
        # The loudest real cause is version skew, and it is worth naming here rather than leaving a
        # reader to diagnose "never came up": a bridge older than 961093f2 cannot parse a newer
        # rigctld's \stream_caps reply and the open is refused against caps it never understood.
        echo "FAIL netrigctl_arm/c$channels: the bridge never reported itself listening. If the" >&2
        echo "  log below shows a rate rejection naming a rate the caps appear to offer, this is" >&2
        echo "  libhamlib version skew between the bridge and rigctld, not a channel fault —" >&2
        echo "  docs/hamlib-streaming-bridge.md § 'Version skew is a separate, louder failure'." >&2
        sed 's/^/    | /' "$log" >&2
        sed 's/^/    r /' "$rlog" >&2
        na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "netrigctl_arm/c$channels bridge"
        na_stop_pid "$rpid" INT "$NA_STOP_DEADLINE" "netrigctl_arm/c$channels rigctld"
        return 2
    fi

    # PREMISE, not the assertion: a real naudio client must receive real audio off this path. This is
    # what stops every counter below from being satisfied by a stream that delivers nothing — gaps=0
    # is also what silence looks like. The probe cannot see the mis-framing fault itself (a client
    # measures 100% parity and a full-scale signal against a pre-fix -c 2 bridge, which is exactly
    # why the bridge's local gap signature is the only symptom), so it is a premise and never a
    # discriminator.
    na_run_deadline $(( SECS + NA_PROBE_MARGIN )) "netrigctl_arm/c$channels probe" "$plog" \
        "$PROBE" --port "$NAPORT" --seconds "$SECS" --expect content
    prc=$NA_DEADLINE_RC
    [ "$prc" -eq 124 ] && prc=2

    # Two health ticks, polled rather than slept for. The tick is ~5 s of the bridge's own wall clock,
    # so a loaded runner stretches it and a fixed sleep would report a slow machine as a missing tick.
    # Bounded: 40 s is far past two ticks and still a diagnosis rather than a hang.
    for _ in $(seq 1 400); do
        if ! kill -0 "$bpid" 2>/dev/null; then break; fi
        if [ "$(grep -c '^  rx: clients=' "$log" 2>/dev/null)" -ge 2 ]; then break; fi
        sleep 0.1
    done

    na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "netrigctl_arm/c$channels bridge"
    bexit=$NA_DEADLINE_RC
    na_stop_pid "$rpid" INT "$NA_STOP_DEADLINE" "netrigctl_arm/c$channels rigctld"

    if [ "$prc" -ne 0 ]; then
        echo "FAIL netrigctl_arm/c$channels: no naudio client received non-silent audio over -m 2," >&2
        echo "  so every gap counter below is vacuous — gaps=0 is also what a dead stream reports." >&2
        sed 's/^/    p /' "$plog" >&2
        sed 's/^/    | /' "$log" >&2
        return 2
    fi

    ticks=$(grep -c '^  rx: clients=' "$log" 2>/dev/null)
    if [ "$ticks" -lt 2 ]; then
        echo "FAIL netrigctl_arm/c$channels: only $ticks health tick(s) — the bridge never reported" >&2
        echo "  a settled RX picture, so there is nothing to measure (harness fault)." >&2
        sed 's/^/    | /' "$log" >&2
        return 2
    fi

    # Last tick wins: both counters are cumulative since the stream opened, so the final one is the
    # longest-baseline measurement the run produced.
    tickline=$(grep '^  rx: clients=' "$log" | tail -1)
    if [[ ! "$tickline" =~ gaps=([0-9]+)\ reads=([0-9]+)\ link_loss=([0-9]+) ]]; then
        echo "FAIL netrigctl_arm/c$channels: could not read gaps/reads/link_loss out of the health" >&2
        echo "  line, so this arm cannot tell what it measured (harness fault). The line was:" >&2
        echo "    | $tickline" >&2
        return 2
    fi
    gaps="${BASH_REMATCH[1]}"
    reads="${BASH_REMATCH[2]}"
    linkloss="${BASH_REMATCH[3]}"

    rateline=$(grep '^  rx: audio ' "$log" | tail -1)
    echo "  netrigctl_arm/c$channels: gaps=$gaps reads=$reads link_loss=$linkloss;${rateline#  rx:} (rate reported, never asserted)"

    if [ "$reads" -lt "$MIN_READS" ]; then
        echo "FAIL netrigctl_arm/c$channels: only $reads RX reads (floor $MIN_READS) — too short a" >&2
        echo "  baseline for a per-read ratio to mean anything (harness fault)." >&2
        sed 's/^/    | /' "$log" >&2
        return 2
    fi

    # THE ASSERTION. Complement of the bridge's own alarm, computed here so this arm does not depend
    # on that alarm still working.
    if [ $(( gaps * 2 )) -gt "$reads" ]; then
        echo "FAIL netrigctl_arm/c$channels: $gaps gaps over $reads RX reads — near one gap per" >&2
        echo "  read, which is not packet loss. The two ends disagree about how many bytes make a" >&2
        echo "  frame: the peer is framing this stream differently than the channels=$channels we" >&2
        echo "  opened, so naudio re-frames its bytes wrongly and clients get MIS-FRAMED audio," >&2
        echo "  not merely less of it. link_loss=$linkloss (genuine loss moves this and lands far" >&2
        echo "  below one gap per read)." >&2
        if [ "$channels" -eq 2 ]; then
            echo "  This is the -c 2 arm, and the known cause is a libhamlib below Hamlib" >&2
            echo "  961093f2 ON THIS BRIDGE — its \\stream_open carries no channels field, so" >&2
            echo "  rigctld opens the rig MONO however the caps advertise. The remote rigctld's" >&2
            echo "  own version does not matter. If the c1 arm above PASSED, that is the" >&2
            echo "  signature. See docs/hamlib-streaming-bridge.md § 'Version floor'." >&2
        else
            echo "  This is the -c 1 arm, which is documented to hold 0 gaps against every" >&2
            echo "  libhamlib that opens the stream at all — including pre-961093f2 ones. So this" >&2
            echo "  is NOT the known channels bug, and netrigctl framing is broken more broadly." >&2
        fi
        sed 's/^/    | /' "$log" >&2
        arc=1
    fi

    # Cross-check, both ways. The bridge prints a one-time explanation on exactly this condition;
    # it and the arithmetic above must agree, and either kind of disagreement is a real finding.
    warned=0
    grep -q 'RX reads with link_loss=0' "$log" 2>/dev/null && warned=1
    if [ "$warned" -eq 1 ] && [ "$arc" -eq 0 ]; then
        echo "FAIL netrigctl_arm/c$channels: the bridge raised its mis-framing warning while the" >&2
        echo "  arithmetic here says $gaps gaps over $reads reads is healthy. One of the two is" >&2
        echo "  wrong — most likely the alarm's condition and this arm's have drifted apart." >&2
        sed -n '/RX reads with link_loss=0/s/^/    | /p' "$log" >&2
        arc=1
    fi
    if [ "$warned" -eq 0 ] && [ "$arc" -ne 0 ]; then
        echo "  NOTE netrigctl_arm/c$channels: the stream is mis-framed and the bridge printed NO" >&2
        echo "  warning. Its alarm needs >= 50 reads and link_loss=0; if both held, the alarm" >&2
        echo "  itself has regressed and this arm is the only thing that can still see the fault." >&2
    fi

    if [ "$bexit" -ne 0 ]; then
        echo "FAIL netrigctl_arm/c$channels: bridge exited $bexit on SIGINT, expected 0 — a" >&2
        echo "  supervisor reads a non-zero exit as a crash (issue #16)." >&2
        arc=1
    fi

    [ "$arc" -eq 0 ] && echo "  netrigctl_arm/c$channels: OK (gap-per-read healthy, audio verified at a client)"
    return "$arc"
}

rc=0
# Both arms always run. "netrigctl is broken" and "stereo over netrigctl is broken" are different
# diagnoses with different fixes, and only running both tells them apart — the same reasoning the
# tone/silence pair in bridge_arm.sh rests on.
run_arm 1 || rc=$?
run_arm 2 || { arc=$?; [ "$rc" -eq 0 ] && rc=$arc; }

if [ "$rc" -eq 0 ]; then
    echo "netrigctl_arm: OK — the \\stream_open round trip carries correctly-framed audio at both" \
         "-c 1 and -c 2, verified at a real naudio client"
fi
na_harness_done
exit "$rc"
