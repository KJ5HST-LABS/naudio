// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — AudioMixer implementation.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/net/AudioMixer.hpp"

#include <chrono>
#include <utility>

namespace naudio::net {

namespace {
std::int64_t systemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

AudioMixer::AudioMixer(AudioStreamConfig config)
    : config_(config),
      txBuffer_(config, config.msToBytes(config.bufferMaxMs * 2)),
      clock_(systemNowMs) {
    // The idle-release thread runs independently of the playback loop so an RX-only server
    // still releases the channel on idle.
    idleThread_ = std::thread(&AudioMixer::idleLoop, this);
}

AudioMixer::~AudioMixer() { shutdown(); }

std::shared_ptr<AudioMixer::TxClient> AudioMixer::clientFor(const std::string& id) const {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    auto it = clients_.find(id);
    return it != clients_.end() ? it->second : nullptr;
}

void AudioMixer::registerClient(std::shared_ptr<TxClient> client) {
    if (!client) return;
    std::lock_guard<std::mutex> lock(clientsMutex_);
    clients_[client->clientId()] = std::move(client);
}

void AudioMixer::unregisterClient(const std::string& clientId) {
    std::shared_ptr<TxClient> removed;
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        auto it = clients_.find(clientId);
        if (it != clients_.end()) {
            removed = it->second;
            clients_.erase(it);
        }
    }
    if (!removed) return;

    Notifications notifications;
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        if (txOwnership_.has_value() && txOwnership_->clientId == clientId) {
            releaseTxChannelLocked(clientId, notifications);
        }
    }
    runNotifications(notifications);
}

AudioMixer::TxResult AudioMixer::submitTxAudio(const std::string& clientId,
                                               const std::uint8_t* data, std::size_t offset,
                                               std::size_t length) {
    auto client = clientFor(clientId);
    if (!client) return TxResult::Rejected;

    // Callbacks are collected under the lock and invoked after unlock:
    // they do blocking sendControl writes, and running them under txMutex_ let one
    // backpressured client block every submitTxAudio call and the playback loop.
    Notifications notifications;
    TxResult result;
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        if (!txOwnership_.has_value()) {
            claimTxChannelLocked(clientId, client->txPriority(), notifications);
            result = TxResult::Accepted;
        } else if (txOwnership_->clientId == clientId) {
            lastTxActivityTime_ = now();  // we own it — refresh activity
            result = TxResult::Accepted;
        } else {
            const TxPriority ourPriority = client->txPriority();
            if (canPreempt(ourPriority, txOwnership_->priority)) {
                preemptCurrentOwnerLocked(clientId, ourPriority, notifications);
                result = TxResult::Accepted;
            } else {
                const std::string holder = txOwnership_->clientId;
                notifications.push_back(
                    [this, holder, clientId]() { notifyTxConflict(holder, clientId); });
                result = TxResult::Rejected;
            }
        }

        if (result == TxResult::Accepted) {
            txBuffer_.write(data, offset, length);
        }
    }

    runNotifications(notifications);
    return result;
}

std::string AudioMixer::currentTxOwner() const {
    std::lock_guard<std::mutex> lock(txMutex_);
    return txOwnership_.has_value() ? txOwnership_->clientId : "";
}

bool AudioMixer::isTxOwner(const std::string& clientId) const {
    std::lock_guard<std::mutex> lock(txMutex_);
    return !clientId.empty() && txOwnership_.has_value() && txOwnership_->clientId == clientId;
}

void AudioMixer::releaseTx(const std::string& clientId) {
    Notifications notifications;
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        if (txOwnership_.has_value() && txOwnership_->clientId == clientId) {
            releaseTxChannelLocked(clientId, notifications);
        }
    }
    runNotifications(notifications);
}

void AudioMixer::start(PlaybackStream* playbackStream) {
    if (running_.exchange(true)) return;
    playbackStream_ = playbackStream;
    playbackThread_ = std::thread(&AudioMixer::playbackLoop, this);
}

void AudioMixer::stop() {
    if (!running_.exchange(false)) return;
    if (playbackThread_.joinable()) {
        playbackThread_.join();
    }
    playbackStream_ = nullptr;
    std::lock_guard<std::mutex> lock(txMutex_);
    txOwnership_.reset();
    txBuffer_.clear();
}

void AudioMixer::shutdown() {
    stop();
    {
        std::lock_guard<std::mutex> lock(idleMutex_);
        idleShutdown_ = true;
    }
    idleCv_.notify_all();
    if (idleThread_.joinable()) {
        idleThread_.join();
    }
}

