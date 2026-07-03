// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — AudioRingBuffer unit tests.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// No conformance vector gates
// the ring buffer, so these tests ARE the contract: write/read, partial read,
// offset write, overrun drop-oldest-and-wrap, stats/reset, non-blocking read,
// config update, display — plus a threaded test exercising the condvar wakeup
// (the blocking read returning when a writer thread produces data).
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "naudio/AudioRingBuffer.hpp"
#include "naudio/AudioStreamConfig.hpp"

using naudio::AudioRingBuffer;
using naudio::AudioStreamConfig;

namespace {

std::vector<std::uint8_t> ramp(int from, int to) {
    std::vector<std::uint8_t> v;
    for (int i = from; i < to; ++i) v.push_back(static_cast<std::uint8_t>(i));
    return v;
}

TEST(AudioRingBuffer, InitialState) {
    AudioRingBuffer buffer{AudioStreamConfig{}};
    EXPECT_EQ(0u, buffer.available());
    EXPECT_EQ(0, buffer.bufferLevelMs());
    EXPECT_EQ(0, buffer.bufferFillPercent());
    EXPECT_FALSE(buffer.hasReachedTargetLevel());
}

TEST(AudioRingBuffer, WriteAndRead) {
    AudioRingBuffer buffer{AudioStreamConfig{}};
    auto writeData = ramp(0, 100);
    buffer.writeBuf(writeData);
    EXPECT_EQ(100u, buffer.available());

    std::vector<std::uint8_t> readData(100, 0);
    const std::int32_t n = buffer.read(readData.data(), 0, 100, 100);
    EXPECT_EQ(100, n);
    EXPECT_EQ(0u, buffer.available());
    EXPECT_EQ(writeData, readData);
}

TEST(AudioRingBuffer, PartialRead) {
    AudioRingBuffer buffer{AudioStreamConfig{}};
    buffer.writeBuf(ramp(0, 100));
    std::vector<std::uint8_t> readData(50, 0);
    const std::int32_t n = buffer.read(readData.data(), 0, 50, 100);
    EXPECT_EQ(50, n);
    EXPECT_EQ(50u, buffer.available());
    EXPECT_EQ(ramp(0, 50), readData);
}

TEST(AudioRingBuffer, WriteWithOffset) {
    AudioRingBuffer buffer{AudioStreamConfig{}};
    auto data = ramp(0, 100);
    buffer.write(data.data(), 25, 50);  // only bytes 25..75
    EXPECT_EQ(50u, buffer.available());
    std::vector<std::uint8_t> readData(50, 0);
    buffer.read(readData.data(), 0, 50, 100);
    EXPECT_EQ(ramp(25, 75), readData);
}

TEST(AudioRingBuffer, Clear) {
    AudioRingBuffer buffer{AudioStreamConfig{}};
    buffer.writeBuf(std::vector<std::uint8_t>(100, 0));
    EXPECT_EQ(100u, buffer.available());
    buffer.clear();
    EXPECT_EQ(0u, buffer.available());
}

TEST(AudioRingBuffer, BufferLevelMs) {
    AudioStreamConfig config{};
    AudioRingBuffer buffer{config};
    const std::size_t bytesFor200ms = static_cast<std::size_t>(config.msToBytes(200));
    buffer.writeBuf(std::vector<std::uint8_t>(bytesFor200ms, 0));
    const std::int32_t levelMs = buffer.bufferLevelMs();
    EXPECT_GE(levelMs, 190);
    EXPECT_LE(levelMs, 210);
}

TEST(AudioRingBuffer, TargetLevelReached) {
    AudioStreamConfig config{};
    AudioRingBuffer buffer{config};
    EXPECT_FALSE(buffer.hasReachedTargetLevel());
    const std::size_t targetBytes = static_cast<std::size_t>(config.msToBytes(config.bufferTargetMs));
    buffer.writeBuf(std::vector<std::uint8_t>(targetBytes, 0));
    EXPECT_TRUE(buffer.hasReachedTargetLevel());
}

TEST(AudioRingBuffer, OverrunDropsOldestAndWraps) {
    AudioRingBuffer buffer{AudioStreamConfig{}, 10};
    EXPECT_EQ(10u, buffer.capacity());
    buffer.writeBuf(ramp(0, 10));  // fill 0..10
    EXPECT_EQ(10u, buffer.available());
    EXPECT_EQ(0, buffer.overrunCount());

    buffer.writeBuf(ramp(10, 15));  // 5 more -> drop oldest 5 (0..5)
    EXPECT_EQ(10u, buffer.available());
    EXPECT_EQ(1, buffer.overrunCount());

    std::vector<std::uint8_t> out(10, 0);
    EXPECT_EQ(10, buffer.read(out.data(), 0, 10, 100));
    EXPECT_EQ(ramp(5, 15), out);  // 5..15 in order
}

TEST(AudioRingBuffer, TotalBytesAndResetStatistics) {
    AudioRingBuffer buffer{AudioStreamConfig{}};
    buffer.writeBuf(std::vector<std::uint8_t>(100, 0));
    std::vector<std::uint8_t> out(40, 0);
    buffer.read(out.data(), 0, 40, 100);
    EXPECT_EQ(100, buffer.totalBytesWritten());
    EXPECT_EQ(40, buffer.totalBytesRead());
    buffer.resetStatistics();
    EXPECT_EQ(0, buffer.totalBytesWritten());
    EXPECT_EQ(0, buffer.totalBytesRead());
    EXPECT_EQ(0, buffer.underrunCount());
    EXPECT_EQ(0, buffer.overrunCount());
}

TEST(AudioRingBuffer, NonBlockingReadReturnsZeroImmediately) {
    AudioRingBuffer buffer{AudioStreamConfig{}};
    std::vector<std::uint8_t> out(10, 0);
    EXPECT_EQ(0, buffer.read(out.data(), 0, 10, 0));  // non-blocking, no data
    EXPECT_EQ(0, buffer.underrunCount());             // no underrun bump
}

TEST(AudioRingBuffer, RatesAreZeroWithinAMinute) {
    AudioRingBuffer buffer{AudioStreamConfig{}, 10};
    buffer.writeBuf(std::vector<std::uint8_t>(10, 0));
    buffer.writeBuf(std::vector<std::uint8_t>(5, 0));  // overrun
    std::vector<std::uint8_t> out(10, 0);
    buffer.read(out.data(), 0, 10, 100);  // drain
    buffer.read(out.data(), 0, 1, 1);     // empty -> underrun
    EXPECT_FLOAT_EQ(0.0f, buffer.overrunRate());
    EXPECT_FLOAT_EQ(0.0f, buffer.underrunRate());
}

TEST(AudioRingBuffer, UpdateConfigReplacesConfig) {
    AudioRingBuffer buffer{AudioStreamConfig{}};
    const std::int32_t originalTarget = buffer.config().bufferTargetMs;
    AudioStreamConfig newConfig{};
    newConfig.bufferTargetMs = originalTarget + 50;
    buffer.updateConfig(newConfig);
    EXPECT_EQ(originalTarget + 50, buffer.config().bufferTargetMs);
}

TEST(AudioRingBuffer, DisplayFormat) {
    AudioRingBuffer buffer{AudioStreamConfig{}, 1000};
    const std::string s = buffer.displayString();
    EXPECT_EQ(0u, s.rfind("AudioRingBuffer[", 0));  // starts_with
    EXPECT_NE(std::string::npos, s.find("/1000 bytes"));
    EXPECT_NE(std::string::npos, s.find("underruns"));
    EXPECT_NE(std::string::npos, s.find("overruns"));
}

// The platform-coupled path: a blocking read parks on the not-empty condvar and
// wakes when a writer thread produces data. (Bounded: writer fires ~20 ms in,
// read timeout 1 s — far longer, so this is deterministic, not a race.)
TEST(AudioRingBuffer, BlockingReadWakesOnWrite) {
    AudioRingBuffer buffer{AudioStreamConfig{}, 1000};
    std::thread writer([&buffer]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        buffer.writeBuf(std::vector<std::uint8_t>(100, 0xAB));
    });
    std::vector<std::uint8_t> out(100, 0);
    const std::int32_t n = buffer.read(out.data(), 0, 100, 1000);  // blocks until the write
    writer.join();
    EXPECT_EQ(100, n);
    EXPECT_EQ(0xAB, out.front());
    EXPECT_EQ(0xAB, out.back());
}

}  // namespace
