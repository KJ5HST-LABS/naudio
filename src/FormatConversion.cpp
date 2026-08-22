// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — audio format conversion building blocks (spec 1.2 companion DSP).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/FormatConversion.hpp"

#include <cassert>
#include <cstring>
#include <stdexcept>

namespace naudio {

// ---------------------------------------------------------------------------
// Anti-alias FIR tap tables, Q15.
//
// Design (self-authored, textbook windowed-sinc — no ported code): for factor
// M, N = 36*M + 1 taps; ideal low-pass sinc at cutoff 0.9/(2*M) cycles per
// input sample (0.9x the output Nyquist); Blackman window
// (0.42 - 0.5 cos + 0.08 cos); quantized round-half-away to Q15 with the
// residual added to the center tap so every table sums to EXACTLY 32768 —
// that exact sum is what makes a constant input pass through bit-identically
// ((c * 32768 + 16384) >> 15 == c for all int16 c). The double-precision trig
// ran once, offline; only these frozen integers ship, so the filter is
// bit-exact on every platform. The golden tests in
// tests/test_format_conversion.cpp are the normative definition (the wire
// spec deliberately prescribes no filter).
// ---------------------------------------------------------------------------

static const std::int32_t kTapsM2[] = {
    0, 0, -1, 1, 4, -1, -9, -2,
    18, 12, -27, -31, 33, 64, -28, -109,
    0, 161, 61, -210, -165, 236, 319, -211,
    -521, 101, 759, 140, -1013, -580, 1256, 1364,
    -1458, -3012, 1592, 10270, 14742, 10270, 1592, -3012,
    -1458, 1364, 1256, -580, -1013, 140, 759, 101,
    -521, -211, 319, 236, -165, -210, 61, 161,
    0, -109, -28, 64, 33, -31, -27, 12,
    18, -2, -9, -1, 4, 1, -1, 0,
    0
};

static const std::int32_t kTapsM3[] = {
    0, 0, 0, 0, 0, 1, 2, 1,
    -3, -6, -5, 3, 12, 13, 0, -18,
    -26, -10, 22, 44, 30, -18, -65, -63,
    0, 83, 110, 41, -87, -167, -110, 65,
    224, 213, 0, -265, -347, -126, 267, 506,
    332, -195, -675, -647, 0, 837, 1134, 430,
    -972, -2015, -1499, 1061, 4932, 8427, 9826, 8427,
    4932, 1061, -1499, -2015, -972, 430, 1134, 837,
    0, -647, -675, -195, 332, 506, 267, -126,
    -347, -265, 0, 213, 224, 65, -110, -167,
    -87, 41, 110, 83, 0, -63, -65, -18,
    30, 44, 22, -10, -26, -18, 0, 13,
    12, 3, -5, -6, -3, 1, 2, 1,
    0, 0, 0, 0, 0
};

static const std::int32_t kTapsM4[] = {
    0, 0, 0, 0, 0, 0, 0, 1,
    2, 1, 0, -3, -5, -4, -1, 4,
    9, 10, 6, -3, -14, -19, -16, -2,
    17, 31, 32, 15, -14, -42, -54, -40,
    0, 48, 81, 77, 30, -41, -105, -125,
    -83, 12, 118, 181, 160, 50, -106, -233,
    -260, -155, 50, 266, 379, 313, 70, -255,
    -506, -538, -290, 164, 628, 860, 682, 85,
    -729, -1394, -1506, -783, 796, 2944, 5135, 6769,
    7368, 6769, 5135, 2944, 796, -783, -1506, -1394,
    -729, 85, 682, 860, 628, 164, -290, -538,
    -506, -255, 70, 313, 379, 266, 50, -155,
    -260, -233, -106, 50, 160, 181, 118, 12,
    -83, -125, -105, -41, 30, 77, 81, 48,
    0, -40, -54, -42, -14, 15, 32, 31,
    17, -2, -16, -19, -14, -3, 6, 10,
    9, 4, -1, -4, -5, -3, 0, 1,
    2, 1, 0, 0, 0, 0, 0, 0,
    0
};

static const std::int32_t kTapsM6[] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 1, 1, 1, 1, 1, 0,
    -1, -2, -3, -3, -2, -1, 2, 4,
    6, 7, 6, 4, 0, -5, -9, -12,
    -13, -10, -5, 3, 11, 18, 22, 21,
    15, 4, -9, -23, -33, -36, -32, -19,
    0, 22, 41, 54, 55, 44, 20, -11,
    -44, -70, -83, -79, -55, -16, 32, 79,
    112, 123, 106, 63, 0, -70, -132, -171,
    -174, -136, -63, 34, 133, 213, 253, 238,
    166, 47, -98, -237, -338, -372, -324, -193,
    0, 220, 419, 548, 567, 455, 215, -118,
    -486, -810, -1007, -1004, -749, -229, 531, 1462,
    2466, 3423, 4213, 4734, 4910, 4734, 4213, 3423,
    2466, 1462, 531, -229, -749, -1004, -1007, -810,
    -486, -118, 215, 455, 567, 548, 419, 220,
    0, -193, -324, -372, -338, -237, -98, 47,
    166, 238, 253, 213, 133, 34, -63, -136,
    -174, -171, -132, -70, 0, 63, 106, 123,
    112, 79, 32, -16, -55, -79, -83, -70,
    -44, -11, 20, 44, 55, 54, 41, 22,
    0, -19, -32, -36, -33, -23, -9, 4,
    15, 21, 22, 18, 11, 3, -5, -10,
    -13, -12, -9, -5, 0, 4, 6, 7,
    6, 4, 2, -1, -2, -3, -3, -2,
    -1, 0, 1, 1, 1, 1, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0
};

