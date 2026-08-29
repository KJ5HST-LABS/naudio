#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# build-streaming-hamlib.sh — build and install a streaming-capable libhamlib.
#
# na_hamlib_bridge needs a libhamlib that exposes the rig_stream_* API (Hamlib
# 5.0+, PR #2116, merged upstream 2026-08-15). No RELEASED Hamlib has it yet: a
# stock Homebrew/apt libhamlib is 4.x, so -DNAUDIO_BUILD_HAMLIB_BRIDGE=ON
# produces no binary and only a CMake warning. This script builds the dependency
# that makes the flag work. It stays necessary until a Hamlib release ships the
# API — merged into master is not the same as packaged by a distro.
#
# It fetches upstream master, builds it into a DURABLE prefix, installs it, and
# verifies the result the same way naudio's build does (a compile+link probe on
# rig_stream_open). It does NOT configure naudio for you — that would rewrite an
# existing build tree's cached hamlib path as a side effect. It prints the exact
# command instead; copy-paste it.
#
# Usage:  tools/build-streaming-hamlib.sh [options]
#   --prefix DIR   install prefix        (default: $HOME/.local/hamlib-streaming)
#   --src DIR      source checkout       (default: <prefix>/src)
#   --ref REF      branch, tag or commit (default: master)
#   --repo URL     git remote            (default: upstream Hamlib/Hamlib)
#   --jobs N       parallel make jobs    (default: detected CPU count)
#   --static       build a STATIC libhamlib (no shared library), configured
#                  --without-samplerate --without-libusb. For release packaging
#                  (issue #93): a bridge linked against this prefix embeds
#                  hamlib, so the shipped binary carries no dependency on a
#                  libhamlib no package manager can install yet — and no
#                  environment-dependent extras either. samplerate costs
#                  nothing the bridge uses (it DEMANDS the native rate and
#                  refuses in-hamlib resampling — the PR #2172 work); dropping
#                  libusb loses only the libusb-direct backends (USB-serial
#                  rigs are /dev/tty* and unaffected) — build from source if
#                  you need one. Also load-bearing: hamlib.pc's Libs.private
#                  omits -lsamplerate (upstream .pc gap, measured 2026-08-29),
#                  so a static build WITH samplerate underlinks every consumer
#                  that trusts pkg-config --static, this script's own verify
#                  probe included. naudio's build detects the static-only
#                  prefix and links the static closure (tools/CMakeLists.txt).
#   -h, --help
#
# The prefix must be durable: naudio's CMakeCache.txt records an ABSOLUTE path
# into it, so a prefix under /tmp silently breaks every later rebuild.
#
# See docs/hamlib-streaming-bridge.md for the manual equivalent and for what to
# do when the build is skipped anyway.
#
set -euo pipefail

# PR #2116 "Add data streaming subsystem for audio and I/Q" MERGED into
# Hamlib/Hamlib on 2026-08-15, so the streaming API now lives upstream and this
# tracks master rather than the PR author's fork branch (issue #81, item 1).
#
# The tracking property is deliberate and unchanged — naudio follows where the
# streaming API goes, and a red hamlib-bridge job is the loud, early notice that
# it moved. What repointing removes is the risk that made #81 urgent: a merged
# PR's fork branch is deletion-eligible, and it was naudio's only source for the
# dependency. Nothing here assumes a struct shape; tools/CMakeLists.txt
# capability-checks the installed revision and compiles the matching code.
default_repo="https://github.com/Hamlib/Hamlib.git"
default_ref="master"

