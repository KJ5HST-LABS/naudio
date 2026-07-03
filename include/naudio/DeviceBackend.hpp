// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <memory>
#include <vector>

#include "naudio/Stream.hpp"
#include "naudio/Types.hpp"

namespace naudio {

// Abstract device backend — the test seam that makes the device layer verifiable
// without hardware. Calling the platform audio API's statics directly would be
// unmockable; this interface is the key architectural change.
//
// It covers device enumeration, format probing, and the raw, exact-format
// stream-open primitives — the mono→stereo fallback *policy* lives one level
// up in StreamOpener so the same logic runs for the real and fake backends (§5.4).
class DeviceBackend {
public:
    virtual ~DeviceBackend() = default;

    // Enumerate raw devices visible to this backend. May throw on backend failure.
    virtual std::vector<RawDevice> enumerate() = 0;

    // Does `backendId` support `format` in `dir`? Uses PortAudio's
    // Pa_IsFormatSupported. Returns false (never throws) when the format is
    // unsupported or the device cannot be queried.
    virtual bool probeFormat(int backendId, const AudioFormat& format, Direction dir) = 0;

    // Open a started, blocking capture/playback stream on `backendId` in EXACTLY `format`
    // (no fallback — StreamOpener owns that policy). Throws
    // DeviceUnavailable on failure.
    virtual std::unique_ptr<CaptureStream> openCaptureStream(int backendId,
                                                             const AudioFormat& format) = 0;
    virtual std::unique_ptr<PlaybackStream> openPlaybackStream(int backendId,
                                                              const AudioFormat& format) = 0;
};

}  // namespace naudio
