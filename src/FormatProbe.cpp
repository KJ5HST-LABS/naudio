// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/FormatProbe.hpp"

#include <string>

#include "naudio/VirtualAudioGuide.hpp"

namespace naudio {

bool FormatProbe::supports(const DeviceInfo& device, const AudioFormat& format,
                           Direction dir) const {
    // The exact-format support decision is the single Pa_IsFormatSupported probe,
    // which never throws here (returns false on error).
    return backend_.probeFormat(device.backendIdFor(dir), format, dir);
}

VerificationResult FormatProbe::verifyConfiguration(const DeviceInfo* device,
                                                    const AudioFormat& format,
                                                    Direction dir) const {
    VerificationResult result;
    result.requiredSampleRate = format.sampleRate;
    result.requiredChannels = format.channels;

    // null device.
    if (device == nullptr) {
        result.success = false;
        result.deviceName = "null";
        result.issues.push_back("No device specified");
        result.suggestions.push_back("Select a virtual audio device");
        return result;
    }

    result.deviceName = device->name;

    // Exact-format probe: Pa_IsFormatSupported gives one definitive supported/not
    // answer.
    const bool formatSupported = backend_.probeFormat(device->backendIdFor(dir), format, dir);

    // "Detected" values. When the format is supported, the required rate/channels are
    // reported; otherwise the device's PortAudio-reported default rate and max channel
    // count.
    int detectedSampleRate;
    int detectedChannels;
    if (formatSupported) {
        detectedSampleRate = format.sampleRate;
        detectedChannels = format.channels;
    } else {
        detectedSampleRate = static_cast<int>(device->defaultSampleRate);
        detectedChannels = (dir == Direction::Capture) ? device->maxInputChannels
                                                       : device->maxOutputChannels;
    }

    // sample-rate analysis.
    if (detectedSampleRate == 0) {
        detectedSampleRate = -1;  // Unknown
        result.issues.push_back("Could not determine device sample rate");
    } else if (detectedSampleRate != format.sampleRate && !formatSupported) {
        result.issues.push_back("Sample rate mismatch: device is " +
                                std::to_string(detectedSampleRate) + " Hz, required " +
                                std::to_string(format.sampleRate) + " Hz");
        // Append the platform-specific sample-rate suggestion text when a guide is
        // injected. With no guide, fall back to a generic OS-agnostic
        // pointer so the negative path stays actionable.
        if (guide_ != nullptr) {
            const auto suggestions = guide_->sampleRateConfigurationSuggestions(device->name);
            result.suggestions.insert(result.suggestions.end(), suggestions.begin(),
                                      suggestions.end());
        } else {
            result.suggestions.push_back("Set the device's format sample rate to " +
                                         std::to_string(format.sampleRate) + " Hz");
        }
    }

    // channel analysis.
    if (detectedChannels == 0) {
        detectedChannels = -1;  // Unknown
        // Symmetric with the rate==0 path above: an undetermined channel count is an issue, so a
        // device whose exact format probe FAILED can't be reported "ready" just because issues
        // is otherwise empty (success = formatSupported || issues.empty()).
        result.issues.push_back("Could not determine device channel count");
    } else if (detectedChannels != format.channels && !formatSupported) {
        result.issues.push_back("Channel mismatch: device has " +
                                std::to_string(detectedChannels) + " channels, required " +
                                std::to_string(format.channels));
    }

    result.actualSampleRate = detectedSampleRate;
    result.actualChannels = detectedChannels;
    result.success = formatSupported || result.issues.empty();
    return result;
}

std::string VerificationResult::toString() const {
    // Human-readable one-line summary of the verification outcome.
    std::string sb;
    sb += "Device: " + deviceName + "\n";
    sb += "Status: " + std::string(success ? "OK" : "CONFIGURATION NEEDED") + "\n";
    sb += "Sample Rate: " + std::to_string(actualSampleRate) + " Hz";
    if (actualSampleRate != requiredSampleRate) {
        sb += " (required: " + std::to_string(requiredSampleRate) + " Hz)";
    }
    sb += "\n";
    sb += "Channels: " + std::to_string(actualChannels);
    if (actualChannels != requiredChannels) {
        sb += " (required: " + std::to_string(requiredChannels) + ")";
    }
    sb += "\n";

    if (!issues.empty()) {
        sb += "\nIssues:\n";
        for (const auto& issue : issues) sb += "  ! " + issue + "\n";
    }
    if (!suggestions.empty()) {
        sb += "\nSuggestions:\n";
        for (const auto& suggestion : suggestions) sb += "  \xE2\x86\x92 " + suggestion + "\n";
    }
    return sb;
}

}  // namespace naudio
