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

SOURCE="${1:?usage: tx_owner_line.sh <na_audio_source> <na_c_inject_tone> [dir-of-libnaudio]}"
TONE="${2:?usage: tx_owner_line.sh <na_audio_source> <na_c_inject_tone> [dir-of-libnaudio]}"
[ -x "$SOURCE" ] || { echo "FAIL: not executable: $SOURCE"; exit 1; }
[ -x "$TONE" ]   || { echo "FAIL: not executable: $TONE"; exit 1; }

# Optional: the directory holding the shared library. On Windows the examples land in
# build/examples/<Config>/ while naudio.dll lands beside the library target, so from ctest's
# environment the exe cannot load (exit 127, "naudio.dll: cannot open shared object file"). The
# CMake registration passes $<TARGET_FILE_DIR:naudio>; it is prepended to PATH here, through
# cygpath because the exe is a NATIVE Windows binary (the same rule as tests/daemon/configcheck.sh).
# On POSIX the rpath already resolves the library and the entry is harmless.
LIBDIR="${3:-}"
if [ -n "$LIBDIR" ]; then
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) LIBDIR="$(cygpath -u "$LIBDIR")" ;;
    esac
    export PATH="$LIBDIR:$PATH"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fails=0

# Windows writes text-mode stdio with CRLF, and every pattern below anchors on $ — so the logs are
# normalised to LF before they are read, here and in the LISTENING wait. Same binaries, same
# assertions on both platforms; only the line ending differs.
lf() { tr -d '\r' < "$1" > "$1.lf" && mv "$1.lf" "$1"; }
STAMP='[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}[+-][0-9]{4}'

# Start the test-tone server on an ephemeral port; sets $SRV and $port. It runs until stop_server,
# with a 60 s ceiling so a dead arm cannot leave it behind. The FIRST version of this arm gave the
# server a fixed 8 s and the client 2 s; on macos-latest the client took long enough that the
# server's deadline fell before the client's disconnect, the release was never logged, and a
# correct server failed the arm. The server's lifetime is now the client's, not a guess at runner
# speed.
start_server() {
    "$SOURCE" --test-tone --port 0 --seconds 60 > "$work/srv.out" 2> "$work/srv.err" &
    SRV=$!
    port=""
    for _ in $(seq 1 100); do
        port="$(tr -d '\r' < "$work/srv.out" | sed -n 's/^LISTENING port=\([0-9][0-9]*\)$/\1/p')"
        [ -n "$port" ] && break
        sleep 0.1
    done
    if [ -z "$port" ]; then
        echo "FAIL: the server printed no LISTENING line"; cat "$work/srv.err"; kill "$SRV" 2>/dev/null
        exit 1
    fi
    sleep 0.5
}

# Give the server up to $1 seconds to log an ERE $2, then stop it (TERM, then KILL) and reap it.
# The lines the assertions read are already on disk — stderr is unbuffered — so how the server
# dies does not matter to them, and its exit code is not asserted.
stop_server_after() {
    local secs="$1" re="$2"
    for _ in $(seq 1 $((secs * 10))); do
        if tr -d '\r' < "$work/srv.err" | grep -Eq "$re"; then break; fi
        sleep 0.1
    done
    kill "$SRV" 2>/dev/null
    for _ in $(seq 1 50); do
        kill -0 "$SRV" 2>/dev/null || break
        sleep 0.1
    done
    kill -9 "$SRV" 2>/dev/null
    wait "$SRV" 2>/dev/null
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
start_server
t0=$(date +%s)
"$TONE" --port "$port" --seconds 2 > "$work/tone.out" 2> "$work/tone.err"; rc=$?
echo "tone client ran $(( $(date +%s) - t0 )) s wall-clock for --seconds 2 (diagnostic, not asserted)"
stop_server_after 15 'tx owner=none'
for f in srv.out srv.err tone.out tone.err; do lf "$work/$f"; done
[ "$rc" -eq 0 ] && echo "ok: tone client exit 0" || { echo "FAIL: tone client exit $rc"; fails=$((fails + 1)); }
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
start_server
"$TONE" --port "$port" --seconds 0 > "$work/tone0.out" 2> "$work/tone0.err"; rc=$?
stop_server_after 5 'client disconnected'
for f in srv.out srv.err tone0.out tone0.err; do lf "$work/$f"; done
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
