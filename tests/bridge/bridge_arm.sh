#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio tests — drive a real na_hamlib_bridge and assert what a client actually receives.
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# This is the half of the bridge harness (issue #15) that CANNOT be portable: it needs
# na_hamlib_bridge, which needs a hand-built streaming-capable libhamlib. CMake registers it only
# when that opt-in target exists, so on a stock build ctest simply has no such test rather than a
# skip that scrolls past. The content detector it relies on is proved separately and unconditionally
# by `naudio_bridge_probe --selftest`, which runs on every platform.
#
# TWO ARMS, AND THE SECOND IS THE POINT.
#   tone    -> the probe must see CONTENT
#   silence -> the probe must see SILENCE
# Measured on 2026-07-31 against the Hamlib dummy, those two runs are indistinguishable on every
# volume-shaped metric (956160 vs 954240 bytes, 996 vs 994 callbacks, and the bridge's own meter
# says 83% of nominal for both). Only peak |sample| separates them, 16383 against 0. A one-armed
# version of this test would pass against a completely silent bridge, which is exactly the failure
# issue #15's own comment calls the single highest-value line in the harness.
#
# The two arms must DISAGREE, so no constant-returning probe satisfies both (Learning 58).
#
# macOS SIP TRAP (CLAUDE.md Learning 6): the bridge is launched DIRECTLY from this script and never
# wrapped in `timeout`, `perl` or another shell. Those are SIP-protected binaries, and exec'ing one
# strips DYLD_* from the environment with no error at all — which silently disables any dylib
# interposition a future fault-injection arm would depend on. Backgrounding plus an explicit kill
# does the same job without the trap.
set -u

BRIDGE="${1:?usage: bridge_arm.sh <na_hamlib_bridge> <naudio_bridge_probe>}"
PROBE="${2:?usage: bridge_arm.sh <na_hamlib_bridge> <naudio_bridge_probe>}"
PORT="${NA_BRIDGE_ARM_PORT:-4599}"
SECS="${NA_BRIDGE_ARM_SECONDS:-4}"

# Learning 9: a green build is NOT evidence the bridge exists — the target warns and skips rather
# than failing. Assert on the binary, never on an exit code.
if [ ! -x "$BRIDGE" ]; then
    echo "bridge_arm: $BRIDGE is not executable — nothing to drive" >&2
    exit 2
fi
if [ ! -x "$PROBE" ]; then
    echo "bridge_arm: $PROBE is not executable" >&2
    exit 2
fi

workdir=$(mktemp -d)
cleanup() { rm -rf "$workdir"; }
trap cleanup EXIT

rc=0

run_arm () {
    mode="$1"      # tone | silence
    expect="$2"    # content | silence
    log="$workdir/bridge.$mode.log"

    "$BRIDGE" -m 1 -S "$mode" -p "$PORT" >"$log" 2>&1 &
    bpid=$!

    # Wait for the bridge to report itself listening rather than sleeping a guessed constant. The
    # "-> naudio :<port>" line is printed only AFTER na_server_start succeeds
    # (tools/na_hamlib_bridge.c:480), and the fflush on the next line exists precisely so it is not
    # stuck in a buffer when stdout is a file — so it is a real readiness signal, not a guess.
    #
    # Retrying the PROBE instead would be far more expensive: a UDP connect to a port with no
    # listener draws no reply and costs the full 10 s handshake timeout, because a connectionless
    # transport cannot refuse (Learning 62). Polling a log line costs milliseconds.
    ready=0
    for _ in $(seq 1 100); do
        if ! kill -0 "$bpid" 2>/dev/null; then break; fi
        if grep -q -- '-> naudio :' "$log" 2>/dev/null; then ready=1; break; fi
        sleep 0.1
    done
    if [ "$ready" -eq 0 ]; then
        echo "FAIL bridge_arm/$mode: the bridge never reported itself listening" >&2
        sed 's/^/    | /' "$log" >&2
        kill -INT "$bpid" 2>/dev/null
        wait "$bpid" 2>/dev/null
        return 2
    fi

    "$PROBE" --port "$PORT" --seconds "$SECS" --expect "$expect"
    prc=$?

    kill -INT "$bpid" 2>/dev/null
    wait "$bpid" 2>/dev/null
    bexit=$?

    # The bridge must also shut down cleanly on SIGINT — a supervisor reads a non-zero exit as a
    # crash, and three startup paths that exit 0 wrongly are already tracked as #16.
    if [ "$bexit" -ne 0 ]; then
        echo "FAIL bridge_arm/$mode: bridge exited $bexit on SIGINT, expected 0" >&2
        prc=1
    fi

    case "$prc" in
        0) echo "  bridge_arm/$mode: OK (expected $expect)" ;;
        1) echo "FAIL bridge_arm/$mode: expected $expect and the stream did not match" >&2 ;;
        *) echo "FAIL bridge_arm/$mode: the probe could not run (harness fault)" >&2 ;;
    esac
    return $prc
}

