// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — AudioBroadcaster (RX fan-out).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// AudioBroadcaster: RX fan-out from one capture source to N targets.
// Hardware-free: a synthetic pattern CaptureStream drives the capture thread; injectAudio
// drives deterministic byte-identity fan-out. Proves snapshot-then-callback, failed-target
// removal, and the audio transform.

#include "naudio/net/AudioBroadcaster.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "naudio/AudioStreamConfig.hpp"
#include "naudio/Stream.hpp"
#include "naudio/Types.hpp"

using namespace naudio;
using namespace naudio::net;

namespace {

// Records every byte it receives, in order. Optionally refuses (returns false) to exercise
// the failed-target removal path.
class RecordingTarget : public AudioBroadcaster::BroadcastTarget {
public:
    explicit RecordingTarget(std::string id, bool accept = true)
        : id_(std::move(id)), accept_(accept) {}

    bool receiveRxAudio(const std::uint8_t* data, std::size_t offset,
                        std::size_t length) override {
        if (!accept_) return false;
        std::lock_guard<std::mutex> lock(m_);
        received_.insert(received_.end(), data + offset, data + offset + length);
        ++calls_;
        return true;
    }
    std::string targetId() const override { return id_; }

    std::vector<std::uint8_t> bytes() {
        std::lock_guard<std::mutex> lock(m_);
        return received_;
    }
    int calls() {
        std::lock_guard<std::mutex> lock(m_);
        return calls_;
    }

private:
    std::string id_;
    bool accept_;
    std::mutex m_;
    std::vector<std::uint8_t> received_;
    int calls_ = 0;
};

// A capture stream that fills each read with a rolling byte counter (continuous across
// reads) and paces itself so the capture loop does not spin at 100% CPU. The continuous
// counter lets a test assert the fanned-out bytes are contiguous and in order.
class PatternCaptureStream : public CaptureStream {
public:
    explicit PatternCaptureStream(AudioFormat fmt) : fmt_(fmt) {}

    IoResult read(void* buffer, int frames, int /*timeoutMs*/) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));  // pace ~real-time-ish
        auto* out = static_cast<std::uint8_t*>(buffer);
        const std::size_t bytes = static_cast<std::size_t>(frames) * fmt_.frameSize();
        for (std::size_t i = 0; i < bytes; ++i) out[i] = counter_++;
        IoResult r;
        r.frames = frames;
        return r;
    }
    const AudioFormat& actualFormat() const override { return fmt_; }

private:
    AudioFormat fmt_;
    std::uint8_t counter_ = 0;
};

}  // namespace

TEST(Broadcaster, AddRemoveTargets) {
    AudioBroadcaster b{AudioStreamConfig{}};
    EXPECT_FALSE(b.hasTargets());
    EXPECT_EQ(b.targetCount(), 0u);

    auto t1 = std::make_shared<RecordingTarget>("a");
    auto t2 = std::make_shared<RecordingTarget>("b");
    b.addTarget(t1);
    b.addTarget(t2);
    EXPECT_EQ(b.targetCount(), 2u);
    EXPECT_TRUE(b.hasTargets());

    auto removed = b.removeTarget("a");
    EXPECT_EQ(removed, t1);
    EXPECT_EQ(b.targetCount(), 1u);
    EXPECT_EQ(b.removeTarget("nope"), nullptr);
    b.addTarget(nullptr);  // null-safe
    EXPECT_EQ(b.targetCount(), 1u);
}

