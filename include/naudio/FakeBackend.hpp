// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "naudio/DeviceBackend.hpp"
#include "naudio/Stream.hpp"

namespace naudio {

// Deterministic in-memory capture stream for stream-logic tests — no hardware. Tests
// construct it directly to drive overflow / timeout / RAII-close, or receive it via
// FakeBackend::openCaptureStream to drive StreamOpener's mono→stereo fallback (§5.4).
class FakeCaptureStream : public CaptureStream {
public:
    explicit FakeCaptureStream(AudioFormat format) : format_(format) {}
    ~FakeCaptureStream() override {
        if (closedFlag != nullptr) *closedFlag = true;
    }

    // --- test knobs ---
    bool  overflow = false;       // read() reports overflow when set (paInputOverflowed analog)
    int   availableFrames = -1;   // bounded-read cap; -1 => unlimited (blocking always fills)
    bool* closedFlag = nullptr;   // dtor sets *closedFlag = true (observe RAII close)

    IoResult read(void* buffer, int frames, int timeoutMs) override {
        IoResult r;
        r.overflowed = overflow;
        if (timeoutMs < 0 || availableFrames < 0) {
            r.frames = frames;  // blocking, or unlimited supply: always fills
        } else {
            r.frames = std::min(frames, availableFrames);
            r.timedOut = r.frames < frames;
        }
        if (buffer != nullptr && r.frames > 0) {
            std::memset(buffer, 0xA5, static_cast<std::size_t>(r.frames) * format_.frameSize());
        }
        return r;
    }
    const AudioFormat& actualFormat() const override { return format_; }

private:
    AudioFormat format_;
};

// Deterministic in-memory playback stream — symmetric to FakeCaptureStream.
class FakePlaybackStream : public PlaybackStream {
public:
    explicit FakePlaybackStream(AudioFormat format) : format_(format) {}
    ~FakePlaybackStream() override {
        if (closedFlag != nullptr) *closedFlag = true;
    }

    // --- test knobs ---
    bool  underflow = false;      // write() reports underflow when set (paOutputUnderflowed analog)
    int   availableFrames = -1;   // bounded-write space cap; -1 => unlimited (blocking always drains)
    bool* closedFlag = nullptr;   // dtor sets *closedFlag = true (observe RAII close)

    IoResult write(const void* buffer, int frames, int timeoutMs) override {
        (void)buffer;
        IoResult r;
        r.underflowed = underflow;
        if (timeoutMs < 0 || availableFrames < 0) {
            r.frames = frames;
        } else {
            r.frames = std::min(frames, availableFrames);
            r.timedOut = r.frames < frames;
        }
        return r;
    }
    const AudioFormat& actualFormat() const override { return format_; }

private:
    AudioFormat format_;
};

// In-memory backend for tests — no hardware. Lets unit tests drive enumeration,
// classification, DUPLEX merge, virtual matching, format probing, and stream
// opening deterministically.
class FakeBackend : public DeviceBackend {
public:
    FakeBackend& add(RawDevice device) {
        devices_.push_back(std::move(device));
        return *this;
    }

    // Register a (device, direction, format) tuple that probeFormat() — and therefore
    // openCaptureStream/openPlaybackStream — will report as supported. Anything not
    // registered probes/opens as unsupported, the stand-in for a real device that does not
    // advertise the required format.
    FakeBackend& addSupportedFormat(int backendId, Direction dir, AudioFormat format) {
        supported_.push_back({backendId, dir, format});
        return *this;
    }

    std::vector<RawDevice> enumerate() override { return devices_; }

    bool probeFormat(int backendId, const AudioFormat& format, Direction dir) override {
        return isSupported(backendId, dir, format);
    }

    // Open succeeds only for a registered (device, dir, format); otherwise throws —
    // modelling a device that cannot open the requested format. StreamOpener probes
    // before calling this for capture; playback opens directly (no fallback), so the
    // throw is its unsupported path.
    std::unique_ptr<CaptureStream> openCaptureStream(int backendId,
                                                     const AudioFormat& format) override {
        if (!isSupported(backendId, Direction::Capture, format)) {
            throw DeviceUnavailable("FakeBackend: capture format unsupported for device " +
                                    std::to_string(backendId));
        }
        return std::make_unique<FakeCaptureStream>(format);
    }
    std::unique_ptr<PlaybackStream> openPlaybackStream(int backendId,
                                                       const AudioFormat& format) override {
        if (!isSupported(backendId, Direction::Playback, format)) {
            throw DeviceUnavailable("FakeBackend: playback format unsupported for device " +
                                    std::to_string(backendId));
        }
        return std::make_unique<FakePlaybackStream>(format);
    }

private:
    struct Supported {
        int backendId;
        Direction dir;
        AudioFormat format;
    };

    bool isSupported(int backendId, Direction dir, const AudioFormat& format) const {
        for (const auto& s : supported_) {
            if (s.backendId == backendId && s.dir == dir && s.format == format) return true;
        }
        return false;
    }

    std::vector<RawDevice> devices_;
    std::vector<Supported> supported_;
};

}  // namespace naudio
