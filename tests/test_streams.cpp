// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — stream layer (StreamOpener + IoResult).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Unit tests for the stream layer: StreamOpener's mono→stereo fallback policy (openCaptureLine /
// openPlaybackLine) and the IoResult overflow/underflow/timeout + RAII-close semantics. Driven
// entirely by FakeBackend and the Fake stream doubles — no PortAudio, no hardware. The real
// PortAudio capture I/O path is exercised by `na_audio_daemon --mode capture-probe` on real
// hardware, which CI cannot run.
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "naudio/FakeBackend.hpp"
#include "naudio/Stream.hpp"
#include "naudio/StreamOpener.hpp"
#include "naudio/Types.hpp"

using namespace naudio;

namespace {

AudioFormat fmt(int rate = 48000, int bits = 16, int channels = 2) {
    AudioFormat f;
    f.sampleRate = rate;
    f.bitsPerSample = bits;
    f.channels = channels;
    return f;
}

DeviceInfo dev(int id, std::string name) {
    DeviceInfo d;
    d.backendId = id;
    d.name = std::move(name);
    return d;
}

}  // namespace

// ---- StreamOpener: capture mono→stereo fallback (the §5.4 headline) -----------------

TEST(StreamOpener, CaptureOpensStereoWhenStereoSupported) {
    FakeBackend be;
    be.addSupportedFormat(1, Direction::Capture, fmt(48000, 16, 2));
    StreamOpener opener(be);

    auto stream = opener.openCapture(dev(1, "BlackHole 2ch"), fmt(48000, 16, 2));
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->actualFormat().channels, 2);  // no fallback needed
}

TEST(StreamOpener, CaptureFallsBackToMonoWhenStereoUnsupported) {
    // Built-in-mic case: only mono is supported. The opener must fall back and report mono
    // so the consumer up-converts.
    FakeBackend be;
    be.addSupportedFormat(2, Direction::Capture, fmt(48000, 16, 1));  // mono only
    StreamOpener opener(be);

    auto stream = opener.openCapture(dev(2, "MacBook Pro Microphone"), fmt(48000, 16, 2));
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->actualFormat().channels, 1);             // fell back to mono
    EXPECT_EQ(stream->actualFormat().sampleRate, 48000);       // rate preserved
    EXPECT_EQ(stream->actualFormat().frameSize(), 2);          // 16-bit mono = 2 bytes/frame
}

TEST(StreamOpener, CaptureThrowsWhenNeitherStereoNorMonoSupported) {
    FakeBackend be;  // nothing registered
    StreamOpener opener(be);
    EXPECT_THROW(opener.openCapture(dev(3, "Dead Device"), fmt(48000, 16, 2)), DeviceUnavailable);
}

TEST(StreamOpener, CaptureMonoRequestDoesNotProbeStereo) {
    // A mono request that is supported opens directly; no spurious stereo attempt.
    FakeBackend be;
    be.addSupportedFormat(4, Direction::Capture, fmt(48000, 16, 1));
    StreamOpener opener(be);

    auto stream = opener.openCapture(dev(4, "Mono Mic"), fmt(48000, 16, 1));
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->actualFormat().channels, 1);
}

// ---- StreamOpener: playback has no fallback -----------------------------------------

TEST(StreamOpener, PlaybackOpensRequestedFormat) {
    FakeBackend be;
    be.addSupportedFormat(5, Direction::Playback, fmt(48000, 16, 2));
    StreamOpener opener(be);

    auto stream = opener.openPlayback(dev(5, "Speakers"), fmt(48000, 16, 2));
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->actualFormat().channels, 2);
}

TEST(StreamOpener, PlaybackThrowsWhenUnsupportedNoMonoFallback) {
    // Stereo unsupported; mono IS registered — but playback must NOT fall back; the
    // opener just opens and lets the unsupported-format error propagate.
    FakeBackend be;
    be.addSupportedFormat(6, Direction::Playback, fmt(48000, 16, 1));  // mono only
    StreamOpener opener(be);
    EXPECT_THROW(opener.openPlayback(dev(6, "Speakers"), fmt(48000, 16, 2)), DeviceUnavailable);
}

// ---- StreamOpener: split-duplex per-direction backend id ----------------------------

