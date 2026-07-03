// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/Platform.hpp"

namespace naudio {

Platform currentPlatform() {
#if defined(__APPLE__)
    return Platform::MacOS;
#elif defined(_WIN32)
    return Platform::Windows;
#elif defined(__linux__)
    return Platform::Linux;
#else
    return Platform::Unknown;
#endif
}

const std::vector<std::string>& virtualPatterns(Platform platform) {
    // Virtual-audio device-name patterns per platform.
    static const std::vector<std::string> kMacOS = {"blackhole", "soundflower", "loopback"};
    static const std::vector<std::string> kWindows = {"vb-audio", "cable", "virtual", "voicemeeter"};
    static const std::vector<std::string> kLinux = {"pulse", "pipewire", "jack", "null"};

    switch (platform) {
        case Platform::MacOS:   return kMacOS;
        case Platform::Windows: return kWindows;
        case Platform::Linux:   return kLinux;
        case Platform::Unknown:
        default:                return kLinux;  // else-branch fallback
    }
}

const std::vector<std::string>& preferredPatterns(Platform platform) {
    // Preference-ordered patterns — note the ordering differs from virtualPatterns, and
    // the default branch is empty (no preference) rather than Linux.
    static const std::vector<std::string> kMacOS = {"blackhole", "loopback", "soundflower"};
    static const std::vector<std::string> kWindows = {"cable", "vb-audio", "voicemeeter"};
    static const std::vector<std::string> kLinux = {"virtual", "null", "jack"};
    static const std::vector<std::string> kNone = {};

    switch (platform) {
        case Platform::MacOS:   return kMacOS;
        case Platform::Windows: return kWindows;
        case Platform::Linux:   return kLinux;
        case Platform::Unknown:
        default:                return kNone;  // default-branch fallback
    }
}

}  // namespace naudio
