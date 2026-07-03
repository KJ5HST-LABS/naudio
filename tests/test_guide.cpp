// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — guidance / diagnostics / shell-config layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Unit tests for the guidance / diagnostics / shell-config layer: text generation, the
// SAFE_SHELL_ARG security control, autoConfigureLinux command sequencing, the macOS BlackHole
// probe, app-name genericization, and the FormatProbe suggestion wiring. Driven by
// FakeShellRunner + FakeBackend — no hardware.
#include <gtest/gtest.h>

#include <cctype>
#include <string>
#include <vector>

#include "naudio/DeviceEnumerator.hpp"
#include "naudio/FakeBackend.hpp"
#include "naudio/FormatProbe.hpp"
#include "naudio/ShellRunner.hpp"
#include "naudio/VirtualAudioGuide.hpp"

using namespace naudio;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Case-insensitive substring search — used for the app-coupling assertions.
bool containsNoCase(const std::string& haystack, std::string needle) {
    std::string h = haystack;
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(needle) != std::string::npos;
}

RawDevice raw(int id, std::string name, std::string hostApi, int in, int out, double rate) {
    RawDevice d;
    d.backendId = id;
    d.name = std::move(name);
    d.hostApi = std::move(hostApi);
    d.maxInputChannels = in;
    d.maxOutputChannels = out;
    d.defaultSampleRate = rate;
    return d;
}

}  // namespace

// ===== SAFE_SHELL_ARG — the security control =====

TEST(ShellSafe, AcceptsAlnumUnderscoreHyphen) {
    EXPECT_TRUE(PlatformConfigurator::isShellSafe("naudio_sink"));
    EXPECT_TRUE(PlatformConfigurator::isShellSafe("app1_audio"));
    EXPECT_TRUE(PlatformConfigurator::isShellSafe("my-sink-2"));
    EXPECT_TRUE(PlatformConfigurator::isShellSafe("ABC123"));
}

TEST(ShellSafe, RejectsEmptyAndShellMetacharacters) {
    EXPECT_FALSE(PlatformConfigurator::isShellSafe(""));
    EXPECT_FALSE(PlatformConfigurator::isShellSafe("a b"));            // space
    EXPECT_FALSE(PlatformConfigurator::isShellSafe("sink;rm -rf /"));  // command injection
    EXPECT_FALSE(PlatformConfigurator::isShellSafe("$(whoami)"));      // command substitution
    EXPECT_FALSE(PlatformConfigurator::isShellSafe("a|b"));            // pipe
    EXPECT_FALSE(PlatformConfigurator::isShellSafe("a`b`"));           // backtick
    EXPECT_FALSE(PlatformConfigurator::isShellSafe("a&b"));            // background
    EXPECT_FALSE(PlatformConfigurator::isShellSafe("../etc"));         // path traversal (dot, slash)
}

// ===== autoConfigureLinux =====

TEST(AutoConfigureLinux, RefusesOnNonLinuxWithoutRunningAnything) {
    FakeShellRunner shell;
    PlatformConfigurator cfg(shell, {}, Platform::MacOS);

    auto r = cfg.autoConfigureLinux();
    EXPECT_FALSE(r.success);
    EXPECT_TRUE(contains(r.message, "only supported on Linux"));
    EXPECT_TRUE(shell.commands.empty());  // nothing executed off-platform
}

TEST(AutoConfigureLinux, RejectsUnsafeSinkNameBeforeAnyCommand) {
    // The security control: an invalid sink name must be refused BEFORE a single shell call.
    FakeShellRunner shell;
    GuideConfig bad;
    bad.sinkName = "evil; rm -rf /";
    PlatformConfigurator cfg(shell, bad, Platform::Linux);

    auto r = cfg.autoConfigureLinux();
    EXPECT_FALSE(r.success);
    EXPECT_TRUE(contains(r.message, "Invalid sink name"));
    EXPECT_TRUE(shell.commands.empty());  // <-- never reached the shell
}

