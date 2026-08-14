// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — format-probe / device-verification logic.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Unit tests for the format-probe / verification logic (supportsSampleRate and
// verifyDeviceConfiguration). Driven entirely by FakeBackend — no PortAudio, no hardware.
#include <gtest/gtest.h>

#include <string>

#include "naudio/DeviceEnumerator.hpp"
#include "naudio/FakeBackend.hpp"
#include "naudio/FormatProbe.hpp"

using namespace naudio;

namespace {

AudioFormat fmt(int rate = 48000, int bits = 16, int channels = 2) {
    AudioFormat f;
    f.sampleRate = rate;
    f.bitsPerSample = bits;
    f.channels = channels;
    return f;
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

// Construct a classified DeviceInfo directly, to unit-test FormatProbe in isolation.
DeviceInfo info(int id, std::string name, double rate, int inCh, int outCh) {
    DeviceInfo d;
    d.backendId = id;
    d.name = std::move(name);
    d.defaultSampleRate = rate;
    d.maxInputChannels = inCh;
    d.maxOutputChannels = outCh;
    return d;
}

bool hasIssueContaining(const VerificationResult& r, const std::string& needle) {
    for (const auto& i : r.issues) {
        if (i.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ---- backend probe primitive --------------------------------------------------------

TEST(FakeBackendProbe, ReportsRegisteredFormatsOnly) {
    FakeBackend be;
    be.addSupportedFormat(7, Direction::Capture, fmt());

    EXPECT_TRUE(be.probeFormat(7, fmt(), Direction::Capture));
    EXPECT_FALSE(be.probeFormat(7, fmt(), Direction::Playback));      // wrong direction
    EXPECT_FALSE(be.probeFormat(7, fmt(44100), Direction::Capture));  // wrong rate
    EXPECT_FALSE(be.probeFormat(9, fmt(), Direction::Capture));       // wrong device
}

// ---- FormatProbe.supports ------------------------------------------------------------

TEST(FormatProbe, SupportsDelegatesToBackendPerDirection) {
    FakeBackend be;
    be.addSupportedFormat(1, Direction::Playback, fmt());
    FormatProbe probe(be);

    EXPECT_TRUE(probe.supports(info(1, "X", 48000, 0, 2), fmt(), Direction::Playback));
    EXPECT_FALSE(probe.supports(info(1, "X", 48000, 0, 2), fmt(), Direction::Capture));
}

// ---- FormatProbe.verifyConfiguration ------------------------------------------------

TEST(FormatProbe, VerifyNullDeviceReportsUnsupported) {
    FakeBackend be;
    FormatProbe probe(be);

    auto r = probe.verifyConfiguration(nullptr, fmt(), Direction::Capture);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.deviceName, "null");
    ASSERT_EQ(r.issues.size(), 1u);
    EXPECT_EQ(r.issues[0], "No device specified");
    ASSERT_EQ(r.suggestions.size(), 1u);
    EXPECT_EQ(r.suggestions[0], "Select a virtual audio device");
}

TEST(FormatProbe, VerifySupportedFormatSucceedsCleanly) {
    FakeBackend be;
    be.addSupportedFormat(2, Direction::Capture, fmt());
    FormatProbe probe(be);

    auto d = info(2, "BlackHole 2ch", 48000, 2, 2);
    auto r = probe.verifyConfiguration(&d, fmt(), Direction::Capture);
    EXPECT_TRUE(r.success);
    EXPECT_TRUE(r.issues.empty());
    EXPECT_EQ(r.actualSampleRate, 48000);  // exact match reports the required values
    EXPECT_EQ(r.actualChannels, 2);
}

// The plan's named must-test case: a virtual device that does not support 48 kHz.
TEST(FormatProbe, Verify48kMismatchProducesIssueAndSuggestion) {
    FakeBackend be;  // nothing registered -> probeFormat is false for 48 kHz
    FormatProbe probe(be);

    auto d = info(3, "CABLE Output", 44100, 2, 2);
    auto r = probe.verifyConfiguration(&d, fmt(48000), Direction::Capture);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.actualSampleRate, 44100);
    EXPECT_EQ(r.requiredSampleRate, 48000);
    EXPECT_TRUE(hasIssueContaining(r, "Sample rate mismatch"));
    EXPECT_FALSE(r.suggestions.empty());
}

// The coincidence hole: the exact-format probe FAILS, but the device's reported defaults happen to
// equal the required rate AND channel count. Both mismatch pushes are diff-gated, so `issues` stays
// empty and the old `formatSupported || issues.empty()` predicate reported the device ready — while
// StreamOpener, which asks the same probe, refuses to open it. The guide printed "Ready: YES" for a
// device that cannot be opened, which is the worst possible answer for on-air setup.
TEST(FormatProbe, VerifyFailedProbeIsNotReadyEvenWhenDefaultsCoincide) {
    FakeBackend be;  // nothing registered -> probeFormat is false for every format
    FormatProbe probe(be);

    auto d = info(9, "Coincidence CODEC", 48000, 2, 2);  // defaults EQUAL the required format
    auto r = probe.verifyConfiguration(&d, fmt(48000, 16, 2), Direction::Capture);

    EXPECT_FALSE(r.success) << "the exact-format probe failed; the device is not ready";
    // The point of the arm: there is no mismatch to report, so `issues` alone cannot carry this.
    EXPECT_TRUE(r.issues.empty()) << "no mismatch exists to describe — that is the trap";
    EXPECT_EQ(r.actualSampleRate, 48000);
    EXPECT_EQ(r.actualChannels, 2);
    // And the verdict agrees with the opener that actually has to open it.
    EXPECT_FALSE(probe.supports(d, fmt(48000, 16, 2), Direction::Capture));
}

// The playback direction has no mono fallback in StreamOpener at all, so a false "ready" there is
// a hard open failure rather than a degraded one.
TEST(FormatProbe, VerifyFailedProbeIsNotReadyOnPlaybackWhenDefaultsCoincide) {
    FakeBackend be;
    FormatProbe probe(be);

    auto d = info(10, "Coincidence Sink", 48000, 2, 2);
    auto r = probe.verifyConfiguration(&d, fmt(48000, 16, 2), Direction::Playback);
    EXPECT_FALSE(r.success);
    EXPECT_TRUE(r.issues.empty());
}

// The other half of the predicate. `success` requires the probe to pass AND no issues, and this is
// the arm for the AND: a backend that waves through a DEGENERATE format (0 Hz) makes
// `formatSupported` true while the rate analysis still cannot determine a rate. A probe pass alone
// must not be enough to call such a device ready. Without this arm, `success = formatSupported`
// alone passes every other test in the file.
TEST(FormatProbe, VerifySupportedButDegenerateFormatIsNotReady) {
    FakeBackend be;
    FormatProbe probe(be);
    const AudioFormat degenerate = fmt(0, 16, 2);  // 0 Hz — no rate can be determined
    be.addSupportedFormat(11, Direction::Capture, degenerate);

    auto d = info(11, "Zero Hz", 0, 2, 2);
    auto r = probe.verifyConfiguration(&d, degenerate, Direction::Capture);

    EXPECT_TRUE(probe.supports(d, degenerate, Direction::Capture)) << "premise: the probe passes";
    EXPECT_FALSE(r.success) << "a probe pass alone must not report ready";
    EXPECT_TRUE(hasIssueContaining(r, "Could not determine device sample rate"));
    EXPECT_EQ(r.actualSampleRate, -1);
}

TEST(FormatProbe, VerifyChannelMismatchWhenRateMatches) {
    FakeBackend be;  // unsupported, but the device's default rate equals the required rate
    FormatProbe probe(be);

    auto d = info(4, "Mono Cable", 48000, 1, 1);
    auto r = probe.verifyConfiguration(&d, fmt(48000, 16, 2), Direction::Capture);
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(hasIssueContaining(r, "Sample rate mismatch"));  // rate matched
    EXPECT_TRUE(hasIssueContaining(r, "Channel mismatch"));
    EXPECT_EQ(r.actualChannels, 1);
}

TEST(FormatProbe, VerifyUnknownSampleRateFlaggedAsMinusOne) {
    FakeBackend be;
    FormatProbe probe(be);

    auto d = info(5, "Mystery", 0, 2, 2);
    auto r = probe.verifyConfiguration(&d, fmt(), Direction::Capture);
    EXPECT_EQ(r.actualSampleRate, -1);
    EXPECT_TRUE(hasIssueContaining(r, "Could not determine device sample rate"));
    EXPECT_FALSE(r.success);
}

// An undetermined channel count (0) on an unsupported format must surface an issue and NOT report
// the device as ready — symmetric with the unknown-sample-rate path (previously it was silent, so
// a rate-matching-but-unsupported device with 0 channels was falsely reported OK).
TEST(FormatProbe, VerifyUnknownChannelsFlaggedAndNotReady) {
    FakeBackend be;  // unsupported; device reports 0 channels for the probed direction
    FormatProbe probe(be);

    auto d = info(8, "Phantom", 48000, 0, 0);  // default rate matches required -> no rate issue
    auto r = probe.verifyConfiguration(&d, fmt(48000, 16, 2), Direction::Capture);
    EXPECT_FALSE(r.success);
    EXPECT_TRUE(hasIssueContaining(r, "Could not determine device channel count"));
    EXPECT_EQ(r.actualChannels, -1);
}

// Detected fields must survive the enumerator's classification + DUPLEX-merge path.
TEST(FormatProbe, IntegratesWithEnumeratorDetectedFields) {
    FakeBackend be;
    be.add(raw(0, "CABLE Output", "MME", 2, 0, 44100.0));  // "cable" => virtual on Windows
    DeviceEnumerator en(be, Platform::Windows);

    auto caps = en.captureDevices();
    ASSERT_EQ(caps.size(), 1u);
    EXPECT_TRUE(caps[0].isVirtual());

    FormatProbe probe(be);
    auto r = probe.verifyConfiguration(&caps[0], fmt(48000), Direction::Capture);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.actualSampleRate, 44100);  // carried through from RawDevice via list()
}

// ---- VerificationResult.toString ------------------------

TEST(VerificationResult, ToStringReflectsStatusAndDetails) {
    VerificationResult ok;
    ok.success = true;
    ok.deviceName = "Dev";
    ok.actualSampleRate = 48000;
    ok.requiredSampleRate = 48000;
    ok.actualChannels = 2;
    ok.requiredChannels = 2;
    EXPECT_NE(ok.toString().find("Status: OK"), std::string::npos);

    VerificationResult bad;
    bad.success = false;
    bad.deviceName = "Dev";
    bad.actualSampleRate = 44100;
    bad.requiredSampleRate = 48000;
    bad.actualChannels = 2;
    bad.requiredChannels = 2;
    bad.issues = {"Sample rate mismatch: device is 44100 Hz, required 48000 Hz"};
    bad.suggestions = {"Set the device's format sample rate to 48000 Hz"};
    const std::string s = bad.toString();
    EXPECT_NE(s.find("CONFIGURATION NEEDED"), std::string::npos);
    EXPECT_NE(s.find("(required: 48000 Hz)"), std::string::npos);
    EXPECT_NE(s.find("Issues:"), std::string::npos);
    EXPECT_NE(s.find("Suggestions:"), std::string::npos);
}

// ---- DeviceEnumerator.bestVirtual ----------------------------------------------------

TEST(BestVirtual, ReturnsNulloptWhenNoVirtualDevices) {
    FakeBackend be;
    be.add(raw(0, "MacBook Pro Microphone", "Core Audio", 1, 0, 48000));  // hardware
    DeviceEnumerator en(be, Platform::MacOS);
    EXPECT_FALSE(en.bestVirtual(fmt(), Direction::Capture).has_value());
}

TEST(BestVirtual, PrefersPatternOrderWhenBothSupported) {
    // "blackhole" precedes "loopback" in the preferred list; both support 48k -> blackhole.
    FakeBackend be;
    be.add(raw(1, "Loopback Audio", "Core Audio", 2, 2, 48000));
    be.add(raw(2, "BlackHole 2ch", "Core Audio", 2, 2, 48000));
    be.addSupportedFormat(1, Direction::Capture, fmt());
    be.addSupportedFormat(2, Direction::Capture, fmt());
    DeviceEnumerator en(be, Platform::MacOS);

    auto best = en.bestVirtualCapture(fmt());
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->name, "BlackHole 2ch");
}

TEST(BestVirtual, SkipsPreferredDeviceThatFailsSampleRate) {
    // The preferred "blackhole" device does not support 48k; "loopback" does -> loopback.
    FakeBackend be;
    be.add(raw(1, "BlackHole 2ch", "Core Audio", 2, 2, 44100));  // 48k not registered
    be.add(raw(2, "Loopback Audio", "Core Audio", 2, 2, 48000));
    be.addSupportedFormat(2, Direction::Capture, fmt());
    DeviceEnumerator en(be, Platform::MacOS);

    auto best = en.bestVirtualCapture(fmt());
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->name, "Loopback Audio");
}

