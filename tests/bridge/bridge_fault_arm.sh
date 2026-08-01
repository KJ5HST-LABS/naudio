#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio tests — drive na_hamlib_bridge with libhamlib's stream calls FORCED to misbehave.
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# The fault-injection half of the bridge harness (issue #35). bridge_arm.sh covers the happy
# path — audio in, content out, silence on a silent source — and deliberately makes nothing go
# wrong. This is the other half: it regression-guards the two bugs whose FIXES already shipped
# but whose verification never did, because both were checked with a hand-built shim in a
# scratchpad that evaporated with it (issues #5 and #6, then #15, then #35).
#
#   #5  a dead worker thread left the bridge running with no audio. Fixed by worker_failed()
#       (tools/na_hamlib_bridge.c:49-57), which sets g_stop and g_failed so main tears down and
#       returns 1. Guarded here by arm A.
#   #6  a short rig_stream_write silently dropped the unwritten tail. Fixed by ring_requeue()
#       (tools/na_hamlib_bridge.c:122-137), which hands the tail back and counts what no longer
#       fits. Guarded here by arm C.
#
# THE ARMS MUST DISAGREE, and arm B is the one that makes the other two mean anything:
#
#   A  rx-fail   NA_FAIL_RX_AFTER   the bridge must EXIT NON-ZERO   (it used to idle forever)
#   B  control   no fault armed     the bridge must STAY UP and report short=0
#   C  tx-short  NA_FAIL_TX_SHORT   the bridge must COUNT the short writes  (short > 0)
#
# Without arm B, arm C proves nothing: a bridge that short-writes during ordinary operation would
# satisfy "short > 0" with no fault injected at all, and arm A cannot tell "exited because of the
# forced error" from "exited because the shim broke it". Arm B is the same shim, the same probe and
# the same TX load with only the fault switched off, so it isolates the fault as the cause.
#
# EVERY ARM ASSERTS THE SHIM ANNOUNCED ITSELF. A preload that fails to load produces exactly the
# output of a healthy run — no error, no diagnostic, nothing — so "no short writes were
# reported" and "interposition never happened" are otherwise indistinguishable, and the second
# one passes arm B silently. failshim.c's constructor prints `na_failshim: armed ...` for this
# reason, and this script greps for it before believing any arm (CLAUDE.md Learning 63).
#
# macOS SIP TRAP (CLAUDE.md Learning 6): the bridge is launched DIRECTLY, never through `timeout`,
# `perl` or a nested shell. Exec'ing a SIP-protected binary strips DYLD_* from the environment with
# no error at all, which would silently disable every fault this file injects and turn arms A and C
# into false failures that look like bridge bugs. Backgrounding plus an explicit kill does the same
# job without the trap.
set -u

BRIDGE="${1:?usage: bridge_fault_arm.sh <na_hamlib_bridge> <naudio_bridge_probe> <failshim>}"
PROBE="${2:?usage: bridge_fault_arm.sh <na_hamlib_bridge> <naudio_bridge_probe> <failshim>}"
SHIM="${3:?usage: bridge_fault_arm.sh <na_hamlib_bridge> <naudio_bridge_probe> <failshim>}"
PORT="${NA_BRIDGE_FAULT_PORT:-4600}"

# Learning 9: a green build is NOT evidence any of these exist — the bridge target warns and
# skips. Assert on the binaries, never on an exit code.
for f in "$BRIDGE" "$PROBE"; do
    if [ ! -x "$f" ]; then
        echo "bridge_fault_arm: $f is not executable — nothing to drive" >&2
        exit 2
    fi
done
if [ ! -f "$SHIM" ]; then
    echo "bridge_fault_arm: $SHIM does not exist — no fault could be injected" >&2
    exit 2
fi

# The preload variable is the one thing here that is genuinely per-platform.
case "$(uname -s)" in
    Darwin) PRELOAD_VAR=DYLD_INSERT_LIBRARIES ;;
    *)      PRELOAD_VAR=LD_PRELOAD ;;
esac

workdir=$(mktemp -d)
cleanup() { rm -rf "$workdir"; }
trap cleanup EXIT

rc=0