TEST(AutoConfigureLinux, HappyPathCreatesSinkWhenNoneExists) {
    FakeShellRunner shell;
    shell.push(1, "")                  // check: sink not found -> skip unload
         .push(0, "")                  // create null-sink: ok
         .push(0, "")                  // loopback: ok
         .push(0, "5  naudio_sink ");  // verify: found, output mentions the sink
    PlatformConfigurator cfg(shell, {}, Platform::Linux);

    auto r = cfg.autoConfigureLinux();
    EXPECT_TRUE(r.success);
    EXPECT_TRUE(contains(r.message, "created successfully"));
    EXPECT_EQ(r.sinkName, "naudio_sink");
    // 4 commands: check, create, loopback, verify (no unload, since the sink did not exist).
    ASSERT_EQ(shell.commands.size(), 4u);
    EXPECT_TRUE(contains(shell.commands[0], "list short sinks"));
    EXPECT_TRUE(contains(shell.commands[1], "load-module module-null-sink"));
    EXPECT_TRUE(contains(shell.commands[1], "rate=48000"));
    EXPECT_TRUE(contains(shell.commands[2], "module-loopback"));
    EXPECT_TRUE(contains(shell.commands[3], "list short sinks"));
}

TEST(AutoConfigureLinux, UnloadsExistingSinkFirst) {
    FakeShellRunner shell;
    shell.push(0, "3  naudio_sink ")   // check: sink EXISTS -> unload branch runs
         .push(0, "")                  // unload xargs command
         .push(0, "")                  // create: ok
         .push(0, "")                  // loopback: ok
         .push(0, "3  naudio_sink ");  // verify: found
    PlatformConfigurator cfg(shell, {}, Platform::Linux);

    auto r = cfg.autoConfigureLinux();
    EXPECT_TRUE(r.success);
    ASSERT_EQ(shell.commands.size(), 5u);                       // includes the unload step
    EXPECT_TRUE(contains(shell.commands[1], "unload-module"));  // the xargs unload
}

TEST(AutoConfigureLinux, ReportsCreateFailure) {
    FakeShellRunner shell;
    shell.push(1, "")                                   // check: not found
         .push(1, "Connection refused");                // create: FAILS
    PlatformConfigurator cfg(shell, {}, Platform::Linux);

    auto r = cfg.autoConfigureLinux();
    EXPECT_FALSE(r.success);
    EXPECT_TRUE(contains(r.message, "Failed to create null sink"));
    EXPECT_TRUE(contains(r.message, "Connection refused"));  // surfaces the command output
}

TEST(AutoConfigureLinux, ReportsVerifyMissAfterApparentSuccess) {
    FakeShellRunner shell;
    shell.push(1, "")   // check: not found
         .push(0, "")   // create: ok
         .push(0, "")   // loopback: ok
         .push(1, "");  // verify: NOT found (grep exit 1, no output)
    PlatformConfigurator cfg(shell, {}, Platform::Linux);

    auto r = cfg.autoConfigureLinux();
    EXPECT_FALSE(r.success);
    EXPECT_TRUE(contains(r.message, "device not found"));
}

// ===== isMacOSBlackHoleInstalled =====

TEST(BlackHoleProbe, FalseOffMacOSWithoutRunning) {
    FakeShellRunner shell;
    PlatformConfigurator cfg(shell, {}, Platform::Linux);
    EXPECT_FALSE(cfg.isMacOSBlackHoleInstalled());
    EXPECT_TRUE(shell.commands.empty());
}

TEST(BlackHoleProbe, TrueWhenProfilerMentionsBlackHole) {
    FakeShellRunner shell;
    shell.push(0, "            BlackHole 2ch:\n");
    PlatformConfigurator cfg(shell, {}, Platform::MacOS);
    EXPECT_TRUE(cfg.isMacOSBlackHoleInstalled());
    ASSERT_EQ(shell.commands.size(), 1u);
    EXPECT_TRUE(contains(shell.commands[0], "system_profiler"));
}

TEST(BlackHoleProbe, FalseWhenProfilerEmpty) {
    FakeShellRunner shell;
    shell.push(1, "");  // grep found nothing
    PlatformConfigurator cfg(shell, {}, Platform::MacOS);
    EXPECT_FALSE(cfg.isMacOSBlackHoleInstalled());
}

// ===== Guidance text + genericization =====

TEST(Guide, InstallInstructionsCarryFormatAndUseGenericDefaults) {
    GuideConfig c;  // generic defaults — the library core bakes in no app-coupling names
    const std::vector<std::string> texts = {
        VirtualAudioGuide(c, Platform::MacOS).installInstructions(),
        VirtualAudioGuide(c, Platform::Windows).installInstructions(),
        VirtualAudioGuide(c, Platform::Linux).installInstructions(),
    };

    for (const std::string& s : texts) {
        EXPECT_TRUE(contains(s, "48000"));  // the required rate is substituted into every platform's text
    }
    // The Linux (PulseAudio) path names the null sink with the generic, parameterized default
    // token; the next test proves a consumer can inject its own identifiers instead.
    EXPECT_TRUE(contains(texts[2], "naudio_sink"));
}

