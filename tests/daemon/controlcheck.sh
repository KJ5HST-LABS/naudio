#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio tests — the local control page served by na_audio_daemon --mode control (issue #96,
# item 4).
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# The control page is the surface an operator reaches instead of a terminal, which makes its
# failures the ones nobody is positioned to diagnose: a browser shows a blank panel and there is
# no exit code anywhere. So this arm drives the real HTTP server in the real binary and asserts
# on status codes and bodies — never on the daemon merely having started.
#
# Four things are checked:
#
#   1. It serves. The page and its script come back with the right status and content type, and
#      the script is a separate asset precisely so the page's CSP can forbid inline script — an
#      assertion, because collapsing it back into one file would silently weaken that header.
#   2. It refuses the three browser attacks. A wrong Host (DNS rebinding), a foreign Origin
#      (cross-site fetch), and a mutating request that is not application/json (the one an HTML
#      form can produce) are each refused — and each has a POSITIVE CONTROL alongside it, the
#      same request with only that one header corrected, which must succeed. A refusal that
#      fires for the wrong reason proves nothing.
#   3. It validates like the command line. A bad value is refused with the SAME message the flag
#      parser gives, the config file is left untouched by every refusal, and a valid write
#      round-trips: what the page posts is what the file says and what the next read reports.
#   4. It is honest about what it did not do. Every case here is decided before any device is
#      opened; see the containment note below.
#
# CONTAINMENT — why this is hardware-free despite driving the hardware tool. Control mode opens
# no device at all until something asks it to: `--autostart false` (asserted here, not assumed)
# means the pipeline stays idle at startup, and this script never posts action=start. The one
# endpoint that touches PortAudio is /api/devices, which ENUMERATES and opens nothing; whether
# even that works is decided by asking this host first (see the --list-devices probe), so the
# assertion is definite on both kinds of machine rather than tolerant on either.
#
# The arm ends with a can-fail control (L222): the same assertions are re-run against a port
# with nothing on it, and they must fail. An arm that cannot produce a failure has not shown
# that its passes mean anything.
set -u

DAEMON="${1:?usage: controlcheck.sh <path-to-na_audio_daemon> [python] [curl]}"
# Both tools are LOCATED BY CMAKE and passed in, rather than looked up here: on Windows the
# interpreter is `python`, not `python3`, and a bare `python3` would fail as "command not found"
# in a way that reads like a defect in the daemon. CMake registers this arm only where it found
# both, so reaching this script means they exist — the fallbacks are for running it by hand.
PYTHON="${2:-python3}"
CURL="${3:-curl}"
[ -x "$DAEMON" ] || { echo "FAIL: not executable: $DAEMON"; exit 1; }
command -v "$CURL" > /dev/null 2>&1 || {
    echo "FAIL: no HTTP client at '$CURL' — this arm cannot run without one"; exit 1; }
command -v "$PYTHON" > /dev/null 2>&1 || {
    echo "FAIL: no Python at '$PYTHON' — needed to read JSON responses"; exit 1; }