prefix="$HOME/.local/hamlib-streaming"
src=""
ref="$default_ref"
repo="$default_repo"
jobs=""
static=0

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
    --static) static=1; shift ;;
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
    This reads like a network blip and sometimes is. The default is now upstream
    Hamlib/Hamlib @ master (PR #2116 merged 2026-08-15), so the likeliest causes are:
      1. You passed --ref streaming-subsystem-pr, the PRE-MERGE default. That branch lives on
         the PR author's fork and is deletion-eligible now that the PR is merged. The streaming
         API is upstream: drop the flag, or --repo https://github.com/mikaelnousiainen/Hamlib.git
         if you specifically need the fork's history.
      2. You pinned a commit that is gone, or one that upstream never had (a fork-only sha).
         Pass a branch name, or a commit reachable from upstream: --ref $default_ref
      3. A mirror that refuses to serve a bare commit (GitHub allows it; many mirrors do not).
      4. Genuine network or DNS failure — the one case where retrying is the right move.
    Check which: git ls-remote --heads $repo $default_ref
    An empty result for upstream master means something is wrong with the remote, not the ref.
    See docs/hamlib-streaming-bridge.md."
fi
git -C "$src" checkout --quiet --detach FETCH_HEAD
head_sha="$(git -C "$src" rev-parse --short HEAD)"
printf '    at %s (%s)\n' "$head_sha" "$(git -C "$src" log -1 --format=%s)"

if ! [ -f "$src/include/hamlib/rig.h" ]; then
  die "$src does not look like a Hamlib checkout (no include/hamlib/rig.h)"
fi
if ! grep -q 'rig_stream_open' "$src/include/hamlib/rig.h"; then
  die "'$ref' has no rig_stream_open in include/hamlib/rig.h — wrong ref?
    The streaming API arrived with Hamlib PR #2116, merged upstream 2026-08-15. A ref that
    predates that merge (an older tag, a 4.x branch, or a release tarball) will not have it.
    The default ref is $default_ref on $default_repo."
fi

# ---------------------------------------------------------------------------
# Build. --without-cxx-binding drops an artifact the bridge never links (it is
# pure C), and --disable-dependency-tracking is a one-shot-build speedup;
# neither is required — they just make this faster. The library shape IS
# required: the default builds shared-only (what a development prefix wants —
# rebuilds relink automatically), --static builds static-only (what a shipped
# bridge wants — the binary embeds hamlib and depends on no libhamlib the
# user cannot install). Never both: a prefix with both shapes makes which one
# the linker picked an accident of the toolchain.
# ---------------------------------------------------------------------------
if [ "$static" = 1 ]; then
  # --without-samplerate / --without-libusb: see the --static help text above.
  # Explicit rather than left to auto-detection, so the static shape does not
  # vary with what happens to be installed on the build machine.
  shape_flags="--enable-static --disable-shared --without-samplerate --without-libusb"
  pc_static_flag="--static"
else
  shape_flags="--disable-static"
  pc_static_flag=""
fi

step "Bootstrapping (autoreconf)"
( cd "$src" && ./bootstrap )

shape_desc="shared"
[ "$static" = 1 ] && shape_desc="static"
step "Configuring --prefix=$prefix ($shape_desc libhamlib)"
# shellcheck disable=SC2086  # shape_flags is a deliberate word-split
( cd "$src" && ./configure --prefix="$prefix" \
                           --without-cxx-binding \
                           $shape_flags \
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
# A static-only prefix needs the PRIVATE closure (-lm etc.) that plain --libs
# omits — exactly the underlink naudio's own build guards against; mirror it.
# shellcheck disable=SC2046,SC2086  # word splitting of pkg-config output is intended
if ! ${CC:-cc} -o "$probe/probe" "$probe/probe.c" \
       $(PKG_CONFIG_PATH="$pkgdir" pkg-config $pc_static_flag --cflags --libs hamlib) 2>"$probe/err"; then
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

IF YOU JUST REFRESHED AN EXISTING PREFIX IN PLACE, DELETE YOUR BUILD DIRECTORY.
naudio detects which streaming revision you have with CMake check_* probes, and
CMake CACHES those results — it does not re-run them when the header underneath
changes. So an existing build tree keeps compiling the code path chosen for the
OLD libhamlib against the NEW headers, and you get errors like

  na_hamlib_bridge.c: error: no member named 'channels_min' in
                             'struct rig_stream_caps'

which look like a naudio bug and are not. Since the default ref is a moving
branch, this is now the NORMAL consequence of re-running this script:

  rm -rf build && PKG_CONFIG_PATH=$pkgdir \\
    cmake -S . -B build -DNAUDIO_BUILD_HAMLIB_BRIDGE=ON
EOF
