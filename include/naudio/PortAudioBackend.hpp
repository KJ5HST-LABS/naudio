// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <memory>
#include <vector>

#include "naudio/DeviceBackend.hpp"
#include "naudio/Stream.hpp"

namespace naudio {

// Production backend over PortAudio. Owns the Pa_Initialize / Pa_Terminate
// lifecycle via RAII; there is no global state.
class PortAudioBackend : public DeviceBackend {
public:
    PortAudioBackend();            // Pa_Initialize — throws std::runtime_error on failure
    ~PortAudioBackend() override;  // Pa_Terminate

    PortAudioBackend(const PortAudioBackend&) = delete;
    PortAudioBackend& operator=(const PortAudioBackend&) = delete;

    // Pa_GetDeviceCount + Pa_GetDeviceInfo. Throws std::runtime_error on backend error.
    std::vector<RawDevice> enumerate() override;

    // Pa_IsFormatSupported for `backendId` in `dir`. Returns false (never throws) when
    // unsupported or the device index is unknown.
    bool probeFormat(int backendId, const AudioFormat& format, Direction dir) override;

    // Pa_OpenStream + Pa_StartStream for exactly `format` (blocking I/O, no callback).
    // Throws DeviceUnavailable on any PaError. The returned RAII stream stops + closes the
    // PaStream in its destructor.
    std::unique_ptr<CaptureStream> openCaptureStream(int backendId,
                                                     const AudioFormat& format) override;
    std::unique_ptr<PlaybackStream> openPlaybackStream(int backendId,
                                                       const AudioFormat& format) override;

private:
    bool initialized_ = false;
};

}  // namespace naudio