TEST(Guide, ParameterizationSubstitutesCustomIdentifiers) {
    // Proves the genericization is real parameterization, not deletion: a consumer can inject
    // its own names and they appear in the generated text + pactl commands.
    GuideConfig c;
    c.sinkName = "myradio_sink";
    c.sinkDescription = "MyRadio_Audio";
    c.digitalModeApp = "MyDigiApp";
    VirtualAudioGuide lin(c, Platform::Linux);

    const std::string text = lin.installInstructions();
    EXPECT_TRUE(contains(text, "myradio_sink"));
    EXPECT_TRUE(contains(text, "MyRadio_Audio"));
    EXPECT_TRUE(contains(text, "MyDigiApp"));

    const auto cmds = lin.linuxConfigurationCommands();
    bool sawSink = false;
    for (const auto& cmd : cmds) {
        if (contains(cmd, "sink_name=myradio_sink")) sawSink = true;
    }
    EXPECT_TRUE(sawSink);
}

TEST(Guide, SampleRateSuggestionsArePlatformSpecific) {
    GuideConfig c;
    EXPECT_TRUE(contains(VirtualAudioGuide(c, Platform::MacOS)
                             .sampleRateConfigurationSuggestions("BlackHole 2ch")[0],
                         "Audio MIDI Setup"));

    // Windows + a CABLE device takes the CABLE-specific branch.
    auto win = VirtualAudioGuide(c, Platform::Windows)
                   .sampleRateConfigurationSuggestions("CABLE Output");
    bool sawCable = false;
    for (const auto& s : win) {
        if (contains(s, "CABLE Output")) sawCable = true;
    }
    EXPECT_TRUE(sawCable);

    // Linux suggestions reference the (parameterized) sink name.
    auto lin = VirtualAudioGuide(c, Platform::Linux).sampleRateConfigurationSuggestions("sink");
    bool sawSink = false;
    for (const auto& s : lin) {
        if (contains(s, "sink_name=naudio_sink")) sawSink = true;
    }
    EXPECT_TRUE(sawSink);

    // Unknown platform yields no suggestions (default branch returns empty).
    EXPECT_TRUE(VirtualAudioGuide(c, Platform::Unknown)
                    .sampleRateConfigurationSuggestions("x")
                    .empty());
}

TEST(Guide, PreferredDeviceNamePerPlatform) {
    GuideConfig c;
    EXPECT_EQ(VirtualAudioGuide(c, Platform::MacOS).preferredDeviceName(), "BlackHole 2ch");
    EXPECT_TRUE(contains(VirtualAudioGuide(c, Platform::Windows).preferredDeviceName(), "CABLE"));
    EXPECT_EQ(VirtualAudioGuide(c, Platform::Linux).preferredDeviceName(), "Virtual Audio");
}

// ===== FormatProbe + guide wiring (verifyConfiguration appends platform suggestions) =====

TEST(FormatProbeWithGuide, MismatchAppendsPlatformSuggestions) {
    FakeBackend be;  // nothing registered -> 48k unsupported -> sample-rate mismatch
    GuideConfig c;
    VirtualAudioGuide guide(c, Platform::MacOS);
    FormatProbe probe(be, &guide);

    DeviceInfo d;
    d.backendId = 3;
    d.name = "BlackHole 2ch";
    d.defaultSampleRate = 44100;
    d.maxInputChannels = 2;

    auto r = probe.verifyConfiguration(&d, AudioFormat{}, Direction::Capture);
    EXPECT_FALSE(r.success);
    bool sawMidi = false;
    for (const auto& s : r.suggestions) {
        if (contains(s, "Audio MIDI Setup")) sawMidi = true;
    }
    EXPECT_TRUE(sawMidi);  // platform-specific text came from the guide, not the generic stub
}

TEST(FormatProbeWithoutGuide, MismatchUsesGenericSuggestion) {
    FakeBackend be;
    FormatProbe probe(be);  // no guide -> generic fallback

    DeviceInfo d;
    d.backendId = 3;
    d.name = "CABLE Output";
    d.defaultSampleRate = 44100;
    d.maxInputChannels = 2;

    auto r = probe.verifyConfiguration(&d, AudioFormat{}, Direction::Capture);
    ASSERT_EQ(r.suggestions.size(), 1u);
    EXPECT_TRUE(contains(r.suggestions[0], "Set the device's format sample rate"));
}

