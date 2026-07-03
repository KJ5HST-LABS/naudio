// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <string>

namespace naudio {

// Sample encoding. The streaming path needs signed PCM; the rest are placeholders
// for format-probe work.
enum class Encoding { PcmSigned, PcmUnsigned, PcmFloat };

enum class Endianness { Little, Big };

// Value type: the format-descriptor subset of AudioStreamConfig
// (sampleRate / bits / channels / encoding / endianness).
// Defaults are the standard digital-mode REQUIRED path: 48 kHz / 16-bit / stereo.
struct AudioFormat {
    int sampleRate = 48000;
    int bitsPerSample = 16;
    int channels = 2;
    Encoding encoding = Encoding::PcmSigned;
    Endianness endianness = Endianness::Little;

    int frameSize() const { return (bitsPerSample / 8) * channels; }
    int bytesPerSecond() const { return frameSize() * sampleRate; }

    bool operator==(const AudioFormat& o) const {
        return sampleRate == o.sampleRate && bitsPerSample == o.bitsPerSample &&
               channels == o.channels && encoding == o.encoding &&
               endianness == o.endianness;
    }
    bool operator!=(const AudioFormat& o) const { return !(*this == o); }
};

// Device classification.
enum class DeviceType { Hardware, Virtual, Unknown };

// Device capability.
enum class Capability { Capture, Playback, Duplex };

// Stream direction for a format probe / open: capture vs playback.
enum class Direction { Capture, Playback };

// Raw device record as reported by a DeviceBackend, before classification/merge.
// Mirrors the fields a PortAudio PaDeviceInfo (or a FakeBackend) supplies.
struct RawDevice {
    int backendId = -1;          // backend-specific index/handle (PortAudio device index)
    std::string name;
    std::string hostApi;         // host-API name (e.g. "Core Audio", "ALSA"); may be empty
    int maxInputChannels = 0;
    int maxOutputChannels = 0;
    double defaultSampleRate = 0.0;
};

// Classified, merged device.
struct DeviceInfo {
    int backendId = -1;
    // Per-direction backend ids. When a backend reports a device as ONE duplex record both equal
    // backendId; when it reports split capture-only + playback-only records of the same identity
    // (ALSA-style), the DUPLEX merge records each direction's own id here so either direction can
    // be opened. -1 means "use backendId" (the direction the device does not support).
    int captureBackendId = -1;
    int playbackBackendId = -1;
    std::string name;
    std::string hostApi;
    std::string displayName;
    DeviceType type = DeviceType::Unknown;
    Capability capability = Capability::Capture;

    // Reported by the backend (PortAudio PaDeviceInfo):
    // PortAudio exposes a single default rate + max channel counts rather than a format
    // list, so these are the "detected" values the verification reports.
    double defaultSampleRate = 0.0;
    int maxInputChannels = 0;
    int maxOutputChannels = 0;

    bool isVirtual() const { return type == DeviceType::Virtual; }
    bool supportsCapture() const {
        return capability == Capability::Capture || capability == Capability::Duplex;
    }
    bool supportsPlayback() const {
        return capability == Capability::Playback || capability == Capability::Duplex;
    }

    // Backend id to open/probe in `dir`. Falls back to backendId when no per-direction id was
    // recorded (the common single-record case, where all three are equal).
    int backendIdFor(Direction dir) const {
        const int id = (dir == Direction::Capture) ? captureBackendId : playbackBackendId;
        return id >= 0 ? id : backendId;
    }
};

}  // namespace naudio
