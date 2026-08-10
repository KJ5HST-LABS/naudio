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
#include "naudio/FakeBackend.hpp"

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

    EXPECT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Live), AudioMixer::TxResult::Accepted);
    EXPECT_EQ(mixer.currentTxOwner(), "a");
    EXPECT_TRUE(mixer.isTxOwner("a"));
    EXPECT_EQ(a->granted(), 1);

    // Owner re-submit is accepted (refresh) without a second grant.
    EXPECT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Live), AudioMixer::TxResult::Accepted);
    EXPECT_EQ(a->granted(), 1);
}

TEST(Mixer, UnknownClientRejected) {
    AudioMixer mixer{AudioStreamConfig{}};
    EXPECT_EQ(mixer.submitTxAudio("ghost", kFrame, Provenance::Live), AudioMixer::TxResult::Rejected);
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

    EXPECT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Live), AudioMixer::TxResult::Accepted);
    EXPECT_EQ(mixer.submitTxAudio("b", kFrame, Provenance::Live), AudioMixer::TxResult::Rejected);
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

    EXPECT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Live), AudioMixer::TxResult::Accepted);
    EXPECT_EQ(a->granted(), 1);

    // b (HIGH) preempts a (NORMAL): b is accepted, a is preempted, ownership moves to b.
    EXPECT_EQ(mixer.submitTxAudio("b", kFrame, Provenance::Live), AudioMixer::TxResult::Accepted);
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
    EXPECT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Live), AudioMixer::TxResult::Accepted);

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

    EXPECT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Live), AudioMixer::TxResult::Accepted);
    mixer.releaseTx("a");
    EXPECT_EQ(mixer.currentTxOwner(), "");
    EXPECT_EQ(a->released(), 1);

    // b claims, then unregisters while owning — ownership clears (the disconnecting client
    // is not notified: it is already gone from the registry).
    EXPECT_EQ(mixer.submitTxAudio("b", kFrame, Provenance::Live), AudioMixer::TxResult::Accepted);
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
    EXPECT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Live), AudioMixer::TxResult::Accepted);

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

// ---- The recovered-frame contract (issue #65 phase 3) ----
//
// submitTxAudio IS the claim mechanism — there is no separate "I want to transmit"
// call — so before provenance existed, any frame worth playing was by the same test a
// frame that took the channel. A frame that never ARRIVED — the peer sent it, it was
// lost, and the parity layer rebuilt it — could therefore
// claim TX on that peer's behalf after its idle release, which on na_hamlib_bridge
// opens a real rig's audio path and, with -k, keys its PTT.
//
// One arm per row of the recovered column. The live column is unchanged and is covered
// by the arms above, several of which would also redden if a row leaked into it.

// Row 1: unowned. The channel was released and txBuffer_ cleared, so the radio is
// silent — a repair here is a click, not a repair, and claiming for an absent peer is
// the defect itself.
// The unknown-client early return precedes ALL arbitration, so it needs its own row.
// A LIVE frame from a ghost is Rejected (covered above); a recovered one must not be, or
// phase 4 would drive txDeniedCount_ and fabricate a TX_DENIED for a peer that never asked
// — row 4's defect reached by a different path.
TEST(Mixer, ARecoveredFrameFromAnUnknownClientIsDeclinedNotRejected) {
    AudioMixer mixer{AudioStreamConfig{}};
    const std::size_t before = mixer.txBuffer().available();

    EXPECT_EQ(mixer.submitTxAudio("ghost", kFrame, Provenance::Live),
              AudioMixer::TxResult::Rejected)
        << "control: a live frame from an unregistered client is still Rejected";
    EXPECT_EQ(mixer.submitTxAudio("ghost", kFrame, Provenance::Recovered),
              AudioMixer::TxResult::DeclinedRecovered)
        << "a repair from an unregistered client returned Rejected, which phase 4 turns "
           "into a fabricated TX_DENIED";
    EXPECT_EQ(mixer.currentTxOwner(), "");
    EXPECT_EQ(mixer.txBuffer().available(), before);
}

TEST(Mixer, ARecoveredFrameDoesNotClaimAnUnownedChannel) {
    AudioMixer mixer{AudioStreamConfig{}};
    auto a = std::make_shared<TestTxClient>("a", AudioMixer::TxPriority::Normal);
    mixer.registerClient(a);
    ASSERT_EQ(mixer.currentTxOwner(), "") << "premise: nobody owns the channel";
    const std::size_t before = mixer.txBuffer().available();

    EXPECT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Recovered),
              AudioMixer::TxResult::DeclinedRecovered);
    EXPECT_EQ(mixer.currentTxOwner(), "")
        << "a reconstructed frame claimed the TX channel — this IS issue #65";
    EXPECT_EQ(a->granted(), 0) << "onTxGranted fired for a frame that was reconstructed, not received";
    EXPECT_EQ(mixer.txBuffer().available(), before)
        << "recovered audio was written to a channel whose buffer had just been cleared";
}

