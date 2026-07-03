// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace naudio {

// ASCII-lowercase a copy of `s`. One shared definition replaces the per-TU copies that used to
// live in DeviceEnumerator / VirtualAudioGuide / PlatformConfigurator.
inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Case-insensitive substring test (the "lowercase then contains" idiom, deduplicated).
inline bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

}  // namespace naudio
