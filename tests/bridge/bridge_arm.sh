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
# FOUR ARMS:
#   tone      -> the probe must see CONTENT                                        (issue #15)
#   silence   -> the probe must see SILENCE                                        (issue #15)
#   exit-code -> a bridge whose na_server_start failed must NOT exit 0             (issue #16)
#   lapse     -> a lost TX channel must name its cause instead of going quiet      (issue #17)
#
# THE FIRST TWO ARE A PAIR, AND THE SECOND IS THE POINT. Measured on 2026-07-31 against the Hamlib
# dummy, those two runs are indistinguishable on every volume-shaped metric (956160 vs 954240 bytes,
# 996 vs 994 callbacks, and the bridge's own meter says 83% of nominal for both). Only peak |sample|
# separates them, 16383 against 0. A one-armed version of this test would pass against a completely
# silent bridge, which is exactly the failure issue #15's own comment calls the single highest-value
# line in the harness.
#
# The two arms must DISAGREE, so no constant-returning probe satisfies both (Learning 58). The
# lapse arm is paired the same way: its `server error` grep means something only because run_arm
# asserts an undisturbed run produces none.
#
# macOS SIP TRAP (CLAUDE.md Learning 6): the bridge is launched DIRECTLY from this script and never
# wrapped in `timeout`, `perl` or another shell. Those are SIP-protected binaries, and exec'ing one
# strips DYLD_* from the environment with no error at all — which silently disables any dylib
# interposition a future fault-injection arm would depend on. Backgrounding plus an explicit kill
# does the same job without the trap.
set -u

# Deadlines: every probe invocation and every wait below is bounded (issue #88 item 2, from #41).
# Sourced rather than exec'd, and deliberately NOT `timeout(1)` — see deadline.sh for why that
# would silently disable the fault injection next door, and why macOS may not have it at all.
. "$(dirname "${BASH_SOURCE[0]}")/deadline.sh"

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
    # (tools/na_hamlib_bridge.c:706), and the fflush just below it exists precisely so it is not
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
        na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "bridge_arm/$mode bridge"
        return 2
    fi

    # THE #41 HANG WAS HERE. `--seconds` is the probe's own idea of how long to RUN, not a promise
    # about when it RETURNS: one wedged in a receive waits forever, and this arm waited with it for
    # ~29 minutes. The deadline is the probe's own runtime plus a fixed margin for connect and
    # teardown, so it scales with SECS rather than being a constant a longer arm silently outgrows.
    na_run_deadline $(( SECS + NA_PROBE_MARGIN )) "bridge_arm/$mode probe" - \
        "$PROBE" --port "$PORT" --seconds "$SECS" --expect "$expect"
    prc=$NA_DEADLINE_RC
    # Normalise the deadline status into this file's own 0/1/2 vocabulary. Letting 124 out would be
    # actively misleading rather than merely untidy: 124 is what `timeout(1)` returns, and this
    # arm's outer ctest TIMEOUT would report the same number — so a reader seeing 124 would conclude
    # the job was killed from outside at exactly the moment the harness had in fact caught it
    # itself. A wedged probe is the `*)` case below, which is 2.
    [ "$prc" -eq 124 ] && prc=2

    na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "bridge_arm/$mode bridge"
    bexit=$NA_DEADLINE_RC

    # The bridge must also shut down cleanly on SIGINT — a supervisor reads a non-zero exit as a
    # crash, and three startup paths that exit 0 wrongly are already tracked as #16.
    if [ "$bexit" -ne 0 ]; then
        echo "FAIL bridge_arm/$mode: bridge exited $bexit on SIGINT, expected 0" >&2
        prc=1
    fi

    # NEGATIVE CONTROL for the lapse arm below (issue #17). That arm passes when the bridge prints
    # a `server error` line; this asserts an undisturbed run prints none, so the grep it keys on
    # cannot be satisfied by something the bridge says anyway. Without this pair the lapse arm would
    # be green against a bridge that logged "server error" unconditionally.
    if grep -q 'server error' "$log" 2>/dev/null; then
        echo "FAIL bridge_arm/$mode: the bridge reported a server error on an undisturbed run," >&2
        echo "  which both breaks this arm and makes the #17 lapse arm's grep meaningless." >&2
        sed -n '/server error/s/^/    | /p' "$log" >&2
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
# THIS ARM WAS A NO-OP ON LINUX UNTIL #83 WAS FIXED, AND THE HISTORY IS WORTH KEEPING. naudio's UDP
# server used to bind with SO_REUSEADDR, and the platforms disagree about what that means for UDP:
# two sockets that BOTH set it share an addr:port on Linux and are refused on macOS. So the second
# bridge here used to start normally on Linux, leaving no failed startup to assert an exit code on —
# a green arm that had tested nothing, on the one platform CI runs it. It said so at the point of
# skip rather than passing quietly, and #83 has since dropped the flag (UdpServerTransport.cpp), so
# the collision below now fails to bind on every platform and this arm is live everywhere.
#
# Keep the collision as the staging mechanism: it is the real deployment case, and it now doubles as
# an end-to-end check that #83 has not regressed — a return of SO_REUSEADDR would show up here as
# this arm quietly costing ~10 s more (the deadline below expiring) instead of ~1 s.
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
        na_stop_pid "$apid" INT "$NA_STOP_DEADLINE" "bridge_arm/exit-code holder"
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
        na_wait_pid "$bpid" "$NA_STOP_DEADLINE" "bridge_arm/exit-code second bridge"
        bexit=$NA_DEADLINE_RC
    else
        na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "bridge_arm/exit-code second bridge"
        bexit=-1
    fi

    na_stop_pid "$apid" INT "$NA_STOP_DEADLINE" "bridge_arm/exit-code holder"
    aexit=$NA_DEADLINE_RC

    # The holder is also the SIGINT control: an orderly shutdown must still be 0, or the fix for
    # this arm has been made by failing everything (issue #16's third acceptance item).
    if [ "$aexit" -ne 0 ]; then
        echo "FAIL bridge_arm/exit-code: the port-holding bridge exited $aexit on SIGINT," >&2
        echo "  expected 0 — a clean operator shutdown must not report failure" >&2
        sed 's/^/    | /' "$alog" >&2
        return 1
    fi

    # The second bridge coming UP is now a naudio regression, not a platform quirk to tolerate:
    # since #83 the UDP server no longer sets SO_REUSEADDR, so a port that is already being served
    # must be refused everywhere. Treating this as a failure rather than a skip is the whole point
    # of fixing #83 — while it was a skip, this arm was green on Linux having asserted nothing.
    if grep -q -- '-> naudio :' "$blog" 2>/dev/null; then
        echo "FAIL bridge_arm/exit-code: a second bridge BOUND a port the first was already" >&2
        echo "  serving and came up. na_server_start reported success for a port it does not" >&2
        echo "  have, so two bridges are live and the datagrams reach only one of them. This is" >&2
        echo "  issue #83 regressing — check that UdpServerTransport still passes reuseAddr=false." >&2
        sed 's/^/    | /' "$blog" >&2
        return 1
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

