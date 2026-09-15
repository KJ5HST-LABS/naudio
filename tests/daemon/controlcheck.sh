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
# Five things are checked:
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
#   5. A saved device follows the device, not its index (issue #107). A second daemon whose
#      device list is in-memory (--fake-devices) is started four times on ONE config file with
#      the list renumbered between runs: the choice the page writes must resolve to the same
#      NAME after the device before it disappears — and the stream, started, must capture that
#      name — and must refuse by name, not open the device now holding the id, when it is gone.
#
# CONTAINMENT — why this is hardware-free despite driving the hardware tool. Control mode opens
# no device at all until something asks it to: `--autostart false` (asserted here, not assumed)
# means the pipeline stays idle at startup, and this script posts action=start ONLY to the
# --fake-devices daemon of section 5, whose backend is in-memory and never touches PortAudio.
# The one endpoint on the real daemon that touches PortAudio is /api/devices, which ENUMERATES
# and opens nothing; whether even that works is decided by asking this host first (see the
# --list-devices probe), so the assertion is definite on both kinds of machine rather than
# tolerant on either.
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
FAKE_PID=""
cleanup() {
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2> /dev/null
    [ -n "$DAEMON_PID" ] && wait "$DAEMON_PID" 2> /dev/null
    [ -n "$FAKE_PID" ] && kill "$FAKE_PID" 2> /dev/null
    [ -n "$FAKE_PID" ] && wait "$FAKE_PID" 2> /dev/null
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
# 1/5 — it serves
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
# 2/5 — the three browser defences, each with its own positive control
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
# 3/5 — it validates like the command line, and never half-writes
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

# And a FLAG must never become a file setting. This daemon was started with `--mode control`
# and `--duration-ms 120000`, both differing from their defaults, so a rewrite that persisted
# the daemon's effective settings would write them here. It must not: the service units pass
# exactly these two flags on the command line so that no file can undo them (#96 items 1 and 2),
# and a page that wrote them back would reverse that silently — a later plain na_audio_daemon
# would start in control mode because a service had once saved its settings.
for leaked in mode duration-ms; do
    grep -q "^$leaked" "$CONF" \
        && bad "the page persisted '$leaked', which came from a FLAG, not from the file" \
        || ok "a flag-supplied value ($leaked) did not leak into the file"
done
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

# ---- the login service's switch (issue #102) --------------------------------------------------
# /api/state carries `service`: which service manager this build asks, whether the unit is
# installed, and whether its switch is on. The VALUES depend on the host (a developer's Mac with
# the agent registered reads true/true; a runner reads false/false), so what is asserted is the
# shape and the one relation that holds everywhere — a switch cannot be on for a unit that is
# not installed. The installer gate asserts the values on a host where it knows them.
expect "GET /api/state carries the service switch" 200 "/api/state"
if ! jget service > /dev/null; then
    bad "/api/state has no 'service' key"
else
    case "$(jget service.manager)" in
        launchd|systemd|task-scheduler) ok "service.manager names a service manager ($(jget service.manager))" ;;
        *) bad "service.manager is '$(jget service.manager)', not launchd, systemd or task-scheduler" ;;
    esac
    case "$(jget service.installed)" in
        true|false) ok "service.installed is a boolean ($(jget service.installed))" ;;
        *) bad "service.installed is '$(jget service.installed)', not a boolean" ;;
    esac
    case "$(jget service.enabled)" in
        true|false) ok "service.enabled is a boolean ($(jget service.enabled))" ;;
        *) bad "service.enabled is '$(jget service.enabled)', not a boolean" ;;
    esac
    if [ "$(jget service.enabled)" = "true" ] && [ "$(jget service.installed)" = "false" ]; then
        bad "service reads enabled without being installed — a switch on nothing"
    else
        ok "service.enabled implies service.installed"
    fi
fi

# The route that opens the pane owning the switch is a POST behind the same three defences as
# every other action — a page on the internet must not be able to pop System Settings open.
# The defences fire BEFORE routing, so a 403 alone would not prove the route exists; the
# well-formed request is what proves it, and it is platform-shaped: a 404 whose message names
# macOS where there is no pane, and a 200 on macOS — exercised there only under CI, because on
# a developer's Mac it opens System Settings on every run of the suite (said here rather than
# silently skipped; the release gate's installer run makes the same POST on a runner).
expect "POST /api/open-login-items from a foreign Origin" 403 "/api/open-login-items" \
    -X POST -H "$CT" -H "Origin: http://evil.example" -d '{}'
expect "POST /api/open-login-items as text/plain" 415 "/api/open-login-items" \
    -X POST -H 'Content-Type: text/plain' -d '{}'
