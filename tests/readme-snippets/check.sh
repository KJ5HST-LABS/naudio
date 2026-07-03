#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# README snippet compile-gate.
#
# Extracts every ```c and ```cpp fenced code block from README.md (and any extra
# markdown files passed as arguments) and compiles each one with -fsyntax-only
# against the IN-TREE naudio headers. If a documented example does not compile
# against the real API, this fails — which is the whole point: the README must
# stay honest as the C ABI / C++ API evolve.
#
# Each block is compiled as its own translation unit: lines beginning with `#`
# (preprocessor directives — the snippet's own #includes) are hoisted to file
# scope, and the remaining statements are wrapped in a function body. A small
# per-language preamble declares the few free identifiers the illustrative
# snippets reference but don't define (on_pcm/events/user for the networking
# example).
#
# A block can opt OUT of the gate with an HTML comment on the line immediately
# above its opening fence:
#     <!-- snippet-check: skip -->
#     ```c
#     ... intentionally-incomplete pseudocode ...
#     ```
#
# Usage:  tests/readme-snippets/check.sh [extra.md ...]
# Env:    CC (default cc), CXX (default c++).
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

CC="${CC:-cc}"
CXX="${CXX:-c++}"
incflags=(-I"$repo/include")

files=("$@")
if [ "${#files[@]}" -eq 0 ]; then
  files=("$repo/README.md")
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# ---------------------------------------------------------------------------
# Per-language preambles: provide the free identifiers the illustrative README
# snippets reference but don't declare. naudio.h is included first so the C
# stubs can name na_client_callbacks; the C++ snippets include their own naudio
# C++ headers (hoisted), so the C++ preamble only needs the stdlib.
# ---------------------------------------------------------------------------
c_preamble='#include "naudio.h"
#include <stddef.h>
#include <stdint.h>
static void on_pcm(const unsigned char* pcm, size_t n_bytes, void* user){(void)pcm;(void)n_bytes;(void)user;}
static na_client_callbacks events;
static void* user = 0;
static int playback_id = 0;'

cpp_preamble='#include <vector>
#include <cstdint>
#include <cstddef>
#include <string>'

# ---------------------------------------------------------------------------
# Extract blocks. awk emits $work/block_NNN.<c|cpp> per checkable fenced block,
# honoring a `<!-- snippet-check: skip -->` directive on the preceding line and
# swallowing non-c/cpp fences (bash, mermaid, ...).
# ---------------------------------------------------------------------------
extract() {
  awk -v dir="$work" -v src="$1" -v base="$2" '
    BEGIN { state=0 }
    state==0 {
      if ($0 ~ /<!--[ ]*snippet-check:[ ]*skip[ ]*-->/) { pending_skip=1; next }
      if ($0 ~ /^```/) {
        lang=$0; sub(/^```/,"",lang); gsub(/[ \t\r]+$/,"",lang); gsub(/^[ \t]+/,"",lang)
        emit=0
        if ((lang=="c" || lang=="cpp") && !pending_skip) {
          n++; curfile=sprintf("%s/block_%s_%03d.%s", dir, base, n, lang); emit=1
          printf "" > curfile
        }
        pending_skip=0; state=1; next
      }
      pending_skip=0; next
    }
    state==1 {
      if ($0 ~ /^```/) { if (emit) close(curfile); state=0; emit=0; next }
      if (emit) print $0 >> curfile
      next
    }
  ' "$1"
}

i=0
for md in "${files[@]}"; do
  i=$((i+1))
  extract "$md" "$i"
done

shopt -s nullglob
blocks=("$work"/block_*.c "$work"/block_*.cpp)
if [ "${#blocks[@]}" -eq 0 ]; then
  echo "readme-snippets: no c/cpp fenced blocks found in: ${files[*]}" >&2
  exit 1
fi

fails=0
checked=0
for block in "${blocks[@]}"; do
  ext="${block##*.}"
  name="$(basename "$block")"
  tu="$work/tu_${name}.${ext}"
  {
    if [ "$ext" = "c" ]; then printf '%s\n' "$c_preamble"; else printf '%s\n' "$cpp_preamble"; fi
    grep -E '^[[:space:]]*#' "$block" || true
    printf 'static int na_snippet_%s(void){\n' "$(printf '%s' "$name" | tr -c 'a-zA-Z0-9' _)"
    grep -vE '^[[:space:]]*#' "$block"
    printf '\nreturn 0;\n}\n'
  } > "$tu"

  if [ "$ext" = "c" ]; then
    comp=("$CC" -std=c11)
  else
    comp=("$CXX" -std=c++17)
  fi

  checked=$((checked+1))
  if "${comp[@]}" -fsyntax-only "${incflags[@]}" "$tu" 2>"$work/err_${name}.txt"; then
    echo "  ok    $name"
  else
    echo "  FAIL  $name"
    sed 's/^/        /' "$work/err_${name}.txt" >&2
    fails=$((fails+1))
  fi
done

echo "readme-snippets: $checked block(s) checked, $fails failure(s)"
[ "$fails" -eq 0 ]
