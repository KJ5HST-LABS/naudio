#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio tests — na_audio_daemon strict numeric-option parsing (issue #63, item 6).
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# The daemon's main() has no injection seam, so driving the built binary is the only way to cover
# its argument parsing. Every case here is decided BEFORE any device work: the parse loop exits on
# a bad value, and --transport is validated immediately after the loop. So this arm opens no
# PortAudio stream, needs no hardware, and is safe on a headless runner.
#
# What it defends: --capture-id used std::atoi, which returns 0 for a non-numeric string with no
# way to distinguish that from a real "0" -- and 0 is a valid PortAudio device index. A typo
# therefore opened device 0 and the RMS gate could pass on whatever that device happened to hear,
# reporting a green on-air check against the wrong radio.
set -u

DAEMON="${1:?usage: argcheck.sh <path-to-na_audio_daemon>}"
[ -x "$DAEMON" ] || { echo "FAIL: not executable: $DAEMON"; exit 1; }

fails=0

# Every case below appends `--transport bogus`, which is rejected immediately after the parse loop.
# That is a containment guard, not decoration: if the numeric parse ever WRONGLY succeeds (as it did
# with atoi), the run must still not reach runHardware and open a real device for 30 seconds. It
# also sharpens each assertion, because every case then keys on WHICH error was reported rather than
# on the shared exit code 2.
GUARD=(--transport bogus)

# Assert: running the daemon with $2.. exits with code $1 and its output matches ERE $2.
expect() {
    local want_rc="$1" want_re="$2"; shift 2
    local out rc
    out="$("$DAEMON" "$@" "${GUARD[@]}" 2>&1)"; rc=$?
    if [ "$rc" -ne "$want_rc" ]; then
        echo "FAIL: [$*] exit $rc, expected $want_rc"; echo "  output: $out"; fails=$((fails + 1)); return
    fi
    if ! printf '%s' "$out" | grep -Eq "$want_re"; then
        echo "FAIL: [$*] output did not match /$want_re/"; echo "  output: $out"; fails=$((fails + 1)); return
    fi
    echo "ok: [$*] -> exit $rc, matched /$want_re/"
}

# Assert the output does NOT match an ERE (used for the positive controls below).
expect_not() {
    local want_rc="$1" bad_re="$2"; shift 2
    local out rc
    out="$("$DAEMON" "$@" "${GUARD[@]}" 2>&1)"; rc=$?
    if [ "$rc" -ne "$want_rc" ]; then
        echo "FAIL: [$*] exit $rc, expected $want_rc"; echo "  output: $out"; fails=$((fails + 1)); return
    fi
    if printf '%s' "$out" | grep -Eq "$bad_re"; then
        echo "FAIL: [$*] output unexpectedly matched /$bad_re/"; echo "  output: $out"; fails=$((fails + 1)); return
    fi
    echo "ok: [$*] -> exit $rc, did not match /$bad_re/"
}

# --- the defect: a non-numeric id must be refused, not silently read as device 0 ---
expect 2 'expects an integer' --capture-id usb
expect 2 'expects an integer' --playback-id BlackHole

# Trailing garbage is the sharper case: atoi accepts the numeric prefix and discards the rest, so
# "3x" became 3 with no complaint. strtoll + endptr is what makes this an error.
expect 2 'expects an integer' --capture-id 3x
expect 2 'expects an integer' --port 8080abc

# An empty value must not read as 0 either.
expect 2 'expects an integer' --capture-id ''

# --- range: 0 is a legitimate device index, so out-of-range must be its own message ---
expect 2 'must be in' --capture-id -1
expect 2 'must be in' --port 99999

# --- Phase A (#91): --rate / --channels parse strictly and range-check like every other int ---
expect 2 'expects an integer' --rate 12000x
expect 2 'expects an integer' --channels ''
expect 2 'must be in' --rate 4000
expect 2 'must be in' --rate 200000
expect 2 'must be in' --channels 0
expect 2 'must be in' --channels 3
# Granularity: an exact 20 ms frame needs rate % 50 == 0. Validated after the parse loop and
# BEFORE the transport check, so this case must name the granularity, not the guard's transport.
expect 2 'divisible by 50' --rate 11025

# --- POSITIVE CONTROLS for the Phase A flags (same shape as the id/port controls below) ---
expect 2 'invalid --transport' --rate 12000 --channels 1
expect_not 2 'expects an integer|must be in|divisible by 50' --rate 12000
expect_not 2 'expects an integer|must be in|divisible by 50' --channels 1

# --- POSITIVE CONTROLS ---
# Without these, a requireInt() that rejected EVERYTHING would pass every case above.
#
# A valid numeric value must get past the parser. The guard's --transport is validated immediately
# after the parse loop, so a good id reaching the TRANSPORT message -- and no integer complaint --
# is the proof it was accepted.
expect 2 'invalid --transport' --capture-id 3
expect_not 2 'expects an integer|must be in' --capture-id 3
expect_not 2 'expects an integer|must be in' --playback-id 0
expect_not 2 'expects an integer|must be in' --port 0
expect_not 2 'expects an integer|must be in' --duration-ms 1000

# And the usage text must carry the silent-fallback warning for a --playback that matches nothing,
# which is the one failure mode here that is documented rather than fixed. (--help returns 0 before
# the guard is reached.)
expect 0 'matches NOTHING' --help

if [ "$fails" -ne 0 ]; then
    echo "argcheck: $fails case(s) FAILED"
    exit 1
fi
echo "argcheck: all cases passed"
