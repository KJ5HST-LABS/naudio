// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — format conversion building blocks (spec 1.2 companion DSP).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Bit-exact golden tests for the Decimator / downmixToMono / selectChannel
// units. The wire spec (§6.2.1) deliberately prescribes no anti-alias filter;
// THESE GOLDENS ARE ITS NORMATIVE DEFINITION. Every expected array below was
// computed by an independent integer reference implementation (Python — both
// sides are pure integer arithmetic, so agreement is a cross-check, not
// self-confirmation), from inputs any language can regenerate:
//
//   LCG(seed): x' = (1103515245*x + 12345) mod 2^31,
//              sample = ((x >> 8) mod 65536) - 32768
//
// A change that alters one output byte on any platform is a behavior change
// and must be made deliberately, goldens regenerated with it.
#include "naudio/FormatConversion.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

using naudio::Decimator;
using naudio::downmixToMono;
using naudio::selectChannel;

namespace {

// The language-neutral deterministic noise source (matches the Python
// reference's lcg()).
std::vector<std::int16_t> lcg(std::uint32_t seed, std::size_t count) {
    std::vector<std::int16_t> out;
    out.reserve(count);
    std::uint32_t x = seed;
    for (std::size_t i = 0; i < count; ++i) {
        x = (1103515245u * x + 12345u) & 0x7FFFFFFFu;
        out.push_back(static_cast<std::int16_t>(
            static_cast<std::int32_t>((x >> 8) % 65536u) - 32768));
    }
    return out;
}

std::vector<std::int16_t> runAll(Decimator& d,
                                 const std::vector<std::int16_t>& in) {
    const std::size_t frames = in.size() / static_cast<std::size_t>(d.channels());
    std::vector<std::int16_t> out(
        d.outputFramesFor(frames) * static_cast<std::size_t>(d.channels()));
    const std::size_t produced = d.process(in.data(), frames, out.data());
    out.resize(produced * static_cast<std::size_t>(d.channels()));
    return out;
}

double rms(const std::vector<std::int16_t>& v, std::size_t skip) {
    double acc = 0;
    for (std::size_t i = skip; i < v.size(); ++i)
        acc += static_cast<double>(v[i]) * v[i];
    return std::sqrt(acc / static_cast<double>(v.size() - skip));
}

}  // namespace

// ---------------------------------------------------------------------------
// downmixToMono — (L+R) >> 1, floor semantics.
// ---------------------------------------------------------------------------

TEST(Downmix, GoldenPairs) {
    // {L, R, expected}. The odd-negative-sum rows pin FLOOR (arithmetic
    // shift): truncating division would give -1 for (-3, 0) and 0 for
    // (32767, -32768).
    static const std::int16_t kIn[] = {
        1000,  2000,    // 1500
        -3,    0,       // -2 (floor of -1.5)
        32767, -32768,  // -1 (floor of -0.5)
        -1,    0,       // -1 (floor of -0.5)
        32767, 32767,   // 32767 — full-scale positive, no overflow
        -32768, -32768, // -32768 — full-scale negative, no overflow
        7,     8,       // 7
    };
    static const std::int16_t kExpected[] = {1500, -2, -1, -1, 32767, -32768, 7};
    std::int16_t out[7];
    downmixToMono(kIn, 7, out);
    for (int i = 0; i < 7; ++i) EXPECT_EQ(kExpected[i], out[i]) << "frame " << i;
}

// ---------------------------------------------------------------------------
// selectChannel
// ---------------------------------------------------------------------------

TEST(SelectChannel, LeftAndRightFromStereo) {
    static const std::int16_t kIn[] = {10, -20, 30, -40, 32767, -32768};
    std::int16_t left[3], right[3];
    selectChannel(kIn, 3, 2, 0, left);
    selectChannel(kIn, 3, 2, 1, right);
    EXPECT_EQ(10, left[0]);
    EXPECT_EQ(30, left[1]);
    EXPECT_EQ(32767, left[2]);
    EXPECT_EQ(-20, right[0]);
    EXPECT_EQ(-40, right[1]);
    EXPECT_EQ(-32768, right[2]);
}

