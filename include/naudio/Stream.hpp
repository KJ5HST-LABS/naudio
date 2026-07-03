// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

#include "naudio/Types.hpp"

namespace naudio {

// Thrown when a device cannot be opened in the requested (and, for capture, the mono
// fallback) format. At the extern "C" boundary it becomes a negative
// NA_ERR_* code.
class DeviceUnavailable : public std::runtime_error {
public:
    explicit DeviceUnavailable(const std::string& msg) : std::runtime_error(msg) {}
};

// timeoutMs sentinel: block until every requested frame is transferred — the path
// the hardware loopback test exercises. A non-negative timeoutMs requests a bounded
// transfer (0 == return whatever is immediately available).
inline constexpr int kBlockForever = -1;

// Outcome of a blocking read/write. Surfaces PortAudio's overflow/underflow. For a
// fully-blocking call `frames` equals the
// requested count; for a bounded call it may be smaller with `timedOut` set.
struct IoResult {
    int  frames = 0;           // frames actually transferred
    bool overflowed = false;   // capture: input was dropped before this read (paInputOverflowed)
    bool underflowed = false;  // playback: output was inserted before this write (paOutputUnderflowed)
    bool timedOut = false;     // bounded call returned before all frames were transferred
};

// RAII capture stream. The destructor stops + closes the underlying device stream.
// Abstract so PortAudioCaptureStream (real) and FakeCaptureStream (tests) share one
// interface — the test seam.
class CaptureStream {
public:
    virtual ~CaptureStream() = default;

    // Blocking read of up to `frames` frames into `buffer`, which must hold at least
    // frames * actualFormat().frameSize() bytes. See kBlockForever for timeout semantics.
    virtual IoResult read(void* buffer, int frames, int timeoutMs = kBlockForever) = 0;

    // The format the stream was actually opened with. May differ from the requested format —
    // mono when stereo was requested but unsupported (§5.4) — and the consumer up-converts.
    virtual const AudioFormat& actualFormat() const = 0;
};

// RAII playback stream — symmetric to CaptureStream.
class PlaybackStream {
public:
    virtual ~PlaybackStream() = default;

    // Blocking write of up to `frames` frames from `buffer` (>= frames *
    // actualFormat().frameSize() bytes). See kBlockForever for timeout semantics.
    virtual IoResult write(const void* buffer, int frames, int timeoutMs = kBlockForever) = 0;

    virtual const AudioFormat& actualFormat() const = 0;
};

}  // namespace naudio
