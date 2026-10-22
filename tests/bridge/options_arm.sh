#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio tests — na_hamlib_bridge's -R transports and -C configuration tokens, driven for real.
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
#   tcp-udp           -R tcp REFUSES a UDP client: tcp is TCP only, not dual. Costs the probe's 10 s
#                     handshake timeout, since nothing answers a UDP connect to a TCP-only port
# A refusal counts only with the probe's own "connect … failed" line: a probe that wedged or could
# not run also exits 2, and must not pass a refusal row.
#   bogus             -R bogus exits 2 with the usage text instead of coming up as wan
#
# -C. A Hamlib configuration token set before rig_open — an Icom network radio's login is two of
# them (net_username, net_password). The dummy backend's stream_mode is the token every streaming
# libhamlib has, and its default is `tone`, so:
#   conf/silence      -C stream_mode=silence delivers silence. Content here means -C reached nothing
#   conf/after-S      -S silence -C stream_mode=tone delivers content: -C acts as -S does, and is
#                     applied after it
#   conf/after-r      -r A -C rig_pathname=B applies B, which the startup line reads back from
#                     libhamlib: -C is applied after -r too
#   conf/unknown      a token the model lacks stops the bridge (exit 1), naming the token
#   conf/no-equals    -C stream_mode, with no '=', stops it (exit 2), naming the token
#   conf/colon        -C stream_mode:<value> and -C 'stream_mode <value>' (the likeliest typos of
#   conf/space        a password option) stop it (exit 2), naming at most the leading name
#   conf/bad-name     a token name with a character no token has, before an '=', likewise
#   conf/empty-token  -C =<value> stops it (exit 2) and prints no argument at all
#   conf/limit        a 65th -C stops it (exit 2): the bridge keeps 64
#   conf/refused      a value the backend refuses stops it (exit 1), naming the token
#   conf/open-fails   a rig_open that fails after -C stops it (exit 1) without printing a -C value
# Every row that puts a value on the command line also requires that the VALUE never reaches the
# output: for a network radio it is a password.
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
SECRET="s3cret-VALUE-never-printed"
# conf/open-fails needs a -C value that libhamlib records in its saved debug history, and `client`
# is the token every streaming libhamlib logs that way ("Client claims to be <value>", at verbose
# level, so recorded but not printed). It must name a client libhamlib knows — it matches them
# without regard to case — or libhamlib prints the value itself at ERR level, which is not the
# bridge's doing. This spelling appears nowhere else in the bridge's output.
CLIENT_VALUE="wSjTx"

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

# A refusal row: run_refuses_client <label> <-R> <client profile>. The client must be refused at
# connect: the probe exits 2 AND prints its "connect to … failed" line. Any other outcome fails.
run_refuses_client () {
    local label="$1" profile="$2" client="$3"
    start_bridge "$label" -S tone -R "$profile" || return $?
    local arc=0 plog="$workdir/probe.${label//\//_}.log"
    na_run_deadline $(( SECS + NA_PROBE_MARGIN )) "options_arm/$label probe" "$plog" \
        "$PROBE" --port "$PORT" --seconds "$SECS" --profile "$client" --expect content
    local prc=$NA_DEADLINE_RC
    if [ "$prc" -eq 2 ] && grep -q "^na_bridge_probe: connect to 127.0.0.1:$PORT failed" "$plog"; then
        echo "  options_arm/$label: OK — -R $profile refuses a $client client"
    else
        echo "FAIL options_arm/$label: a $client client against -R $profile exited $prc without the" >&2
        echo "  probe's connect-failed line; expected it to be refused at connect. Either the" >&2
        echo "  probe's --profile $client is not the transport it names, or -R $profile serves it." >&2
        sed 's/^/    p /' "$plog" >&2
        arc=1
    fi
    stop_bridge "$label" || { [ "$arc" -eq 0 ] && arc=1; }
    return "$arc"
}