// Row 2: we already own it — the ONLY row where a repair has a consumer, so the only
// row that writes recovered audio. The lease is NOT refreshed (operator decision,
// 2026-08-07): the idle timeout keeps running from the last LIVE frame.
TEST(Mixer, ARecoveredFrameIsAudibleToItsOwnerButDoesNotExtendTheLease) {
    AudioStreamConfig cfg;  // txIdleTimeoutMs default 500
    AudioMixer mixer{cfg};
    std::atomic<std::int64_t> fakeNow{1000};
    mixer.setClock([&]() { return fakeNow.load(); });
    auto a = std::make_shared<TestTxClient>("a", AudioMixer::TxPriority::Normal);
    mixer.registerClient(a);

    ASSERT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Live),
              AudioMixer::TxResult::Accepted);
    ASSERT_EQ(mixer.currentTxOwner(), "a") << "premise: the live frame claimed the channel";

    const std::int64_t idle = cfg.txIdleTimeoutMs;

    // The repair arrives one tick before the lease would expire, and IS written.
    fakeNow.store(1000 + idle - 1);
    const std::size_t before = mixer.txBuffer().available();
    EXPECT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Recovered),
              AudioMixer::TxResult::Accepted);
    EXPECT_GT(mixer.txBuffer().available(), before)
        << "the owner's own repair was dropped — row 2 is the one row that must write it";

    // It did not restart the clock. The lease is measured from the LIVE frame at 1000,
    // so it expires at 1000+idle. Had the repair refreshed it, the deadline would have
    // moved to (1000+idle-1)+idle and the owner would still hold the channel here.
    fakeNow.store(1000 + idle);
    mixer.checkIdleTimeout();
    EXPECT_EQ(mixer.currentTxOwner(), "")
        << "the repair extended the lease; the operator's decision was NO refresh";
    EXPECT_EQ(a->released(), 1);

    // POSITIVE CONTROL, and it is not optional: everything above asserts a NON-EVENT (the
    // deadline did not move), which cannot tell "a repair does not refresh" apart from
    // "nothing refreshes, ever". MEASURED before this control existed: deleting the LIVE
    // refresh outright left all 335 tests green.
    //
    // It must exercise the REFRESH path specifically, which means submitting while ALREADY
    // the owner. A submit made after the release re-CLAIMS instead, and claimTxChannelLocked
    // stamps lastTxActivityTime_ itself — so a control built that way passes with the
    // refresh deleted, which is exactly what the first version of this control did.
    const std::int64_t t0 = 1000 + idle;  // re-claim; the claim stamps the lease at t0
    ASSERT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Live),
              AudioMixer::TxResult::Accepted);
    ASSERT_EQ(mixer.currentTxOwner(), "a") << "control premise: the live frame re-claimed";

    // Still the owner, one tick short of expiry: THIS submit can only take the refresh path.
    fakeNow.store(t0 + idle - 1);
    ASSERT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Live),
              AudioMixer::TxResult::Accepted);
    ASSERT_EQ(mixer.currentTxOwner(), "a") << "control premise: still the owner, not a re-claim";

    // Without the refresh the lease would expire at t0+idle. With it, the deadline moved to
    // (t0+idle-1)+idle, so the owner must survive here.
    fakeNow.store(t0 + idle);
    mixer.checkIdleTimeout();
    EXPECT_EQ(mixer.currentTxOwner(), "a")
        << "a LIVE frame failed to refresh the lease — the refresh path is dead, so the "
           "no-refresh assertion above was measuring nothing";
}

// Row 3: someone else holds it and we outrank them. UNREACHABLE in the shipping server
// — every ClientSession is hard-wired Normal and canPreempt is strict-greater, pinned
// normatively at docs/audio-streaming-protocol-v1.md:326 (strict-greater itself is
// stated at :321) — but reachable here because
// the mixer takes priority from the TxClient. Specified so the proposed §13.3
// client-settable-priority feature cannot inherit the bug.
TEST(Mixer, ARecoveredFrameDoesNotPreemptEvenFromAHigherPriorityClient) {
    AudioMixer mixer{AudioStreamConfig{}};
    auto a = std::make_shared<TestTxClient>("a", AudioMixer::TxPriority::Normal);
    auto b = std::make_shared<TestTxClient>("b", AudioMixer::TxPriority::High);
    mixer.registerClient(a);
    mixer.registerClient(b);
    ASSERT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Live),
              AudioMixer::TxResult::Accepted);
    ASSERT_EQ(mixer.currentTxOwner(), "a");
    ASSERT_TRUE(AudioMixer::canPreempt(AudioMixer::TxPriority::High,
                                       AudioMixer::TxPriority::Normal))
        << "premise: b outranks a, so a LIVE frame from b would preempt";
    const std::size_t before = mixer.txBuffer().available();

    EXPECT_EQ(mixer.submitTxAudio("b", kFrame, Provenance::Recovered),
              AudioMixer::TxResult::DeclinedRecovered);
    EXPECT_EQ(mixer.currentTxOwner(), "a")
        << "a reconstructed frame preempted a live talker mid-transmission";
    EXPECT_EQ(a->preempted(), 0);
    EXPECT_EQ(b->granted(), 0);
    EXPECT_EQ(mixer.txBuffer().available(), before);
}

