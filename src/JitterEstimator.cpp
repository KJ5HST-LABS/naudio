// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — reliability algorithms.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/JitterEstimator.hpp"

#include <chrono>
#include <cmath>
#include <stdexcept>

namespace naudio {

JitterEstimator::JitterEstimator(std::int32_t minMs, std::int32_t maxMs, double multiplier)
    : minMs_(minMs), maxMs_(maxMs), multiplier_(multiplier), adaptiveTargetMs_(minMs) {
    if (minMs <= 0) throw std::invalid_argument("minMs must be > 0");
    if (maxMs < minMs) throw std::invalid_argument("maxMs must be >= minMs");
    if (multiplier <= 0.0) throw std::invalid_argument("multiplier must be > 0");
}

void JitterEstimator::recordArrival(std::int64_t sendTimestampNanos) {
    recordArrivalAt(sendTimestampNanos, nowNanos());
}

void JitterEstimator::recordArrivalAt(std::int64_t sendTimestampNanos, std::int64_t arrivalNanos) {
    ++packetCount_;

    if (prevArrivalNanos_ < 0) {
        // First packet — establish the baseline, no jitter sample yet.
        prevArrivalNanos_ = arrivalNanos;
        prevSendNanos_ = sendTimestampNanos;
        return;
    }

    // RFC 3550 inter-arrival jitter sample.
    const std::int64_t arrivalDelta = arrivalNanos - prevArrivalNanos_;
    const std::int64_t sendDelta = sendTimestampNanos - prevSendNanos_;
    const double jitterSampleMs =
        std::fabs(static_cast<double>(arrivalDelta - sendDelta) / 1'000'000.0);

    // EMA with gain 1/16.
    jitterMs_ += (jitterSampleMs - jitterMs_) / 16.0;

    prevArrivalNanos_ = arrivalNanos;
    prevSendNanos_ = sendTimestampNanos;

    // Desired target from jitter, clamped to [minMs, maxMs]. The clamp is
    // applied to the DOUBLE before the int32 cast: jitterMs_ can grow
    // arbitrarily large — or non-finite on a pathological/forged send
    // timestamp — and converting an out-of-range or NaN double to int32 is
    // undefined behavior. Clamping first keeps the cast always in range.
    // The `!(x >= minMs_)` form is deliberate: it is true for NaN (every NaN
    // comparison is false), folding NaN down to the floor.
    double desiredMs = std::ceil(jitterMs_ * multiplier_ + static_cast<double>(minMs_));
    if (!(desiredMs >= static_cast<double>(minMs_))) desiredMs = static_cast<double>(minMs_);
    if (desiredMs > static_cast<double>(maxMs_)) desiredMs = static_cast<double>(maxMs_);
    const std::int32_t desired = static_cast<std::int32_t>(desiredMs);

    if (desired > adaptiveTargetMs_) {
        // Ramp up immediately on a spike.
        adaptiveTargetMs_ = desired;
        lastRampDownPacket_ = packetCount_;
    } else if (desired < adaptiveTargetMs_ &&
               packetCount_ - lastRampDownPacket_ >= RAMP_DOWN_INTERVAL) {
        // Ramp down 1 ms per interval.
        const std::int32_t stepped = adaptiveTargetMs_ - 1;
        adaptiveTargetMs_ = desired > stepped ? desired : stepped;
        lastRampDownPacket_ = packetCount_;
    }
}

double JitterEstimator::jitterMs() const { return jitterMs_; }

std::int32_t JitterEstimator::adaptiveBufferTargetMs() const {
    return packetCount_ == 0 ? -1 : adaptiveTargetMs_;
}

std::int64_t JitterEstimator::packetCount() const { return packetCount_; }

void JitterEstimator::reset() {
    jitterMs_ = 0.0;
    prevArrivalNanos_ = -1;
    prevSendNanos_ = -1;
    adaptiveTargetMs_ = minMs_;
    packetCount_ = 0;
    lastRampDownPacket_ = 0;
}

std::int64_t JitterEstimator::nowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace naudio