TEST(SelectChannel, MonoPassthrough) {
    static const std::int16_t kIn[] = {1, -2, 3};
    std::int16_t out[3];
    selectChannel(kIn, 3, 1, 0, out);
    EXPECT_EQ(1, out[0]);
    EXPECT_EQ(-2, out[1]);
    EXPECT_EQ(3, out[2]);
}

// ---------------------------------------------------------------------------
// Decimator — construction contract.
// ---------------------------------------------------------------------------

TEST(Decimator, SupportedFactors) {
    // The divisor family of a 48 kHz native rate: 24/16/12/8 kHz, plus the
    // factor-1 identity.
    for (int m : {1, 2, 3, 4, 6}) EXPECT_TRUE(Decimator::supportsFactor(m)) << m;
    for (int m : {0, -1, 5, 7, 8, 12, 48000})
        EXPECT_FALSE(Decimator::supportsFactor(m)) << m;
}

TEST(Decimator, ConstructorValidates) {
    EXPECT_THROW(Decimator(5, 1), std::invalid_argument);
    EXPECT_THROW(Decimator(0, 1), std::invalid_argument);
    EXPECT_THROW(Decimator(2, 0), std::invalid_argument);
    EXPECT_THROW(Decimator(2, 3), std::invalid_argument);
    EXPECT_NO_THROW(Decimator(6, 2));
}

TEST(Decimator, FactorOneIsIdentity) {
    Decimator d(1, 2);
    const std::vector<std::int16_t> in = lcg(9, 64);  // 32 stereo frames
    const std::vector<std::int16_t> out = runAll(d, in);
    EXPECT_EQ(in, out);
}

// ---------------------------------------------------------------------------
// Decimator — bit-exact goldens (independent-reference expected values).
// ---------------------------------------------------------------------------

TEST(Decimator, GoldenM2MonoLcgSeed1) {
    static const std::int16_t kExpected[] = {
        0, 1, 0, -3, 12, -26, 45, -55, 50, -6,
        -99, 278, -540, 874, -1251, 1653, -2239, 5413, 7548, -11111,
        22304, 18425, 16542, 14488, -3827, -151, -9151, 7232, -12166, -17735,
        -3780, 1132, -4285, -272, -4140, -8134, 16132, 4804, -12242, -16655,
        -4393, 3545, -12049, 8656, 12414, -12270, 6305, 12123
    };
    Decimator d(2, 1);
    const std::vector<std::int16_t> out = runAll(d, lcg(1, 96));
    ASSERT_EQ(48u, out.size());
    for (std::size_t i = 0; i < out.size(); ++i)
        EXPECT_EQ(kExpected[i], out[i]) << "output " << i;
}

TEST(Decimator, GoldenM6MonoLcgSeed2) {
    static const std::int16_t kExpected[] = {
        0, -1, 2, -7, 16, -27, 46, -61, 59, -28,
        -56, 204, -429, 728, -1092, 1531, -2095, 3086, 4804, -8193,
        8027, 1635, -2953, 5746, -2445, 1858, -11700, -17236, -6939, -8457,
        3125, 3680, 10481, 4410, -1343, 3033
    };
    Decimator d(6, 1);
    const std::vector<std::int16_t> out = runAll(d, lcg(2, 216));
    ASSERT_EQ(36u, out.size());
    for (std::size_t i = 0; i < out.size(); ++i)
        EXPECT_EQ(kExpected[i], out[i]) << "output " << i;
}

TEST(Decimator, GoldenM4StereoLcgSeed3) {
    // 96 interleaved stereo frames in, 24 out; each channel filtered
    // independently.
    static const std::int16_t kExpected[] = {
        0, 0, 0, 1, 0, -1, 2, -1, 0, 9,
        -4, -27, 11, 56, -24, -93, 39, 131, -48, -155,
        42, 137, -10, -50, -62, -134, 172, 454, -321, -937,
        484, 1650, -592, -2828, -816, 7529, -8614, -1583, -1098, -14179,
        17959, 7418, -4600, 3847, -7944, 11767, -9022, 8163
    };
    Decimator d(4, 2);
    const std::vector<std::int16_t> out = runAll(d, lcg(3, 192));
    ASSERT_EQ(48u, out.size());
    for (std::size_t i = 0; i < out.size(); ++i)
        EXPECT_EQ(kExpected[i], out[i]) << "sample " << i;
}

