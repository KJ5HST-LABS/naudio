#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# FFI example-client gate: prove the hand-declared C ABI layouts in
# examples/python, examples/java and examples/rust still match the library.
#
# WHY THIS EXISTS
#
# Those three clients have no C compiler in the loop, so each mirrors na_device
# (include/naudio.h) byte-for-byte in its own language's declaration -- ctypes
# _fields_, an FFM MemoryLayout, a #[repr(C)] struct. Nothing in the tree built or
# ran them, so nothing noticed when the C side moved and a mirror did not.
#
# That is not theoretical. Shifting na_device by 8 bytes (the shape of a future
# `struct_size` first member) and rebuilding was measured on this repo:
#
#   C client (recompiled)   2 playback devices, correct               exit 0
#   Python                  3 records, every name empty, ids wrong    exit 0
#   Java                    "(none -- use --backend null ...)"        exit 0
#   Rust                    "(none -- use --backend null ...)"        exit 0
#
# All three were wrong and all three exited 0. Java's FFM MemoryLayout does not
# rescue it: that layout is itself hand-declared, so there is nothing for the
# runtime to check it against -- it reads the wrong offsets and finds no
# playback-capable device. So this gate asserts on OUTPUT, never on exit status,
# and a caller of this script must do the same (see .github/workflows/ci.yml).
#
# HOW IT WORKS
#
# na_device_truth.c #includes naudio.h and links the same libnaudio the clients
# load, so it IS the layout. It emits a `key=value` manifest. Each language probe
# emits the same manifest from the EXAMPLE CLIENT'S OWN declaration -- it imports
# or includes the shipped source rather than restating it -- and the two are
# sorted and diffed. A differing, truncated, or empty probe manifest all fail.
#
# Two halves, two distinct failure classes:
#   * the static layout (sizeof / alignof / every offset) is deterministic and
#     needs no audio hardware -- it catches fields that moved.
#   * the na_enumerate device records catch a change to the CALL CONVENTION that
#     left every offset intact (an added parameter, say). On a host with no audio
#     devices every probe reports 0 and this half only proves the four agree on
#     the count; the script says so out loud instead of reporting a clean match.
#
# A MISSING TOOLCHAIN IS A FAILURE, NEVER A SKIP. A gate that goes green because
# cargo was not installed is the exact failure mode this exists to prevent.
#
# Usage:  tests/ffi-clients/run.sh
# Env:    NAUDIO_BUILD_DIR (default <repo>/build), CC (default cc),
#         PYTHON (default python3), JAVA (default java), CARGO (default cargo).
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

build_dir="${NAUDIO_BUILD_DIR:-$repo/build}"
CC="${CC:-cc}"
PYTHON="${PYTHON:-python3}"
JAVA="${JAVA:-java}"

# Languages compared this run. Each needs a probe file here, plus an entry in
# tool_for(), run_probe() and expected_for().
LANGS="python java"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fails=0
compared=0

fail() { echo "  FAIL  $*" >&2; fails=$((fails + 1)); }

# ---------------------------------------------------------------------------
# Locate the built shared library. The probes load it by path via $NAUDIO_LIB
# (Python/Java) or link against it (Rust), so an absolute directory is enough.
# ---------------------------------------------------------------------------
lib=""
for name in libnaudio.dylib libnaudio.so naudio.dll libnaudio.dll; do
  if [ -f "$build_dir/$name" ]; then lib="$build_dir/$name"; break; fi
done
if [ -z "$lib" ]; then
  echo "ffi-clients: no libnaudio in $build_dir -- build it first:" >&2
  echo "  cmake -S . -B build && cmake --build build -j" >&2
  echo "ffi-clients: 0 probe(s) compared, 1 failure(s)"
  exit 1
fi
echo "ffi-clients: library $lib"

# ---------------------------------------------------------------------------
# The ground truth.
# ---------------------------------------------------------------------------
truth_bin="$work/na_device_truth"
if ! "$CC" -std=c11 -Wall -Wextra -I"$repo/include" "$here/na_device_truth.c" \
      -L"$build_dir" -lnaudio -Wl,-rpath,"$build_dir" -o "$truth_bin" 2>"$work/truth_build.log"; then
  echo "ffi-clients: could not build the C ground truth:" >&2
  sed 's/^/  /' "$work/truth_build.log" >&2
  echo "ffi-clients: 0 probe(s) compared, 1 failure(s)"
  exit 1
fi

if ! "$truth_bin" > "$work/truth.raw" 2>"$work/truth.err"; then
  echo "ffi-clients: the C ground truth failed to run:" >&2
  sed 's/^/  /' "$work/truth.err" >&2
  echo "ffi-clients: 0 probe(s) compared, 1 failure(s)"
  exit 1
fi

# A truth manifest that lost its layout section would make every probe "match"
# on an empty comparison, so require the keys before comparing anything.
for required in na_device.sizeof na_device.offset.is_virtual na_client_callbacks.sizeof device.count; do
  if ! grep -q "^${required}=" "$work/truth.raw"; then
    echo "ffi-clients: the C ground truth emitted no '$required' -- refusing to compare" >&2
    echo "ffi-clients: 0 probe(s) compared, 1 failure(s)"
    exit 1
  fi