TMP="$(mktemp -d)"
CONF="$TMP/daemon.conf"
: > "$CONF"
DAEMON_PID=""
cleanup() {
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2> /dev/null
    [ -n "$DAEMON_PID" ] && wait "$DAEMON_PID" 2> /dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

fails=0
ok()  { echo "ok: $*"; }
bad() { echo "FAIL: $*"; fails=$((fails + 1)); }

# ---- start the daemon -------------------------------------------------------------------------
# --control-port 0 asks the OS for a free port: a fixed one would make two of these arms running
# at once (a -j ctest, two checkouts) fail as a port clash and read like a defect in the daemon.
# --duration-ms is a backstop only — the trap above is what normally stops it — but without one a
# harness that died between here and the kill would leave a daemon running forever.
"$DAEMON" --mode control --config "$CONF" --control-port 0 --autostart false \
          --duration-ms 120000 > "$TMP/daemon.log" 2>&1 &
DAEMON_PID=$!

URL=""
for _ in $(seq 1 100); do
    URL="$(sed -n 's/^control page : //p' "$TMP/daemon.log" 2> /dev/null | tr -d '\r')"
    [ -n "$URL" ] && break
    kill -0 "$DAEMON_PID" 2> /dev/null || break
    sleep 0.1
done
if [ -z "$URL" ]; then
    echo "FAIL: the daemon never announced a control page"
    cat "$TMP/daemon.log"
    exit 1
fi
echo "controlcheck: $URL (config $CONF)"
PORT="$(printf '%s' "$URL" | sed -E 's|.*:([0-9]+)/?$|\1|')"

# ---- helpers ----------------------------------------------------------------------------------
# Every request goes through here so no case can accidentally omit a header the daemon requires
# and then blame the endpoint for the refusal.
# status <path> [curl args...] -> the HTTP status code alone
status() {
    local path="$1"; shift
    # ${URL%/} strips the announced URL's trailing slash: the paths below all begin with one,
    # and "http://host:port//api/state" is a DIFFERENT path that the router correctly 404s.
    "$CURL" -sS -o "$TMP/body" -w '%{http_code}' "$@" "${URL%/}${path}" 2> "$TMP/curlerr"
}
body() { cat "$TMP/body"; }

# A JSON field, read with python3 rather than a regex: a message containing a comma or a quote
# would silently truncate a sed-based extraction, and several assertions below are ON messages.
jget() {  # jget <dotted.path> ; reads $TMP/body
    "$PYTHON" - "$1" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(__import__("os").environ["NA_BODY"]))
except Exception:
    sys.exit(1)
for part in sys.argv[1].split("."):
    if isinstance(d, dict) and part in d:
        d = d[part]
    else:
        sys.exit(1)
# Bools go through json.dumps too, so a test compares against "true"/"false" rather than
# Python's "True"/"False" — the JSON spelling is the one the reader expects.
print("" if d is None else (d if isinstance(d, str) else json.dumps(d)))
PY
}
export NA_BODY="$TMP/body"

expect() {  # expect <what> <wanted-status> <path> [curl args...]
    local what="$1" want="$2" path="$3"; shift 3
    local got
    got="$(status "$path" "$@")"
    if [ "$got" = "$want" ]; then
        ok "$what -> $got"
        return 0
    fi
    bad "$what -> $got, expected $want"
    echo "  body: $(body | head -c 300)"
    return 1
}

CT='Content-Type: application/json'
ORIGIN="Origin: http://127.0.0.1:$PORT"

# ================================================================================================
# 1/4 — it serves
# ================================================================================================
expect "GET /" 200 "/"
body | grep -q 'naudio control' \
    && ok "the page is the control page" || bad "GET / did not return the control page"
# The script must be its own asset. If it is ever inlined the page still works and the CSP below
# silently stops protecting anything, so this is asserted rather than left to review.
body | grep -q '<script src="/app.js">' \
    && ok "the page loads its script from /app.js, not inline" \
    || bad "the page no longer references /app.js — an inlined script defeats script-src 'self'"

expect "GET /app.js" 200 "/app.js"
body | grep -q "addEventListener" && ok "/app.js carries the page script" || bad "/app.js is not the script"

# The header that makes the separation worth having.
"$CURL" -sS -D "$TMP/head" -o /dev/null "$URL" 2> /dev/null
grep -qi "^content-security-policy:.*script-src 'self'" "$TMP/head" \
    && ok "CSP restricts script to 'self'" || bad "CSP does not carry script-src 'self'"
grep -qi "^content-type: text/html" "$TMP/head" \
    && ok "the page is served as text/html" || bad "wrong content type for the page"

expect "GET /nope" 404 "/nope"

# ================================================================================================
# 2/4 — the three browser defences, each with its own positive control
# ================================================================================================
# DNS rebinding: an attacker's hostname that resolves to 127.0.0.1 arrives with THEIR name here.
expect "GET / with a foreign Host" 421 "/" -H "Host: evil.example"
# Control: the same request differing only in Host must succeed, or the 421 above proves nothing
# (it could have been refused for any other reason).
expect "control: GET / with our own Host" 200 "/" -H "Host: 127.0.0.1:$PORT"
expect "control: GET / with localhost:PORT" 200 "/" -H "Host: localhost:$PORT"

