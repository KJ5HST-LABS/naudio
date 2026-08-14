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
//
// The name is matched as a WHOLE FIELD, not as a substring. `pactl list short sinks` is
// tab-separated `index<TAB>name<TAB>driver<TAB>...`, so field 2 is cut out and compared as a
// complete line with `-F` (fixed string) and `-x` (whole line). A bare `grep <name>` reported a
// sink as existing whenever its name was a substring of ANY other sink's name — or of a driver or
// sample-spec field on an unrelated line.
inline std::string checkSinkCommand(const std::string& sinkName) {
    return "pactl list short sinks | cut -f2 | grep -Fx " + sinkName;
}

// Unload ONLY the null-sink module bound to `sinkName` (not every null sink).
//
// The `sink_name=<name>` token is matched exactly, delimited by whitespace or the line edges.
// `pactl list short modules` is `index<TAB>module<TAB>argument<TAB>usage` and the argument is a
// space-separated list such as `sink_name=naudio rate=48000 ...`, so those are the only boundaries
// a real token can have. A bare `grep <name>` matched every module whose argument merely CONTAINED
// the name, and the result is piped straight into `xargs pactl unload-module` — so unloading a sink
// named `naudio` would also have destroyed `naudio_2` and `naudio_backup`. SAFE_SHELL_ARG
// guarantees the name cannot inject shell syntax; it does not make the name unique, and it permits
// `-`, which is why `grep -w` is not sufficient either (`-` is not a word constituent, so
// `sink_name=my-sink` would still match `sink_name=my-sink-2`).
inline std::string unloadSinkPipeline(const std::string& sinkName) {
    return "pactl list short modules | grep module-null-sink | grep -E "
           "'(^|[[:space:]])sink_name=" +
           sinkName +
           "([[:space:]]|$)'"
           " | cut -f1 | xargs -r pactl unload-module";
}

}  // namespace pulse
}  // namespace naudio
