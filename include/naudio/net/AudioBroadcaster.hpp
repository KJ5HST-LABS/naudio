// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — RX fan-out from one capture source to N clients.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "naudio/AudioStreamConfig.hpp"
#include "naudio/Stream.hpp"

namespace naudio::net {

// Broadcasts RX audio from a single capture source to multiple clients.
//
// One capture std::thread reads a CaptureStream* (the existing device seam,
// Stream.hpp) and fans the bytes out to every registered BroadcastTarget. The key
// design decisions:
//   * Non-blocking fan-out: a slow target cannot block the others. (At the server level
//     the BroadcastTarget is a ClientSession that only *enqueues* to its writer thread,
//     so receiveRxAudio never does socket I/O.)
//   * snapshot-then-callback: the target map is copied under the lock and the callbacks
//     run OUTSIDE it (collect-under-lock / dispatch-after-unlock), so a target may
//     register/unregister concurrently without holding the fan-out lock across the call.
//   * A target that returns false (or whose send fails) is removed after the iteration.
//
// Targets are held by shared_ptr so a snapshot keeps a target alive across the callback
// even if it is removed/destroyed concurrently. There is no strong cycle: the server holds
// the broadcaster (strong) and the broadcaster holds its targets (strong), while each
// target/session holds only a non-owning back-reference to the server.
//
// A periodic diagnostic block (per-5s channel-level analysis + logging) is
// intentionally omitted — it is pure logging.
class AudioBroadcaster {
public:
    // A consumer of fanned-out RX audio (one per client). receiveRxAudio MUST NOT block;
    // it returns false to request its own removal.
    class BroadcastTarget {
    public:
        virtual ~BroadcastTarget() = default;
        // Receives RX audio bytes data[offset..offset+length]. Returns false to be removed.
        virtual bool receiveRxAudio(const std::uint8_t* data, std::size_t offset,
                                    std::size_t length) = 0;
        virtual std::string targetId() const = 0;
    };

    // Called when a target fails (returns false) and is removed.
    using BroadcastListener = std::function<void(const std::string& targetId,
                                                 const std::string& reason)>;
    // A transform applied to PCM before broadcast (channel routing/mute).
    using AudioTransform = std::function<std::vector<std::uint8_t>(const std::vector<std::uint8_t>&)>;

    explicit AudioBroadcaster(AudioStreamConfig config) : config_(config) {}
    ~AudioBroadcaster() { stop(); }

    AudioBroadcaster(const AudioBroadcaster&) = delete;
    AudioBroadcaster& operator=(const AudioBroadcaster&) = delete;

    void setBroadcastListener(BroadcastListener listener) { listener_ = std::move(listener); }
    void setAudioTransform(AudioTransform transform) {
        std::lock_guard<std::mutex> lock(transformMutex_);
        audioTransform_ = std::move(transform);
    }

    // Adds/removes a fan-out target (keyed by targetId()).
    void addTarget(std::shared_ptr<BroadcastTarget> target);
    std::shared_ptr<BroadcastTarget> removeTarget(const std::string& targetId);

    std::size_t targetCount() const;
    bool hasTargets() const;

    // Starts the capture thread reading `captureStream` (borrowed; must outlive the
    // broadcaster's running window). Idempotent.
    void start(CaptureStream* captureStream);
    // Stops the capture thread (joins it). Idempotent.
    void stop();
    bool isRunning() const { return running_.load(); }

    // Injects audio to fan out to all targets without a capture device (recordings /
    // engine audio / tests). The transform is applied (AudioBroadcaster.injectAudio).
    void injectAudio(const std::vector<std::uint8_t>& data);
    void injectAudio(const std::uint8_t* data, std::size_t offset, std::size_t length);

private:
    void captureLoop();
    void broadcastToTargets(const std::uint8_t* data, std::size_t offset, std::size_t length);
    void notifyTargetFailed(const std::string& targetId, const std::string& reason);

    AudioStreamConfig config_;
    mutable std::mutex targetsMutex_;
    std::map<std::string, std::shared_ptr<BroadcastTarget>> targets_;
    std::atomic<bool> running_{false};

    CaptureStream* captureStream_ = nullptr;  // borrowed
    std::thread captureThread_;
    BroadcastListener listener_;

    std::mutex transformMutex_;
    AudioTransform audioTransform_;
};

}  // namespace naudio::net
