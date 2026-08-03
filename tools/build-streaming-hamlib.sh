#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# build-streaming-hamlib.sh — build and install a streaming-capable libhamlib.
#
# na_hamlib_bridge needs a libhamlib that exposes the rig_stream_* API (Hamlib
# 5.0+ / PR #2116). No released Hamlib has it yet: a stock Homebrew/apt libhamlib
# is 4.x, so -DNAUDIO_BUILD_HAMLIB_BRIDGE=ON produces no binary and only a CMake
# warning. This script builds the dependency that makes the flag work.
#
# It fetches the PR branch, builds it into a DURABLE prefix, installs it, and
# verifies the result the same way naudio's build does (a compile+link probe on
# rig_stream_open). It does NOT configure naudio for you — that would rewrite an
# existing build tree's cached hamlib path as a side effect. It prints the exact
# command instead; copy-paste it.
#
# Usage:  tools/build-streaming-hamlib.sh [options]
#   --prefix DIR   install prefix        (default: $HOME/.local/hamlib-streaming)
#   --src DIR      source checkout       (default: <prefix>/src)
#   --ref REF      branch, tag or commit (default: streaming-subsystem-pr)
#   --repo URL     git remote            (default: PR #2116's head repository)
#   --jobs N       parallel make jobs    (default: detected CPU count)
#   -h, --help
#
# The prefix must be durable: naudio's CMakeCache.txt records an ABSOLUTE path
# into it, so a prefix under /tmp silently breaks every later rebuild.
#
# See docs/hamlib-streaming-bridge.md for the manual equivalent and for what to
# do when the build is skipped anyway.
#
set -euo pipefail

# PR #2116 "Add data streaming subsystem for audio and I/Q" is filed against
# Hamlib/Hamlib from this fork; the branch is the PR head, so it tracks review.
default_repo="https://github.com/mikaelnousiainen/Hamlib.git"
default_ref="streaming-subsystem-pr"

prefix="$HOME/.local/hamlib-streaming"
src=""
ref="$default_ref"
repo="$default_repo"
jobs=""

