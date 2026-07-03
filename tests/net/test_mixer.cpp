// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — AudioMixer (priority-based TX arbitration).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// AudioMixer: priority-based TX arbitration. Deterministic where possible: an
// injected clock drives idle-timeout (§3.9), so grant/preempt/deny/release need no sleeps.
// One bounded-wait test proves the independent idle thread fires on an RX-only mixer.

#include "naudio/net/AudioMixer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "naudio/AudioStreamConfig.hpp"

using namespace naudio;
using namespace naudio::net;

namespace {

class TestTxClient : public AudioMixer::TxClient {
public:
    TestTxClient(std::string id, AudioMixer::TxPriority prio) : id_(std::move(id)), prio_(prio) {}

    std::string clientId() const override { return id_; }
    AudioMixer::TxPriority txPriority() const override { return prio_; }
    void onPreempted(const std::string& by) override {
        std::lock_guard<std::mutex> l(m_);
        ++preempted_;
        preemptedBy_ = by;
    }
    void onTxGranted() override {
        std::lock_guard<std::mutex> l(m_);
        ++granted_;
    }
    void onTxReleased() override {
        std::lock_guard<std::mutex> l(m_);
        ++released_;
    }

    int granted() { std::lock_guard<std::mutex> l(m_); return granted_; }
    int released() { std::lock_guard<std::mutex> l(m_); return released_; }
    int preempted() { std::lock_guard<std::mutex> l(m_); return preempted_; }
    std::string preemptedBy() { std::lock_guard<std::mutex> l(m_); return preemptedBy_; }

private:
    std::string id_;
    AudioMixer::TxPriority prio_;
    std::mutex m_;
    int granted_ = 0, released_ = 0, preempted_ = 0;
    std::string preemptedBy_;
};

const std::vector<std::uint8_t> kFrame(64, 0x11);

}  // namespace

TEST(Mixer, CanPreemptByLevel) {
    using P = AudioMixer::TxPriority;
    EXPECT_TRUE(AudioMixer::canPreempt(P::High, P::Normal));
    EXPECT_TRUE(AudioMixer::canPreempt(P::Exclusive, P::High));
    EXPECT_FALSE(AudioMixer::canPreempt(P::Normal, P::Normal));
    EXPECT_FALSE(AudioMixer::canPreempt(P::Low, P::Normal));
}

TEST(Mixer, FirstClientClaimsChannel) {
    AudioMixer mixer{AudioStreamConfig{}};
    auto a = std::make_shared<TestTxClient>("a", AudioMixer::TxPriority::Normal);
    mixer.registerClient(a);

    EXPECT_EQ(mixer.submitTxAudio("a", kFrame), AudioMixer::TxResult::Accepted);
    EXPECT_EQ(mixer.currentTxOwner(), "a");
    EXPECT_TRUE(mixer.isTxOwner("a"));
    EXPECT_EQ(a->granted(), 1);

    // Owner re-submit is accepted (refresh) without a second grant.
    EXPECT_EQ(mixer.submitTxAudio("a", kFrame), AudioMixer::TxResult::Accepted);
    EXPECT_EQ(a->granted(), 1);
}

TEST(Mixer, UnknownClientRejected) {
    AudioMixer mixer{AudioStreamConfig{}};
    EXPECT_EQ(mixer.submitTxAudio("ghost", kFrame), AudioMixer::TxResult::Rejected);
    EXPECT_EQ(mixer.currentTxOwner(), "");
}

TEST(Mixer, EqualPriorityCannotPreemptIsDenied) {
    AudioMixer mixer{AudioStreamConfig{}};
    std::atomic<int> conflicts{0};
    AudioMixer::MixerListener ml;
    ml.onTxConflict = [&](const std::string&, const std::string&) { conflicts++; };
    mixer.setMixerListener(ml);

    auto a = std::make_shared<TestTxClient>("a", AudioMixer::TxPriority::Normal);
    auto b = std::make_shared<TestTxClient>("b", AudioMixer::TxPriority::Normal);
    mixer.registerClient(a);
    mixer.registerClient(b);

    EXPECT_EQ(mixer.submitTxAudio("a", kFrame), AudioMixer::TxResult::Accepted);
    EXPECT_EQ(mixer.submitTxAudio("b", kFrame), AudioMixer::TxResult::Rejected);
    EXPECT_EQ(mixer.currentTxOwner(), "a");  // a keeps it
    EXPECT_EQ(conflicts.load(), 1);
    EXPECT_EQ(b->granted(), 0);
}

