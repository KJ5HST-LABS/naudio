// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <string>
#include <vector>

#include "naudio/DeviceBackend.hpp"
#include "naudio/Types.hpp"

namespace naudio {

class VirtualAudioGuide;  // optional source of platform-specific mismatch suggestions

// Result of verifying a device's configuration against a required format.
// Carries the verification outcome plus a human-readable summary string used for
// the diagnostic report.
struct VerificationResult {
    bool success = false;
    std::string deviceName;
    int actualSampleRate = 0;
    int requiredSampleRate = 0;
    int actualChannels = 0;
    int requiredChannels = 0;
    std::vector<std::string> issues;
    std::vector<std::string> suggestions;

    // Human-readable one-line summary of the verification outcome.
    std::string toString() const;
};

// Validates that a device supports a required audio format. The backend is
// injected (the test seam) so both paths are unit-testable without hardware.
//
// Note: PortAudio has no format-list query — it answers Pa_IsFormatSupported
// (a yes/no probe) and reports one default rate + max channel counts per device. So a
// "scan all formats for a matching rate" approach collapses into a single
// probeFormat() call, and the "detected" rate/channels come from DeviceInfo's reported
// defaults rather than an enumerated format list.
class FormatProbe {
public:
    // `guide` is optional: when supplied, a sample-rate mismatch appends its
    // platform-specific suggestion text; when null, a single generic OS-agnostic
    // suggestion is used.
    explicit FormatProbe(DeviceBackend& backend, const VirtualAudioGuide* guide = nullptr)
        : backend_(backend), guide_(guide) {}

    // Does `device` support `format` in `dir`?
    bool supports(const DeviceInfo& device, const AudioFormat& format, Direction dir) const;

    // Verifies a device against a required format. `device` may be null, which
    // reports a "No device specified" result.
    VerificationResult verifyConfiguration(const DeviceInfo* device, const AudioFormat& format,
                                           Direction dir) const;

private:
    DeviceBackend& backend_;
    const VirtualAudioGuide* guide_;
};

}  // namespace naudio
