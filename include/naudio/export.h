/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio — symbol-visibility macros for the shared library.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * Symbol-visibility macros for the naudio shared library. The library is built
 * with hidden default visibility (CMake CXX_VISIBILITY_PRESET hidden), so only
 * symbols explicitly tagged NA_EXPORT are part of the public ABI. NA_LOCAL force-
 * hides a symbol even in a translation unit that would otherwise default-export.
 *
 * Hand-written (no CMake generate_export_header) to keep the public C ABI header
 * self-contained and the build offline-clean.
 *
 *   NAUDIO_STATIC          - define when consuming naudio as a static archive
 *                            (no import/export decoration needed).
 *   NAUDIO_BUILDING_SHARED - defined by the build for the naudio shared target
 *                            (selects dllexport over dllimport on Windows).
 */
#ifndef NAUDIO_EXPORT_H
#define NAUDIO_EXPORT_H

#if defined(NAUDIO_STATIC)
#  define NA_EXPORT
#  define NA_LOCAL
#elif defined(_WIN32) || defined(__CYGWIN__)
#  ifdef NAUDIO_BUILDING_SHARED
#    define NA_EXPORT __declspec(dllexport)
#  else
#    define NA_EXPORT __declspec(dllimport)
#  endif
#  define NA_LOCAL
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define NA_EXPORT __attribute__((visibility("default")))
#  define NA_LOCAL  __attribute__((visibility("hidden")))
#else
#  define NA_EXPORT
#  define NA_LOCAL
#endif

#endif /* NAUDIO_EXPORT_H */
