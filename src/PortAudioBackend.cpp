// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/PortAudioBackend.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#include <portaudio.h>

#include "naudio/Stream.hpp"

namespace naudio {
namespace {

// AudioFormat encoding/bit-depth -> PaSampleFormat. The standard 48 kHz / 16-bit digital-mode
// path (AudioStreamConfig.DEFAULT_SAMPLE_RATE) maps to paInt16. Other depths are supported
// for completeness; unknown depths fall back to paInt16.
PaSampleFormat paSampleFormat(const AudioFormat& f) {
    if (f.encoding == Encoding::PcmFloat) return paFloat32;
    switch (f.bitsPerSample) {
        case 8:  return f.encoding == Encoding::PcmUnsigned ? paUInt8 : paInt8;
        case 16: return paInt16;
        case 24: return paInt24;
        case 32: return paInt32;
        default: return paInt16;
    }
}

// Build the PaStreamParameters for one direction, using the device's reported low-latency
// hint (low latency).
PaStreamParameters makeParams(int backendId, const AudioFormat& f, Direction dir) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(backendId);
    PaStreamParameters p{};
    p.device = backendId;
    p.channelCount = f.channels;
    p.sampleFormat = paSampleFormat(f);
    p.suggestedLatency = (info != nullptr) ? (dir == Direction::Capture ? info->defaultLowInputLatency
                                                                        : info->defaultLowOutputLatency)
                                           : 0.0;
    p.hostApiSpecificStreamInfo = nullptr;
    return p;
}

// RAII PortAudio capture stream. Opens + starts a blocking input stream; the destructor
// stops + closes it.
class PortAudioCaptureStream : public CaptureStream {
public:
    PortAudioCaptureStream(int backendId, const AudioFormat& format) : format_(format) {
        PaStreamParameters in = makeParams(backendId, format, Direction::Capture);
        PaError err = Pa_OpenStream(&stream_, &in, nullptr, static_cast<double>(format.sampleRate),
                                    paFramesPerBufferUnspecified, paNoFlag, nullptr, nullptr);
        if (err != paNoError) {
            throw DeviceUnavailable(std::string("Pa_OpenStream (capture) failed: ") +
                                    Pa_GetErrorText(err));
        }
        err = Pa_StartStream(stream_);
        if (err != paNoError) {
            Pa_CloseStream(stream_);
            stream_ = nullptr;
            throw DeviceUnavailable(std::string("Pa_StartStream (capture) failed: ") +
                                    Pa_GetErrorText(err));
        }
    }
    ~PortAudioCaptureStream() override {
        if (stream_ != nullptr) {
            Pa_StopStream(stream_);
            Pa_CloseStream(stream_);
        }
    }

    IoResult read(void* buffer, int frames, int timeoutMs) override {
        IoResult r;
        if (frames <= 0) return r;
        if (timeoutMs < 0) {  // block until all `frames` are read
            const PaError err = Pa_ReadStream(stream_, buffer, static_cast<unsigned long>(frames));
            applyReadStatus(err, r);
            r.frames = frames;  // PortAudio fills the buffer even on paInputOverflowed
            return r;
        }
        // Bounded read: pull only what is available, up to the deadline (for the M2 pump).
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        char* out = static_cast<char*>(buffer);
        const int fs = format_.frameSize();
        int got = 0;
        while (got < frames) {
            const long avail = Pa_GetStreamReadAvailable(stream_);
            if (avail < 0) {
                throw DeviceUnavailable(std::string("Pa_GetStreamReadAvailable failed: ") +
                                        Pa_GetErrorText(static_cast<PaError>(avail)));
            }
            const long take = std::min<long>(avail, frames - got);
            if (take > 0) {
                const PaError err = Pa_ReadStream(
                    stream_, out + static_cast<std::size_t>(got) * fs,
                    static_cast<unsigned long>(take));
                applyReadStatus(err, r);
                got += static_cast<int>(take);
            }
            if (got >= frames || std::chrono::steady_clock::now() >= deadline) break;
            Pa_Sleep(1);
        }
        r.frames = got;
        r.timedOut = got < frames;
        return r;
    }
    const AudioFormat& actualFormat() const override { return format_; }

private:
    // paNoError / paInputOverflowed are normal (the buffer is still delivered); any other
    // PaError means the stream is broken — surface it as a C++ exception (RAII model).
    static void applyReadStatus(PaError err, IoResult& r) {
        if (err == paInputOverflowed) {
            r.overflowed = true;
        } else if (err != paNoError) {
            throw DeviceUnavailable(std::string("Pa_ReadStream failed: ") + Pa_GetErrorText(err));
        }
    }

