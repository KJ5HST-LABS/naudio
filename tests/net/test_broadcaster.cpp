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

#include "naudio/AudioPacket.hpp"
#include "naudio/AudioStreamConfig.hpp"
#include "naudio/FakeBackend.hpp"
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
        callLengths_.push_back(length);
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
    // The length of each individual fan-out call. bytes() concatenates them and so cannot see
    // where one frame ended and the next began — which is exactly what #20's framing is about.
    std::vector<std::size_t> callLengths() {
        std::lock_guard<std::mutex> lock(m_);
        return callLengths_;
    }

private:
    std::string id_;
    bool accept_;
    std::mutex m_;
    std::vector<std::uint8_t> received_;
    std::vector<std::size_t> callLengths_;
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

// #20: the fan-out frames an oversized buffer to the wire limit — and never mid-sample.
//
// The end-to-end proof that no bytes are LOST is Server.OversizedInjectIsFramedNotTruncated;
// this arm pins the framing MECHANISM at the choke point, which the end-to-end arm cannot see
// cleanly: where the chunk boundaries fall.
//
// The 3-channel config is the point of the arm, not incidental. At 16-bit stereo a sample frame
// is 4 bytes and MAX_PAYLOAD (16384) is an exact multiple of it, so chunking at MAX_PAYLOAD is
// sample-aligned BY COINCIDENCE and a naive implementation looks correct forever. At 3 channels
// the sample frame is 6 bytes, 16384 is NOT a multiple of 6, and a naive chunk splits a sample
// across two packets — every subsequent sample in that chunk is then decoded one channel out of
// phase. AudioStreamConfig is a public C++ struct with no channel-count guard, so this is
// reachable; the C ABI separately pins channels to 1 or 2 (naudio_c_api.cpp:1178), which is why
// no C-level arm can reach it.
TEST(Broadcaster, OversizedInjectIsFramedToWireLimitOnSampleBoundaries) {
    AudioStreamConfig cfg{};
    cfg.channels = 3;
    cfg.bitsPerSample = 16;
    const std::size_t kSampleFrame = 6;  // (16 / 8) * 3

    AudioBroadcaster b{cfg};
    auto t = std::make_shared<RecordingTarget>("a");
    b.addTarget(t);

    // A multiple of the sample frame, spanning two full chunks plus a partial.
    const std::size_t kInject = kSampleFrame * 5500;  // 33000
    std::vector<std::uint8_t> payload(kInject);
    for (std::size_t i = 0; i < kInject; ++i) {
        payload[i] = static_cast<std::uint8_t>((i * 7 + 13) & 0xFF);
    }
    b.injectAudio(payload);

    EXPECT_EQ(t->bytes(), payload) << "bytes lost or reordered by the framing";
    const std::vector<std::size_t> lengths = t->callLengths();
    ASSERT_GT(lengths.size(), 1u) << "did not frame at all — one oversized call";
    for (std::size_t n : lengths) {
        EXPECT_LE(n, AudioPacket::MAX_PAYLOAD) << "frame exceeds what serialize() will emit";
        EXPECT_EQ(n % kSampleFrame, 0u) << "frame boundary split a sample";
    }
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

// #59: the capture DEVICE dying mid-stream must not take the server process with it.
// captureThread_ is a plain std::thread, so before the fix an escaping DeviceUnavailable was
// std::terminate — this binary would ABORT rather than fail. Reaching the assertions at all is
// therefore part of what the arm proves; the reported reason is the part a mutation can move.
TEST(Broadcaster, CaptureDeviceLostMidStreamIsReportedNotFatal) {
    AudioStreamConfig config{};
    AudioBroadcaster b{config};

    std::mutex m;
    std::string reason;
    b.setCaptureErrorListener([&](const std::string& r) {
        std::lock_guard<std::mutex> l(m);
        reason = r;
    });

    AudioFormat fmt;
    FakeCaptureStream stream{fmt};
    stream.throwAfterReads = 2;  // two good reads, then the device goes away
    b.start(&stream);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        {
            std::lock_guard<std::mutex> l(m);
            if (!reason.empty()) break;
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    {
        std::lock_guard<std::mutex> l(m);
        EXPECT_NE(reason.find("injected mid-stream device loss"), std::string::npos)
            << "capture error not reported; got: [" << reason << "]";
    }
    // stop() must still complete. isRunning() is deliberately NOT asserted false here: it is the
    // lifecycle flag that stop() uses to decide whether to join, so the loop must leave it set
    // (see captureLoop's catch). This stop() completing IS the assertion that the join contract
    // survived the device loss — the first version of this fix cleared running_ and this line
    // aborted the binary.
    b.stop();
    EXPECT_FALSE(b.isRunning());
}