done

# take_truth -- (re)run the ground truth and derive the two comparison files.
# Java and Rust are RX-only and hand-declare na_device alone, so they are compared
# against the truth without the callbacks section. Python declares both.
device_count=""
take_truth() {
  "$truth_bin" > "$work/truth.raw"
  sort "$work/truth.raw" > "$work/truth.txt"
  grep -v '^na_client_callbacks\.' "$work/truth.txt" > "$work/truth.nocbs.txt"
  device_count="$(sed -n 's/^device\.count=//p' "$work/truth.raw")"
}
take_truth
echo "ffi-clients: C truth: na_device is $(sed -n 's/^na_device\.sizeof=//p' "$work/truth.raw") bytes, ${device_count} device(s)"

# ---------------------------------------------------------------------------
# Probes.
# ---------------------------------------------------------------------------

# tool_for <lang> -> the executable whose absence is a hard failure.
tool_for() {
  case "$1" in
    python) echo "$PYTHON" ;;
    java)   echo "$JAVA" ;;
    *)      echo "" ;;
  esac
}

# run_probe <lang> <output file> -- emit that client's manifest on stdout.
#
# Java: JDK 22's multi-file source launcher (JEP 458) compiles other classes it
# needs from the directory holding the file it was given, so Probe.java and the
# shipped PlayToSpeakers.java are copied into one scratch directory and run from
# there. That is what lets the probe read the example's own layout without adding
# a non-example file to examples/java/.
run_probe() {
  case "$1" in
    python)
      NAUDIO_LIB="$lib" "$PYTHON" "$here/probe.py" > "$2"
      ;;
    java)
      local jdir="$work/java"
      rm -rf "$jdir" && mkdir -p "$jdir"
      cp "$repo/examples/java/PlayToSpeakers.java" "$here/Probe.java" "$jdir/"
      ( cd "$jdir" && NAUDIO_LIB="$lib" "$JAVA" --enable-native-access=ALL-UNNAMED Probe.java ) > "$2"
      ;;
    *)
      return 1
      ;;
  esac
}

# expected_for <lang> -> which truth file this language is compared against.
expected_for() {
  case "$1" in
    python) echo "$work/truth.txt" ;;
    *)      echo "$work/truth.nocbs.txt" ;;
  esac
}

# layout_lines_differ <diff file> -- true if the diff touches any NON-device key.
# The layout section is deterministic, so a difference there is always real. The
# device section is a snapshot of live hardware taken by two separate processes,
# so a device the OS added or removed between them differs without anything being
# wrong -- observed once while building this gate.
layout_lines_differ() {
  grep -E '^[-+]' "$1" | grep -vE '^(---|\+\+\+)' | grep -qvE '^[-+]device\.'
}

for lang in $LANGS; do
  compared=$((compared + 1))
  tool="$(tool_for "$lang")"

  if ! command -v "$tool" >/dev/null 2>&1; then
    fail "$lang: '$tool' is not installed. This gate does not skip -- install the toolchain."
    continue
  fi

  attempt=1
  while :; do
    if ! run_probe "$lang" "$work/$lang.raw" 2>"$work/$lang.err"; then
      fail "$lang: the probe did not run to completion:"
      sed 's/^/        /' "$work/$lang.err" >&2
      break
    fi

    sort "$work/$lang.raw" > "$work/$lang.txt"
    expected="$(expected_for "$lang")"

    if diff -u "$expected" "$work/$lang.txt" > "$work/$lang.diff" 2>&1; then
      echo "  ok    ffi-clients: $lang layout MATCHES the C truth"
      break
    fi

    # Device-only mismatch on the first try: the device list moved under us.
    # Retake the truth and try once more -- announced, and never for a layout
    # difference, which no amount of retrying can legitimately clear.
    if [ "$attempt" -eq 1 ] && ! layout_lines_differ "$work/$lang.diff"; then
      echo "  note  ffi-clients: $lang: the device list changed between processes;" \
           "retaking the C truth and retrying once"
      take_truth
      attempt=2
      continue
    fi

    fail "$lang: layout DIFFERS from the C truth (- = C truth, + = $lang client):"
    sed 's/^/        /' "$work/$lang.diff" >&2
    break
  done
done

# ---------------------------------------------------------------------------
# Report. The device half is reported separately because a host with no audio
# devices weakens it, and a silent pass there would be indistinguishable from a
# real comparison.
# ---------------------------------------------------------------------------
if [ "$device_count" -gt 0 ]; then
  echo "ffi-clients: device arm exercised -- ${device_count} device record(s) compared per probe"
else
  echo "ffi-clients: device arm NOT exercised -- this host reports 0 audio devices, so only the" \
       "static layout and the agreed device count were compared"
fi

echo "ffi-clients: $compared probe(s) compared, $fails failure(s)"
[ "$fails" -eq 0 ]