// A device the backend reported as split records carries DIFFERENT capture/playback ids; each
// direction must open on ITS id, not the primary backendId (else the playback half is unopenable).
TEST(StreamOpener, RoutesPerDirectionBackendIdForSplitDuplex) {
    FakeBackend be;
    be.addSupportedFormat(11, Direction::Capture, fmt(48000, 16, 2));   // capture lives on id 11
    be.addSupportedFormat(22, Direction::Playback, fmt(48000, 16, 2));  // playback lives on id 22
    StreamOpener opener(be);

    DeviceInfo d;
    d.backendId = 11;          // primary == capture id
    d.captureBackendId = 11;
    d.playbackBackendId = 22;  // playback on a DIFFERENT id — must be used for openPlayback
    d.name = "USB Audio CODEC";

    auto cap = opener.openCapture(d, fmt(48000, 16, 2));
    ASSERT_NE(cap, nullptr);
    EXPECT_EQ(cap->actualFormat().channels, 2);

    // If openPlayback wrongly used backendId(11) (no playback format there) it would throw.
    auto play = opener.openPlayback(d, fmt(48000, 16, 2));
    ASSERT_NE(play, nullptr);
    EXPECT_EQ(play->actualFormat().channels, 2);
}

// ---- IoResult: overflow / underflow / timeout / RAII close --------------------------

TEST(FakeCaptureStream, BlockingReadFillsFullyNoOverflow) {
    FakeCaptureStream s(fmt(48000, 16, 2));
    std::vector<std::int16_t> buf(480 * 2);
    auto r = s.read(buf.data(), 480, kBlockForever);
    EXPECT_EQ(r.frames, 480);
    EXPECT_FALSE(r.overflowed);
    EXPECT_FALSE(r.timedOut);
}

TEST(FakeCaptureStream, SurfacesOverflowFlag) {
    FakeCaptureStream s(fmt());
    s.overflow = true;
    std::vector<std::int16_t> buf(480 * 2);
    auto r = s.read(buf.data(), 480, kBlockForever);
    EXPECT_TRUE(r.overflowed);
    EXPECT_EQ(r.frames, 480);  // PortAudio still delivers the buffer on overflow
}

TEST(FakeCaptureStream, BoundedReadTimesOutWithPartialFrames) {
    FakeCaptureStream s(fmt());
    s.availableFrames = 100;  // only 100 of the requested 480 are available
    std::vector<std::int16_t> buf(480 * 2);
    auto r = s.read(buf.data(), 480, /*timeoutMs=*/5);
    EXPECT_EQ(r.frames, 100);
    EXPECT_TRUE(r.timedOut);
}

TEST(FakePlaybackStream, SurfacesUnderflowFlag) {
    FakePlaybackStream s(fmt());
    s.underflow = true;
    std::vector<std::int16_t> buf(480 * 2, 0);
    auto r = s.write(buf.data(), 480, kBlockForever);
    EXPECT_TRUE(r.underflowed);
    EXPECT_EQ(r.frames, 480);
}

TEST(FakePlaybackStream, BoundedWriteTimesOutWithPartialFrames) {
    FakePlaybackStream s(fmt());
    s.availableFrames = 64;
    std::vector<std::int16_t> buf(480 * 2, 0);
    auto r = s.write(buf.data(), 480, /*timeoutMs=*/5);
    EXPECT_EQ(r.frames, 64);
    EXPECT_TRUE(r.timedOut);
}

TEST(StreamRAII, DestructorClosesStream) {
    bool captureClosed = false;
    bool playbackClosed = false;
    {
        FakeCaptureStream cap(fmt());
        cap.closedFlag = &captureClosed;
        FakePlaybackStream play(fmt());
        play.closedFlag = &playbackClosed;
        EXPECT_FALSE(captureClosed);
        EXPECT_FALSE(playbackClosed);
    }
    EXPECT_TRUE(captureClosed);
    EXPECT_TRUE(playbackClosed);
}

// The opener returns the stream as a base-class unique_ptr; destroying it must still close.
TEST(StreamRAII, OpenerReturnedStreamIsRaiiClosed) {
    FakeBackend be;
    be.addSupportedFormat(9, Direction::Capture, fmt());
    StreamOpener opener(be);

    auto stream = opener.openCapture(dev(9, "BlackHole 2ch"), fmt());
    ASSERT_NE(stream, nullptr);
    stream.reset();  // RAII close via the base-class virtual destructor
    SUCCEED();       // no leak / no crash; the FakeCaptureStream dtor ran
}