TEST(BestVirtual, FallsBackToAnyValidatingVirtualBeyondPreferred) {
    // Linux: "pulse" is virtual (virtualPatterns) but matches NO preferred pattern
    // ({virtual,null,jack}); it supports 48k -> chosen by the step-2 fallback.
    FakeBackend be;
    be.add(raw(1, "pulse", "ALSA", 32, 32, 48000));
    be.addSupportedFormat(1, Direction::Capture, fmt());
    DeviceEnumerator en(be, Platform::Linux);

    auto best = en.bestVirtualCapture(fmt());
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->name, "pulse");
}

TEST(BestVirtual, LastResortReturnsFirstWhenNoneValidate) {
    // No virtual device supports 48k -> last-resort returns the first in order.
    FakeBackend be;
    be.add(raw(1, "BlackHole 2ch", "Core Audio", 2, 2, 44100));
    be.add(raw(2, "Loopback Audio", "Core Audio", 2, 2, 44100));
    DeviceEnumerator en(be, Platform::MacOS);

    auto best = en.bestVirtualCapture(fmt(48000));
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->name, "BlackHole 2ch");
}

TEST(BestVirtual, PlaybackUsesPlaybackSupport) {
    // Device validates for playback but not capture: bestVirtualPlayback validates,
    // bestVirtualCapture last-resorts to the same (only) virtual device.
    FakeBackend be;
    be.add(raw(1, "BlackHole 2ch", "Core Audio", 2, 2, 48000));
    be.addSupportedFormat(1, Direction::Playback, fmt());
    DeviceEnumerator en(be, Platform::MacOS);

    ASSERT_TRUE(en.bestVirtualPlayback(fmt()).has_value());
    auto cap = en.bestVirtualCapture(fmt());
    ASSERT_TRUE(cap.has_value());
    EXPECT_EQ(cap->name, "BlackHole 2ch");
}
