// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — reliability algorithms.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/AudioRingBuffer.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace naudio {

AudioRingBuffer::AudioRingBuffer(AudioStreamConfig config)
    : AudioRingBuffer(config, config.msToBytes(config.bufferMaxMs * 2)) {}

AudioRingBuffer::AudioRingBuffer(AudioStreamConfig config, std::int32_t capacityBytes)
    : capacity_(capacityBytes > 0 ? static_cast<std::size_t>(capacityBytes) : 0),
      buffer_(capacity_, 0),
      config_(config) {}

void AudioRingBuffer::updateConfig(AudioStreamConfig newConfig) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = newConfig;
}

std::size_t AudioRingBuffer::write(const std::uint8_t* data, std::size_t offset,
                                   std::size_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (capacity_ == 0) return 0;

    // Overrun: drop the oldest data to make room.
    if (available_ + length > capacity_) {
        const std::size_t toDrop = (available_ + length) - capacity_;
        readPos_ = (readPos_ + toDrop) % capacity_;
        available_ -= toDrop;
        ++overrunCount_;
        noteOverrun();
    }

    std::size_t bytesWritten = 0;
    while (bytesWritten < length) {
        const std::size_t spaceToEnd = capacity_ - writePos_;
        const std::size_t toWrite = std::min(length - bytesWritten, spaceToEnd);
        std::memcpy(&buffer_[writePos_], data + offset + bytesWritten, toWrite);
        writePos_ = (writePos_ + toWrite) % capacity_;
        bytesWritten += toWrite;
        available_ += toWrite;
    }

    totalBytesWritten_ += static_cast<std::int64_t>(bytesWritten);
    notEmpty_.notify_all();
    return bytesWritten;
}

std::size_t AudioRingBuffer::writeBuf(const std::vector<std::uint8_t>& data) {
    return write(data.data(), 0, data.size());
}

std::int32_t AudioRingBuffer::read(std::uint8_t* data, std::size_t offset, std::size_t length,
                                   std::int64_t timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (capacity_ == 0) return 0;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0);

    while (available_ == 0) {
        if (timeoutMs == 0) return 0;  // non-blocking, no data
        if (std::chrono::steady_clock::now() >= deadline) {
            ++underrunCount_;  // timeout with no data — an underrun
            noteUnderrun();
            return 0;
        }
        notEmpty_.wait_until(lock, deadline);
    }

    const std::size_t toRead = std::min(length, available_);
    std::size_t bytesRead = 0;
    while (bytesRead < toRead) {
        const std::size_t dataToEnd = capacity_ - readPos_;
        const std::size_t chunk = std::min(toRead - bytesRead, dataToEnd);
        std::memcpy(data + offset + bytesRead, &buffer_[readPos_], chunk);
        readPos_ = (readPos_ + chunk) % capacity_;
        bytesRead += chunk;
        available_ -= chunk;
    }

    totalBytesRead_ += static_cast<std::int64_t>(bytesRead);
    notFull_.notify_all();
    return static_cast<std::int32_t>(bytesRead);
}

std::int32_t AudioRingBuffer::readBuf(std::vector<std::uint8_t>& data) {
    std::int64_t timeout;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        timeout = static_cast<std::int64_t>(config_.frameDurationMs) * 2;
    }
    return read(data.data(), 0, data.size(), timeout);
}

std::size_t AudioRingBuffer::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_;
}

std::int32_t AudioRingBuffer::bufferLevelMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.bytesToMs(static_cast<std::int32_t>(available_));
}

std::int32_t AudioRingBuffer::bufferFillPercent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capacity_ == 0 ? 0 : static_cast<std::int32_t>(available_ * 100 / capacity_);
}

bool AudioRingBuffer::hasReachedTargetLevel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.bytesToMs(static_cast<std::int32_t>(available_)) >= config_.bufferTargetMs;
}

bool AudioRingBuffer::isBelowMinimum() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.bytesToMs(static_cast<std::int32_t>(available_)) < config_.bufferMinMs;
}

bool AudioRingBuffer::isAboveMaximum() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.bytesToMs(static_cast<std::int32_t>(available_)) > config_.bufferMaxMs;
}

void AudioRingBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    writePos_ = 0;
    readPos_ = 0;
    available_ = 0;
}

std::int64_t AudioRingBuffer::totalBytesWritten() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalBytesWritten_;
}

std::int64_t AudioRingBuffer::totalBytesRead() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalBytesRead_;
}

std::int32_t AudioRingBuffer::underrunCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return underrunCount_;
}

std::int32_t AudioRingBuffer::overrunCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return overrunCount_;
}

void AudioRingBuffer::resetStatistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    totalBytesWritten_ = 0;
    totalBytesRead_ = 0;
    underrunCount_ = 0;
    overrunCount_ = 0;
}

std::size_t AudioRingBuffer::capacity() const { return capacity_; }

AudioStreamConfig AudioRingBuffer::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

float AudioRingBuffer::overrunRate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::int64_t elapsed = nowMillis() - lastOverrunLogTime_;
    if (elapsed < 60'000) return 0.0f;
    return (static_cast<float>(overrunsSinceLastLog_) * 60'000.0f) / static_cast<float>(elapsed);
}

float AudioRingBuffer::underrunRate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::int64_t elapsed = nowMillis() - lastUnderrunLogTime_;
    if (elapsed < 60'000) return 0.0f;
    return (static_cast<float>(underrunsSinceLastLog_) * 60'000.0f) / static_cast<float>(elapsed);
}

std::string AudioRingBuffer::displayString() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return "AudioRingBuffer[" + std::to_string(available_) + "/" + std::to_string(capacity_) +
           " bytes, " + std::to_string(config_.bytesToMs(static_cast<std::int32_t>(available_))) +
           " ms, " + std::to_string(underrunCount_) + " underruns, " +
           std::to_string(overrunCount_) + " overruns]";
}

std::int64_t AudioRingBuffer::nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void AudioRingBuffer::noteOverrun() {
    ++overrunsSinceLastLog_;
    const std::int64_t now = nowMillis();
    if (firstOverrun_) {
        firstOverrun_ = false;
        lastOverrunLogTime_ = now;
        overrunsSinceLastLog_ = 0;
        return;
    }
    if (now - lastOverrunLogTime_ >= 60'000) {
        lastOverrunLogTime_ = now;
        overrunsSinceLastLog_ = 0;
    }
}

void AudioRingBuffer::noteUnderrun() {
    ++underrunsSinceLastLog_;
    const std::int64_t now = nowMillis();
    if (firstUnderrun_) {
        firstUnderrun_ = false;
        lastUnderrunLogTime_ = now;
        underrunsSinceLastLog_ = 0;
        return;
    }
    if (now - lastUnderrunLogTime_ >= 60'000) {
        lastUnderrunLogTime_ = now;
        underrunsSinceLastLog_ = 0;
    }
}

}  // namespace naudio
