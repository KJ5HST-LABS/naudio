/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * Per-file C preamble for docs/hamlib-streaming-bridge.md — see the "PER-FILE
 * preamble" note in check.sh.
 *
 * That document's C blocks are fragments of a client the surrounding prose has
 * already built and connected, so they use its handle without declaring it, and
 * they print with printf. Both are supplied here rather than in check.sh's shared
 * preamble, so README.md's blocks keep having to declare their own.
 */
#include <stdio.h>

static na_stream_client* c;
