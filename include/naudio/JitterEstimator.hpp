// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — reliability algorithms.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstdint>

namespace naudio {

// Estimates network jitter (RFC 3550) and derives an adaptive buffer target.
//
// On each received audio packet, call recordArrival() with the packet's send
// timestamp (nanoseconds). Inter-arrival jitter is tracked with an exponential
// moving average (gain 1/16, RFC 3550); the adaptive buffer target ramps up
// immediately on a jitter spike and ramps down 1 ms per RAMP_DOWN_INTERVAL
// packets to prevent oscillation. Per the toolkit's clock-injection convention,
// recordArrivalAt() (explicit arrival timestamp) is the primary, testable form;
// recordArrival() is a thin wrapper that reads the monotonic clock.
//
// Pure: no I/O, no external dependencies. Constructor arguments are validated
// (throwing std::invalid_argument on an invalid value).
//
// Compiled into naudio_core (definitions in JitterEstimator.cpp).
class JitterEstimator {
public:
    // Number of packets between ramp-down steps (~1 s at 100 packets/s for 10 ms
    // frames).
    static constexpr std::int64_t RAMP_DOWN_INTERVAL = 100;

    // Creates an estimator. minMs must be > 0, maxMs >= minMs, multiplier > 0.
    JitterEstimator(std::int32_t minMs, std::int32_t maxMs, double multiplier);

    // Records a packet arrival using the monotonic clock for the arrival time.
    void recordArrival(std::int64_t sendTimestampNanos);

    // Records a packet arrival with an explicit arrival timestamp (the primary,
    // testable form; the conformance loader drives this).
    void recordArrivalAt(std::int64_t sendTimestampNanos, std::int64_t arrivalNanos);

    // The current jitter estimate in milliseconds.
    double jitterMs() const;

    // The adaptive buffer target (ms), or -1 if no packets have been recorded.
    std::int32_t adaptiveBufferTargetMs() const;

    // The total number of packets recorded.
    std::int64_t packetCount() const;

    // Resets to the initial state.
    void reset();

private:
    // Monotonic nanoseconds (only deltas are used).
    static std::int64_t nowNanos();

    std::int32_t minMs_;
    std::int32_t maxMs_;
    double multiplier_;
    double jitterMs_ = 0.0;
    std::int64_t prevArrivalNanos_ = -1;
    std::int64_t prevSendNanos_ = -1;
    std::int32_t adaptiveTargetMs_;
    std::int64_t packetCount_ = 0;
    std::int64_t lastRampDownPacket_ = 0;
};

}  // namespace naudio
