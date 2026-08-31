#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio tests — na_audio_daemon config-file loading (issue #96, item 1).
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# Like argcheck.sh, this drives the built binary: main() has no injection seam. Every case is
# decided BEFORE any device work — the config file is read and merged ahead of the flag pass, and
# the post-merge rate/transport/mode checks all fire before anything opens PortAudio. So this arm
# opens no stream, needs no hardware, and is safe on a headless runner.
#
# What it defends: precedence (defaults < config file < flags), strict parsing (an unknown key or
# malformed line is refused with file:line, never skipped — a typo'd key silently ignored would be
# the config-file version of the atoi defect argcheck.sh exists for: the daemon would run, on the
# wrong device or port, and report green), and the --config/--no-config locator flags.
#
# NOT covered here, deliberately: the #91 format-declaration refusal armed by a config-file
# rate/channels key. That refusal fires only after a real device opens with a different format, so
# it is hardware-gated like the refusal itself; the arming is the same code path the flags take
# (one shared settings table).
set -u

DAEMON="${1:?usage: configcheck.sh <path-to-na_audio_daemon>}"
[ -x "$DAEMON" ] || { echo "FAIL: not executable: $DAEMON"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fails=0

# Every case appends `--mode zzz`, rejected at the mode dispatch — the LAST check before any
# device work. Containment, same as argcheck's transport guard: if config loading ever silently
# did nothing, a case whose bogus transport lives only in the FILE would otherwise sail through
# validation and open a real device for 30 seconds. It never masks an assertion, because the
# rate and transport checks both run before mode dispatch.
GUARD=(--mode zzz)

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

# Assert the output does NOT match an ERE (the counter-controls below).
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

# --- locator flags ---------------------------------------------------------------------------
# An operator-named file that is absent is an error — they asked for THIS file.
expect 2 'cannot read config file' --config "$TMP/absent.conf"
expect 2 'mutually exclusive' --config "$TMP/absent.conf" --no-config

# --config as the LAST argument (no guard: appending one would become the missing value; the
# locator pre-scan errors before any device work regardless).
out="$("$DAEMON" --config 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && printf '%s' "$out" | grep -Eq 'config needs a value'; then
    echo "ok: [--config (no value)] -> exit 2, matched"
else
    echo "FAIL: [--config (no value)] exit $rc"; echo "  output: $out"; fails=$((fails + 1))
fi

# --- strict parsing: every refusal names file:line ------------------------------------------
printf '# comment\ntransport = tcp\nprot = 4533\n' > "$TMP/unknown.conf"
expect 2 "unknown\\.conf:3: unknown key 'prot'" --config "$TMP/unknown.conf"

printf 'transport tcp\n' > "$TMP/noeq.conf"
expect 2 "noeq\\.conf:1: expected 'key = value'" --config "$TMP/noeq.conf"

printf 'port = 8080abc\n' > "$TMP/badint.conf"
expect 2 'badint\.conf:1: port expects an integer' --config "$TMP/badint.conf"

printf 'channels = 3\n' > "$TMP/range.conf"
expect 2 'must be in' --config "$TMP/range.conf"

# --- file values actually LAND in Args (the positive control for every refusal above) --------
# Comments and blank lines are tolerated; the bogus transport comes from the FILE alone and is
# caught by the post-merge transport check — proof the key was parsed, applied, and merged.
printf '# full-line comment\n\ntransport = bogus\n' > "$TMP/applies.conf"
expect 2 "invalid --transport 'bogus'" --config "$TMP/applies.conf"

# Inner spaces in a value survive; outer whitespace is trimmed (device patterns need no quoting).
printf '  transport   =   a b  \n' > "$TMP/spaces.conf"
expect 2 "invalid --transport 'a b'" --config "$TMP/spaces.conf"

# Post-merge validation reaches file values: the 20 ms-frame granularity check.
printf 'rate = 11025\n' > "$TMP/rategran.conf"
expect 2 'divisible by 50' --config "$TMP/rategran.conf"

# --- precedence: flags override the file -----------------------------------------------------
printf 'transport = udp\n' > "$TMP/goodtrans.conf"
expect 2 "invalid --transport 'bogus'" --config "$TMP/goodtrans.conf" --transport bogus
# Counter-control: the same file with no transport flag is NOT refused for transport — the udp
# value from the file was accepted (the run exits on the rate flag instead).
expect_not 2 'invalid --transport' --config "$TMP/goodtrans.conf" --rate 11025
expect 2 'divisible by 50' --config "$TMP/goodtrans.conf" --rate 11025

# A flag rate overrides a file rate that would have failed the granularity check — the file
# value must be GONE after the merge, not merely also-validated.
printf 'rate = 11025\ntransport = bogus\n' > "$TMP/rateover.conf"
expect 2 'invalid --transport' --config "$TMP/rateover.conf" --rate 12000
expect_not 2 'divisible by 50' --config "$TMP/rateover.conf" --rate 12000

# --- default-location search -----------------------------------------------------------------
# On macOS the default path is under $HOME, so it can be pointed at a scratch tree. The
# /etc/naudio path on other platforms cannot be redirected hermetically — the path construction
# is the same three-line function either way.
if [ "$(uname -s)" = "Darwin" ]; then
    FAKEHOME="$TMP/home"
    mkdir -p "$FAKEHOME/Library/Application Support/naudio"
    CONF="$FAKEHOME/Library/Application Support/naudio/daemon.conf"

    # The default file is found and applied: its bogus transport is the only argument source.
    printf 'transport = bogusfile\n' > "$CONF"
    out="$(HOME="$FAKEHOME" "$DAEMON" "${GUARD[@]}" 2>&1)"; rc=$?
    if [ "$rc" -eq 2 ] && printf '%s' "$out" | grep -Eq "invalid --transport 'bogusfile'"; then
        echo "ok: [default search] -> exit 2, file at \$HOME default path applied"
    else
        echo "FAIL: [default search] exit $rc"; echo "  output: $out"; fails=$((fails + 1))
    fi

    # --no-config skips it: a file that would error on load (unknown key) must go unread.
    printf 'junkkey = 1\n' > "$CONF"
    out="$(HOME="$FAKEHOME" "$DAEMON" --no-config --transport bogus "${GUARD[@]}" 2>&1)"; rc=$?
    if [ "$rc" -eq 2 ] && printf '%s' "$out" | grep -Eq "invalid --transport 'bogus'" \
        && ! printf '%s' "$out" | grep -Eq 'unknown key'; then
        echo "ok: [--no-config] -> exit 2, default file left unread"
    else
        echo "FAIL: [--no-config] exit $rc"; echo "  output: $out"; fails=$((fails + 1))
    fi

    # An ABSENT default file is not an error: defaults apply and the run reaches the guard.
    rm "$CONF"
    out="$(HOME="$FAKEHOME" "$DAEMON" --transport bogus "${GUARD[@]}" 2>&1)"; rc=$?
    if [ "$rc" -eq 2 ] && printf '%s' "$out" | grep -Eq "invalid --transport 'bogus'" \
        && ! printf '%s' "$out" | grep -Eq 'cannot read config file'; then
        echo "ok: [absent default] -> exit 2, absence is not an error"
    else
        echo "FAIL: [absent default] exit $rc"; echo "  output: $out"; fails=$((fails + 1))
    fi
else
    echo "skip: [default search] default path is /etc/naudio/daemon.conf here (not redirectable)"
fi

# --help still prints on a machine whose config file is broken (config loading is suppressed).
printf 'junkkey = 1\n' > "$TMP/broken.conf"
expect 0 'usage:' --config "$TMP/broken.conf" --help

if [ "$fails" -ne 0 ]; then
    echo "configcheck: $fails case(s) FAILED"
    exit 1
fi
echo "configcheck: all cases passed"
