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

echo "bridge_arm: $BRIDGE on :$PORT, ${SECS}s per arm"

run_arm tone content   || rc=$?
# The negative control runs even if the first arm failed, because "both arms report content" and
# "both arms report silence" are different diagnoses and the second arm is what tells them apart.
run_arm silence silence || { arc=$?; [ "$rc" -eq 0 ] && rc=$arc; }

if [ "$rc" -eq 0 ]; then
    echo "bridge_arm: OK — the bridge delivers content on tone and silence on silence"
fi
exit "$rc"
