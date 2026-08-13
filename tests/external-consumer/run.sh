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

echo "== [1/4] configure + build + install naudio -> $PREFIX =="
cmake -S "$NAUDIO_SRC" -B "$NBUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DNAUDIO_BUILD_TESTS=OFF -DNAUDIO_BUILD_APPS=OFF >/dev/null
cmake --build "$NBUILD" -j >/dev/null
cmake --install "$NBUILD" >/dev/null
echo "   installed libs: $(ls "$PREFIX/lib"/libnaudio* | xargs -n1 basename | tr '\n' ' ')"

echo "== [2/4] external project: find_package(naudio) configure + build =="
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

echo "== [3/4] run the find_package consumers =="
run_consumer "$EBUILD/consume_c"
run_consumer "$EBUILD/consume_cxx"
if [ "$PA_ARM_ACTUAL" = yes ]; then
    run_consumer "$EBUILD/consume_pa"
else
    echo "   consume_pa: not built (no consumer-side PortAudio on this host) — stated, not skipped silently"
fi

echo "== [4/4] pkg-config path: compile + run the C ABI consumer =="
CFLAGS_PC="$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --cflags naudio)"
LIBS_PC="$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --libs naudio)"
echo "   pkg-config --cflags: $CFLAGS_PC"
echo "   pkg-config --libs:   $LIBS_PC"
# shellcheck disable=SC2086
cc $CFLAGS_PC "$HERE/consume_c.c" $LIBS_PC -o "$WORK/consume_c_pc"
run_consumer "$WORK/consume_c_pc"

echo "== E3 DONE GATE: PASS =="
