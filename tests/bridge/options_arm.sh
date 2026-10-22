#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio tests — na_hamlib_bridge's -R transports, driven for real.
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# Registered beside naudio_bridge_arm and only where it is (CMakeLists.txt): it needs the bridge,
# which needs a hand-built streaming libhamlib.
#
# -R. A client that selects no reliability profile is on the C ABI's default transport, plain TCP,
# and the bridge used to serve UDP only — `parse_profile` turned any name it did not know, `tcp`
# included, into `wan` without a word. So:
#   tcp               -R tcp serves a TCP client the dummy's tone
#   dual              -R dual serves the same tone to a TCP client and to a UDP client, on one port
#   control/wan-tcp   -R wan REFUSES a TCP client. The control for the two rows above: a probe whose
#                     `--profile tcp` quietly ran UDP would pass them both, and fails this one
#   bogus             -R bogus exits 2 with the usage text instead of coming up as wan
#
# macOS SIP TRAP (CLAUDE.md Learning 6): every child is launched DIRECTLY from this script, never
# through `timeout` or another shell — see deadline.sh.
set -u

. "$(dirname "${BASH_SOURCE[0]}")/deadline.sh"

BRIDGE="${1:?usage: options_arm.sh <na_hamlib_bridge> <naudio_bridge_probe>}"
PROBE="${2:?usage: options_arm.sh <na_hamlib_bridge> <naudio_bridge_probe>}"
PORT="${NA_BRIDGE_OPTIONS_PORT:-4602}"
SECS="${NA_BRIDGE_OPTIONS_SECONDS:-4}"
# Long enough for a refusal to have happened, short enough that a bridge which came up instead of
# refusing costs seconds, not minutes.
REFUSE_DEADLINE=10

for exe in "$BRIDGE" "$PROBE"; do
    if [ ! -x "$exe" ]; then
        echo "options_arm: $exe is not executable — nothing to drive" >&2
        exit 2
    fi
done

workdir=$(mktemp -d)
cleanup() { rm -rf "$workdir"; na_harness_guard "options_arm"; }
trap cleanup EXIT

rc=0
fail () { [ "$rc" -eq 0 ] && rc="$1"; }

# Start a bridge with the given arguments and wait for its listening line. Sets $bpid and $blog.
# Returns 0 when listening, 2 (harness fault) when it never got there.
start_bridge () {
    local label="$1"; shift
    blog="$workdir/bridge.${label//\//_}.log"
    "$BRIDGE" -m 1 -p "$PORT" "$@" >"$blog" 2>&1 &
    bpid=$!
    local i
    for i in $(seq 1 100); do
        if ! kill -0 "$bpid" 2>/dev/null; then break; fi
        if grep -q -- '-> naudio :' "$blog" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    echo "FAIL options_arm/$label: the bridge never reported itself listening" >&2
    sed 's/^/    | /' "$blog" >&2
    na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "options_arm/$label bridge"
    return 2
}

# Run one probe against the running bridge. Echoes nothing; returns the probe's 0/1/2.
probe () {
    local label="$1"; shift
    na_run_deadline $(( SECS + NA_PROBE_MARGIN )) "options_arm/$label probe" - \
        "$PROBE" --port "$PORT" --seconds "$SECS" "$@"
    local prc=$NA_DEADLINE_RC
    [ "$prc" -eq 124 ] && prc=2
    return "$prc"
}

# Stop the bridge started by start_bridge and require an orderly exit and a quiet log.
stop_bridge () {
    local label="$1"
    na_stop_pid "$bpid" INT "$NA_STOP_DEADLINE" "options_arm/$label bridge"
    local bexit=$NA_DEADLINE_RC
    if [ "$bexit" -ne 0 ]; then
        echo "FAIL options_arm/$label: bridge exited $bexit on SIGINT, expected 0" >&2
        return 1
    fi
    if grep -q 'server error' "$blog" 2>/dev/null; then
        echo "FAIL options_arm/$label: the bridge reported a server error on an undisturbed run" >&2
        sed -n '/server error/s/^/    | /p' "$blog" >&2
        return 1
    fi
    return 0
}

# The startup line names the profile it serves; a bridge that came up on another one is the
# fault this arm exists for, so read it back rather than trusting the flag.
served_profile () {
    sed -n 's/.*  profile=\([a-z0-9]*\)  .*/\1/p' "$blog" | head -n 1
}