TEST(Mixer, HigherPriorityPreempts) {
    AudioMixer mixer{AudioStreamConfig{}};
    auto a = std::make_shared<TestTxClient>("a", AudioMixer::TxPriority::Normal);
    auto b = std::make_shared<TestTxClient>("b", AudioMixer::TxPriority::High);
    mixer.registerClient(a);
    mixer.registerClient(b);

    EXPECT_EQ(mixer.submitTxAudio("a", kFrame), AudioMixer::TxResult::Accepted);
    EXPECT_EQ(a->granted(), 1);

    // b (HIGH) preempts a (NORMAL): b is accepted, a is preempted, ownership moves to b.
    EXPECT_EQ(mixer.submitTxAudio("b", kFrame), AudioMixer::TxResult::Accepted);
    EXPECT_EQ(mixer.currentTxOwner(), "b");
    EXPECT_EQ(a->preempted(), 1);
    EXPECT_EQ(a->preemptedBy(), "b");
    EXPECT_EQ(b->granted(), 1);
}

TEST(Mixer, IdleTimeoutReleasesViaInjectedClock) {
    AudioMixer mixer{AudioStreamConfig{}};  // txIdleTimeoutMs default 500
    std::atomic<std::int64_t> fakeNow{1000};
    mixer.setClock([&]() { return fakeNow.load(); });

    auto a = std::make_shared<TestTxClient>("a", AudioMixer::TxPriority::Normal);
    mixer.registerClient(a);
    EXPECT_EQ(mixer.submitTxAudio("a", kFrame), AudioMixer::TxResult::Accepted);

    // Not yet idle.
    fakeNow.store(1000 + 499);
    mixer.checkIdleTimeout();
    EXPECT_EQ(mixer.currentTxOwner(), "a");

    // Past the idle timeout — released, owner cleared, client notified.
    fakeNow.store(1000 + 500);
    mixer.checkIdleTimeout();
    EXPECT_EQ(mixer.currentTxOwner(), "");
    EXPECT_EQ(a->released(), 1);
}

TEST(Mixer, ExplicitReleaseAndUnregisterClearOwnership) {
    AudioMixer mixer{AudioStreamConfig{}};
    auto a = std::make_shared<TestTxClient>("a", AudioMixer::TxPriority::Normal);
    auto b = std::make_shared<TestTxClient>("b", AudioMixer::TxPriority::Normal);
    mixer.registerClient(a);
    mixer.registerClient(b);

    EXPECT_EQ(mixer.submitTxAudio("a", kFrame), AudioMixer::TxResult::Accepted);
    mixer.releaseTx("a");
    EXPECT_EQ(mixer.currentTxOwner(), "");
    EXPECT_EQ(a->released(), 1);

    // b claims, then unregisters while owning — ownership clears (the disconnecting client
    // is not notified: it is already gone from the registry).
    EXPECT_EQ(mixer.submitTxAudio("b", kFrame), AudioMixer::TxResult::Accepted);
    mixer.unregisterClient("b");
    EXPECT_EQ(mixer.currentTxOwner(), "");
    EXPECT_EQ(b->released(), 0);
}

// The independent idle thread releases on an RX-only mixer (no playback device started).
// Bounded wait (the 500 ms idle poll + the small idle timeout).
TEST(Mixer, IndependentIdleThreadReleasesRxOnlyMixer) {
    AudioStreamConfig config{};
    config.txIdleTimeoutMs = 50;
    AudioMixer mixer{config};  // never start() — no playback device (RX-only)

    auto a = std::make_shared<TestTxClient>("a", AudioMixer::TxPriority::Normal);
    mixer.registerClient(a);
    EXPECT_EQ(mixer.submitTxAudio("a", kFrame), AudioMixer::TxResult::Accepted);

    // The mixer clears ownership under its lock and notifies the client after
    // dropping it — poll for BOTH, or a busy scheduler can observe the owner
    // gone while onTxReleased() is still in flight (seen on CI runners).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while ((mixer.isTxOwner("a") || a->released() < 1) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(mixer.currentTxOwner(), "");
    EXPECT_GE(a->released(), 1);
}
