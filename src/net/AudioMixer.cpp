// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — AudioMixer implementation.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/net/AudioMixer.hpp"

#include <algorithm>
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
                                               std::size_t length, Provenance provenance) {
    auto client = clientFor(clientId);
    if (!client) {
        // Unknown client. A LIVE frame from one is Rejected exactly as before. A recovered
        // one must not be: this return precedes all arbitration, so once phase 4 forwards
        // real provenance, Rejected here would drive the server's txDeniedCount_ and
        // fabricate a TX_DENIED on behalf of a peer that never asked — the same defect
        // row 4 exists to prevent, reached by a different path.
        return provenance == Provenance::Recovered ? TxResult::DeclinedRecovered
                                                   : TxResult::Rejected;
    }

    // Callbacks are collected under the lock and invoked after unlock:
    // they do blocking sendControl writes, and running them under txMutex_ let one
    // backpressured client block every submitTxAudio call and the playback loop.
    Notifications notifications;
    TxResult result;
    // The arbitration verdict and the audio write are now decided SEPARATELY (issue #65).
    // They used to be one decision — `if (result == Accepted) txBuffer_.write(...)` — which
    // is precisely what made a repair indistinguishable from a claim: any frame worth
    // playing was, by the same test, a frame that took the channel. Each row below sets
    // both explicitly.
    //
    // They still coincide on every row of the current table (writeAudio is true exactly
    // when result is Accepted). Do NOT collapse them back on that basis: the contract is
    // that a frame may be audible without arbitrating, and the coincidence is a property
    // of today's four rows, not of the design.
    bool writeAudio;
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        const bool recovered = (provenance == Provenance::Recovered);

        if (!txOwnership_.has_value()) {
            // Unowned. The channel was released and txBuffer_ cleared (releaseTxChannelLocked),
            // so the radio is silent: a repair here would be a click, not a repair — and
            // claiming on behalf of a peer that never sent this frame is the #65 defect.
            if (recovered) {
                result = TxResult::DeclinedRecovered;
                writeAudio = false;
            } else {
                claimTxChannelLocked(clientId, client->txPriority(), notifications);
                result = TxResult::Accepted;
                writeAudio = true;
            }
        } else if (txOwnership_->clientId == clientId) {
            // We own it — the only state in which a repair has a consumer, so it is the
            // only row where recovered audio is written. The lease is NOT refreshed for a
            // repair (operator decision, 2026-08-07): the idle timeout keeps running from
            // the last LIVE frame, so a tail carried only by repairs releases early rather
            // than holding the channel open on reconstructed evidence.
            if (!recovered) lastTxActivityTime_ = now();
            result = TxResult::Accepted;
            writeAudio = true;
        } else {
            const TxPriority ourPriority = client->txPriority();
            if (canPreempt(ourPriority, txOwnership_->priority)) {
                // Someone else holds it and we outrank them. Unreachable in the shipping
                // server — every session is hard-wired Normal and canPreempt is strict-
                // greater, and the frozen spec pins that (audio-streaming-protocol-v1.md:496)
                // — but specified so the proposed §13.3 priority feature cannot inherit the bug.
                if (recovered) {
                    result = TxResult::DeclinedRecovered;
                    writeAudio = false;
                } else {
                    preemptCurrentOwnerLocked(clientId, ourPriority, notifications);
                    result = TxResult::Accepted;
                    writeAudio = true;
                }
            } else {
                // Someone else holds it and we cannot outrank them.
                if (recovered) {
                    // Declining SILENTLY, and be precise about what "silently" buys:
                    // in the shipping server TX_DENIED is driven by this RETURN VALUE, not
                    // by the notification — handleTxAudio sends it on TxResult::Rejected,
                    // while the onTxConflict listener is wired to an empty lambda
                    // (AudioStreamServer.cpp). So returning DeclinedRecovered rather than
                    // Rejected is what stops a repair from spending the client's single
                    // per-episode TX_DENIED (spec :493) and silencing its next genuine one.
                    // Skipping the notification matters for any OTHER listener a consumer
                    // installs, which is why both are done.
                    result = TxResult::DeclinedRecovered;
                    writeAudio = false;
                } else {
                    const std::string holder = txOwnership_->clientId;
                    notifications.push_back(
                        [this, holder, clientId]() { notifyTxConflict(holder, clientId); });
                    result = TxResult::Rejected;
                    writeAudio = false;
                }
            }
        }

        if (writeAudio) {
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

    // #59: PlaybackStream::write throws DeviceUnavailable on mid-stream device loss, and this is
    // the SHARED TX-to-rig path — on the bridge it is the transmitter's audio. playbackThread_ is
    // a plain std::thread, so an escaping exception is std::terminate: the whole server dies
    // mid-transmission. Catch, report, stop driving the dead device.
    try {
        while (running_.load()) {
            checkIdleTimeout();
            const std::int32_t bytesRead =
                txBuffer_.read(buffer.data(), 0, static_cast<std::size_t>(bufferBytes),
                               static_cast<std::int64_t>(config_.frameDurationMs) * 2);
            if (bytesRead > 0) {
                playbackStream_->write(buffer.data(), bytesRead / frameSize, kBlockForever);
            } else if (bytesRead == 0 && txBuffer_.available() == 0) {
                // Buffer empty — play silence to avoid audio glitches.
                playbackStream_->write(silence.data(), bufferBytes / frameSize, kBlockForever);
            }
        }
    } catch (const DeviceUnavailable& e) {
        // Do NOT clear running_ — same trap as AudioBroadcaster::captureLoop: stop() early-returns
        // on `!running_.exchange(false)` before playbackThread_.join(), so clearing it here leaves
        // a joinable thread for the destructor to terminate on.
        if (listener_.onPlaybackDeviceError) listener_.onPlaybackDeviceError(e.what());
    }
}

void AudioMixer::idleLoop() {
    // Poll at the CONFIGURED timeout's cadence rather than a fixed 500 ms, capped at 500 ms
    // and floored at 10 ms. The cap keeps every shipped profile byte-for-byte as it was:
    // txIdleTimeoutMs defaults to DEFAULT_TX_IDLE_TIMEOUT_MS (500) and no preset in
    // AudioStreamConfig.hpp overrides it, so min(500, 500) is the same 500 ms tick.
    //
    // It matters for a caller that asks for a SHORTER timeout, where the fixed tick made the
    // setting mean much less than it says: a 100 ms txIdleTimeoutMs released somewhere in
    // [100, 600] ms, up to 6x its configured value, because release latency was
    // timeout + up to one full poll period. The floor stops a pathologically small timeout
    // from turning this into a spin loop.
    const auto tickMs = std::min<std::int64_t>(500, std::max<std::int64_t>(10, config_.txIdleTimeoutMs));
    std::unique_lock<std::mutex> lock(idleMutex_);
    while (!idleShutdown_) {
        idleCv_.wait_for(lock, std::chrono::milliseconds(tickMs), [this]() { return idleShutdown_; });
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