expect "GET /api/open-login-items (not a GET route)" 404 "/api/open-login-items"
case "$(uname -s)" in
    Darwin)
        if [ "${CI:-}" = "true" ]; then
            expect "POST /api/open-login-items opens the pane (CI runner)" 200 \
                "/api/open-login-items" -X POST -H "$CT" -d '{}'
        else
            echo "note: not POSTing /api/open-login-items on a developer's Mac — it would open"
            echo "      System Settings on every run; CI runners and the release gate make that request"
        fi ;;
    *)
        expect "POST /api/open-login-items where there is no pane" 404 "/api/open-login-items" \
            -X POST -H "$CT" -d '{}'
        case "$(jget error)" in
            *macOS*) ok "  ... and says the pane is macOS's" ;;
            *) bad "  ... with the generic 404 message: '$(jget error)' — the route is not wired" ;;
        esac ;;
esac

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
# 4/5 — a saved device follows the device, not its index (issue #107)
# ================================================================================================
# A device id is a position in the enumeration at that moment. The operator's file on the Mac
# that found this said `capture-id = 1`; a USB microphone that had been id 0 went away, every
# id after it moved down one, and the same file opened the laptop's microphone as "the radio" —
# with no diagnostic, because a microphone is a microphone. The page now writes the device's
# NAME beside its id, and the daemon lets the name decide. That is proved here, not described:
# the same config file is read by three daemons whose in-memory device lists differ by one
# device, and what each resolves — and what the started stream actually captures — is read off
# /api/state. --fake-devices swaps PortAudio for that in-memory list; nothing here opens real
# hardware, and the paced-silence capture the fake serves is what the stream reads.
FAKE_CONF="$TMP/fake.conf"
: > "$FAKE_CONF"
REAL_URL="$URL"

start_fake_daemon() {  # start_fake_daemon <comma-separated device names>; sets URL, FAKE_PID
    : > "$TMP/fake.log"
    "$DAEMON" --mode control --config "$FAKE_CONF" --control-port 0 --autostart false \
              --duration-ms 120000 --fake-devices "$1" > "$TMP/fake.log" 2>&1 &
    FAKE_PID=$!
    URL=""
    for _ in $(seq 1 100); do
        URL="$(sed -n 's/^control page : //p' "$TMP/fake.log" 2> /dev/null | tr -d '\r')"
        [ -n "$URL" ] && break
        kill -0 "$FAKE_PID" 2> /dev/null || break
        sleep 0.1
    done
    if [ -z "$URL" ]; then
        bad "the --fake-devices daemon never announced a control page"
        cat "$TMP/fake.log"
        return 1
    fi
    echo "fake daemon [$1]: $URL"
}
stop_fake_daemon() {
    [ -n "$FAKE_PID" ] && kill "$FAKE_PID" 2> /dev/null
    [ -n "$FAKE_PID" ] && wait "$FAKE_PID" 2> /dev/null
    FAKE_PID=""
    URL="$REAL_URL"
}
# Wait for stream.state to reach one of the given values; prints the state it stopped on.
await_stream_state() {  # await_stream_state <state>[|<state>...]
    local want="$1" got=""
    for _ in $(seq 1 100); do
        status "/api/state" > /dev/null
        got="$(jget stream.state)"
        case "|$want|" in *"|$got|"*) break ;; esac
        sleep 0.1
    done
    printf '%s' "$got"
}

# Run 1 — three devices; the radio is [1]. Save it the way the page does: name AND id.
if start_fake_daemon "TKD Microphone,USB Audio Device,MacBook Pro Microphone"; then
    expect "fake run 1: GET /api/devices" 200 "/api/devices"
    [ "$(jget devices.capture)" != "" ] && ok "  ... three fake capture devices enumerate" \
        || bad "  ... the fake list is empty"
    expect "fake run 1: save the radio as name + id" 200 "/api/config" -X POST -H "$CT" \
        -d '{"capture":"USB Audio Device","capture-id":"1"}'
    grep -qx -- 'capture = USB Audio Device' "$FAKE_CONF" \
        && ok "  ... the file carries the name" || bad "  ... the file is missing the name"
    grep -qx -- 'capture-id = 1' "$FAKE_CONF" \
        && ok "  ... and the id" || bad "  ... the file is missing the id"
    expect "fake run 1: GET /api/state" 200 "/api/state"
    [ "$(jget resolved.capture.id)" = "1" ] && [ "$(jget resolved.capture.how)" = "name" ] \
        && [ "$(jget resolved.capture.moved)" = "false" ] \
        && ok "  ... resolves to [1] by name, not moved" \
        || bad "  ... resolved.capture is id=$(jget resolved.capture.id) how=$(jget resolved.capture.how) moved=$(jget resolved.capture.moved)"
    stop_fake_daemon
fi