# Cross-site fetch: the browser attaches the attacker page's origin.
expect "GET /api/state from a foreign Origin" 403 "/api/state" -H "Origin: http://evil.example"
expect "control: GET /api/state from our own Origin" 200 "/api/state" -H "$ORIGIN"

# The one an HTML form can actually produce. A cross-origin form POST cannot set a JSON content
# type, and a cross-origin fetch cannot either without a preflight this server never answers.
expect "POST as an HTML form would send it" 415 "/api/stream" \
    -X POST -H 'Content-Type: application/x-www-form-urlencoded' -d 'action=stop'
expect "POST with text/plain" 415 "/api/stream" \
    -X POST -H 'Content-Type: text/plain' -d '{"action":"stop"}'
expect "control: the same POST as application/json" 200 "/api/stream" \
    -X POST -H "$CT" -d '{"action":"stop"}'

# ================================================================================================
# 3/4 — it validates like the command line, and never half-writes
# ================================================================================================
expect "GET /api/state" 200 "/api/state"
[ "$(jget stream.state)" = "idle" ] \
    && ok "the pipeline is idle at startup (--autostart false)" \
    || bad "the pipeline is not idle at startup: '$(jget stream.state)' — this arm is NOT hardware-free"
# Matched on the SCRATCH DIRECTORY plus the file name, never on the full path. Git Bash
# rewrites /tmp/tmp.XXXX/daemon.conf into C:/Users/.../Temp/tmp.XXXX/daemon.conf on the way to
# a native binary, so the daemon truthfully reports a path this script never typed. The scratch
# directory's name is unique to this run, which keeps the assertion sharp — it still fails if
# the daemon reported the DEFAULT location instead of the one it was given. Same rule as
# configcheck.sh's basename-only path assertions, and for the same reason.
case "$(jget configPath)" in
    *"$(basename "$TMP")"[/\\]daemon.conf)
        ok "the page edits the config file this invocation read" ;;
    *)  bad "configPath is '$(jget configPath)', which is not this run's scratch daemon.conf" ;;
esac
[ "$(jget settings.transport.value)" = "tcp" ] \
    && ok "settings are reported (transport = tcp)" || bad "settings.transport missing"

# The config file starts empty; every refusal below must leave it that way.
before="$(cat "$CONF")"
[ -z "$before" ] || bad "the scratch config file did not start empty"

refuse() {  # refuse <what> <json-body> <message-substring>
    local what="$1" payload="$2" want="$3"
    if ! expect "$what" 400 "/api/config" -X POST -H "$CT" -d "$payload"; then return; fi
    local msg; msg="$(jget error)"
    case "$msg" in
        *"$want"*) ok "  ... refused with: $msg" ;;
        *) bad "  ... refused, but the message was '$msg' (wanted '$want')" ;;
    esac
    if [ "$(cat "$CONF")" != "$before" ]; then
        bad "  ... and it WROTE the config file on a refusal"
        before="$(cat "$CONF")"
    fi
}

# The message is the flag parser's own, word for word — the settings table is shared, and a
# divergence here would mean the page and the command line had drifted apart.
refuse "POST a non-numeric port" '{"port":"abc"}' "--port expects an integer, got 'abc'"
refuse "POST an out-of-range port" '{"port":"99999"}' "--port must be in [0, 65535], got '99999'"
refuse "POST an unknown key" '{"junkkey":"1"}' "unknown setting 'junkkey'"
# The whole-config checks main() applies to a command line. Without them the page could write a
# file that makes the daemon refuse to start at the next logon.
# 11025, not 44100: 44100 IS divisible by 50 (882 frames per 20 ms) and the daemon is right to
# accept it. 11025 is the realistic operator mistake that genuinely cannot be framed exactly.
refuse "POST a rate that is not a 20 ms frame" '{"rate":"11025"}' "does not make an exact 20 ms frame"
refuse "POST an invalid transport" '{"transport":"carrier-pigeon"}' "invalid transport"
refuse "POST a body that is not an object" '"nope"' "body is not a JSON object"
refuse "POST a nested value" '{"port":{"n":"1"}}' "must be a string"
refuse "POST a numeric JSON value" '{"port":4533}' "must be a string"