die() { printf 'build-streaming-hamlib: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

# Print this file's header comment, so --help cannot drift from the source.
usage() {
  awk 'NR == 1 { next }                                  # shebang
       !/^#/ { exit }                                    # end of the header block
       { sub(/^#[ ]?/, "") }
       /^SPDX-License-Identifier:|^Copyright \(C\)/ { next }
       { print }' "$0"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) [ $# -ge 2 ] || die "--prefix needs an argument"; prefix="$2"; shift 2 ;;
    --src)    [ $# -ge 2 ] || die "--src needs an argument";    src="$2";    shift 2 ;;
    --ref)    [ $# -ge 2 ] || die "--ref needs an argument";    ref="$2";    shift 2 ;;
    --repo)   [ $# -ge 2 ] || die "--repo needs an argument";   repo="$2";   shift 2 ;;
    --jobs)   [ $# -ge 2 ] || die "--jobs needs an argument";   jobs="$2";   shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
done

# An absolute prefix is not a style preference: it is baked into hamlib.pc and
# from there into naudio's CMakeCache.txt.
case "$prefix" in
  /*) ;;
  *) prefix="$(pwd)/$prefix" ;;
esac
[ -n "$src" ] || src="$prefix/src"
case "$src" in
  /*) ;;
  *) src="$(pwd)/$src" ;;
esac

if [ -z "$jobs" ]; then
  if command -v nproc >/dev/null 2>&1; then jobs="$(nproc)"
  elif command -v sysctl >/dev/null 2>&1; then jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  else jobs=4
  fi
fi

# ---------------------------------------------------------------------------
# Prerequisites. hamlib's ./bootstrap runs autoreconf, so this needs the full
# autotools set, not just a compiler. bootstrap itself resolves GNU libtoolize
# vs Apple's /usr/bin/libtool name clash on Darwin, so checking for either name
# here matches what it will actually invoke.
# ---------------------------------------------------------------------------
step "Checking prerequisites"
missing=""
for t in git autoconf automake pkg-config make; do
  command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
if ! command -v libtoolize >/dev/null 2>&1 && ! command -v glibtoolize >/dev/null 2>&1; then
  missing="$missing libtool"
fi
if [ -n "$missing" ]; then
  printf 'build-streaming-hamlib: missing build tools:%s\n\n' "$missing" >&2
  printf '  macOS:         brew install autoconf automake libtool pkg-config\n' >&2
  printf '  Debian/Ubuntu: sudo apt-get install autoconf automake libtool pkg-config build-essential\n' >&2
  exit 1
fi
printf '    ok — git, autoconf, automake, libtool, pkg-config, make\n'

# ---------------------------------------------------------------------------
# Fetch. A depth-1 fetch of the exact ref keeps this small (Hamlib's full
# history is large) and works for a branch or a commit alike, so --ref can pin.
# ---------------------------------------------------------------------------
step "Fetching $repo @ $ref"
mkdir -p "$src"
if [ ! -d "$src/.git" ]; then
  git init --quiet "$src"
  git -C "$src" remote add origin "$repo"
else
  git -C "$src" remote set-url origin "$repo"
fi
if ! git -C "$src" fetch --quiet --depth 1 origin "$ref"; then
  die "could not fetch '$ref' from $repo
    A commit can only be fetched directly from a server that allows it (GitHub does).
    If you are using a mirror, pass a branch name: --ref $default_ref"
fi
git -C "$src" checkout --quiet --detach FETCH_HEAD
head_sha="$(git -C "$src" rev-parse --short HEAD)"
printf '    at %s (%s)\n' "$head_sha" "$(git -C "$src" log -1 --format=%s)"

if ! [ -f "$src/include/hamlib/rig.h" ]; then
  die "$src does not look like a Hamlib checkout (no include/hamlib/rig.h)"
fi
if ! grep -q 'rig_stream_open' "$src/include/hamlib/rig.h"; then
  die "'$ref' has no rig_stream_open in include/hamlib/rig.h — wrong branch?
    The streaming API arrives with Hamlib PR #2116; the default ref is $default_ref."
fi

# ---------------------------------------------------------------------------
# Build. --without-cxx-binding and --disable-static drop artifacts the bridge
# never links (it is pure C against the shared library); --disable-dependency-
# tracking is a one-shot-build speedup. None of the three is required — they
# just make this faster. The prefix is the part that matters.
# ---------------------------------------------------------------------------
step "Bootstrapping (autoreconf)"
( cd "$src" && ./bootstrap )

step "Configuring --prefix=$prefix"
( cd "$src" && ./configure --prefix="$prefix" \
                           --without-cxx-binding \
                           --disable-static \
                           --disable-dependency-tracking )

step "Building (make -j$jobs)"
( cd "$src" && make -j"$jobs" )

step "Installing to $prefix"
( cd "$src" && make install )

# ---------------------------------------------------------------------------
# Verify. Two checks, because they fail differently: pkg-config proves naudio's
# find step will resolve the library at all, and the compile+link probe proves
# the streaming API is really there — mirroring the check_symbol_exists() in
# tools/CMakeLists.txt, which is what actually gates the bridge target.
# ---------------------------------------------------------------------------
step "Verifying"
pkgdir="$prefix/lib/pkgconfig"
[ -f "$pkgdir/hamlib.pc" ] || die "no hamlib.pc under $pkgdir — install did not complete"

version="$(PKG_CONFIG_PATH="$pkgdir" pkg-config --modversion hamlib)"
printf '    pkg-config --modversion hamlib -> %s\n' "$version"

probe="$(mktemp -d)"
trap 'rm -rf "$probe"' EXIT
cat > "$probe/probe.c" <<'PROBE'
#include <hamlib/rig.h>
/* Mirrors tools/CMakeLists.txt's check_symbol_exists(rig_stream_open ...): the
   assignment fails to COMPILE if the header lacks the declaration and fails to
   LINK if the library lacks the definition. volatile keeps the store. */
int main(void) {
    void (*volatile sym)(void) = (void (*)(void))rig_stream_open;
    (void)sym;
    return 0;
}
PROBE
# shellcheck disable=SC2046  # word splitting of pkg-config output is intended
if ! ${CC:-cc} -o "$probe/probe" "$probe/probe.c" \
       $(PKG_CONFIG_PATH="$pkgdir" pkg-config --cflags --libs hamlib) 2>"$probe/err"; then
  sed 's/^/      /' "$probe/err" >&2
  die "libhamlib $version installed, but rig_stream_open did not compile+link.
    naudio's build applies the same test, so it would skip na_hamlib_bridge."
fi
printf '    rig_stream_open compiles and links\n'

# ---------------------------------------------------------------------------
step "Done — libhamlib $version ($head_sha) installed to $prefix"
cat <<EOF

Build naudio's bridge against it:

  PKG_CONFIG_PATH=$pkgdir \\
    cmake -S . -B build -DNAUDIO_BUILD_HAMLIB_BRIDGE=ON
  cmake --build build -j

Confirm the target was really enabled — CMake only WARNS when it skips it:

  cmake --build build --target na_hamlib_bridge
  ./build/tools/na_hamlib_bridge -h

The prefix above is recorded as an absolute path in build/CMakeCache.txt. Moving
or deleting it breaks every later rebuild of the bridge; re-run this script or
reconfigure from scratch if that happens.
EOF