# One served row: run_served <label> <profile the bridge must report> <expect> <client profiles>
# <bridge args...>. One probe per client profile, each expecting <expect>.
run_served () {
    local label="$1" want_profile="$2" expect="$3" clients="$4"; shift 4
    start_bridge "$label" "$@" || return $?
    local arc=0 got
    got=$(served_profile)
    if [ "$got" != "$want_profile" ]; then
        echo "FAIL options_arm/$label: the bridge serves profile=${got:-?}, expected $want_profile" >&2
        arc=1
    fi
    local p prc
    for p in $clients; do
        probe "$label/$p" --profile "$p" --expect "$expect"
        prc=$?
        case "$prc" in
            0) echo "  options_arm/$label: OK — a $p client got $expect" ;;
            1) echo "FAIL options_arm/$label: a $p client expected $expect and did not get it" >&2
               [ "$arc" -eq 0 ] && arc=1 ;;
            *) echo "FAIL options_arm/$label: a $p client could not attach or the probe could not run" >&2
               [ "$arc" -eq 0 ] && arc=2 ;;
        esac
    done
    stop_bridge "$label" || { [ "$arc" -eq 0 ] && arc=1; }
    [ "$arc" -ne 0 ] && sed 's/^/    | /' "$blog" >&2
    return "$arc"
}

# The control: a TCP client against the default -R wan must be refused at connect. The probe exits 2
# when it cannot attach — that is the PASS here, and anything else fails the row.
run_wan_refuses_tcp () {
    local label="control/wan-tcp"
    start_bridge "$label" -S tone || return $?
    local arc=0
    probe "$label" --profile tcp --expect content
    local prc=$?
    if [ "$prc" -eq 2 ]; then
        echo "  options_arm/$label: OK — -R wan refuses a TCP client"
    else
        echo "FAIL options_arm/$label: a TCP client against -R wan exited $prc, expected 2 (refused)." >&2
        echo "  Either the probe's --profile tcp is not TCP, which makes the tcp and dual rows" >&2
        echo "  meaningless, or -R wan now serves TCP." >&2
        arc=1
    fi
    stop_bridge "$label" || { [ "$arc" -eq 0 ] && arc=1; }
    return "$arc"
}

# One refusal row: the bridge must exit on its own with exactly $want_exit, never come up, and its
# output must match $must (an ERE).
run_refused () {
    local label="$1" want_exit="$2" must="$3"; shift 3
    local log="$workdir/bridge.${label//\//_}.log"
    "$BRIDGE" -m 1 -p "$PORT" "$@" >"$log" 2>&1 &
    local pid=$!
    na_wait_pid "$pid" "$REFUSE_DEADLINE" "options_arm/$label bridge"
    local bexit=$NA_DEADLINE_RC arc=0
    if [ "$bexit" -eq 124 ]; then
        echo "FAIL options_arm/$label: the bridge was still running ${REFUSE_DEADLINE}s later" >&2
        echo "  instead of refusing to start" >&2
        arc=1
    elif [ "$bexit" -ne "$want_exit" ]; then
        echo "FAIL options_arm/$label: the bridge exited $bexit, expected $want_exit" >&2
        arc=1
    fi
    if grep -q -- '-> naudio :' "$log"; then
        echo "FAIL options_arm/$label: the bridge came up and served before it stopped" >&2
        arc=1
    fi
    if ! grep -Eq -- "$must" "$log"; then
        echo "FAIL options_arm/$label: nothing in its output matches /$must/" >&2
        arc=1
    fi
    if [ "$arc" -ne 0 ]; then
        sed 's/^/    | /' "$log" >&2
        return "$arc"
    fi
    echo "  options_arm/$label: OK (exit $bexit, $(grep -Em1 -- "$must" "$log"))"
    return 0
}

echo "options_arm: $BRIDGE on :$PORT, ${SECS}s per probe"

# Every row runs whatever an earlier one did: each names a different fault.
run_served tcp  tcp  content "tcp"     -S tone -R tcp  || fail $?
run_served dual dual content "tcp ft8" -S tone -R dual || fail $?
run_wan_refuses_tcp || fail $?
run_refused bogus 2 '^usage: ' -S tone -R bogus || fail $?

if [ "$rc" -eq 0 ]; then
    echo "options_arm: OK — -R tcp and -R dual serve TCP clients, -R wan refuses them, and an" \
         "unknown -R is refused"
fi
na_harness_done
exit "$rc"