// ===== Diagnostic reports (generate[Enhanced]DiagnosticReport) =====

TEST(DiagnosticReport, ListsDevicesAndFlagsVirtual) {
    FakeBackend be;
    be.add(raw(0, "MacBook Pro Microphone", "Core Audio", 1, 0, 48000));  // hardware
    be.add(raw(1, "BlackHole 2ch", "Core Audio", 2, 2, 48000));           // virtual (duplex)
    DeviceEnumerator en(be, Platform::MacOS);
    VirtualAudioGuide guide({}, Platform::MacOS);

    const std::string report = diagnosticReport(en, guide);
    EXPECT_TRUE(contains(report, "MacBook Pro Microphone"));
    EXPECT_TRUE(contains(report, "BlackHole 2ch [VIRTUAL]"));
    EXPECT_TRUE(contains(report, "Platform: macOS"));
    // A virtual capture + virtual playback both exist (BlackHole is duplex) -> available.
    EXPECT_TRUE(contains(report, "Virtual Audio Available: YES"));
}

TEST(DiagnosticReport, AppendsInstallInstructionsWhenNoVirtualAudio) {
    FakeBackend be;
    be.add(raw(0, "Built-in Output", "Core Audio", 0, 2, 48000));  // hardware only
    DeviceEnumerator en(be, Platform::MacOS);
    VirtualAudioGuide guide({}, Platform::MacOS);

    const std::string report = diagnosticReport(en, guide);
    EXPECT_TRUE(contains(report, "Virtual Audio Available: NO"));
    EXPECT_TRUE(contains(report, "macOS Virtual Audio Setup"));  // install instructions appended
}

TEST(EnhancedDiagnosticReport, MarksRecommendedAndFlagsMismatch) {
    FakeBackend be;
    // Virtual device whose default rate is 44100 and 48k is unsupported -> mismatch issue.
    be.add(raw(1, "BlackHole 2ch", "Core Audio", 2, 2, 44100));
    DeviceEnumerator en(be, Platform::MacOS);
    VirtualAudioGuide guide({}, Platform::MacOS);
    FormatProbe probe(be, &guide);

    const std::string report = enhancedDiagnosticReport(en, probe, guide);
    EXPECT_TRUE(contains(report, "BlackHole 2ch"));
    EXPECT_TRUE(contains(report, "RECOMMENDED"));        // best-virtual selection marked
    EXPECT_TRUE(contains(report, "Sample rate mismatch"));  // verify issue surfaced inline
    EXPECT_TRUE(contains(report, "NEEDS CONFIGURATION"));
}

// A healthy capture device must NOT suppress a misconfigured playback device's remediation steps:
// the CONFIGURATION NEEDED block should carry the PLAYBACK suggestions even though capture is OK.
TEST(EnhancedDiagnosticReport, ShowsPlaybackSuggestionsEvenWhenCaptureIsReady) {
    FakeBackend be;
    be.add(raw(1, "BlackHole In", "Core Audio", 2, 0, 48000));   // virtual capture-only, 48 kHz OK
    be.add(raw(2, "BlackHole Out", "Core Audio", 0, 2, 44100));  // virtual playback-only, 44.1 kHz
    be.addSupportedFormat(1, Direction::Capture, AudioFormat{}); // capture supports 48k; playback does not
    DeviceEnumerator en(be, Platform::MacOS);
    VirtualAudioGuide guide({}, Platform::MacOS);
    FormatProbe probe(be, &guide);

    const std::string report = enhancedDiagnosticReport(en, probe, guide);
    EXPECT_TRUE(contains(report, "Capture Device Ready: YES"));
    EXPECT_TRUE(contains(report, "Playback Device Ready: NEEDS CONFIGURATION"));
    EXPECT_TRUE(contains(report, "CONFIGURATION NEEDED"));
    // The playback device's platform suggestion appears (it was suppressed before the fix — the
    // suggestions were taken only from the healthy capture device). It is NOT the install text
    // (virtual audio IS available), so this string can only come from the suggestions block.
    EXPECT_TRUE(contains(report, "Audio MIDI Setup"));
}