    AudioFormat format_;
    PaStream* stream_ = nullptr;
};

// RAII PortAudio playback stream — symmetric to PortAudioCaptureStream.
class PortAudioPlaybackStream : public PlaybackStream {
public:
    PortAudioPlaybackStream(int backendId, const AudioFormat& format) : format_(format) {
        PaStreamParameters out = makeParams(backendId, format, Direction::Playback);
        PaError err = Pa_OpenStream(&stream_, nullptr, &out, static_cast<double>(format.sampleRate),
                                    paFramesPerBufferUnspecified, paNoFlag, nullptr, nullptr);
        if (err != paNoError) {
            throw DeviceUnavailable(std::string("Pa_OpenStream (playback) failed: ") +
                                    Pa_GetErrorText(err));
        }
        err = Pa_StartStream(stream_);
        if (err != paNoError) {
            Pa_CloseStream(stream_);
            stream_ = nullptr;
            throw DeviceUnavailable(std::string("Pa_StartStream (playback) failed: ") +
                                    Pa_GetErrorText(err));
        }
    }
    ~PortAudioPlaybackStream() override {
        if (stream_ != nullptr) {
            Pa_StopStream(stream_);
            Pa_CloseStream(stream_);
        }
    }

    IoResult write(const void* buffer, int frames, int timeoutMs) override {
        IoResult r;
        if (frames <= 0) return r;
        if (timeoutMs < 0) {  // block until all `frames` are written
            const PaError err =
                Pa_WriteStream(stream_, buffer, static_cast<unsigned long>(frames));
            applyWriteStatus(err, r);
            r.frames = frames;
            return r;
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        const char* in = static_cast<const char*>(buffer);
        const int fs = format_.frameSize();
        int put = 0;
        while (put < frames) {
            const long avail = Pa_GetStreamWriteAvailable(stream_);
            if (avail < 0) {
                throw DeviceUnavailable(std::string("Pa_GetStreamWriteAvailable failed: ") +
                                        Pa_GetErrorText(static_cast<PaError>(avail)));
            }
            const long take = std::min<long>(avail, frames - put);
            if (take > 0) {
                const PaError err = Pa_WriteStream(
                    stream_, in + static_cast<std::size_t>(put) * fs,
                    static_cast<unsigned long>(take));
                applyWriteStatus(err, r);
                put += static_cast<int>(take);
            }
            if (put >= frames || std::chrono::steady_clock::now() >= deadline) break;
            Pa_Sleep(1);
        }
        r.frames = put;
        r.timedOut = put < frames;
        return r;
    }
    const AudioFormat& actualFormat() const override { return format_; }

private:
    static void applyWriteStatus(PaError err, IoResult& r) {
        if (err == paOutputUnderflowed) {
            r.underflowed = true;
        } else if (err != paNoError) {
            throw DeviceUnavailable(std::string("Pa_WriteStream failed: ") + Pa_GetErrorText(err));
        }
    }

    AudioFormat format_;
    PaStream* stream_ = nullptr;
};

}  // namespace

PortAudioBackend::PortAudioBackend() {
    const PaError err = Pa_Initialize();
    if (err != paNoError) {
        throw std::runtime_error(std::string("Pa_Initialize failed: ") + Pa_GetErrorText(err));
    }
    initialized_ = true;
}

PortAudioBackend::~PortAudioBackend() {
    if (initialized_) Pa_Terminate();
}

std::vector<RawDevice> PortAudioBackend::enumerate() {
    const PaDeviceIndex count = Pa_GetDeviceCount();
    if (count < 0) {
        throw std::runtime_error(std::string("Pa_GetDeviceCount failed: ") +
                                 Pa_GetErrorText(count));
    }

    std::vector<RawDevice> devices;
    devices.reserve(static_cast<std::size_t>(count));
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info == nullptr) continue;

        RawDevice d;
        d.backendId = static_cast<int>(i);
        d.name = (info->name != nullptr) ? info->name : "";
        const PaHostApiInfo* hostApi = Pa_GetHostApiInfo(info->hostApi);
        d.hostApi = (hostApi != nullptr && hostApi->name != nullptr) ? hostApi->name : "";
        d.maxInputChannels = info->maxInputChannels;
        d.maxOutputChannels = info->maxOutputChannels;
        d.defaultSampleRate = info->defaultSampleRate;
        devices.push_back(std::move(d));
    }
    return devices;
}

bool PortAudioBackend::probeFormat(int backendId, const AudioFormat& format, Direction dir) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(backendId);
    if (info == nullptr) return false;  // unknown device index — treat as unsupported (return false)

    PaStreamParameters params{};
    params.device = backendId;
    params.channelCount = format.channels;
    params.sampleFormat = paSampleFormat(format);
    params.suggestedLatency = (dir == Direction::Capture) ? info->defaultLowInputLatency
                                                          : info->defaultLowOutputLatency;
    params.hostApiSpecificStreamInfo = nullptr;

    const PaError err =
        (dir == Direction::Capture)
            ? Pa_IsFormatSupported(&params, nullptr, static_cast<double>(format.sampleRate))
            : Pa_IsFormatSupported(nullptr, &params, static_cast<double>(format.sampleRate));
    return err == paFormatIsSupported;
}

std::unique_ptr<CaptureStream> PortAudioBackend::openCaptureStream(int backendId,
                                                                  const AudioFormat& format) {
    return std::make_unique<PortAudioCaptureStream>(backendId, format);
}

std::unique_ptr<PlaybackStream> PortAudioBackend::openPlaybackStream(int backendId,
                                                                    const AudioFormat& format) {
    return std::make_unique<PortAudioPlaybackStream>(backendId, format);
}

}  // namespace naudio
