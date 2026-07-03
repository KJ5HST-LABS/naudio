// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — reliability algorithms.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "naudio/AudioStreamConfig.hpp"

namespace naudio {

// Thread-safe circular audio buffer with jitter compensation.
//
// A circular byte buffer with a non-blocking write (overwrites the oldest data
// on overrun) and a blocking read (waits up to a timeout for data, returns
// partial data immediately). It uses a std::mutex + two std::condition_variables,
// and the synchronous shape is correct
// here: the eventual consumer is the real-time audio callback, itself
// synchronous, so a blocking buffer is appropriate, not premature. This is the one
// genuinely platform-coupled subsystem; no conformance vector gates it.
//
// Not copyable/movable (holds a mutex + condvars) — construct in place and pass
// by reference/pointer.
//
// Compiled into naudio_core (definitions in AudioRingBuffer.cpp); the passive
// std::mutex/condition_variable synchronization is why naudio_core links
// Threads. The buffer itself spawns no thread — the blocking read parks the
// caller's own thread.
class AudioRingBuffer {
public:
    // Creates a ring buffer sized from the config (msToBytes(bufferMaxMs * 2)).
    explicit AudioRingBuffer(AudioStreamConfig config);

    // Creates a ring buffer with an explicit capacity in bytes.
    AudioRingBuffer(AudioStreamConfig config, std::int32_t capacityBytes);

    AudioRingBuffer(const AudioRingBuffer&) = delete;
    AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;

    // Updates the buffer threshold configuration (target/min/max).
    void updateConfig(AudioStreamConfig newConfig);

    // Writes data[offset..offset+length] to the buffer. On overrun the oldest
    // data is overwritten. Never blocks. Returns bytes written (always length).
    std::size_t write(const std::uint8_t* data, std::size_t offset, std::size_t length);

    // Writes the whole vector.
    std::size_t writeBuf(const std::vector<std::uint8_t>& data);

    // Reads up to `length` bytes into data[offset..]. Blocks until some data is
    // available or timeoutMs elapses (0 = non-blocking). Returns bytes read (may
    // be partial), 0 on timeout with no data.
    std::int32_t read(std::uint8_t* data, std::size_t offset, std::size_t length,
                      std::int64_t timeoutMs);

    // Reads the whole vector with the default timeout (frameDurationMs * 2).
    std::int32_t readBuf(std::vector<std::uint8_t>& data);

    std::size_t available() const;
    std::int32_t bufferLevelMs() const;
    std::int32_t bufferFillPercent() const;
    bool hasReachedTargetLevel() const;
    bool isBelowMinimum() const;
    bool isAboveMaximum() const;
    void clear();
    std::int64_t totalBytesWritten() const;
    std::int64_t totalBytesRead() const;
    std::int32_t underrunCount() const;
    std::int32_t overrunCount() const;
    void resetStatistics();
    std::size_t capacity() const;

    // A copy of the configuration. (Mutate via updateConfig.)
    AudioStreamConfig config() const;

    // Overruns per minute since the last reset; 0 if less than a minute elapsed.
    float overrunRate() const;

    // Underruns per minute since the last reset; 0 if less than a minute elapsed.
    float underrunRate() const;

    // "AudioRingBuffer[avail/cap bytes, N ms, U underruns, O overruns]".
    std::string displayString() const;

private:
    static std::int64_t nowMillis();

    // Records an overrun for rate tracking (logging is omitted).
    void noteOverrun();
    void noteUnderrun();

    const std::size_t capacity_;
    std::vector<std::uint8_t> buffer_;
    AudioStreamConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;  // signaled by read

    std::size_t writePos_ = 0;
    std::size_t readPos_ = 0;
    std::size_t available_ = 0;
    std::int64_t totalBytesWritten_ = 0;
    std::int64_t totalBytesRead_ = 0;
    std::int32_t underrunCount_ = 0;
    std::int32_t overrunCount_ = 0;

    // Rate-tracking bookkeeping (the rate-limited logging itself is omitted).
    std::int64_t lastOverrunLogTime_ = 0;
    std::int64_t lastUnderrunLogTime_ = 0;
    std::int32_t overrunsSinceLastLog_ = 0;
    std::int32_t underrunsSinceLastLog_ = 0;
    bool firstOverrun_ = true;
    bool firstUnderrun_ = true;
};

}  // namespace naudio
