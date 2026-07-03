// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — TX arbitration for multiple clients sharing one radio.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "naudio/AudioRingBuffer.hpp"
#include "naudio/AudioStreamConfig.hpp"
#include "naudio/Stream.hpp"

namespace naudio::net {

// Manages TX audio arbitration for multiple clients sharing a single playback device.
//
// Priority-based arbitration with an idle timeout:
// only one client transmits at a time; the holder keeps the channel until it stops (idle
// timeout), is preempted by a higher-priority client, or disconnects.
//
// Two threads (besides the caller):
//   * playback thread — drains the TX AudioRingBuffer into a PlaybackStream*
//     (the existing device seam), writing silence on underrun. Runs only when started with
//     a playback device.
//   * idle-release thread — independent of the playback loop: on an RX-only
//     server (no playback device) the first TX claim would otherwise never be idle-released,
//     so equal-priority clients stayed denied until the owner disconnected. This thread polls
//     checkIdleTimeout() every 500 ms regardless of whether a playback device exists.
//
// The load-bearing lock discipline: client/listener callbacks are collected under
// txMutex_ and invoked AFTER unlock (the `notifications` vector). They do blocking socket
// writes (sendControl), and running them under the lock let one backpressured client block
// every submitTxAudio call and the playback loop.
class AudioMixer {
public:
    // TX priority levels for arbitration.
    enum class TxPriority { Low = 0, Normal = 1, High = 2, Exclusive = 3 };
    static bool canPreempt(TxPriority a, TxPriority b) {
        return static_cast<int>(a) > static_cast<int>(b);
    }

    // Result of a TX submission attempt.
    enum class TxResult { Accepted, Rejected, Preempted };

    // A TX client. Callbacks run on the mixer's threads, after the
    // lock is released; they MUST NOT re-enter the mixer.
    class TxClient {
    public:
        virtual ~TxClient() = default;
        virtual std::string clientId() const = 0;
        virtual TxPriority txPriority() const = 0;
        virtual void onPreempted(const std::string& preemptingClientId) = 0;
        virtual void onTxGranted() = 0;
        virtual void onTxReleased() = 0;
    };

    // Mixer event callbacks. newOwner == "" means "no owner".
    struct MixerListener {
        std::function<void(const std::string& holding, const std::string& requesting)> onTxConflict;
        std::function<void(const std::string& newOwner)> onTxOwnerChanged;
    };

    explicit AudioMixer(AudioStreamConfig config);
    ~AudioMixer();

    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    void setMixerListener(MixerListener listener) { listener_ = std::move(listener); }

    // Test seam: override the millisecond clock used for idle-timeout (clock injection).
    // Defaults to the system clock. Call before exercising idle timing.
    void setClock(std::function<std::int64_t()> clock) { clock_ = std::move(clock); }

    void registerClient(std::shared_ptr<TxClient> client);
    // Unregisters a client; releases the channel if it currently holds it.
    void unregisterClient(const std::string& clientId);

    // Submits TX audio from a client (claim / refresh / preempt / reject).
    TxResult submitTxAudio(const std::string& clientId, const std::uint8_t* data,
                           std::size_t offset, std::size_t length);
    TxResult submitTxAudio(const std::string& clientId, const std::vector<std::uint8_t>& data) {
        return submitTxAudio(clientId, data.data(), 0, data.size());
    }

    // The current TX owner ("" if none).
    std::string currentTxOwner() const;
    bool isTxOwner(const std::string& clientId) const;
    // Explicitly releases the channel for `clientId` (no-op if it is not the owner).
    void releaseTx(const std::string& clientId);

    // Starts the playback thread draining the TX buffer into `playbackStream` (borrowed).
    void start(PlaybackStream* playbackStream);
    // Stops the playback thread (the idle thread keeps running — see shutdown()).
    void stop();
    // Permanently shuts the mixer down (stops the idle thread too). Call when discarding.
    void shutdown();
    bool isRunning() const { return running_.load(); }

    AudioRingBuffer& txBuffer() { return txBuffer_; }

    // Releases the channel if the owner has been idle past txIdleTimeoutMs. Public so the
    // idle thread, the playback loop, and deterministic tests can all drive it.
    void checkIdleTimeout();

private:
    struct TxOwnership {
        std::string clientId;
        TxPriority priority;
    };

    void playbackLoop();
    void idleLoop();
    std::int64_t now() const { return clock_(); }
    std::shared_ptr<TxClient> clientFor(const std::string& id) const;

    // The *Locked methods require txMutex_ held; they append deferred callbacks to
    // `notifications` for invocation after unlock (the collect/dispatch discipline).
    using Notifications = std::vector<std::function<void()>>;
    void claimTxChannelLocked(const std::string& clientId, TxPriority priority, Notifications& n);
    void preemptCurrentOwnerLocked(const std::string& newClientId, TxPriority newPriority,
                                   Notifications& n);
    void releaseTxChannelLocked(const std::string& clientId, Notifications& n);
    static void runNotifications(const Notifications& n);
    void notifyTxConflict(const std::string& holding, const std::string& requesting) const;
    void notifyTxOwnerChanged(const std::string& newOwner) const;

    AudioStreamConfig config_;
    mutable std::mutex txMutex_;
    mutable std::mutex clientsMutex_;
    std::map<std::string, std::shared_ptr<TxClient>> clients_;
    std::atomic<bool> running_{false};

    PlaybackStream* playbackStream_ = nullptr;  // borrowed
    std::thread playbackThread_;
    MixerListener listener_;

    // TX ownership — guarded by txMutex_.
    std::optional<TxOwnership> txOwnership_;
    std::int64_t lastTxActivityTime_ = 0;

    AudioRingBuffer txBuffer_;
    std::function<std::int64_t()> clock_;

    // Independent idle-timeout thread.
    std::thread idleThread_;
    std::mutex idleMutex_;
    std::condition_variable idleCv_;
    bool idleShutdown_ = false;
};

}  // namespace naudio::net
