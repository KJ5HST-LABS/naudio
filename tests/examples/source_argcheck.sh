#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio tests — na_audio_source --playback-id refusals that need no device.
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# The example's main() has no injection seam, so driving the built binary is the only way to cover
# its argument handling. Every case here is decided BEFORE any device work: the --test-tone pairing
# is refused straight after the parse loop's range checks, so this arm opens no PortAudio stream,
# needs no hardware, and is safe on a headless runner. The other refusal — an id that no
# playback-capable device carries — needs device enumeration to say so by name, and is exercised
# by hand on a machine with devices rather than asserted here.
#
# What it defends: --playback-id is a SYSTEM-backend setting. The test-tone server runs on the
# hardware-free NULL backend, whose na_server_set_playback_device returns a bare NA_ERR_UNSUPPORTED;
# the example must refuse the pairing by name, before start, instead of surfacing that code.
set -u

SOURCE="${1:?usage: source_argcheck.sh <path-to-na_audio_source>}"
[ -x "$SOURCE" ] || { echo "FAIL: not executable: $SOURCE"; exit 1; }

fails=0

# Containment, not decoration: every case appends `--port 0 --seconds 1`, so a pairing that is
# WRONGLY accepted starts an ephemeral-port server that exits 0 after one second — a failed
# assertion, not a hung test.
GUARD=(--port 0 --seconds 1)

# Assert: running the example with $2.. exits with code $1 and its output matches ERE $2.
expect() {
    local want_rc="$1" want_re="$2"; shift 2
    local out rc
    out="$("$SOURCE" "$@" "${GUARD[@]}" 2>&1)"; rc=$?
    if [ "$rc" -ne "$want_rc" ]; then
        echo "FAIL: [$*] exit $rc, expected $want_rc"; echo "  output: $out"; fails=$((fails + 1)); return
    fi
    if ! printf '%s' "$out" | grep -Eq "$want_re"; then
        echo "FAIL: [$*] output did not match /$want_re/"; echo "  output: $out"; fails=$((fails + 1)); return
    fi
    echo "ok: [$*] -> exit $rc, matched /$want_re/"
}

# The same assertion WITHOUT the guard, for the one case whose last argument must stay last.
expect_bare() {
    local want_rc="$1" want_re="$2"; shift 2
    local out rc
    out="$("$SOURCE" "$@" 2>&1)"; rc=$?
    if [ "$rc" -ne "$want_rc" ]; then
        echo "FAIL: [$*] exit $rc, expected $want_rc"; echo "  output: $out"; fails=$((fails + 1)); return
    fi
    if ! printf '%s' "$out" | grep -Eq "$want_re"; then
        echo "FAIL: [$*] output did not match /$want_re/"; echo "  output: $out"; fails=$((fails + 1)); return
    fi
    echo "ok: [$*] -> exit $rc, matched /$want_re/"
}

# Assert the output does NOT match an ERE (the controls below).
expect_not() {
    local want_rc="$1" bad_re="$2"; shift 2
    local out rc
    out="$("$SOURCE" "$@" "${GUARD[@]}" 2>&1)"; rc=$?
    if [ "$rc" -ne "$want_rc" ]; then
        echo "FAIL: [$*] exit $rc, expected $want_rc"; echo "  output: $out"; fails=$((fails + 1)); return
    fi
    if printf '%s' "$out" | grep -Eq "$bad_re"; then
        echo "FAIL: [$*] output unexpectedly matched /$bad_re/"; echo "  output: $out"; fails=$((fails + 1)); return
    fi
    echo "ok: [$*] -> exit $rc, did not match /$bad_re/"
}

# --- the refusal: --playback-id with --test-tone is named, and nothing starts ---
expect 2 'needs capture mode' --test-tone --playback-id 0
expect_not 2 'LISTENING|unknown option' --test-tone --playback-id 0
# Either order — the check runs after the whole parse loop, not on whichever flag came second.
expect 2 'needs capture mode' --playback-id 0 --test-tone

# A missing value is the parse loop's own refusal, ahead of the pairing check. This is the one
# case that cannot carry the guard: the guard's first flag would BE the value. It is still
# hardware-free — the parse loop exits before anything is created.
expect_bare 2 'needs a value' --test-tone --playback-id

# --- POSITIVE CONTROLS ---
# Without these, an example that refused EVERY --test-tone run would pass the cases above. The
# tone alone must still start and run out its one second (exit 0, LISTENING on stdout) — this is
# the one case that opens a server, on an ephemeral port, for one second, on the NULL backend.
expect 0 'LISTENING port=[0-9]+' --test-tone
expect_not 0 'needs capture mode' --test-tone

# The parse loop's range checks run BEFORE the pairing check, so a bad --rate is reported as the
# rate and not as the pairing: proof the refusal has a fixed place in the order, after them.
expect 2 'invalid --rate' --test-tone --playback-id 0 --rate 4000
expect_not 2 'needs capture mode' --test-tone --playback-id 0 --rate 4000

# And the usage text must name the option and its capture-mode limit (--help returns 0 before the
# guard is reached).
expect 0 'playback-id N' --help
expect 0 'capture mode only' --help

if [ "$fails" -ne 0 ]; then
    echo "source_argcheck: $fails case(s) FAILED"
    exit 1
fi
echo "source_argcheck: all cases passed"
