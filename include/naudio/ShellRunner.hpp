// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace naudio {

// Result of running a shell command: the process exit code and its captured stdout.
struct ShellResult {
    int exitCode = -1;
    std::string output;  // captured stdout (stderr redirected per the command)
};

// Seam over the OS process primitive, exactly analogous to DeviceBackend over
// PortAudio. The shell-out *policy* — SAFE_SHELL_ARG validation,
// command construction, exit-code interpretation — lives in PlatformConfigurator (core) on
// top of this; the raw "run a shell line" primitive lives here so tests drive the policy with
// a FakeShellRunner instead of a real PulseAudio / system_profiler.
//
// SECURITY: runShell() executes an arbitrary shell line — it performs NO sanitization. Every
// caller MUST validate any externally-influenced substring against SAFE_SHELL_ARG before
// interpolating it into the command (defense-in-depth).
class ShellRunner {
public:
    virtual ~ShellRunner() = default;

    // Run `command` through the shell and return {exit code, stdout} by running
    // `bash -c <command>`.
    virtual ShellResult runShell(const std::string& command) = 0;
};

// Production runner: executes the command via popen() (POSIX) / _popen() (Windows).
// popen reads stdout and yields the child's exit status.
class PosixShellRunner : public ShellRunner {
public:
    ShellResult runShell(const std::string& command) override;
};

// In-memory runner for tests — no real process. Responses are a SEQUENTIAL script consumed in
// call order: each runShell() pops the next queued {exit, stdout}. Sequential (not substring-
// matched) because autoConfigureLinux runs the *same* "list sinks | grep" command before and
// after creating the sink, expecting different results — only call order distinguishes them.
// `commands` records every command in order, so a test can assert the exact sequence AND that an
// invalid sink name is rejected BEFORE any command runs (the security control).
class FakeShellRunner : public ShellRunner {
public:
    // Enqueue the next scripted response (returned by the next runShell call). Chainable.
    FakeShellRunner& push(int exitCode, std::string output = "") {
        responses_.push_back({exitCode, std::move(output)});
        return *this;
    }

    // Result returned once the scripted queue is exhausted (default: exit 0, empty output).
    FakeShellRunner& setDefault(int exitCode, std::string output = "") {
        default_ = {exitCode, std::move(output)};
        return *this;
    }

    ShellResult runShell(const std::string& command) override {
        commands.push_back(command);
        if (next_ < responses_.size()) return responses_[next_++];
        return default_;
    }

    // Every command runShell() was asked to run, in order — the test's audit log.
    std::vector<std::string> commands;

private:
    std::vector<ShellResult> responses_;
    std::size_t next_ = 0;
    ShellResult default_{0, ""};
};

}  // namespace naudio