// Row 4: someone else holds it and we cannot outrank them. Declining must be SILENT.
// TX_DENIED is sent once per denial episode (docs/audio-streaming-protocol-v1.md:323),
// so a denial fabricated by a repair does not add noise — it SPENDS the client's single
// message and converts its next genuine denial into silence.
TEST(Mixer, ARecoveredFrameIsDeclinedWithoutSpendingTheClientsOneTxDenied) {
    AudioMixer mixer{AudioStreamConfig{}};
    std::atomic<int> conflicts{0};
    AudioMixer::MixerListener listener;
    listener.onTxConflict = [&conflicts](const std::string&, const std::string&) {
        ++conflicts;
    };
    mixer.setMixerListener(listener);

    auto a = std::make_shared<TestTxClient>("a", AudioMixer::TxPriority::Normal);
    auto b = std::make_shared<TestTxClient>("b", AudioMixer::TxPriority::Normal);
    mixer.registerClient(a);
    mixer.registerClient(b);
    ASSERT_EQ(mixer.submitTxAudio("a", kFrame, Provenance::Live),
              AudioMixer::TxResult::Accepted);
    ASSERT_EQ(mixer.currentTxOwner(), "a");

    // POSITIVE CONTROL (L63): a GENUINE denial must still notify. Without this the arm
    // below passes just as well against a mixer that never notifies at all.
    const std::size_t beforeLiveDenial = mixer.txBuffer().available();
    EXPECT_EQ(mixer.submitTxAudio("b", kFrame, Provenance::Live),
              AudioMixer::TxResult::Rejected);
    ASSERT_EQ(conflicts.load(), 1) << "control: a live denial must still reach the client";
    EXPECT_EQ(mixer.txBuffer().available(), beforeLiveDenial)
        << "a REJECTED live frame was mixed into the owner's transmitted audio";

    // The subject: a repair from the same denied client must not spend a second one...
    const std::size_t beforeRepair = mixer.txBuffer().available();
    EXPECT_EQ(mixer.submitTxAudio("b", kFrame, Provenance::Recovered),
              AudioMixer::TxResult::DeclinedRecovered);
    EXPECT_EQ(conflicts.load(), 1)
        << "a reconstructed frame fabricated a denial, spending the client's one "
           "per-episode TX_DENIED and silencing its next genuine one";

    // ...and must not be AUDIBLE either. This assertion is the one the first version of
    // this arm was missing: MEASURED, flipping row 4's writeAudio to true left the entire
    // suite green, so a denied non-owner's reconstructed audio could be mixed into the
    // live talker's transmission — on air, on the bridge's default profile — undetected.
    // The two `writeAudio = false;` lines in that branch are byte-identical and adjacent,
    // which is exactly the shape that invites a collapsing edit.
    EXPECT_EQ(mixer.txBuffer().available(), beforeRepair)
        << "a DECLINED recovered frame from a non-owner was written into the live "
           "talker's outgoing audio";
}

// #59: the shared TX-to-rig PLAYBACK device dying mid-stream must not take the server process
// with it. playbackThread_ is a plain std::thread, so before the fix this was std::terminate —
// on the bridge that is the server dying mid-transmission, with PTT keyed.
TEST(Mixer, PlaybackDeviceLostMidStreamIsReportedNotFatal) {
    AudioStreamConfig config{};
    AudioMixer mixer{config};

    std::mutex m;
    std::string reason;
    AudioMixer::MixerListener ml;
    ml.onPlaybackDeviceError = [&](const std::string& r) {
        std::lock_guard<std::mutex> l(m);
        reason = r;
    };
    mixer.setMixerListener(ml);

    AudioFormat fmt;
    FakePlaybackStream stream{fmt};
    stream.throwAfterWrites = 2;  // the loop writes silence when idle, so this fires promptly
    mixer.start(&stream);

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
            << "playback error not reported; got: [" << reason << "]";
    }
    mixer.stop();
}