TEST(Decimator, GoldenM3StepSaturates) {
    // A full-scale step: 60 samples of -32767 then 60 of +32767. The step
    // response overshoots int16 range; the reference counted 11 pre-clamp
    // overflows, so this golden is what pins the saturation behavior (and
    // would catch a dropped clamp as well as any filter change).
    static const std::int16_t kExpected[] = {
        0, -1, -1, 7, -18, 36, -60, 86, -107, 106,
        -73, -21, 185, -458, 864, -1537, 2949, -11471, -32768, -32202,
        -32768, -32768, -32444, -32768, -32445, -32768, -32499, -32768, -32511, -32768,
        -32621, -32720, -32768, -31848, -32768, -29693, -32768, -9826, 32767, 31637
    };
    std::vector<std::int16_t> in(120);
    for (std::size_t i = 0; i < 60; ++i) in[i] = -32767;
    for (std::size_t i = 60; i < 120; ++i) in[i] = 32767;
    Decimator d(3, 1);
    const std::vector<std::int16_t> out = runAll(d, in);
    ASSERT_EQ(40u, out.size());
    for (std::size_t i = 0; i < out.size(); ++i)
        EXPECT_EQ(kExpected[i], out[i]) << "output " << i;
}

// ---------------------------------------------------------------------------
// Decimator — structural properties.
// ---------------------------------------------------------------------------

TEST(Decimator, DcPassesBitExactlyAfterWarmup) {
    // The Q15 tap sum is forced to exactly 32768, so once the window holds
    // only the constant, (c * 32768 + 16384) >> 15 == c for every int16 c —
    // including the extremes.
    for (std::int16_t c : {static_cast<std::int16_t>(12345),
                           static_cast<std::int16_t>(-7),
                           static_cast<std::int16_t>(32767),
                           static_cast<std::int16_t>(-32768)}) {
        Decimator d(2, 1);
        const std::vector<std::int16_t> in(100, c);
        const std::vector<std::int16_t> out = runAll(d, in);
        ASSERT_EQ(50u, out.size());
        // Warmup: output j has a full window once j >= N/M - 1 = 35.5.
        for (std::size_t i = 40; i < out.size(); ++i)
            EXPECT_EQ(c, out[i]) << "constant " << c << " output " << i;
    }
}

TEST(Decimator, ChunkingIsInvisible) {
    // Feeding sample-by-sample, in ragged 7-frame chunks, or all at once must
    // produce identical bytes — the streaming contract B3's per-subscription
    // frame cadence relies on.
    for (int m : {2, 3, 4, 6}) {
        for (int ch : {1, 2}) {
            const std::vector<std::int16_t> in =
                lcg(static_cast<std::uint32_t>(100 * m + ch),
                    240 * static_cast<std::size_t>(ch));
            const std::size_t frames = in.size() / static_cast<std::size_t>(ch);

            Decimator whole(m, ch);
            const std::vector<std::int16_t> expected = runAll(whole, in);

            for (std::size_t chunk : {std::size_t{1}, std::size_t{7}}) {
                Decimator d(m, ch);
                std::vector<std::int16_t> got;
                std::vector<std::int16_t> buf(
                    (chunk / static_cast<std::size_t>(m) + 1) *
                    static_cast<std::size_t>(ch));
                for (std::size_t f = 0; f < frames; f += chunk) {
                    const std::size_t n = std::min(chunk, frames - f);
                    const std::size_t produced = d.process(
                        in.data() + f * static_cast<std::size_t>(ch), n,
                        buf.data());
                    got.insert(got.end(), buf.begin(),
                               buf.begin() +
                                   static_cast<std::ptrdiff_t>(
                                       produced * static_cast<std::size_t>(ch)));
                }
                EXPECT_EQ(expected, got)
                    << "factor " << m << " channels " << ch << " chunk " << chunk;
            }
        }
    }
}

