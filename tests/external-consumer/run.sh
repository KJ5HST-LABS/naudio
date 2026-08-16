#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# External-consumer gate: prove a throwaway external project
# builds + runs against the INSTALLED naudio prefix using the client C ABI + the
# server C ABI + the C++ API, via BOTH find_package(naudio) and pkg-config.
#
# Everything happens in a mktemp workdir (install prefix + both build trees), so a
# successful or failed run leaves no residue in the source tree or the system.
#
# Usage:  tests/external-consumer/run.sh
# Exit 0 = E3 install/export surface verified.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAUDIO_SRC="$(cd "$HERE/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PREFIX="$WORK/prefix"
NBUILD="$WORK/naudio-build"
EBUILD="$WORK/ext-build"

run_consumer() { DYLD_LIBRARY_PATH="$PREFIX/lib" LD_LIBRARY_PATH="$PREFIX/lib" "$@"; }

echo "== [1/5] configure + build + install naudio -> $PREFIX =="
cmake -S "$NAUDIO_SRC" -B "$NBUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DNAUDIO_BUILD_TESTS=OFF -DNAUDIO_BUILD_APPS=OFF >/dev/null
cmake --build "$NBUILD" -j >/dev/null
cmake --install "$NBUILD" >/dev/null
echo "   installed libs: $(ls "$PREFIX/lib"/libnaudio* | xargs -n1 basename | tr '\n' ' ')"

echo "== [2/5] external project: find_package(naudio) configure + build =="
# Keep the configure log: the naudio_pa arm is conditional, and a silently skipped
# arm must not be able to read as a passing one.
cmake -S "$HERE" -B "$EBUILD" -DCMAKE_PREFIX_PATH="$PREFIX" > "$WORK/consumer-configure.log" 2>&1
cmake --build "$EBUILD" -j >/dev/null

# The naudio_pa arm is conditional, so first require that it STATED itself either
# way — an arm that prints nothing is the silent-skip failure, not a pass.
grep -qE 'external-consumer: naudio_pa arm (ENABLED|SKIPPED)' "$WORK/consumer-configure.log" \
    || { echo "   FAIL: the consumer never reported naudio_pa arm status at all"; exit 1; }

grep -q 'external-consumer: naudio_pa arm ENABLED' "$WORK/consumer-configure.log" \
    && PA_ARM_ACTUAL=yes || PA_ARM_ACTUAL=no

# Then gate in the ONE direction that cannot produce a false failure. naudioConfig.cmake
# resolves PortAudio by two routes (pkg-config, else find_library), so "pkg-config
# misses" does NOT imply the arm should be off — a host where only find_library sees
# PortAudio is legitimately ENABLED. The implication that does hold is the reverse: if
# pkg-config resolves portaudio-2.0 and the arm is still off, naudio_PORTAUDIO_FOUND
# has regressed (issue #61).
if pkg-config --exists portaudio-2.0 2>/dev/null && [ "$PA_ARM_ACTUAL" = no ]; then
    echo "   FAIL: pkg-config resolves portaudio-2.0, yet naudio_PORTAUDIO_FOUND was FALSE"
    echo "   (the consumer-side PortAudio resolution in naudioConfig.cmake regressed — issue #61)"
    exit 1
fi
echo "   naudio_pa arm ENABLED=$PA_ARM_ACTUAL (pkg-config portaudio-2.0: $(pkg-config --exists portaudio-2.0 2>/dev/null && echo present || echo absent))"

echo "== [3/5] run the find_package consumers =="
run_consumer "$EBUILD/consume_c"
run_consumer "$EBUILD/consume_cxx"
if [ "$PA_ARM_ACTUAL" = yes ]; then
    run_consumer "$EBUILD/consume_pa"
else
    echo "   consume_pa: not built (no consumer-side PortAudio on this host) — stated, not skipped silently"
fi

echo "== [4/5] pkg-config path: compile + run the C ABI consumer =="
CFLAGS_PC="$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --cflags naudio)"
LIBS_PC="$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --libs naudio)"
echo "   pkg-config --cflags: $CFLAGS_PC"
echo "   pkg-config --libs:   $LIBS_PC"
# shellcheck disable=SC2086
cc $CFLAGS_PC "$HERE/consume_c.c" $LIBS_PC -o "$WORK/consume_c_pc"
run_consumer "$WORK/consume_c_pc"

