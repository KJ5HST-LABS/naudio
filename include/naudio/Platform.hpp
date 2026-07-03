// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <string>
#include <vector>

namespace naudio {

enum class Platform { MacOS, Windows, Linux, Unknown };

// Detect the host platform.
Platform currentPlatform();

// Virtual-audio-device name patterns for a platform. Unknown falls back to the
// Linux list.
const std::vector<std::string>& virtualPatterns(Platform platform);

// Preference-ordered patterns for picking the *best* virtual device. Distinct
// from virtualPatterns — different ordering, and Unknown returns an EMPTY list
// (no preferences, unlike virtualPatterns).
const std::vector<std::string>& preferredPatterns(Platform platform);

}  // namespace naudio