# Wait for the bridge to report itself listening rather than sleeping a guessed constant. The
# "-> naudio :<port>" line is printed only AFTER na_server_start succeeds
# (tools/na_hamlib_bridge.c:480) and is explicitly fflush'd on the next line so it is not stuck in a
# buffer when stdout is redirected — a real readiness signal, not a guess (Learning 64). Retrying
# the probe's connect instead would cost the full 10 s UDP handshake timeout per attempt (L62).
wait_ready () {
    log="$1"; pid="$2"
    for _ in $(seq 1 100); do
        if ! kill -0 "$pid" 2>/dev/null; then return 1; fi
        if grep -q -- '-> naudio :' "$log" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    return 1
}

# The shim's own load marker. Checked in every arm, including the ones that FAIL for other reasons,
# because "the shim never loaded" is a harness fault (exit 2) and not a bridge bug (exit 1).
assert_shim_loaded () {
    log="$1"; arm="$2"
    if ! grep -q 'na_failshim: armed' "$log" 2>/dev/null; then
        echo "FAIL bridge_fault_arm/$arm: the shim never loaded — $PRELOAD_VAR did not take" >&2
        echo "  (on macOS this is what SIP stripping looks like: no error, no interposition)" >&2
        sed 's/^/    | /' "$log" >&2
        return 2
    fi
    return 0
}

# ---------------------------------------------------------------- arm A: a dead RX worker exits
#
# Issue #5's actual symptom was NOT a crash — it was the opposite. rx_thread returned on the error
# and left main() ticking every 200 ms against a still-open stream, so the health line kept printing
# a plausible client count with frozen counters while no audio moved at all. The assertion is
# therefore "the process is GONE", with a deadline: a bridge that is still alive here is the bug.
arm_rx_fail () {
    log="$workdir/rx_fail.log"
    env "$PRELOAD_VAR=$SHIM" NA_FAIL_RX_AFTER=10 \
        "$BRIDGE" -m 1 -S tone -p "$PORT" >"$log" 2>&1 &
    bpid=$!

    if ! wait_ready "$log" "$bpid"; then
        echo "FAIL bridge_fault_arm/rx-fail: the bridge never reported itself listening" >&2
        sed 's/^/    | /' "$log" >&2
        kill -INT "$bpid" 2>/dev/null; wait "$bpid" 2>/dev/null
        return 2
    fi

    # 10 reads are allowed through first, so the bridge is genuinely serving before the fault.
    exited=0
    for _ in $(seq 1 100); do
        if ! kill -0 "$bpid" 2>/dev/null; then exited=1; break; fi
        sleep 0.1
    done

    if [ "$exited" -eq 0 ]; then
        kill -INT "$bpid" 2>/dev/null; wait "$bpid" 2>/dev/null
        assert_shim_loaded "$log" rx-fail || return 2
        echo "FAIL bridge_fault_arm/rx-fail: rig_stream_read has been failing for 10 s and the" >&2
        echo "  bridge is still running. This is issue #5 exactly: a dead worker leaves the" >&2
        echo "  process up, the port open and the health line printing, with no audio moving." >&2
        return 1
    fi
    wait "$bpid" 2>/dev/null
    bexit=$?

    assert_shim_loaded "$log" rx-fail || return 2

    if [ "$bexit" -eq 0 ]; then
        echo "FAIL bridge_fault_arm/rx-fail: the bridge exited 0 after an RX stream error." >&2
        echo "  A supervisor reads that as a clean shutdown and will not restart it." >&2
        return 1
    fi
    # The stderr line names WHICH worker died. Without it the operator gets an exit code and no
    # cause, which is half of what issue #5 asked for.
    if ! grep -q 'rx worker stopped' "$log" 2>/dev/null; then
        echo "FAIL bridge_fault_arm/rx-fail: exited $bexit but never said which worker died" >&2
        sed 's/^/    | /' "$log" >&2
        return 1
    fi
    echo "  bridge_fault_arm/rx-fail: OK (exited $bexit and named the failed worker)"
    return 0
}