# ------------------------------------------------- arm: a TX lapse must NAME ITSELF (issue #17)
#
# #17: the bridge holds TX ownership for about a second, loses it, and discards every later frame in
# silence — "unattended box, healthy console, no audio". Its root cause was characterized and fixed
# in the LIBRARY (a legal RX packet exceeding SO_SNDBUF was refused, writerLoop closed the session,
# and closing it released the TX channel), and the library has named the direction and size of that
# failed send ever since. None of it reached this bridge: na_hamlib_bridge registered no
# na_server_callbacks table, so the C ABI forwarded onError to nobody and every server fault was
# constructed, dispatched, and dropped. That is what this arm guards.
#
# It cannot reproduce the ORIGINAL fault — that needs an RX frame over 9216 bytes, and the Hamlib
# dummy tops out at 2880 (measured in #17's own investigation). It does not need to. What was broken
# was DELIVERY, one seam shared by every server fault, so any real notifyError exercises it. An
# abruptly-killed client is the cheap one: no clean DISCONNECT, so the session dies on the UDP
# connection timeout (UdpClientConnection.hpp UDP_CONNECTION_TIMEOUT_MS = 8000) and the server
# reports it exactly as it would report a refused send.
#
# FOUR ASSERTIONS, AND THEY FAIL SEPARATELY ON PURPOSE:
#   1  tx owner acquired           the client really did take the channel (else 2 is vacuous)
#   2  tx owner released after     the lapse itself is logged, with duration and bytes delivered
#   3  server error [...]          the CAUSE reaches the operator — the registration this fixes
#   4  client ... disconnected     the roster change reaches the operator
# 1 is what keeps the rest honest: without it a bridge that never saw a client would satisfy "no
# audio was silently lost" by never having any. The negative control lives in run_arm above, which
# asserts an undisturbed run prints no `server error` at all.
#
# The probe is SIGKILLed rather than asked to stop: a clean na_client_disconnect is an ordinary
# departure the server has no reason to report, and asserting on it would test nothing.
run_lapse_arm () {
    log="$workdir/bridge.lapse.log"
    plog="$workdir/probe.lapse.log"

    "$BRIDGE" -m 1 -S loopback -p "$PORT" >"$log" 2>&1 &
    bpid=$!
    ready=0
    for _ in $(seq 1 100); do
        if ! kill -0 "$bpid" 2>/dev/null; then break; fi
        if grep -q -- '-> naudio :' "$log" 2>/dev/null; then ready=1; break; fi
        sleep 0.1
    done
    if [ "$ready" -eq 0 ]; then
        echo "FAIL bridge_arm/lapse: the bridge never reported itself listening" >&2
        sed 's/^/    | /' "$log" >&2
        na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "bridge_arm/lapse bridge"
        return 2
    fi

    # --seconds outlives the arm deliberately: the probe must still be transmitting when it is
    # killed, so the channel is lost mid-transmission rather than released on its way out. This one
    # needs no na_run_deadline: it is BACKGROUNDED and every gate between here and the na_stop_pid
    # that ends it is itself bounded, so its lifetime is already capped by the arm's own polls.
    "$PROBE" --port "$PORT" --seconds 60 --tx --expect content >"$plog" 2>&1 &
    ppid=$!

    # Wait for the client to ARRIVE before killing anything. Sleeping a constant here would race the
    # connect on a loaded runner and kill a probe that never transmitted, reporting a harness timing
    # fault as a bridge defect.
    #
    # The gate keys on `clients=1` from the periodic health block — NOT on any line this issue added.
    # That matters: gating on `tx owner acquired` was the first version, and it made deleting the
    # ownership log look like "no client ever connected" (a harness fault, exit 2) instead of the
    # regression it is. A readiness gate must never be the thing under test, or its absence and the
    # defect's presence become the same observation. `clients=` is na_server_client_count, which
    # predates all of this. Costs up to one 5 s health tick.
    arrived=0
    for _ in $(seq 1 150); do
        if ! kill -0 "$ppid" 2>/dev/null; then break; fi
        if grep -q 'clients=1' "$log" 2>/dev/null; then arrived=1; break; fi
        sleep 0.1
    done
    if [ "$arrived" -eq 0 ]; then
        echo "FAIL bridge_arm/lapse: no client ever reached the bridge, so this arm cannot" >&2
        echo "  measure a lapse (harness fault, not a bridge defect)." >&2
        sed 's/^/    | /' "$log" >&2
        sed 's/^/    p /' "$plog" >&2
        na_stop_pid "$ppid" KILL "$NA_STOP_DEADLINE" "bridge_arm/lapse probe"
        na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "bridge_arm/lapse bridge"
        return 2
    fi
    # A client is provably present, so from here the ownership line's absence is a REGRESSION, not a
    # timing fault. Bounded wait: the probe keys up immediately after connect, and `clients=1` may
    # have been printed on a tick before ownership was claimed.
    for _ in $(seq 1 100); do
        if grep -q 'tx owner acquired' "$log" 2>/dev/null; then break; fi
        if ! kill -0 "$ppid" 2>/dev/null; then break; fi
        sleep 0.1
    done

    na_stop_pid "$ppid" KILL "$NA_STOP_DEADLINE" "bridge_arm/lapse probe"

    # Deadline past the 8 s connection timeout with room for a slow runner. Polling the log rather
    # than sleeping the worst case keeps the arm at about the timeout's length on a healthy machine.
    for _ in $(seq 1 250); do
        if ! kill -0 "$bpid" 2>/dev/null; then break; fi
        if grep -q 'server error' "$log" 2>/dev/null; then break; fi
        sleep 0.1
    done

    na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "bridge_arm/lapse bridge"
    bexit=$NA_DEADLINE_RC

    arc=0
    if ! grep -q 'tx owner acquired' "$log" 2>/dev/null; then
        echo "FAIL bridge_arm/lapse: the bridge never logged taking the TX channel" >&2
        arc=1
    fi
    if ! grep -q 'tx owner released after' "$log" 2>/dev/null; then
        echo "FAIL bridge_arm/lapse: TX ownership was lost and the bridge never said so. This is" >&2
        echo "  issue #17's silence: every later frame is discarded correctly and invisibly." >&2
        arc=1
    fi
    if ! grep -q 'server error' "$log" 2>/dev/null; then
        echo "FAIL bridge_arm/lapse: the server reported a fault and the bridge printed nothing." >&2
        echo "  na_hamlib_bridge must register an na_server_callbacks table with on_error set" >&2
        echo "  BEFORE na_server_start, or the C ABI forwards onError to nobody and a lost" >&2
        echo "  session has no stated cause — issue #17's open half regressing." >&2
        arc=1
    fi
    if ! grep -q 'disconnected' "$log" 2>/dev/null; then
        echo "FAIL bridge_arm/lapse: the client was dropped and the roster change was not logged" >&2
        arc=1
    fi
    if [ "$bexit" -ne 0 ]; then
        echo "FAIL bridge_arm/lapse: bridge exited $bexit on SIGINT, expected 0" >&2
        arc=1
    fi
    if [ "$arc" -ne 0 ]; then
        sed 's/^/    | /' "$log" >&2
        return "$arc"
    fi
    echo "  bridge_arm/lapse: OK ($(grep -c 'server error' "$log") server error line(s), TX episode logged)"
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
# Same reasoning again: "the bridge delivers no audio" and "the bridge loses a session in silence"
# are different diagnoses, and this arm is the only one that can report the second.
run_lapse_arm           || { arc=$?; [ "$rc" -eq 0 ] && rc=$arc; }

if [ "$rc" -eq 0 ]; then
    echo "bridge_arm: OK — content on tone, silence on silence, a failed startup exits non-zero," \
         "and a lost TX channel names its cause"
fi
exit "$rc"