# ------------------------------------------------- arm: a failed startup must NOT exit 0 (issue #16)
#
# The two arms above assert what a HEALTHY bridge delivers. This one asserts what a bridge that
# never came up reports, which is the other half of the same supervisor contract: three failures
# after the naudio server object exists reached the shared teardown by goto and fell through to
# `return g_failed ? 1 : 0` with g_failed == 0 — g_failed is set only by a dead WORKER. So the
# bridge printed its diagnostic to stderr and exited 0, and a supervisor (systemd, launchd, a
# container restart policy) reads 0 as "completed its work and shut down cleanly" and does not
# restart it. The bridge never comes up and its exit status actively argues against intervention.
#
# na_server_start failing on an already-taken port is staged here because it is the likeliest of the
# three in real deployment — usually a previous instance that has not fully exited.
#
# THIS ARM'S PREMISE IS PLATFORM-DEPENDENT AND IT SAYS SO RATHER THAN ASSUMING IT. naudio's UDP
# server binds with SO_REUSEADDR (src/net/UdpServerTransport.cpp:43), and the platforms disagree
# about what that means for UDP. Measured 2026-08-12 with two controls, so the result is not one
# implementation's opinion (Learning 183):
#
#                     UDP, reuse on BOTH      UDP, reuse on 2nd only     TCP, reuse on BOTH
#     macOS           bind FAILS              bind fails                 bind fails
#     Linux           bind SUCCEEDS           bind fails                 bind fails
#
# So on Linux the second bridge BINDS THE SAME PORT and starts, there is no failed startup, and
# there is no exit code to assert. The arm reports that in so many words instead of passing quietly:
# a skip that reads as a pass is exactly the failure the rest of this harness exists to prevent
# (Learning 63). That Linux double-bind is a naudio-side defect in its own right — issue #83 — so
# this arm is a real detector on macOS and a DECLARED NO-OP on the Linux CI job until #83 is fixed.
# Do not read a green Linux run as evidence issue #16 is guarded. Fixing #83 by dropping reuseAddr
# on the UDP server bind makes this arm live everywhere and turns the PREMISE NOT MET branch below
# into dead code worth deleting.
run_exit_code_arm () {
    alog="$workdir/exitcode.holder.log"
    blog="$workdir/exitcode.second.log"

    "$BRIDGE" -m 1 -S silence -p "$PORT" >"$alog" 2>&1 &
    apid=$!
    ready=0
    for _ in $(seq 1 100); do
        if ! kill -0 "$apid" 2>/dev/null; then break; fi
        if grep -q -- '-> naudio :' "$alog" 2>/dev/null; then ready=1; break; fi
        sleep 0.1
    done
    if [ "$ready" -eq 0 ]; then
        echo "FAIL bridge_arm/exit-code: the port-holding bridge never reported itself listening" >&2
        sed 's/^/    | /' "$alog" >&2
        kill -INT "$apid" 2>/dev/null
        wait "$apid" 2>/dev/null
        return 2
    fi

    # Deadline rather than a bare `wait`: on a platform where the premise does not hold the second
    # bridge runs forever, and an unbounded wait here would hang ctest instead of reporting.
    "$BRIDGE" -m 1 -S silence -p "$PORT" >"$blog" 2>&1 &
    bpid=$!
    exited=0
    for _ in $(seq 1 100); do
        if ! kill -0 "$bpid" 2>/dev/null; then exited=1; break; fi
        sleep 0.1
    done
    if [ "$exited" -eq 1 ]; then
        wait "$bpid" 2>/dev/null
        bexit=$?
    else
        kill -INT "$bpid" 2>/dev/null
        wait "$bpid" 2>/dev/null
        bexit=-1
    fi

    kill -INT "$apid" 2>/dev/null
    wait "$apid" 2>/dev/null
    aexit=$?

    # The holder is also the SIGINT control: an orderly shutdown must still be 0, or the fix for
    # this arm has been made by failing everything (issue #16's third acceptance item).
    if [ "$aexit" -ne 0 ]; then
        echo "FAIL bridge_arm/exit-code: the port-holding bridge exited $aexit on SIGINT," >&2
        echo "  expected 0 — a clean operator shutdown must not report failure" >&2
        sed 's/^/    | /' "$alog" >&2
        return 1
    fi

    if grep -q -- '-> naudio :' "$blog" 2>/dev/null; then
        echo "  bridge_arm/exit-code: PREMISE NOT MET on $(uname -s) — the second bridge bound the"
        echo "    same UDP port and started, so no startup failure happened and this arm asserted"
        echo "    NOTHING about issue #16. Not a bridge regression; see the SO_REUSEADDR table in"
        echo "    this file. The arm is a live detector only where the second bind fails."
        return 0
    fi
    if ! grep -q 'na_server_start' "$blog" 2>/dev/null; then
        echo "FAIL bridge_arm/exit-code: the second bridge neither started nor failed in" >&2
        echo "  na_server_start, so this arm cannot tell what it measured (harness fault)" >&2
        sed 's/^/    | /' "$blog" >&2
        return 2
    fi
    if [ "$bexit" -eq 0 ]; then
        echo "FAIL bridge_arm/exit-code: na_server_start failed and the bridge still exited 0." >&2
        echo "  This is issue #16: a supervisor reads 0 as a clean shutdown and will not restart," >&2
        echo "  so the bridge never comes up and its exit status argues against intervention." >&2
        sed 's/^/    | /' "$blog" >&2
        return 1
    fi
    echo "  bridge_arm/exit-code: OK (a failed na_server_start exits $bexit, and SIGINT still 0)"
    return 0
}

echo "bridge_arm: $BRIDGE on :$PORT, ${SECS}s per arm"

run_arm tone content   || rc=$?
# The negative control runs even if the first arm failed, because "both arms report content" and
# "both arms report silence" are different diagnoses and the second arm is what tells them apart.
run_arm silence silence || { arc=$?; [ "$rc" -eq 0 ] && rc=$arc; }
# Runs even if the content arms failed, for the same reason they run independently of each other:
# "the bridge delivers nothing" and "the bridge misreports a failed startup" are different
# diagnoses, and only running both tells them apart.
run_exit_code_arm       || { arc=$?; [ "$rc" -eq 0 ] && rc=$arc; }

if [ "$rc" -eq 0 ]; then
    echo "bridge_arm: OK — content on tone, silence on silence, and a failed startup exits non-zero"
fi
exit "$rc"
