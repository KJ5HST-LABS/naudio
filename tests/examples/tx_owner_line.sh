#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio tests — na_audio_source's TX-owner line, driven by na_c_inject_tone.
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# Hardware-free end to end: the test-tone server runs on the NULL backend on an ephemeral port,
# and the tone client transmits from the NULL backend. What is asserted is the LOG LINE a station
# reads its PTT-to-audio latency from — that it appears when a client's audio is being mixed,
# carries a wall-clock stamp in rigctld -Z's shape, and reads "none" once the channel is released
# — plus the client's own view of the same grant. The negative control connects a client that
# sends NOTHING: the server must name no owner, because ownership follows audio, not connection.
set -u

SOURCE="${1:?usage: tx_owner_line.sh <na_audio_source> <na_c_inject_tone>}"
TONE="${2:?usage: tx_owner_line.sh <na_audio_source> <na_c_inject_tone>}"
[ -x "$SOURCE" ] || { echo "FAIL: not executable: $SOURCE"; exit 1; }
[ -x "$TONE" ]   || { echo "FAIL: not executable: $TONE"; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fails=0
STAMP='[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}[+-][0-9]{4}'

# Start the test-tone server for $1 seconds on an ephemeral port; sets $SRV and $port.
start_server() {
    "$SOURCE" --test-tone --port 0 --seconds "$1" > "$work/srv.out" 2> "$work/srv.err" &
    SRV=$!
    port=""
    for _ in $(seq 1 100); do
        port="$(sed -n 's/^LISTENING port=\([0-9][0-9]*\)$/\1/p' "$work/srv.out")"
        [ -n "$port" ] && break
        sleep 0.1
    done
    if [ -z "$port" ]; then
        echo "FAIL: the server printed no LISTENING line"; cat "$work/srv.err"; kill "$SRV" 2>/dev/null
        exit 1
    fi
    sleep 0.5
}

# Assert a file matches (or, with "not", does not match) an ERE.
expect() {
    local file="$1" re="$2" label="$3"
    if grep -Eq "$re" "$file"; then echo "ok: $label"; else
        echo "FAIL: $label — no line matched /$re/ in $(basename "$file")"; fails=$((fails + 1)); fi
}
expect_not() {
    local file="$1" re="$2" label="$3"
    if grep -Eq "$re" "$file"; then
        echo "FAIL: $label — a line matched /$re/ in $(basename "$file")"; fails=$((fails + 1))
    else echo "ok: $label"; fi
}

# --- arm 1: a client that sends for two seconds is named owner, stamped, then released ---------
start_server 8
"$TONE" --port "$port" --seconds 2 > "$work/tone.out" 2> "$work/tone.err"; rc=$?
wait "$SRV"; srv_rc=$?
[ "$rc" -eq 0 ]     && echo "ok: tone client exit 0" || { echo "FAIL: tone client exit $rc"; fails=$((fails + 1)); }
[ "$srv_rc" -eq 0 ] && echo "ok: server exit 0"      || { echo "FAIL: server exit $srv_rc"; fails=$((fails + 1)); }
expect "$work/tone.out" '^RESULT injected_bytes=[1-9][0-9]* frames=[1-9]' "the client injected audio"
expect "$work/srv.err"  "^\[source\] $STAMP tx owner=audio-[0-9]+$" "the server named an owner, stamped"
expect "$work/srv.err"  "^\[source\] $STAMP tx owner=none$"         "the server released it, stamped"
expect "$work/tone.err" '^\[tone\] server tx owner=audio-[0-9]+$'   "the client saw its own grant"
# Order: the grant precedes the release.
grant="$(grep -nE 'tx owner=audio-' "$work/srv.err" | head -1 | cut -d: -f1)"
release="$(grep -nE 'tx owner=none' "$work/srv.err" | head -1 | cut -d: -f1)"
if [ -n "$grant" ] && [ -n "$release" ] && [ "$grant" -lt "$release" ]; then
    echo "ok: grant (line $grant) precedes release (line $release)"
else
    echo "FAIL: grant/release order (grant=$grant release=$release)"; fails=$((fails + 1))
fi
echo "--- arm 1 server log"; cat "$work/srv.err"

# --- negative control: a client that connects and sends nothing is never named owner ----------
start_server 4
"$TONE" --port "$port" --seconds 0 > "$work/tone0.out" 2> "$work/tone0.err"; rc=$?
wait "$SRV"
[ "$rc" -eq 1 ] && echo "ok: send-nothing client exit 1" || { echo "FAIL: send-nothing client exit $rc, expected 1"; fails=$((fails + 1)); }
expect "$work/tone0.out"    '^RESULT injected_bytes=0 frames=0' "the control injected nothing"
expect "$work/srv.err"      'client connected id=audio-'         "the control did connect (its own control)"
expect_not "$work/srv.err"  'tx owner=audio-'                    "no owner named for a silent client"
echo "--- control server log"; cat "$work/srv.err"

if [ "$fails" -ne 0 ]; then
    echo "tx_owner_line: $fails assertion(s) FAILED"
    exit 1
fi
echo "tx_owner_line: all assertions passed"