# ---------------------------------------------------------------------------
# [5/5] VERSION SKEW — issue #32 / #88 item 1.
#
# Every other arm above, and every test in the build tree, compiles against the
# same header as the library it links. That cannot observe the one hazard
# `struct_size` exists to prevent. This arm compiles a consumer against a PINNED
# OLDER <naudio.h> taken from git history and links it against the library just
# installed, which is the only configuration that actually tests the promise
# README.md makes to packagers.
# ---------------------------------------------------------------------------
echo "== [5/5] version skew: pinned OLD header vs the CURRENT installed library =="

# Pins are deliberately fixed strings, not "HEAD~n" — a pin that moves with the
# branch is not a pin. Each names why it is here.
#
#   v0.4.0    the first tagged release: the ABI a packager can actually pin to.
#             Zero skew until the first post-tag append, at which point this arm
#             starts carrying the real load with no edit required.
#   d4f3c3a^  the commit before na_client_stats gained fec_pending_discarded, so
#             its header knows NA_CLIENT_STATS_SIZE_V1..V3 and a struct one field
#             shorter than the library's. This is what makes the gate non-vacuous
#             TODAY rather than at some future append.
PINS=("v0.4.0" "d4f3c3a^")

PINS_RESOLVED=0
PINS_WITH_REAL_SKEW=0

for pin in "${PINS[@]}"; do
    pindir="$WORK/pin-$(echo "$pin" | tr -c 'A-Za-z0-9._-' '_')"
    mkdir -p "$pindir"

    if ! git -C "$NAUDIO_SRC" show "$pin:include/naudio.h" > "$pindir/naudio.h" 2>/dev/null; then
        echo "   pin '$pin': UNAVAILABLE — git could not resolve it in this checkout."
        echo "      Not a pass. A shallow clone (actions/checkout defaults to depth 1) cannot"
        echo "      reach tags or history; CI sets fetch-depth: 0 for this job so it can."
        continue
    fi
    PINS_RESOLVED=$((PINS_RESOLVED + 1))

    # The pinned header must SHADOW the installed one, so -I"$pindir" precedes the
    # prefix's include dir. Its own #include "naudio/export.h" still resolves out of
    # the prefix, which is correct: export.h is ABI-neutral visibility plumbing.
    # shellcheck disable=SC2086
    cc -I"$pindir" $CFLAGS_PC "$HERE/consume_skew.c" $LIBS_PC -o "$WORK/consume_skew_$PINS_RESOLVED"

    skewlog="$WORK/skew-$PINS_RESOLVED.log"
    run_consumer "$WORK/consume_skew_$PINS_RESOLVED" | tee "$skewlog"

    # The consumer always prints its verdict line; a run that printed nothing is a
    # harness fault, not a pass.
    grep -q 'consume_skew: pinned-header version=' "$skewlog" \
        || { echo "   FAIL: pin '$pin' consumer produced no version line at all"; exit 1; }

    if grep -q 'SKEW REAL' "$skewlog"; then
        PINS_WITH_REAL_SKEW=$((PINS_WITH_REAL_SKEW + 1))
        echo "   pin '$pin': REAL SKEW exercised"
    else
        echo "   pin '$pin': no skew (header == library) — stated, not counted as coverage"
    fi
done

if [ "$PINS_RESOLVED" -eq 0 ]; then
    echo "   SKIPPED: no pin resolved, so the skew gate did not run."
    echo "   Stated rather than silently passed (issue #32's third acceptance item)."
elif [ "$PINS_WITH_REAL_SKEW" -eq 0 ]; then
    echo "   FAIL: $PINS_RESOLVED pin(s) resolved but NONE produced real version skew."
    echo "   The gate has gone vacuous — it would now pass against a library that ignores"
    echo "   struct_size entirely. Add a pin older than the newest ABI append."
    exit 1
else
    echo "   skew arms: $PINS_WITH_REAL_SKEW of $PINS_RESOLVED resolved pin(s) had real skew"
fi

# The other direction of the promise: a caller declaring MORE than the library
# knows must get the excess zero-filled, and nothing written past its declaration.
# shellcheck disable=SC2086
cc $CFLAGS_PC "$HERE/consume_zerofill.c" $LIBS_PC -o "$WORK/consume_zerofill"
run_consumer "$WORK/consume_zerofill"

echo "== E3 DONE GATE: PASS =="