# ------------------------------------------------- arms B and C: short writes are counted, not lost
#
# Both run the probe in --tx mode, because tx_thread calls rig_stream_write only while
# na_server_tx_owner reports an owner (tools/na_hamlib_bridge.c:275-286) — an RX-only probe never
# reaches the code under test. The TX loss counters surface in the bridge's own periodic health
# line, which prints every ~5 s (tick 25 at 200 ms, tools/na_hamlib_bridge.c:502), so each of these
# arms costs one health interval and no less. That is the floor on this test's wall clock.
#
# Asserting on the BRIDGE's counter rather than on the shim's own tally is deliberate: a fixture
# that reports its own opinion of the fault is testing itself (Learning 43). short= is the number
# issue #6 added, so it is the number that has to move.
arm_tx () {
    arm="$1"; expect="$2"; shift 2      # expect: zero | nonzero
    log="$workdir/$arm.log"
    plog="$workdir/$arm.probe.log"

    env "$PRELOAD_VAR=$SHIM" "$@" \
        "$BRIDGE" -m 1 -S silence -p "$PORT" >"$log" 2>&1 &
    bpid=$!

    if ! wait_ready "$log" "$bpid"; then
        echo "FAIL bridge_fault_arm/$arm: the bridge never reported itself listening" >&2
        sed 's/^/    | /' "$log" >&2
        kill -INT "$bpid" 2>/dev/null; wait "$bpid" 2>/dev/null
        return 2
    fi

    # 6 s: one health tick (5 s) plus margin. --expect is deliberately NOT passed — this arm is
    # about the TX direction, and the RX source is `silence` precisely so the two cannot be
    # confused. bridge_arm.sh owns the RX content claim.
    "$PROBE" --port "$PORT" --seconds 6 --tx >"$plog" 2>&1
    prc=$?

    kill -INT "$bpid" 2>/dev/null; wait "$bpid" 2>/dev/null

    assert_shim_loaded "$log" "$arm" || return 2

    if [ "$prc" -ne 0 ]; then
        echo "FAIL bridge_fault_arm/$arm: the TX probe could not run (exit $prc)" >&2
        sed 's/^/    | /' "$plog" >&2
        return 2
    fi

    # The last health line wins: counters are cumulative, so the final sample carries the whole run.
    short=$(grep -o 'short=[0-9]*' "$log" 2>/dev/null | tail -1 | cut -d= -f2)
    if [ -z "${short:-}" ]; then
        echo "FAIL bridge_fault_arm/$arm: the bridge printed no TX health line in 6 s, so the" >&2
        echo "  short-write counter could not be read at all" >&2
        sed 's/^/    | /' "$log" >&2
        return 2
    fi

    case "$expect" in
        zero)
            if [ "$short" -ne 0 ]; then
                echo "FAIL bridge_fault_arm/$arm: short=$short with NO fault injected. The" >&2
                echo "  fault arm's short>0 would then prove nothing — this rules that out." >&2
                return 1
            fi
            echo "  bridge_fault_arm/$arm: OK (short=0 — the shim alone changes nothing)"
            ;;
        nonzero)
            if [ "$short" -eq 0 ]; then
                echo "FAIL bridge_fault_arm/$arm: every rig_stream_write was halved and" >&2
                echo "  the bridge reported short=0. This is issue #6: the unwritten tail is" >&2
                echo "  discarded with no counter and no log, so TX audio vanishes silently." >&2
                sed 's/^/    | /' "$log" >&2
                return 1
            fi
            echo "  bridge_fault_arm/$arm: OK (short=$short forced short writes accounted for)"
            ;;
    esac
    return 0
}

echo "bridge_fault_arm: $BRIDGE on :$PORT under $(basename "$SHIM")"

arm_rx_fail                                        || rc=$?
# Arms B and C both run even if an earlier arm failed: "the control also failed" and "only the fault
# arm failed" are different diagnoses, and running both is what tells them apart.
arm_tx control  zero                               || { a=$?; [ "$rc" -eq 0 ] && rc=$a; }
arm_tx tx-short nonzero NA_FAIL_TX_SHORT=2         || { a=$?; [ "$rc" -eq 0 ] && rc=$a; }

if [ "$rc" -eq 0 ]; then
    echo "bridge_fault_arm: OK — a dead RX worker stops the bridge, and short writes are counted"
fi
exit "$rc"
