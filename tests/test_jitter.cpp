// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — JitterEstimator unit tests.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// These cover the EMA/ramp paths that
// the 2 structural conformance vectors (empty -> -1, first-packet -> minMs) do
// NOT exercise — the conformance suite gates the baseline contract; these gate
// the adaptive math.
#include <gtest/gtest.h>

#include "naudio/JitterEstimator.hpp"

using naudio::JitterEstimator;

namespace {

// Default bounds used across these tests: minMs=20, maxMs=200, mult=3.0.
JitterEstimator estimator() { return JitterEstimator(20, 200, 3.0); }

constexpr std::int64_t kInterval = 10'000'000;  // 10 ms in nanos

TEST(JitterEstimator, ConstructorValidation) {
    EXPECT_THROW(JitterEstimator(0, 200, 3.0), std::invalid_argument);
    EXPECT_THROW(JitterEstimator(20, 10, 3.0), std::invalid_argument);
    EXPECT_THROW(JitterEstimator(20, 200, 0.0), std::invalid_argument);
    EXPECT_NO_THROW(JitterEstimator(1, 1, 0.1));
}

// R2: a pathological send-timestamp delta drives the desired buffer target far
// past INT32_MAX. The clamp is applied to the DOUBLE before the int32 cast, so the
// target stays within [minMs, maxMs] and there is no out-of-range double->int32
// conversion (UB — UBSan would abort without the clamp; without sanitizers the
// pre-clamp produces the correct ceiling=maxMs instead of a garbage value).
TEST(JitterEstimator, PathologicalDeltaClampsTargetNoUb) {
    JitterEstimator e(10, 200, 3.0);
    e.recordArrivalAt(0, 0);  // baseline (first packet)
    // arrivalDelta=0, sendDelta=-9e17 -> jitter sample ~9e11 ms -> raw target ~1.7e11,
    // far beyond INT32_MAX; the int32 cast would be UB without the clamp.
    e.recordArrivalAt(-900'000'000'000'000'000LL, 0);
    const std::int32_t target = e.adaptiveBufferTargetMs();
    EXPECT_GE(target, 10);
    EXPECT_LE(target, 200);  // clamped to maxMs, not an overflowed/garbage value
}

TEST(JitterEstimator, NoPacketsReturnsNegativeOne) {
    JitterEstimator e = estimator();
    EXPECT_EQ(-1, e.adaptiveBufferTargetMs());
    EXPECT_DOUBLE_EQ(0.0, e.jitterMs());
    EXPECT_EQ(0, e.packetCount());
}

TEST(JitterEstimator, FirstPacketEstablishesBaseline) {
    JitterEstimator e = estimator();
    e.recordArrivalAt(1'000'000'000, 2'000'000'000);
    EXPECT_EQ(1, e.packetCount());
    EXPECT_DOUBLE_EQ(0.0, e.jitterMs());
    EXPECT_EQ(20, e.adaptiveBufferTargetMs());
}

TEST(JitterEstimator, ZeroJitterTargetAtMin) {
    JitterEstimator e = estimator();
    // Perfectly spaced packets: 10 ms apart in both send and arrival time.
    for (std::int64_t i = 0; i < 200; ++i) {
        e.recordArrivalAt(1000 * kInterval + i * kInterval, 2000 * kInterval + i * kInterval);
    }
    EXPECT_EQ(20, e.adaptiveBufferTargetMs());
    EXPECT_LT(e.jitterMs(), 1.0);
}

TEST(JitterEstimator, VaryingJitterProducesElevatedTarget) {
    JitterEstimator e = estimator();
    const std::int64_t jitter = 5'000'000;  // 5 ms
    e.recordArrivalAt(1'000'000'000, 2'000'000'000);
    for (std::int64_t i = 1; i < 500; ++i) {
        const std::int64_t sign = (i % 2 == 0) ? 1 : -1;
        e.recordArrivalAt(1'000'000'000 + i * kInterval,
                          2'000'000'000 + i * kInterval + sign * jitter);
    }
    EXPECT_GT(e.jitterMs(), 1.0);
    const std::int32_t target = e.adaptiveBufferTargetMs();
    EXPECT_GT(target, 20);
    EXPECT_LE(target, 200);
}

TEST(JitterEstimator, SpikeRampsUpImmediately) {
    JitterEstimator e = estimator();
    for (std::int64_t i = 0; i < 50; ++i) {
        e.recordArrivalAt(1'000'000'000 + i * kInterval, 2'000'000'000 + i * kInterval);
    }
    const std::int32_t stable = e.adaptiveBufferTargetMs();
    EXPECT_EQ(20, stable);

    // Inject a 30 ms jitter spike (arrival delayed by 30 ms).
    e.recordArrivalAt(1'000'000'000 + 50 * kInterval,
                      2'000'000'000 + 50 * kInterval + 30'000'000);
    EXPECT_GT(e.adaptiveBufferTargetMs(), stable);
}

TEST(JitterEstimator, RampDownIsGradual) {
    JitterEstimator e = estimator();
    const std::int64_t spikeDelay = 30'000'000;
    e.recordArrivalAt(1'000'000'000, 2'000'000'000);
    e.recordArrivalAt(1'000'000'000 + kInterval, 2'000'000'000 + kInterval + spikeDelay);
    const std::int32_t afterSpike = e.adaptiveBufferTargetMs();
    EXPECT_GT(afterSpike, 20);

    // Feed 200 stable packets — arrivals at consistent intervals from the spike.
    const std::int64_t spikedArrival = 2'000'000'000 + kInterval + spikeDelay;
    for (std::int64_t i = 2; i < 202; ++i) {
        e.recordArrivalAt(1'000'000'000 + i * kInterval, spikedArrival + (i - 1) * kInterval);
    }
    const std::int32_t afterRecovery = e.adaptiveBufferTargetMs();
    EXPECT_LT(afterRecovery, afterSpike);
    EXPECT_GE(afterRecovery, 20);
}

TEST(JitterEstimator, MinMaxBoundsRespected) {
    JitterEstimator bounded(30, 100, 3.0);
    bounded.recordArrivalAt(1'000'000'000, 2'000'000'000);
    // Massive 200 ms spike — without clamping the target would exceed 100.
    bounded.recordArrivalAt(1'010'000'000, 2'010'000'000LL + 200'000'000);
    const std::int32_t target = bounded.adaptiveBufferTargetMs();
    EXPECT_GE(target, 30);
    EXPECT_LE(target, 100);
}

TEST(JitterEstimator, ResetClearsState) {
    JitterEstimator e = estimator();
    e.recordArrivalAt(1'000'000'000, 2'000'000'000);
    e.recordArrivalAt(1'010'000'000, 2'010'000'000);
    EXPECT_GT(e.packetCount(), 0);
    e.reset();
    EXPECT_EQ(0, e.packetCount());
    EXPECT_DOUBLE_EQ(0.0, e.jitterMs());
    EXPECT_EQ(-1, e.adaptiveBufferTargetMs());
}

}  // namespace
