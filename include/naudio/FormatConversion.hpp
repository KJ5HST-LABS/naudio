// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — audio format conversion building blocks (spec 1.2 companion DSP).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstddef>
#include <cstdint>

namespace naudio {

// Integer sample-rate decimator with a linear-phase anti-alias low-pass.
//
// Reduces an int16 PCM stream's rate by an integer factor M — the conversion
// the per-subscription RX format grants of wire spec 1.2 (§6.2.1) call for,
// where every grantable rate is an integer divisor of the native rate. The
// wire spec deliberately does not prescribe the filter; the golden tests in
// tests/test_format_conversion.cpp define this implementation bit-exactly.
//
// Filter: windowed-sinc FIR (Blackman window), N = 36*M + 1 taps, cutoff at
// 0.9x the output Nyquist, quantized to Q15 with the tap sum forced to
// exactly 32768 — so a constant (DC) input passes through bit-exactly once
// the filter is warmed up. Stopband rejection ~74 dB; passband flat to
// ~0.75x the output Nyquist.
//
// All arithmetic is integer (int64 accumulator, round-half-up via
// (acc + 16384) >> 15, saturated to int16), so output is bit-identical on
// every platform and for every input chunking. Per channel, with x[<0] = 0,
// output j is emitted after input sample i = j*M + (M-1) arrives:
//
//   y[j] = clamp((sum_k h[k] * x[i-k] + 16384) >> 15)
//
// Streaming: history and phase persist across process() calls, so feeding
// one 20 ms frame at a time (the server's cadence) and feeding the whole
// stream at once produce identical bytes.
//
// Pure: no I/O, no external dependencies. Constructor arguments are
// validated (throwing std::invalid_argument on an invalid value).
//
// Compiled into naudio_core (definitions in FormatConversion.cpp).
class Decimator {
public:
    // Largest supported decimation factor (the 48 kHz -> 8 kHz case).
    static constexpr int MAX_FACTOR = 6;

    // True for the supported factors: 1 (identity), 2, 3, 4, 6 — the
    // divisor family of a 48 kHz native rate (24/16/12/8 kHz).
    static bool supportsFactor(int factor);

    // Creates a decimator. factor must satisfy supportsFactor(); channels
    // must be 1 or 2 (interleaved). Throws std::invalid_argument otherwise.
    Decimator(int factor, int channels);

    int factor() const { return factor_; }
    int channels() const { return channels_; }

    // Number of output frames the next process(frames) call will produce.
    std::size_t outputFramesFor(std::size_t frames) const;

    // Consumes `frames` input frames (frames * channels samples, interleaved)
    // and writes the produced output frames to out (interleaved). Returns the
    // number of output frames written — outputFramesFor(frames), computed
    // before the call. in and out must not overlap. A zero-frame call is a
    // no-op returning 0.
    std::size_t process(const std::int16_t* in, std::size_t frames,
                        std::int16_t* out);

    // Clears the filter history and phase to the freshly-constructed state.
    void reset();

private:
    int factor_;
    int channels_;
    const std::int32_t* taps_;  // null for factor 1 (identity)
    std::size_t tapCount_;
    std::size_t phase_;         // input frames consumed since the last output
    std::size_t writePos_;      // ring-buffer write index (frames)
    // Per-channel sample history, ring buffer of tapCount_ frames,
    // interleaved like the input. Empty for factor 1.
    std::int16_t history_[2 * (36 * MAX_FACTOR + 1)];
};

// (L+R)/2 mono downmix of interleaved int16 stereo — spec 1.2 layout
// MONO_DOWNMIX. Floor semantics, pinned by the golden tests:
// out[i] = (int32(L) + int32(R)) >> 1 (never overflows, never saturates).
// out must not overlap in.
void downmixToMono(const std::int16_t* interleavedStereo, std::size_t frames,
                   std::int16_t* out);

// Extracts one channel from interleaved int16 audio — spec 1.2 layouts
// LEFT_ONLY (channel 0) / RIGHT_ONLY (channel 1). channels must be >= 1 and
// 0 <= channel < channels (asserted in debug; out-of-range is undefined).
// out must not overlap in.
void selectChannel(const std::int16_t* interleaved, std::size_t frames,
                   int channels, int channel, std::int16_t* out);

}  // namespace naudio