# Run 2 — the first device is gone: every id after it moved down one. The SAME file must open
# the same radio, now [0]; the daemon that pinned the index would open "MacBook Pro Microphone".
if start_fake_daemon "USB Audio Device,MacBook Pro Microphone"; then
    expect "fake run 2: GET /api/devices after one device left" 200 "/api/devices"
    [ "$(jget resolved.capture.id)" = "0" ] \
        && [ "$(jget resolved.capture.name)" = "USB Audio Device" ] \
        && [ "$(jget resolved.capture.moved)" = "true" ] \
        && ok "  ... the saved choice resolves to [0] USB Audio Device and says it moved" \
        || bad "  ... resolved.capture is id=$(jget resolved.capture.id) name='$(jget resolved.capture.name)' moved=$(jget resolved.capture.moved)"
    expect "fake run 2: start the stream" 200 "/api/stream" -X POST -H "$CT" -d '{"action":"start"}'
    got="$(await_stream_state 'running|error|idle')"
    if [ "$got" = "running" ]; then
        ok "  ... the stream is running"
        [ "$(jget stream.captureName)" = "USB Audio Device" ] \
            && ok "  ... and it captures \"USB Audio Device\", not whatever now holds id 1" \
            || bad "  ... it captures '$(jget stream.captureName)' — the index was followed, not the device"
        grep -q 'server capture: device \[0\] USB Audio Device (matched by name "USB Audio Device", saved as device 1 — it moved)' "$TMP/fake.log" \
            && ok "  ... and the log names the device and the rule that chose it" \
            || bad "  ... the log's 'server capture' line does not say how the device was chosen"
    else
        bad "  ... the stream did not reach running (state '$got', error '$(jget stream.error)')"
    fi
    expect "fake run 2: stop the stream" 200 "/api/stream" -X POST -H "$CT" -d '{"action":"stop"}'
    [ "$(await_stream_state 'idle|error')" = "idle" ] && ok "  ... and it stopped" \
        || bad "  ... the stream did not return to idle"
    stop_fake_daemon
fi

# Run 3 — the radio is gone and a different device holds id 1. The name matches nothing, so the
# choice resolves to NOTHING, and Start refuses naming the device — it must not open [1].
if start_fake_daemon "MacBook Pro Microphone,MacBook Pro Speakers"; then
    expect "fake run 3: GET /api/devices with the radio gone" 200 "/api/devices"
    [ "$(jget resolved.capture.how)" = "none" ] && [ "$(jget resolved.capture.id)" = "-1" ] \
        && ok "  ... the saved choice resolves to nothing" \
        || bad "  ... resolved.capture is how=$(jget resolved.capture.how) id=$(jget resolved.capture.id)"
    case "$(jget resolved.capture.reason)" in
        *'no device matches "USB Audio Device"'*'device 1 is now "MacBook Pro Speakers"'*)
            ok "  ... and the reason names the missing device and what holds its id now" ;;
        *) bad "  ... the reason is '$(jget resolved.capture.reason)'" ;;
    esac
    expect "fake run 3: start the stream" 200 "/api/stream" -X POST -H "$CT" -d '{"action":"start"}'
    got="$(await_stream_state 'running|error|idle')"
    if [ "$got" = "error" ]; then
        case "$(jget stream.error)" in
            *'no device matches "USB Audio Device"'*) ok "  ... Start refused, naming the device" ;;
            *) bad "  ... Start failed with '$(jget stream.error)', which does not name the device" ;;
        esac
    else
        bad "  ... the stream reached '$got' (capturing '$(jget stream.captureName)') instead of refusing"
        "$CURL" -sS -o /dev/null -X POST -H "$CT" -d '{"action":"stop"}' "${URL%/}/api/stream" 2> /dev/null
    fi
    stop_fake_daemon
fi

# Run 4 — a file that predates the page writing names carries the id alone. That still follows
# the number (it is what the flag has always meant, and there is no name to prefer), which is
# why the page upgrades such a file on its next save; asserted so the legacy path is on record
# and this section can tell the two behaviours apart.
printf 'capture-id = 1\n' > "$FAKE_CONF"
if start_fake_daemon "USB Audio Device,MacBook Pro Microphone"; then
    expect "fake run 4: GET /api/devices on a legacy id-only file" 200 "/api/devices"
    [ "$(jget resolved.capture.how)" = "id" ] \
        && [ "$(jget resolved.capture.name)" = "MacBook Pro Microphone" ] \
        && ok "  ... an id alone is opened as given: [1] is \"MacBook Pro Microphone\" today" \
        || bad "  ... resolved.capture is how=$(jget resolved.capture.how) name='$(jget resolved.capture.name)'"
    stop_fake_daemon
fi

# ================================================================================================
# 5/5 — can-fail control (L222)
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
