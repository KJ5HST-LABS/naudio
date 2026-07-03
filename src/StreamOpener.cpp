// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/StreamOpener.hpp"

namespace naudio {

std::unique_ptr<CaptureStream> StreamOpener::openCapture(const DeviceInfo& device,
                                                         const AudioFormat& requested) {
    // Use the device's capture-direction backend id (matters for a split-record duplex device).
    const int id = device.backendIdFor(Direction::Capture);

    // Try the requested format first.
    if (backend_.probeFormat(id, requested, Direction::Capture)) {
        return backend_.openCaptureStream(id, requested);
    }

    // Fall back to mono if a multi-channel format was requested. Many mics are
    // mono-only; report the mono format (via the stream's actualFormat) so the consumer
    // converts to stereo. PortAudio does not do this automatically (§5.4).
    if (requested.channels != 1) {
        AudioFormat mono = requested;
        mono.channels = 1;
        if (backend_.probeFormat(id, mono, Direction::Capture)) {
            return backend_.openCaptureStream(id, mono);
        }
    }

    // Neither stereo nor mono is supported.
    throw DeviceUnavailable("Device does not support required audio format: " + device.name);
}

std::unique_ptr<PlaybackStream> StreamOpener::openPlayback(const DeviceInfo& device,
                                                           const AudioFormat& requested) {
    // No fallback for playback: open directly; the backend
    // raises DeviceUnavailable if the format is unsupported. Use the playback-direction id.
    return backend_.openPlaybackStream(device.backendIdFor(Direction::Playback), requested);
}

}  // namespace naudio