# One started row: run_started <label> <must> <never> <bridge args...>. The bridge must come up, its
# startup line must match <must> (an ERE) and not contain <never>, and it must stop cleanly.
run_started () {
    local label="$1" must="$2" never="$3"; shift 3
    start_bridge "$label" "$@" || return $?
    local arc=0 line
    line=$(grep -m1 -- '-> naudio :' "$blog")
    if ! printf '%s\n' "$line" | grep -Eq -- "$must"; then
        echo "FAIL options_arm/$label: the startup line does not match /$must/" >&2
        arc=1
    fi
    if [ -n "$never" ] && printf '%s\n' "$line" | grep -Fq -- "$never"; then
        echo "FAIL options_arm/$label: the startup line names $never" >&2
        arc=1
    fi
    stop_bridge "$label" || { [ "$arc" -eq 0 ] && arc=1; }
    if [ "$arc" -ne 0 ]; then
        sed 's/^/    | /' "$blog" >&2
        return "$arc"
    fi
    echo "  options_arm/$label: OK ($line)"
    return 0
}

# One refusal row: run_refused <label> <exit> <must> <never> <bridge args...>. The bridge must exit
# on its own with exactly <exit>, never come up, and its output must match <must> (an ERE) and must
# not contain <never>, the -C value the row passed — "" for a row that passes none.
run_refused () {
    local label="$1" want_exit="$2" must="$3" never="$4"; shift 4
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
    if [ -n "$never" ] && grep -Fq -- "$never" "$log"; then
        echo "FAIL options_arm/$label: the -C value reached the output — it may be a password" >&2
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
run_refuses_client control/wan-tcp wan tcp || fail $?
run_refuses_client tcp-udp          tcp wan || fail $?
run_refused bogus 2 '^usage: ' "" -S tone -R bogus || fail $?

run_served conf/silence wan silence wan -C stream_mode=silence           || fail $?
run_served conf/after-S wan content wan -S silence -C stream_mode=tone || fail $?
# The dummy takes any rig_pathname and opens nothing with it, so both addresses are inert here.
run_started conf/after-r ' s80-path-from-C -> naudio :' 's80-path-from-r' \
    -S tone -r s80-path-from-r -C rig_pathname=s80-path-from-C || fail $?
run_refused conf/unknown   1 '-C no_such_token: ' "$SECRET" -C "no_such_token=$SECRET" || fail $?
run_refused conf/no-equals 2 '^na_hamlib_bridge: -C stream_mode: expected token=value' "" \
    -C stream_mode || fail $?
run_refused conf/colon       2 '^na_hamlib_bridge: -C stream_mode' "$SECRET" -C "stream_mode:$SECRET" \
    || fail $?
run_refused conf/space       2 '^na_hamlib_bridge: -C stream_mode' "$SECRET" -C "stream_mode $SECRET" \
    || fail $?
run_refused conf/bad-name    2 '^na_hamlib_bridge: -C stream_mode' "$SECRET" \
    -C "stream_mode:$SECRET=tone" || fail $?
run_refused conf/empty-token 2 "^na_hamlib_bridge: -C: no token name before '='" "$SECRET" \
    -C "=$SECRET" || fail $?
limit_args=()
for _ in $(seq 1 65); do limit_args+=(-C stream_mode=tone); done
run_refused conf/limit 2 '^na_hamlib_bridge: more than 64 -C options' "" "${limit_args[@]}" || fail $?
run_refused conf/refused   1 '-C stream_mode: '   "$SECRET" -C "stream_mode=$SECRET"   || fail $?
# -m 2 (the later -m wins over the helper's -m 1) against this arm's own port, where nothing
# listens for TCP: netrigctl's rig_open is refused at connect in every streaming libhamlib.
#
# The row has no positive control of its own. It means something only while libhamlib records the
# `client` value in its saved history — frontend_set_conf's VERBOSE "Client claims to be %s" in
# src/conf.c, present in 9f412fe, master 06335264d and 0748e823e. Upstream dropping that line
# would make this row pass whatever the bridge prints; the fault arm's open-fail row plants its own
# history line and checks that it is there.
run_refused conf/open-fails 1 '^na_hamlib_bridge: rig_open: ' "$CLIENT_VALUE" \
    -m 2 -r "127.0.0.1:$PORT" -C "client=$CLIENT_VALUE" || fail $?

if [ "$rc" -eq 0 ]; then
    echo "options_arm: OK — -R tcp and -R dual serve TCP clients, -R wan refuses them and -R tcp" \
         "refuses UDP ones, an unknown -R is refused, and -C reaches the backend after -S and stops the bridge on any failure" \
         "without printing its value, a failed rig_open included"
fi
na_harness_done
exit "$rc"
