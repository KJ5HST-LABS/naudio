// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <string>

#include "naudio/Types.hpp"

// Single source of truth for the PulseAudio/PipeWire `pactl` commands. Both the human-facing
// guidance (VirtualAudioGuide::linuxConfigurationCommands) and the executed auto-config
// (PlatformConfigurator::autoConfigureLinux) build their commands here, so the documented
// commands cannot drift from the ones actually run. Callers MUST validate any externally
// influenced substring (sink name / description) with PlatformConfigurator::isShellSafe before
// passing it here — these builders do no sanitization.
namespace naudio {
namespace pulse {

// pactl module-null-sink creation for a virtual sink at the required rate/channels/format.
inline std::string nullSinkCommand(const std::string& sinkName, const std::string& sinkDescription,
                                   const AudioFormat& fmt) {
    return "pactl load-module module-null-sink sink_name=" + sinkName +
           " sink_properties=device.description=" + sinkDescription +
           " rate=" + std::to_string(fmt.sampleRate) +
           " channels=" + std::to_string(fmt.channels) +
           " format=s" + std::to_string(fmt.bitsPerSample) + "le";
}

// pactl module-loopback so the sink's monitor feeds back for bidirectional audio.
inline std::string loopbackCommand(const std::string& sinkName, int latencyMsec = 20) {
    return "pactl load-module module-loopback source=" + sinkName + ".monitor sink=" + sinkName +
           " latency_msec=" + std::to_string(latencyMsec);
}

// Check whether the sink currently exists (grep exit 0 == found).
inline std::string checkSinkCommand(const std::string& sinkName) {
    return "pactl list short sinks | grep " + sinkName;
}

// Unload ONLY the null-sink module bound to `sinkName` (not every null sink).
inline std::string unloadSinkPipeline(const std::string& sinkName) {
    return "pactl list short modules | grep module-null-sink | grep " + sinkName +
           " | cut -f1 | xargs -r pactl unload-module";
}

}  // namespace pulse
}  // namespace naudio
