/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — cross-thread flag qualifier for the C smoke tests.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * NA_TEST_ATOMIC qualifies a global that a naudio worker thread writes from inside a test's
 * callback and that main() then reads. TSan proved `volatile` is not sufficient for that job
 * (issue #28): volatile orders nothing between threads, and the poll loops' nanosleep is not a
 * synchronisation edge — the report says so in as many words, "As if synchronized via sleep".
 *
 * So everywhere the detector runs, this is C11 `_Atomic`.
 *
 * MSVC is the exception, and it is a TOOLCHAIN limit rather than a judgement. MSVC gates C11
 * atomics behind /experimental:c11atomics even at /std:c11, and <stdatomic.h> is a hard
 * `#error "C atomic support is not enabled"` without it. That flag was delivered two ways —
 * add_compile_options($<$<COMPILE_LANGUAGE:C>:...>) and then a plain string(APPEND CMAKE_C_FLAGS) —
 * and windows-latest returned the BYTE-IDENTICAL error to all four targets both times, so the
 * mechanism was never established and further attempts would have been guesses against a toolchain
 * this project cannot run locally.
 *
 * On MSVC the qualifier therefore falls back to `volatile`, which is exactly what these globals
 * were before #28 — so Windows is unchanged, not regressed. The residual exposure is bounded and
 * worth stating plainly: the race is in TEST code, not in libnaudio, and the sanitizer jobs that
 * would observe it are Linux-only, so no naudio CI configuration can currently see a Windows-only
 * instance of it. If MSVC's C11 atomics ever become reachable, delete this header and use _Atomic
 * directly — the four call sites need no other change.
 */
#ifndef NAUDIO_TESTS_C_ATOMIC_COMPAT_H
#define NAUDIO_TESTS_C_ATOMIC_COMPAT_H

#if defined(_MSC_VER)
#  define NA_TEST_ATOMIC volatile
#else
#  include <stdatomic.h>
#  define NA_TEST_ATOMIC _Atomic
#endif

#endif /* NAUDIO_TESTS_C_ATOMIC_COMPAT_H */