namespace {

struct TapTable {
    int factor;
    const std::int32_t* taps;
    std::size_t count;
};

const TapTable kTables[] = {
    {2, kTapsM2, sizeof(kTapsM2) / sizeof(kTapsM2[0])},
    {3, kTapsM3, sizeof(kTapsM3) / sizeof(kTapsM3[0])},
    {4, kTapsM4, sizeof(kTapsM4) / sizeof(kTapsM4[0])},
    {6, kTapsM6, sizeof(kTapsM6) / sizeof(kTapsM6[0])},
};

const TapTable* findTable(int factor) {
    for (const TapTable& t : kTables) {
        if (t.factor == factor) return &t;
    }
    return nullptr;
}

std::int16_t saturate(std::int64_t acc) {
    // Round half up, then clamp to int16. The >> on a negative signed value is
    // an arithmetic shift on every supported toolchain (and the cross-platform
    // golden tests pin it).
    const std::int64_t y = (acc + 16384) >> 15;
    if (y > 32767) return 32767;
    if (y < -32768) return -32768;
    return static_cast<std::int16_t>(y);
}

}  // namespace

bool Decimator::supportsFactor(int factor) {
    return factor == 1 || findTable(factor) != nullptr;
}

Decimator::Decimator(int factor, int channels)
    : factor_(factor), channels_(channels), taps_(nullptr), tapCount_(0),
      phase_(0), writePos_(0) {
    if (!supportsFactor(factor))
        throw std::invalid_argument("Decimator: unsupported factor");
    if (channels != 1 && channels != 2)
        throw std::invalid_argument("Decimator: channels must be 1 or 2");
    if (factor != 1) {
        const TapTable* t = findTable(factor);
        taps_ = t->taps;
        tapCount_ = t->count;
    }
    std::memset(history_, 0, sizeof(history_));
}

std::size_t Decimator::outputFramesFor(std::size_t frames) const {
    return (phase_ + frames) / static_cast<std::size_t>(factor_);
}

std::size_t Decimator::process(const std::int16_t* in, std::size_t frames,
                               std::int16_t* out) {
    if (frames == 0) return 0;
    if (factor_ == 1) {
        std::memcpy(out, in, frames * static_cast<std::size_t>(channels_) *
                                 sizeof(std::int16_t));
        return frames;
    }
    const std::size_t n = tapCount_;
    std::size_t produced = 0;
    for (std::size_t f = 0; f < frames; ++f) {
        // Push one input frame into the ring (zero-initialized, so slots not
        // yet written behave as the x[<0] = 0 prehistory).
        for (int c = 0; c < channels_; ++c)
            history_[writePos_ * static_cast<std::size_t>(channels_) +
                     static_cast<std::size_t>(c)] =
                in[f * static_cast<std::size_t>(channels_) +
                   static_cast<std::size_t>(c)];
        writePos_ = (writePos_ + 1) % n;
        if (++phase_ < static_cast<std::size_t>(factor_)) continue;
        phase_ = 0;
        // The newest frame x[i] sits at writePos_ - 1; tap k reads x[i-k].
        for (int c = 0; c < channels_; ++c) {
            std::int64_t acc = 0;
            std::size_t pos = (writePos_ + n - 1) % n;
            for (std::size_t k = 0; k < n; ++k) {
                acc += static_cast<std::int64_t>(taps_[k]) *
                       history_[pos * static_cast<std::size_t>(channels_) +
                                static_cast<std::size_t>(c)];
                pos = (pos + n - 1) % n;
            }
            out[produced * static_cast<std::size_t>(channels_) +
                static_cast<std::size_t>(c)] = saturate(acc);
        }
        ++produced;
    }
    return produced;
}

void Decimator::reset() {
    phase_ = 0;
    writePos_ = 0;
    std::memset(history_, 0, sizeof(history_));
}

void downmixToMono(const std::int16_t* interleavedStereo, std::size_t frames,
                   std::int16_t* out) {
    for (std::size_t i = 0; i < frames; ++i) {
        const std::int32_t l = interleavedStereo[2 * i];
        const std::int32_t r = interleavedStereo[2 * i + 1];
        // Floor semantics: arithmetic shift, not truncating division — the
        // golden tests pin the odd-negative-sum case.
        out[i] = static_cast<std::int16_t>((l + r) >> 1);
    }
}

void selectChannel(const std::int16_t* interleaved, std::size_t frames,
                   int channels, int channel, std::int16_t* out) {
    assert(channels >= 1 && channel >= 0 && channel < channels);
    for (std::size_t i = 0; i < frames; ++i)
        out[i] = interleaved[i * static_cast<std::size_t>(channels) +
                             static_cast<std::size_t>(channel)];
}

}  // namespace naudio
