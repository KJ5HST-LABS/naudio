// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
//
// Shell-dependent platform operations: autoConfigureLinux and
// isMacOSBlackHoleInstalled. The OS process primitive is injected as
// a ShellRunner (the test seam) so the SAFE_SHELL_ARG validation + command sequence + exit-code
// interpretation run identically under a real PulseAudio and under FakeShellRunner.
#include "naudio/VirtualAudioGuide.hpp"

#include <cctype>
#include <string>

#include "naudio/PulseCommands.hpp"
#include "naudio/ShellRunner.hpp"
#include "naudio/StringUtil.hpp"

namespace naudio {

bool PlatformConfigurator::isShellSafe(const std::string& arg) {
    // SAFE_SHELL_ARG = ^[a-zA-Z0-9_-]+$. The security control: any value interpolated
    // into a shell command must match before use (defense-in-depth).
    if (arg.empty()) return false;
    for (const char ch : arg) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

ConfigurationResult PlatformConfigurator::autoConfigureLinux() {
    ConfigurationResult result;

    // Linux only.
    if (platform_ != Platform::Linux) {
        result.message = "Auto-configuration is only supported on Linux";
        return result;
    }

    const std::string sinkName = config_.sinkName;
    result.sinkName = sinkName;

    // validate the sink name BEFORE it touches any shell command (the security control).
    if (!isShellSafe(sinkName)) {
        result.message = "Invalid sink name: " + sinkName;
        return result;
    }
    // The description is ALSO interpolated into the pactl command, so it MUST pass the same
    // SAFE_SHELL_ARG check (validate ANY externally-influenced substring before
    // interpolation). The generic default "Naudio_Virtual_Audio" passes; a value with spaces or
    // shell metacharacters is rejected here rather than reaching the shell.
    if (!isShellSafe(config_.sinkDescription)) {
        result.message = "Invalid sink description: " + config_.sinkDescription;
        return result;
    }

    // if the sink already exists, unload it first.
    const std::string checkCmd = pulse::checkSinkCommand(sinkName);
    result.commands.push_back(checkCmd);
    const ShellResult check = shell_.runShell(checkCmd);
    if (check.exitCode == 0) {
        // Record the ACTUAL command run (unload only the null sink bound to this sink_name), not a
        // placeholder — so result.commands is a faithful, replayable audit trail.
        const std::string unloadCmd = pulse::unloadSinkPipeline(sinkName);
        result.commands.push_back(unloadCmd);
        shell_.runShell(unloadCmd);
    }

    // create the null sink with the required rate/channels/format.
    const std::string createSinkCmd =
        pulse::nullSinkCommand(sinkName, config_.sinkDescription, config_.format);
    result.commands.push_back(createSinkCmd);
    const ShellResult create = shell_.runShell(createSinkCmd);
    if (create.exitCode != 0) {
        result.message =
            "Failed to create null sink. Is PulseAudio/PipeWire running? Output: " + create.output;
        return result;
    }

    // create the monitor loopback (a failure here is non-fatal; the sink still exists).
    const std::string loopbackCmd = pulse::loopbackCommand(sinkName);
    result.commands.push_back(loopbackCmd);
    shell_.runShell(loopbackCmd);

    // verify the sink now appears.
    const std::string verifyCmd = pulse::checkSinkCommand(sinkName);
    result.commands.push_back(verifyCmd);
    const ShellResult verify = shell_.runShell(verifyCmd);
    if (verify.exitCode == 0 && verify.output.find(sinkName) != std::string::npos) {
        result.success = true;
        result.message = "Virtual audio device created successfully: " + sinkName;
    } else {
        result.message = "Sink creation command succeeded but device not found";
    }
    return result;
}

bool PlatformConfigurator::isMacOSBlackHoleInstalled() {
    // macOS only; system_profiler | grep -i blackhole, true iff output mentions it.
    if (platform_ != Platform::MacOS) {
        return false;
    }
    const ShellResult r =
        shell_.runShell("system_profiler SPAudioDataType 2>/dev/null | grep -i blackhole");

    return containsCaseInsensitive(r.output, "blackhole");
}

}  // namespace naudio
