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
cmake -S "$HERE" -B "$EBUILD" -DCMAKE_PREFIX_PATH="$PREFIX" >/dev/null
cmake --build "$EBUILD" -j >/dev/null

echo "== [3/4] run the find_package consumers =="
run_consumer "$EBUILD/consume_c"
run_consumer "$EBUILD/consume_cxx"

echo "== [4/4] pkg-config path: compile + run the C ABI consumer =="
CFLAGS_PC="$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --cflags naudio)"
LIBS_PC="$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --libs naudio)"
echo "   pkg-config --cflags: $CFLAGS_PC"
echo "   pkg-config --libs:   $LIBS_PC"
# shellcheck disable=SC2086
cc $CFLAGS_PC "$HERE/consume_c.c" $LIBS_PC -o "$WORK/consume_c_pc"
run_consumer "$WORK/consume_c_pc"

echo "== E3 DONE GATE: PASS =="
