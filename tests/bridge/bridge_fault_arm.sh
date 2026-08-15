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
#       (tools/na_hamlib_bridge.c:61-69), which sets g_stop and g_failed so main tears down and
#       returns 1. #5's acceptance list has TWO checkboxes — an RX stream error and a TX one —
#       because rx_thread and tx_thread have the same shape and the same fix. Arm A guards the RX
#       half; arm D guards the TX half (issue #37). One arm did not cover both: the two workers
#       reach worker_failed() from separate call sites, and a mutation that deletes only the TX
#       one leaves arm A entirely green (measured — see the mutation table in issue #37).
#   #6  a short rig_stream_write silently dropped the unwritten tail. Fixed by ring_requeue()
#       (tools/na_hamlib_bridge.c:146-161), which hands the tail back and counts what no longer
#       fits. Guarded here by arm C.
#
# THE ARMS MUST DISAGREE, and arm B is the one that makes the others mean anything:
#
#   A  rx-fail   NA_FAIL_RX_AFTER   the bridge must EXIT NON-ZERO   (it used to idle forever)
#   B  control   no fault armed     the bridge must STAY UP and report short=0
#   C  tx-short  NA_FAIL_TX_SHORT   the bridge must COUNT the short writes  (short > 0)
#   D  tx-fail   NA_FAIL_TX_AFTER   the bridge must EXIT NON-ZERO   (the TX half of #5)
#
# Without arm B, arm C proves nothing: a bridge that short-writes during ordinary operation would
# satisfy "short > 0" with no fault injected at all, and arms A and D cannot tell "exited because of
# the forced error" from "exited because the shim broke it". Arm B is the same shim, the same probe
# and the same TX load with only the fault switched off, so it isolates the fault as the cause.
#
# EVERY ARM ASSERTS THE SHIM LOADED, AND THAT THE CALLS IT DEPENDS ON WERE INTERPOSED. These are
# two different checks because they are two different failures with one symptom (issue #39):
#
#   `na_failshim: armed ...`        from the constructor      — the library LOADED
#   `na_failshim: interposed <fn>`  from inside the call      — that call BOUND
#
# A preload that fails to load produces exactly the output of a healthy run — no error, no
# diagnostic, nothing — so "no short writes were reported" and "interposition never happened" are
# otherwise indistinguishable, and the second one passes arm B silently (CLAUDE.md Learning 63).
# The `armed` marker fixes that and is NOT sufficient: a shim can load, print it, and still have
# both interposers as unreachable dead code, which is precisely what the ELF visibility trap did on
# the first Linux build — every arm failed reporting issues #5 and #6 as live bridge bugs. The
# `interposed` marker is emitted from inside the interposed call, on the pass-through path as well
# as the failing one, so no shim that failed to bind can produce it. Which calls each arm asserts
# is set by which calls it actually makes, measured rather than assumed:
#
#   A  rx-fail   read           — connects no client, so tx_thread never writes at all
#   B  control   read + write   — short=0 is satisfied by a shim that never bound; this is the fix
#   C  tx-short  read + write   — short=0 is reported as issue #6 regressing; same trap, inverted
#   D  tx-fail   read           — plus its stronger forced-write gate, which subsumes the write one
#
# macOS SIP TRAP (CLAUDE.md Learning 6): the bridge is launched DIRECTLY, never through `timeout`,
# `perl` or a nested shell. Exec'ing a SIP-protected binary strips DYLD_* from the environment with
# no error at all, which would silently disable every fault this file injects and turn arms A and C
# into false failures that look like bridge bugs. Backgrounding plus an explicit kill does the same
# job without the trap.
set -u

# Deadlines: every probe invocation and every wait below is bounded (issue #88 item 2, from #41).
# Sourced rather than exec'd, and deliberately NOT `timeout(1)` — see deadline.sh for why that
# would silently disable the fault injection next door, and why macOS may not have it at all.
. "$(dirname "${BASH_SOURCE[0]}")/deadline.sh"

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

