// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/ShellRunner.hpp"

#include <array>
#include <cstdio>

#if defined(_WIN32)
#define NAUDIO_POPEN  _popen
#define NAUDIO_PCLOSE _pclose
#else
#include <sys/wait.h>  // WIFEXITED / WEXITSTATUS for the pclose wait status
#define NAUDIO_POPEN  popen
#define NAUDIO_PCLOSE pclose
#endif

namespace naudio {

ShellResult PosixShellRunner::runShell(const std::string& command) {
    // popen runs `command` via the shell (sh -c on POSIX), reads its stdout, and
    // yields the child's exit code.
    // No shell metacharacter sanitization happens here by design: the SAFE_SHELL_ARG check in
    // the caller (PlatformConfigurator) is the security boundary (see ShellRunner.hpp).
    ShellResult result;
    FILE* pipe = NAUDIO_POPEN(command.c_str(), "r");
    if (pipe == nullptr) {
        result.exitCode = -1;
        return result;
    }

    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output += buffer.data();
    }

    const int status = NAUDIO_PCLOSE(pipe);
#if defined(_WIN32)
    result.exitCode = status;  // _pclose returns the child exit code directly
#else
    // POSIX: pclose returns a wait(2) status; extract the exit code (WEXITSTATUS) so callers
    // compare against 0.
    if (status == -1) {
        result.exitCode = -1;
    } else if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else {
        result.exitCode = -1;
    }
#endif
    return result;
}

}  // namespace naudio