expect "POST a bogus stream action" 400 "/api/stream" -X POST -H "$CT" -d '{"action":"levitate"}'

# ---- the write that must work, and must round-trip --------------------------------------------
expect "POST a valid settings change" 200 "/api/config" -X POST -H "$CT" \
    -d '{"transport":"udp","port":"5001","channels":"1","autostart":"true","capture":"USB Codec"}'

for want in 'transport = udp' 'port = 5001' 'channels = 1' 'autostart = true' 'capture = USB Codec'; do
    grep -qx -- "$want" "$CONF" \
        && ok "the file says '$want'" || bad "the file is missing '$want'"
done
# A value equal to its default is deliberately NOT written: a file full of defaults freezes them.
grep -q '^rate' "$CONF" \
    && bad "an unchanged default (rate) was written into the file" \
    || ok "unchanged defaults are left out of the file"
# Spaces in a value survive without quoting — the reason the format is `key = value` at all.
grep -qx -- 'capture = USB Codec' "$CONF" \
    && ok "a value with a space needs no quoting" || bad "a spaced value did not round-trip"

expect "GET /api/state after the write" 200 "/api/state"
[ "$(jget settings.transport.value)" = "udp" ] && ok "the daemon now reports transport = udp" \
    || bad "the write did not reach the daemon's own settings"
[ "$(jget settings.autostart.value)" = "true" ] && ok "the daemon now reports autostart = true" \
    || bad "autostart did not reach the daemon's settings"

# The file the page wrote must be a file the daemon accepts. This is the assertion that catches a
# renderer emitting a key the parser does not take — the page would look like it worked and the
# service would refuse to start at the next logon, which is exactly the failure #96 exists to end.
if "$DAEMON" --config "$CONF" --mode zzz > "$TMP/reread.log" 2>&1; then
    bad "re-reading the written config did not reach the mode guard"
else
    grep -q "invalid --mode 'zzz'" "$TMP/reread.log" \
        && ok "the daemon re-reads the file the page wrote" \
        || { bad "the daemon rejected its own control page's config file"; cat "$TMP/reread.log"; }
fi

# ---- devices ----------------------------------------------------------------------------------
# Whether PortAudio can enumerate is a property of the HOST, not of this code, so ask the host
# rather than tolerating either answer: --list-devices runs the same enumeration through a
# different entry point and opens no stream.
if "$DAEMON" --list-devices --no-config > "$TMP/devs.log" 2>&1; then
    expect "GET /api/devices where PortAudio works" 200 "/api/devices"
    [ -n "$(jget devices.capture)" ] && ok "the device list carries a capture array" \
        || bad "no capture array in /api/devices"
    [ -n "$(jget devices.playback)" ] && ok "the device list carries a playback array" \
        || bad "no playback array in /api/devices"
    [ "$(jget stale)" = "false" ] && ok "a fresh scan is not marked stale" \
        || bad "a fresh scan reported stale=$(jget stale)"
else
    echo "note: PortAudio cannot enumerate on this host (--list-devices failed); asserting the"
    echo "      error path instead of skipping"
    expect "GET /api/devices where PortAudio does not work" 500 "/api/devices"
    case "$(jget error)" in
        *"device enumeration failed"*) ok "  ... and says why" ;;
        *) bad "  ... with no explanation: '$(jget error)'" ;;
    esac
fi

# ================================================================================================
# 4/4 — can-fail control (L222)
# ================================================================================================
# Everything above ran against a live daemon. Re-point the same helper at a port with nothing on
# it: if the assertions still pass, they were never reaching the server and none of this counts.
live_url="$URL"
URL="http://127.0.0.1:1/"
control_fails=0
status "/api/state" > /dev/null 2>&1
[ "$(status "/api/state")" = "200" ] && control_fails=1
URL="$live_url"
if [ "$control_fails" -eq 0 ]; then
    ok "control fires: the same check against a dead port does not return 200"
else
    bad "control did NOT fire — these assertions do not depend on the server answering"
fi

if [ "$fails" -ne 0 ]; then
    echo "controlcheck: $fails case(s) FAILED"
    echo "--- daemon log ---"
    cat "$TMP/daemon.log"
    exit 1
fi
echo "controlcheck: all cases passed"
