// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — AudioBroadcaster implementation.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/net/AudioBroadcaster.hpp"

#include <algorithm>
#include <utility>

#include "naudio/AudioPacket.hpp"

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

    // #59: CaptureStream::read throws DeviceUnavailable on any mid-stream PortAudio error. This
    // loop runs on captureThread_, a plain std::thread, so an escaping exception is
    // std::terminate — the SERVER process dies, taking every connected client with it, and the
    // C ABI's on_error never fires. Catch it, report the source loss, and stop capturing; the
    // server and its sessions stay up, so clients see silence rather than a vanished peer.
    try {
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
                std::vector<std::uint8_t> chunk(
                    buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytesRead));
                std::vector<std::uint8_t> transformed = xform(chunk);
                broadcastToTargets(transformed.data(), 0, transformed.size());
            } else {
                broadcastToTargets(buffer.data(), 0, bytesRead);
            }
        }
    } catch (const DeviceUnavailable& e) {
        // Do NOT clear running_ here. It is the LIFECYCLE flag that owns the join contract:
        // stop() is `if (!running_.exchange(false)) return;` BEFORE captureThread_.join(), so
        // clearing it from inside the loop makes stop() early-return without joining, and the
        // destructor then destroys a joinable thread — std::terminate, i.e. exactly the defect
        // this catch exists to prevent, re-entered through a different door. Returning is what
        // ends the thread; stop() still joins it.
        if (captureErrorListener_) captureErrorListener_(e.what());
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

    // #20: FRAME TO THE WIRE LIMIT — do not hand a target more than one packet can carry.
    //
    // AudioPacket::serialize() clamps its payload to MAX_PAYLOAD (AudioPacket.cpp:73) because the
    // length field is a u16 and deserialize() rejects anything longer — the encoder must never
    // emit a frame the decoder would reject. That guard is right, but nothing above it split an
    // oversized buffer, so a single oversized fan-out became ONE clamped packet and the remainder
    // was dropped while every layer reported success. MEASURED before this change:
    // na_server_inject_audio(srv, buf, 70000) returned NA_OK and delivered 16384 bytes; 16385
    // delivered 16384. No error, no counter, no log.
    //
    // This is NOT app-layer fragmentation and NOT a 0xAF01 wire change: each chunk is a complete,
    // independently-sequenced audio packet, exactly like the frames the capture path already
    // emits. The receiver reassembles nothing — it plays them in sequence order, as it always has.
    //
    // The capture path is bounded by config_.bytesPerFrame() and so almost never chunks; the
    // inject path carries whatever the host handed the C ABI. Both funnel through here, so the
    // invariant "no fan-out frame exceeds what serialize() will emit" holds for every producer.
    const std::size_t chunk = maxFrameBytes();

    std::vector<std::string> failed;
    for (const auto& [id, target] : snapshot) {
        // do/while, not while: a zero-length call must still reach the target exactly once, which
        // is the pre-#20 behaviour for an empty buffer. A buffer at or under the limit takes one
        // iteration and is byte-identical to before.
        std::size_t sent = 0;
        bool ok = true;
        do {
            const std::size_t n = std::min(length - sent, chunk);
            ok = target->receiveRxAudio(data, offset + sent, n);
            sent += n;
        } while (ok && sent < length);
        if (!ok) {
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

std::size_t AudioBroadcaster::maxFrameBytes() const {
    // Round the wire limit DOWN to a whole sample frame. At 16-bit stereo the sample frame is
    // 4 bytes and MAX_PAYLOAD is already an exact multiple, so this is a no-op on every format
    // the C ABI admits (it pins bits to 16 and channels to 1 or 2). AudioStreamConfig is a public
    // C++ struct with no such guard, though, and at 3 channels the sample frame is 6 bytes —
    // 16384 is not a multiple of 6, so an unrounded chunk would split a sample and decode every
    // sample after it one channel out of phase. Alignment makes the property true rather than
    // true by coincidence.
    const std::int64_t sampleFrame =
        static_cast<std::int64_t>(config_.bitsPerSample / 8) * config_.channels;
    if (sampleFrame <= 0 ||
        static_cast<std::size_t>(sampleFrame) > AudioPacket::MAX_PAYLOAD) {
        return AudioPacket::MAX_PAYLOAD;  // degenerate config: the raw wire limit still bounds it
    }
    const std::size_t sf = static_cast<std::size_t>(sampleFrame);
    return (AudioPacket::MAX_PAYLOAD / sf) * sf;
}

void AudioBroadcaster::notifyTargetFailed(const std::string& targetId, const std::string& reason) {
    if (listener_) {
        listener_(targetId, reason);
    }
}

}  // namespace naudio::net