TEST(Decimator, OutputFramesForMatchesProcessAcrossPhases) {
    Decimator d(3, 1);
    const std::vector<std::int16_t> in = lcg(42, 50);
    std::size_t total = 0;
    std::int16_t buf[32];
    for (std::size_t f = 0; f < 50; f += 7) {
        const std::size_t n = std::min<std::size_t>(7, 50 - f);
        const std::size_t predicted = d.outputFramesFor(n);
        const std::size_t produced = d.process(in.data() + f, n, buf);
        EXPECT_EQ(predicted, produced) << "at frame " << f;
        total += produced;
    }
    EXPECT_EQ(16u, total);  // floor(50 / 3)
}

TEST(Decimator, ResetRestoresFreshState) {
    const std::vector<std::int16_t> in = lcg(7, 120);
    Decimator d(4, 1);
    const std::vector<std::int16_t> first = runAll(d, in);
    d.reset();
    Decimator fresh(4, 1);
    std::vector<std::int16_t> afterReset(first.size());
    std::vector<std::int16_t> fromFresh(first.size());
    EXPECT_EQ(30u, d.process(in.data(), 120, afterReset.data()));
    EXPECT_EQ(30u, fresh.process(in.data(), 120, fromFresh.data()));
    EXPECT_EQ(first, afterReset);
    EXPECT_EQ(first, fromFresh);
}

TEST(Decimator, StereoChannelsAreIndependent) {
    // R held at zero stays exactly zero; L matches the mono run of the same
    // signal — cross-channel bleed would break both.
    const std::vector<std::int16_t> mono = lcg(11, 96);
    std::vector<std::int16_t> stereo(192);
    for (std::size_t i = 0; i < 96; ++i) {
        stereo[2 * i] = mono[i];
        stereo[2 * i + 1] = 0;
    }
    Decimator dm(2, 1);
    Decimator ds(2, 2);
    const std::vector<std::int16_t> monoOut = runAll(dm, mono);
    const std::vector<std::int16_t> stereoOut = runAll(ds, stereo);
    ASSERT_EQ(2 * monoOut.size(), stereoOut.size());
    for (std::size_t i = 0; i < monoOut.size(); ++i) {
        EXPECT_EQ(monoOut[i], stereoOut[2 * i]) << "L frame " << i;
        EXPECT_EQ(0, stereoOut[2 * i + 1]) << "R frame " << i;
    }
}

// ---------------------------------------------------------------------------
// Decimator — filter quality (tolerance-based; these are the tests a
// drop-sample or box-average "decimator" cannot pass).
// ---------------------------------------------------------------------------

TEST(Decimator, PassbandToneSurvives) {
    // 1 kHz at 48 kHz, decimated 4:1 to 12 kHz — well inside the passband;
    // amplitude must come through within 2%.
    const std::size_t kFrames = 4800;
    std::vector<std::int16_t> in(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i)
        in[i] = static_cast<std::int16_t>(std::lround(
            16000.0 * std::sin(2.0 * M_PI * 1000.0 * static_cast<double>(i) / 48000.0)));
    Decimator d(4, 1);
    const std::vector<std::int16_t> out = runAll(d, in);
    ASSERT_EQ(1200u, out.size());
    const double inRms = rms(in, 200);
    const double outRms = rms(out, 200);  // skip the startup transient
    EXPECT_NEAR(inRms, outRms, 0.02 * inRms);
}

TEST(Decimator, StopbandToneIsRejected) {
    // 9 kHz at 48 kHz is 1.5x the 12 kHz output's Nyquist: pure aliasing
    // energy. The filter's ~74 dB stopband must crush it; even 1% (-40 dB)
    // surviving fails. A drop-sample decimator passes ~100% of it.
    const std::size_t kFrames = 4800;
    std::vector<std::int16_t> in(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i)
        in[i] = static_cast<std::int16_t>(std::lround(
            16000.0 * std::sin(2.0 * M_PI * 9000.0 * static_cast<double>(i) / 48000.0)));
    Decimator d(4, 1);
    const std::vector<std::int16_t> out = runAll(d, in);
    ASSERT_EQ(1200u, out.size());
    EXPECT_LT(rms(out, 200), 0.01 * rms(in, 200));
}
