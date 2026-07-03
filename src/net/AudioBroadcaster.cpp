// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — AudioBroadcaster implementation.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/net/AudioBroadcaster.hpp"

#include <utility>

namespace naudio::net {

void AudioBroadcaster::addTarget(std::shared_ptr<BroadcastTarget> target) {
    if (!target) return;
    std::lock_guard<std::mutex> lock(targetsMutex_);
    targets_[target->targetId()] = std::move(target);
}

std::shared_ptr<AudioBroadcaster::BroadcastTarget> AudioBroadcaster::removeTarget(
    const std::string& targetId) {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    auto it = targets_.find(targetId);
    if (it == targets_.end()) return nullptr;
    auto removed = it->second;
    targets_.erase(it);
    return removed;
}

std::size_t AudioBroadcaster::targetCount() const {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    return targets_.size();
}

bool AudioBroadcaster::hasTargets() const {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    return !targets_.empty();
}

void AudioBroadcaster::start(CaptureStream* captureStream) {
    if (running_.exchange(true)) return;  // already running
    captureStream_ = captureStream;
    captureThread_ = std::thread(&AudioBroadcaster::captureLoop, this);
}

void AudioBroadcaster::stop() {
    if (!running_.exchange(false)) return;  // not running
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    captureStream_ = nullptr;
}

void AudioBroadcaster::injectAudio(const std::vector<std::uint8_t>& data) {
    if (data.empty()) return;
    {
        std::lock_guard<std::mutex> lock(targetsMutex_);
        if (targets_.empty()) return;
    }
    AudioTransform xform;
    {
        std::lock_guard<std::mutex> lock(transformMutex_);
        xform = audioTransform_;
    }
    if (xform) {
        std::vector<std::uint8_t> transformed = xform(data);
        broadcastToTargets(transformed.data(), 0, transformed.size());
    } else {
        broadcastToTargets(data.data(), 0, data.size());
    }
}

void AudioBroadcaster::injectAudio(const std::uint8_t* data, std::size_t offset,
                                   std::size_t length) {
    if (data == nullptr || length == 0) return;
    {
        std::lock_guard<std::mutex> lock(targetsMutex_);
        if (targets_.empty()) return;
    }
    broadcastToTargets(data, offset, length);
}

void AudioBroadcaster::captureLoop() {
    // Read one frame-group at a time (a config_.bytesPerFrame()-sized buffer). Sized to
    // the format the stream was ACTUALLY opened with (mono fallback yields a smaller frame).
    const int frames = config_.samplesPerFrame();
    const int frameSize = captureStream_ != nullptr ? captureStream_->actualFormat().frameSize()
                                                     : config_.bytesPerFrame() / config_.samplesPerFrame();
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(frames) * frameSize);

    while (running_.load()) {
        IoResult r = captureStream_->read(buffer.data(), frames, kBlockForever);
        const std::size_t bytesRead = static_cast<std::size_t>(r.frames) * frameSize;
        if (bytesRead == 0) continue;

        AudioTransform xform;
        {
            std::lock_guard<std::mutex> lock(transformMutex_);
            xform = audioTransform_;
        }
        if (xform) {
            std::vector<std::uint8_t> chunk(buffer.begin(),
                                            buffer.begin() + static_cast<std::ptrdiff_t>(bytesRead));
            std::vector<std::uint8_t> transformed = xform(chunk);
            broadcastToTargets(transformed.data(), 0, transformed.size());
        } else {
            broadcastToTargets(buffer.data(), 0, bytesRead);
        }
    }
}

void AudioBroadcaster::broadcastToTargets(const std::uint8_t* data, std::size_t offset,
                                          std::size_t length) {
    // Snapshot the targets under the lock, then call them OUTSIDE it (§3.3). Collect any
    // that ask for removal and erase them afterward.
    std::vector<std::pair<std::string, std::shared_ptr<BroadcastTarget>>> snapshot;
    {
        std::lock_guard<std::mutex> lock(targetsMutex_);
        snapshot.reserve(targets_.size());
        for (const auto& [id, target] : targets_) {
            snapshot.emplace_back(id, target);
        }
    }

    std::vector<std::string> failed;
    for (const auto& [id, target] : snapshot) {
        if (!target->receiveRxAudio(data, offset, length)) {
            failed.push_back(id);
        }
    }

    for (const auto& id : failed) {
        {
            std::lock_guard<std::mutex> lock(targetsMutex_);
            targets_.erase(id);
        }
        notifyTargetFailed(id, "Target indicated removal");
    }
}

void AudioBroadcaster::notifyTargetFailed(const std::string& targetId, const std::string& reason) {
    if (listener_) {
        listener_(targetId, reason);
    }
}

}  // namespace naudio::net
