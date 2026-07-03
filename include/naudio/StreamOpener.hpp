// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <memory>

#include "naudio/DeviceBackend.hpp"
#include "naudio/Stream.hpp"
#include "naudio/Types.hpp"

namespace naudio {

// Opens blocking capture/playback streams with this open policy: capture tries the
// requested format then falls back to mono; playback opens the requested format with
// no fallback. The policy drives the injected backend's probeFormat + open*Stream, so the
// SAME fallback logic runs for the production PortAudio backend and the FakeBackend tests
// — that is the point of the seam. PortAudio will not fall back for you.
class StreamOpener {
public:
    explicit StreamOpener(DeviceBackend& backend) : backend_(backend) {}

    // Try `requested`; if it is multi-channel and
    // unsupported, fall back to mono and report mono via the stream's actualFormat() so the
    // consumer up-converts. Throws DeviceUnavailable if neither is supported.
    std::unique_ptr<CaptureStream> openCapture(const DeviceInfo& device,
                                               const AudioFormat& requested);

    // Open `requested` directly — no fallback; the backend throws DeviceUnavailable
    // if unsupported.
    std::unique_ptr<PlaybackStream> openPlayback(const DeviceInfo& device,
                                                 const AudioFormat& requested);

private:
    DeviceBackend& backend_;
};

}  // namespace naudio
