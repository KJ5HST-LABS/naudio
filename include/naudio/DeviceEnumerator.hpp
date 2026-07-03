// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "naudio/DeviceBackend.hpp"
#include "naudio/Platform.hpp"
#include "naudio/Types.hpp"

namespace naudio {

// Classifies and merges raw backend devices into DeviceInfo, and finds devices by
// pattern: discover, classify, DUPLEX-merge, and find-by-pattern.
//
// The platform is injected so classification is deterministic in tests regardless of
// the host OS.
class DeviceEnumerator {
public:
    explicit DeviceEnumerator(DeviceBackend& backend, Platform platform = currentPlatform());

    // All devices, classified (Hardware/Virtual) with capability (Capture/Playback/
    // Duplex). Capture and playback records of the same identity are merged to DUPLEX.
    std::vector<DeviceInfo> list();

    // Capture-only / playback-only views (discoverCaptureDevices / discoverPlaybackDevices).
    std::vector<DeviceInfo> captureDevices();
    std::vector<DeviceInfo> playbackDevices();

    // First device whose (name + hostApi) contains any pattern, case-insensitive.
    std::optional<DeviceInfo> find(const std::vector<DeviceInfo>& devices,
                                   const std::vector<std::string>& patterns) const;

    // Virtual device for capture / playback using the platform pattern list.
    std::optional<DeviceInfo> findVirtualCapture();
    std::optional<DeviceInfo> findVirtualPlayback();

    // Best virtual device for `dir`, validated against `format`'s sample rate: try
    // platform-preferred patterns (validated) first, then any device that validates, then
    // — as a last resort — the first virtual device even unvalidated. nullopt only when
    // there are no virtual devices for this direction. Probes via the injected backend.
    std::optional<DeviceInfo> bestVirtual(const AudioFormat& format, Direction dir);
    std::optional<DeviceInfo> bestVirtualCapture(const AudioFormat& format);
    std::optional<DeviceInfo> bestVirtualPlayback(const AudioFormat& format);

    Platform platform() const { return platform_; }

private:
    DeviceType classify(const RawDevice& raw) const;

    DeviceBackend& backend_;
    Platform platform_;
};

}  // namespace naudio
