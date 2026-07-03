// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "naudio/Platform.hpp"
#include "naudio/ShellRunner.hpp"
#include "naudio/Types.hpp"

namespace naudio {

class DeviceEnumerator;  // for the diagnostic-report free functions below
class FormatProbe;

// Parameterizes app-specific identifiers so the library core ships generic defaults
// and carries no app-coupling names in behavior; a consumer injects its own names at
// construction:
//
//   sinkName        — the pactl sink_name (must be SAFE_SHELL_ARG-valid)
//   sinkDescription — the pactl device.description
//   clientAppName   — the consumer's audio-client name
//   digitalModeApp  — the digital-mode application being bridged
struct GuideConfig {
    AudioFormat format{};  // 48 kHz / 16-bit / stereo default (the digital-mode REQUIRED path)
    std::string sinkName = "naudio_sink";
    std::string sinkDescription = "Naudio_Virtual_Audio";
    std::string clientAppName = "your audio client";
    std::string digitalModeApp = "your digital-mode application";
};

// Pure text generation — no hardware, no shell. Generates install instructions,
// digital-mode guidance, sample-rate configuration suggestions, the per-platform
// configuration/diagnostic commands, and preferred-device-name lookup.
class VirtualAudioGuide {
public:
    explicit VirtualAudioGuide(GuideConfig config = {}, Platform platform = currentPlatform())
        : config_(std::move(config)), platform_(platform) {}

    Platform platform() const { return platform_; }
    const GuideConfig& config() const { return config_; }

    std::string installInstructions() const;
    std::string digitalModeInstructions() const;
    std::string linuxPersistentConfig() const;
    std::string preferredDeviceName() const;
    std::vector<std::string> linuxConfigurationCommands() const;
    std::vector<std::string> macOSDiagnosticCommands() const;
    std::vector<std::string> windowsDiagnosticCommands() const;

    // Platform-specific text appended to a VerificationResult on a sample-rate mismatch.
    // Wired into FormatProbe::verifyConfiguration.
    std::vector<std::string> sampleRateConfigurationSuggestions(const std::string& deviceName) const;

private:
    GuideConfig config_;
    Platform platform_;
};

// Result of attempting to auto-configure virtual audio.
struct ConfigurationResult {
    bool success = false;
    std::string message;
    std::string sinkName;
    std::vector<std::string> commands;  // the shell commands attempted, in order
};

// The shell-dependent platform operations. Takes an injected ShellRunner (the test seam) so
// the SAFE_SHELL_ARG validation + command sequence + exit-code interpretation are unit-tested
// without a real PulseAudio / system_profiler.
class PlatformConfigurator {
public:
    explicit PlatformConfigurator(ShellRunner& shell, GuideConfig config = {},
                                  Platform platform = currentPlatform())
        : shell_(shell), config_(std::move(config)), platform_(platform) {}

    // Validate the sink name against SAFE_SHELL_ARG (the security control), unload any
    // existing sink, create the null sink at the required rate/channels/format, add a
    // monitor loopback, and verify. Linux-only.
    ConfigurationResult autoConfigureLinux();

    // system_profiler | grep -i blackhole. macOS-only; false on any other platform
    // or on error.
    bool isMacOSBlackHoleInstalled();

    // Exposed for testing: the SAFE_SHELL_ARG allow-list. A sink name is shell-safe
    // iff it is non-empty and contains only [A-Za-z0-9_-].
    static bool isShellSafe(const std::string& arg);

private:
    ShellRunner& shell_;
    GuideConfig config_;
    Platform platform_;
};

// Diagnostic reports — need live device data, so they take the enumerator (and, for the
// enhanced report, the format probe). They produce a plain diagnostic report and an
// enhanced one.
std::string diagnosticReport(DeviceEnumerator& enumerator, const VirtualAudioGuide& guide);
std::string enhancedDiagnosticReport(DeviceEnumerator& enumerator, FormatProbe& probe,
                                     const VirtualAudioGuide& guide);

}  // namespace naudio