# ...and the marker that proves a given call was actually INTERPOSED (issue #39). Loading and
# binding are different failures with the same symptom, and assert_shim_loaded above cannot tell
# them apart: the ELF visibility trap left this shim loaded, its constructor printing `armed` on
# every run, and both interposers unreachable dead code — so all four arms failed as though the
# BRIDGE had regressed and reported issues #5 and #6 as live bugs (measured, see the note in
# failshim.c). `interposed <fn>` is printed from INSIDE the interposed call, on the pass-through
# path as well as the failing one, so a shim that never bound cannot produce it however healthy the
# run looks. Emitted immediately after the bridge's own readiness line, which every arm already
# waits for, so no arm is too short to see it; once per function per process, so it does not swamp
# the log the arms grep (measured: 3 shim lines in a 6 s control run).
assert_shim_bound () {
    log="$1"; arm="$2"; fn="$3"
    if ! grep -q "na_failshim: interposed $fn" "$log" 2>/dev/null; then
        echo "FAIL bridge_fault_arm/$arm: the shim loaded but $fn was never interposed, so no" >&2
        echo "  fault could reach the bridge and this arm asserted nothing about it. The armed" >&2
        echo "  marker is present below — this is a HARNESS fault, not a bridge regression." >&2
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
        na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "bridge_fault_arm/rx-fail bridge"
        return 2
    fi

    # 10 reads are allowed through first, so the bridge is genuinely serving before the fault.
    exited=0
    for _ in $(seq 1 100); do
        if ! kill -0 "$bpid" 2>/dev/null; then exited=1; break; fi
        sleep 0.1
    done

    if [ "$exited" -eq 0 ]; then
        na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "bridge_fault_arm/rx-fail bridge"
        assert_shim_loaded "$log" rx-fail || return 2
        # Before blaming the bridge. A read interposer that loaded without binding injects no fault,
        # so the bridge correctly stays up and this branch is reached — which is issue #39's exact
        # misdiagnosis, and it is this arm that produced it on the first Linux build.
        assert_shim_bound "$log" rx-fail rig_stream_read || return 2
        echo "FAIL bridge_fault_arm/rx-fail: rig_stream_read has been failing for 10 s and the" >&2
        echo "  bridge is still running. This is issue #5 exactly: a dead worker leaves the" >&2
        echo "  process up, the port open and the health line printing, with no audio moving." >&2
        return 1
    fi
    na_wait_pid "$bpid" "$NA_STOP_DEADLINE" "bridge_fault_arm/rx-fail bridge"
    bexit=$NA_DEADLINE_RC

    assert_shim_loaded "$log" rx-fail || return 2
    # Only rig_stream_read. This arm connects no client, so na_server_tx_owner never reports an
    # owner and tx_thread never calls rig_stream_write at all — measured, not assumed: the write
    # marker is absent from this arm's log on a healthy run, so requiring it here would fail a
    # correct shim. Arms control/tx-short/tx-fail run a --tx probe and do assert both.
    assert_shim_bound "$log" rx-fail rig_stream_read || return 2

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
        na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "bridge_fault_arm/$arm bridge"
        return 2
    fi

    # 6 s: one health tick (5 s) plus margin. --expect is deliberately NOT passed — this arm is
    # about the TX direction, and the RX source is `silence` precisely so the two cannot be
    # confused. bridge_arm.sh owns the RX content claim.
    # Bounded for the same reason as bridge_arm.sh's probe: --seconds bounds how long it RUNS,
    # not how long it can BLOCK. A probe wedged in a receive here used to hang the whole suite.
    na_run_deadline $(( 6 + NA_PROBE_MARGIN )) "bridge_fault_arm/$arm probe" "$plog" \
        "$PROBE" --port "$PORT" --seconds 6 --tx
    prc=$NA_DEADLINE_RC

    na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "bridge_fault_arm/$arm bridge"

    assert_shim_loaded "$log" "$arm" || return 2

    # Before the interposition checks below, not after: a probe that never ran also produces no TX
    # owner and so no writes at all, which would surface as "rig_stream_write was never interposed"
    # — true, but the wrong proximate cause to put in front of whoever is reading. Both are harness
    # faults, so the verdict is unchanged either way; only the diagnosis differs.
    if [ "$prc" -ne 0 ]; then
        echo "FAIL bridge_fault_arm/$arm: the TX probe could not run (exit $prc)" >&2
        sed 's/^/    | /' "$plog" >&2
        return 2
    fi

    # Both calls, and for the CONTROL arm this is the whole point of issue #39. Its pass condition
    # is short=0 — which a shim that never bound satisfies perfectly, because a write that was never
    # interposed is never short. "The pass-through changed nothing" and "nothing ran" are otherwise
    # the same observation, one level in from the `armed` marker that was added to fix exactly this
    # shape (Learning 68). tx-short needs it for the converse reason: short=0 there is reported as a
    # regression of issue #6, and an unbound write interposer would produce it with #6 intact.
    assert_shim_bound "$log" "$arm" rig_stream_read  || return 2
    assert_shim_bound "$log" "$arm" rig_stream_write || return 2

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

# ---------------------------------------------------------------- arm D: a dead TX worker exits
#
# The TX half of issue #5 (issue #37). Same symptom as arm A and a different call site: tx_thread
# returns on the error (tools/na_hamlib_bridge.c:285-291), main() never learns, and the process keeps
# serving a client whose audio now goes nowhere.
#
# THE PROBE MUST TRANSMIT. tx_thread calls rig_stream_write only while na_server_tx_owner reports an
# owner (tools/na_hamlib_bridge.c:275-286), so an idle client never reaches the fault. Measured, not
# assumed: armed at NA_FAIL_TX_AFTER=0 — fail the very first write — with no client at all, the
# bridge logged zero forced writes in 3 s and exited 0 on SIGINT. That is also why this arm cannot
# reuse arm A's shape, and why it asserts the fault FIRED before it reports anything about the bridge.
#
# Unlike arms B and C this needs no health-line interval: process exit and the `tx worker stopped`
# line are both immediate, so it costs ~1 s rather than the ~6 s those two pay for one health tick.
arm_tx_fail () {
    log="$workdir/tx_fail.log"
    plog="$workdir/tx_fail.probe.log"

    env "$PRELOAD_VAR=$SHIM" NA_FAIL_TX_AFTER=10 \
        "$BRIDGE" -m 1 -S silence -p "$PORT" >"$log" 2>&1 &
    bpid=$!

    if ! wait_ready "$log" "$bpid"; then
        echo "FAIL bridge_fault_arm/tx-fail: the bridge never reported itself listening" >&2
        sed 's/^/    | /' "$log" >&2
        na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "bridge_fault_arm/tx-fail bridge"
        return 2
    fi

    # 10 writes are allowed through first, so the bridge is genuinely carrying TX audio before the
    # fault. -S silence keeps the RX direction out of it; bridge_arm.sh owns the RX content claim.
    # Backgrounded, so like the lapse probe next door it needs no na_run_deadline: the poll below is
    # bounded and the na_stop_pid after it caps this probe's life regardless of what it is doing.
    "$PROBE" --port "$PORT" --seconds 6 --tx >"$plog" 2>&1 &
    ppid=$!

    exited=0
    for _ in $(seq 1 100); do
        if ! kill -0 "$bpid" 2>/dev/null; then exited=1; break; fi
        sleep 0.1
    done

    # SIGTERM, not SIGINT. A background job started by a non-interactive shell inherits SIG_IGN for
    # SIGINT, and the probe installs no handler of its own — so `kill -INT` here would be a silent
    # no-op and this arm would pay the probe's full --seconds on every run. The bridge is immune to
    # that trap only because it installs a handler, which overrides the inherited disposition.
    na_stop_pid "$ppid" TERM "$NA_STOP_DEADLINE" "bridge_fault_arm/tx-fail probe"

    if [ "$exited" -eq 1 ]; then
        na_wait_pid "$bpid" "$NA_STOP_DEADLINE" "bridge_fault_arm/tx-fail bridge"
        bexit=$NA_DEADLINE_RC
    else
        na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "bridge_fault_arm/tx-fail bridge"
        bexit=-1
    fi

    assert_shim_loaded "$log" tx-fail || return 2
    # The read interposer is not what this arm faults, but nothing else here asserts it bound and
    # the bridge is reading throughout. `interposed rig_stream_write` is deliberately NOT asserted:
    # the forced-write gate below is strictly stronger — it proves the write interposer bound AND
    # that the fault fired — and two assertions on one observable let the weaker one rot unnoticed
    # (Learning 43).
    assert_shim_bound "$log" tx-fail rig_stream_read || return 2

    # The shim announcing it LOADED is not evidence that it BOUND, and for this arm the difference
    # is the whole verdict: an unbound interposer injects no fault, the bridge correctly stays up,
    # and "the bridge ignored a failing write" is then indistinguishable from "no write ever failed"
    # (Learning 68; issue #39 is the general form). The forced-write line is emitted from INSIDE the
    # interposed call, so it is the marker that separates them. It gates both verdicts below rather
    # than only the failing one, so a pass means the bridge died OF THE INJECTED FAULT.
    if ! grep -q 'na_failshim: rig_stream_write .* (forced)' "$log" 2>/dev/null; then
        echo "FAIL bridge_fault_arm/tx-fail: no rig_stream_write ever failed, so tx_thread never" >&2
        echo "  reached the code under test and this arm asserted nothing about the bridge. The" >&2
        echo "  shim loaded — the armed marker is above — so this is either a probe that never" >&2
        echo "  keyed up, or an interposer that loaded without binding." >&2
        sed 's/^/    | /' "$plog" >&2
        return 2
    fi

    if [ "$exited" -eq 0 ]; then
        echo "FAIL bridge_fault_arm/tx-fail: rig_stream_write has been failing for 10 s and the" >&2
        echo "  bridge is still running. This is issue #5's TX half: tx_thread returns on the" >&2
        echo "  error, main() never learns, and the process keeps serving with no TX audio." >&2
        sed 's/^/    | /' "$log" >&2
        return 1
    fi
    if [ "$bexit" -eq 0 ]; then
        echo "FAIL bridge_fault_arm/tx-fail: the bridge exited 0 after a TX stream error." >&2
        echo "  A supervisor reads that as a clean shutdown and will not restart it." >&2
        return 1
    fi
    # Named separately from arm A's `rx worker stopped`: the two workers share worker_failed(), so
    # only the label distinguishes which one died, and an operator gets a cause rather than a code.
    if ! grep -q 'tx worker stopped' "$log" 2>/dev/null; then
        echo "FAIL bridge_fault_arm/tx-fail: exited $bexit but never said which worker died" >&2
        sed 's/^/    | /' "$log" >&2
        return 1
    fi
    echo "  bridge_fault_arm/tx-fail: OK (exited $bexit and named the failed worker)"
    return 0
}

echo "bridge_fault_arm: $BRIDGE on :$PORT under $(basename "$SHIM")"

arm_rx_fail                                        || rc=$?
# Every later arm runs even if an earlier one failed: "the control also failed" and "only the fault
# arm failed" are different diagnoses, and running them all is what tells them apart.
arm_tx control  zero                               || { a=$?; [ "$rc" -eq 0 ] && rc=$a; }
arm_tx tx-short nonzero NA_FAIL_TX_SHORT=2         || { a=$?; [ "$rc" -eq 0 ] && rc=$a; }
arm_tx_fail                                        || { a=$?; [ "$rc" -eq 0 ] && rc=$a; }

if [ "$rc" -eq 0 ]; then
    echo "bridge_fault_arm: OK — a dead RX or TX worker stops the bridge, and short writes counted"
fi
exit "$rc"