void AudioMixer::checkIdleTimeout() {
    Notifications notifications;
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        if (!txOwnership_.has_value()) return;
        if (now() - lastTxActivityTime_ >= config_.txIdleTimeoutMs) {
            const std::string owner = txOwnership_->clientId;
            releaseTxChannelLocked(owner, notifications);
        }
    }
    runNotifications(notifications);
}

void AudioMixer::playbackLoop() {
    const int frameSize = playbackStream_->actualFormat().frameSize();
    const int bufferBytes = config_.bytesPerFrame();
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(bufferBytes));
    std::vector<std::uint8_t> silence(static_cast<std::size_t>(bufferBytes), 0);

    // Initial buffering with a wall-clock timeout (steady_clock — independent of the
    // injectable TX-activity clock).
    const auto bufferingStart = std::chrono::steady_clock::now();
    const auto maxBuffering = std::chrono::milliseconds(AudioStreamConfig::MAX_INITIAL_BUFFERING_MS);
    while (running_.load() && !txBuffer_.hasReachedTargetLevel()) {
        if (std::chrono::steady_clock::now() - bufferingStart >= maxBuffering) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    while (running_.load()) {
        checkIdleTimeout();
        const std::int32_t bytesRead = txBuffer_.read(buffer.data(), 0,
                                                       static_cast<std::size_t>(bufferBytes),
                                                       static_cast<std::int64_t>(config_.frameDurationMs) * 2);
        if (bytesRead > 0) {
            playbackStream_->write(buffer.data(), bytesRead / frameSize, kBlockForever);
        } else if (bytesRead == 0 && txBuffer_.available() == 0) {
            // Buffer empty — play silence to avoid audio glitches.
            playbackStream_->write(silence.data(), bufferBytes / frameSize, kBlockForever);
        }
    }
}

void AudioMixer::idleLoop() {
    std::unique_lock<std::mutex> lock(idleMutex_);
    while (!idleShutdown_) {
        idleCv_.wait_for(lock, std::chrono::milliseconds(500), [this]() { return idleShutdown_; });
        if (idleShutdown_) break;
        lock.unlock();
        checkIdleTimeout();
        lock.lock();
    }
}

void AudioMixer::claimTxChannelLocked(const std::string& clientId, TxPriority priority,
                                      Notifications& n) {
    txOwnership_ = TxOwnership{clientId, priority};
    lastTxActivityTime_ = now();
    txBuffer_.clear();

    auto client = clientFor(clientId);
    n.push_back([this, client, clientId]() {
        if (client) client->onTxGranted();
        notifyTxOwnerChanged(clientId);
    });
}

void AudioMixer::preemptCurrentOwnerLocked(const std::string& newClientId, TxPriority newPriority,
                                           Notifications& n) {
    const std::string previousOwner = txOwnership_.has_value() ? txOwnership_->clientId : "";
    auto prevClient = !previousOwner.empty() ? clientFor(previousOwner) : nullptr;

    txBuffer_.clear();
    txOwnership_ = TxOwnership{newClientId, newPriority};
    lastTxActivityTime_ = now();

    auto newClient = clientFor(newClientId);
    n.push_back([this, prevClient, newClient, newClientId]() {
        if (prevClient) prevClient->onPreempted(newClientId);
        if (newClient) newClient->onTxGranted();
        notifyTxOwnerChanged(newClientId);
    });
}

void AudioMixer::releaseTxChannelLocked(const std::string& clientId, Notifications& n) {
    if (!txOwnership_.has_value() || txOwnership_->clientId != clientId) return;

    txOwnership_.reset();
    txBuffer_.clear();

    auto client = clientFor(clientId);
    n.push_back([this, client]() {
        if (client) client->onTxReleased();
        notifyTxOwnerChanged("");
    });
}

void AudioMixer::runNotifications(const Notifications& n) {
    for (const auto& notification : n) {
        try {
            notification();
        } catch (...) {
            // Isolate callback errors.
        }
    }
}

void AudioMixer::notifyTxConflict(const std::string& holding, const std::string& requesting) const {
    if (listener_.onTxConflict) {
        try {
            listener_.onTxConflict(holding, requesting);
        } catch (...) {
        }
    }
}

void AudioMixer::notifyTxOwnerChanged(const std::string& newOwner) const {
    if (listener_.onTxOwnerChanged) {
        try {
            listener_.onTxOwnerChanged(newOwner);
        } catch (...) {
        }
    }
}

}  // namespace naudio::net