// The core fan-out property: one injected payload reaches every target byte-identically.
TEST(Broadcaster, InjectFansOutByteIdenticalToAllTargets) {
    AudioBroadcaster b{AudioStreamConfig{}};
    auto t1 = std::make_shared<RecordingTarget>("a");
    auto t2 = std::make_shared<RecordingTarget>("b");
    auto t3 = std::make_shared<RecordingTarget>("c");
    b.addTarget(t1);
    b.addTarget(t2);
    b.addTarget(t3);

    std::vector<std::uint8_t> p1 = {0xDE, 0xAD, 0xBE, 0xEF};
    std::vector<std::uint8_t> p2 = {0x01, 0x02, 0x03};
    b.injectAudio(p1);
    b.injectAudio(p2);

    std::vector<std::uint8_t> expected = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
    EXPECT_EQ(t1->bytes(), expected);
    EXPECT_EQ(t2->bytes(), expected);
    EXPECT_EQ(t3->bytes(), expected);
    EXPECT_EQ(t1->bytes(), t2->bytes());
    EXPECT_EQ(t2->bytes(), t3->bytes());
}

TEST(Broadcaster, InjectWithNoTargetsIsNoOp) {
    AudioBroadcaster b{AudioStreamConfig{}};
    std::vector<std::uint8_t> p = {1, 2, 3};
    b.injectAudio(p);  // no targets — must not crash
    EXPECT_FALSE(b.hasTargets());
}

// A target that returns false is removed after the iteration and the listener is notified.
TEST(Broadcaster, FailedTargetIsRemovedAndListenerNotified) {
    AudioBroadcaster b{AudioStreamConfig{}};
    std::string failedId;
    b.setBroadcastListener([&](const std::string& id, const std::string&) { failedId = id; });

    auto good = std::make_shared<RecordingTarget>("good");
    auto bad = std::make_shared<RecordingTarget>("bad", /*accept=*/false);
    b.addTarget(good);
    b.addTarget(bad);

    std::vector<std::uint8_t> p = {9, 9, 9};
    b.injectAudio(p);

    EXPECT_EQ(b.targetCount(), 1u);          // bad removed
    EXPECT_EQ(failedId, "bad");
    EXPECT_EQ(good->bytes(), p);             // good still received the bytes
}

TEST(Broadcaster, AudioTransformIsAppliedOnInject) {
    AudioBroadcaster b{AudioStreamConfig{}};
    // Transform: increment every byte (channel-routing stand-in).
    b.setAudioTransform([](const std::vector<std::uint8_t>& in) {
        std::vector<std::uint8_t> out = in;
        for (auto& x : out) x = static_cast<std::uint8_t>(x + 1);
        return out;
    });
    auto t = std::make_shared<RecordingTarget>("a");
    b.addTarget(t);

    std::vector<std::uint8_t> p = {10, 20, 30};
    b.injectAudio(p);
    std::vector<std::uint8_t> expected = {11, 21, 31};
    EXPECT_EQ(t->bytes(), expected);
}

// The capture thread reads a CaptureStream and fans the bytes out faithfully and in order.
TEST(Broadcaster, CaptureThreadFansOutContiguousBytes) {
    AudioStreamConfig config{};  // 48k/16-bit/stereo
    AudioBroadcaster b{config};

    auto target = std::make_shared<RecordingTarget>("cap");
    b.addTarget(target);  // register BEFORE start so no chunk is dropped

    AudioFormat fmt;  // default 48k/16/stereo, frameSize 4
    PatternCaptureStream stream{fmt};
    b.start(&stream);
    EXPECT_TRUE(b.isRunning());

    // Poll (bounded) until more than one frame has fanned out instead of a fixed
    // sleep: the assertion needs the just-spawned capture thread to complete two
    // 2ms-paced reads, which a loaded CI runner can't guarantee inside any fixed
    // small budget. Same assertion strength, no schedule dependence.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (target->bytes().size() <= static_cast<std::size_t>(config.bytesPerFrame()) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    b.stop();
    EXPECT_FALSE(b.isRunning());

    auto got = target->bytes();
    ASSERT_GT(got.size(), static_cast<std::size_t>(config.bytesPerFrame()));  // >= two reads
    // Continuous rolling counter, in order, across all fanned-out chunks.
    for (std::size_t k = 0; k < got.size(); ++k) {
        ASSERT_EQ(got[k], static_cast<std::uint8_t>(k)) << "mismatch at byte " << k;
    }
}
