// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — na_hamlib_bridge's sample-rate negotiation policy (issue #84).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// WHY THESE ARMS EXIST HERE RATHER THAN IN A BRIDGE ARM. #84 says in as many words that no
// existing test can reach the state it describes: every bridge arm and every CI job runs
// against the Hamlib dummy, whose native rate set CONTAINS 48000, so the negotiation is a
// no-op there by construction and a detector "needs a backend whose native rate excludes
// 48 kHz" — which is hardware this project does not have (#12 is still open for exactly
// that reason). Testing the policy as a pure function over the rate lists is what closes
// that gap without inventing hardware: these arms run on all eight CI jobs, Windows and
// both sanitizers included, while na_hamlib_bridge itself is opt-in, POSIX-only and needs
// a hand-built libhamlib prefix.
//
// What that buys is real but bounded, and the boundary is worth stating plainly: this
// proves the CHOICE, not the PLUMBING. That the chosen rate then reaches
// rig_stream_config.sample_rate, na_server_set_audio_format and the health metric is
// checked by reading those call sites and by the bridge arm continuing to pass at 48 kHz —
// not by anything here. Nothing in this file opens a socket or a rig.
//
// The rate lists in the "dummy" arms are not invented: they were measured from the real
// backend through rig_stream_caps_at() against the b538567b prefix — the same call the
// bridge makes — and are quoted here so a future change to the policy has to reckon with
// what the one backend everything is tested against actually publishes.

#include <gtest/gtest.h>

#include "na_stream_rate.h"

namespace {

// Hardware-native rates the Hamlib dummy backend publishes for BOTH audio types
// (RIG_STREAM_TYPE_AUDIO_RX = 0 and AUDIO_TX = 1), measured at b538567b. 48000 is a
// member, which is what makes negotiation a no-op against every existing bridge arm.
constexpr int kDummyNative[] = {8000, 16000, 24000, 48000, 96000, 0};
constexpr int kCap = 32;   // HAMLIB_MAX_STREAM_RATES on a post-961093f2 prefix.

}  // namespace

// --- The case that must not change -------------------------------------------------

TEST(StreamRate, KeepsTheRequestedRateWhenEveryDirectionHasItNatively) {
    EXPECT_EQ(na_choose_stream_rate(kDummyNative, kCap, kDummyNative, kCap,
                                    NA_STREAM_RATE_PREFERRED),
              48000);
}

TEST(StreamRate, KeepsTheRequestedRateWhenThereIsNoNativeInformationAtAll) {
    // A libhamlib older than PR #2116 commit 961093f2 has no native_* fields, so the
    // bridge compiles the negotiation out and passes nothing; a caps block that leaves
    // them zero arrives here as an empty list. Both must land on today's behaviour.
    static constexpr int kEmpty[] = {0};
    EXPECT_EQ(na_choose_stream_rate(nullptr, kCap, nullptr, kCap, NA_STREAM_RATE_PREFERRED),
              48000);
    EXPECT_EQ(na_choose_stream_rate(kEmpty, kCap, kEmpty, kCap, NA_STREAM_RATE_PREFERRED),
              48000);
    // RX absent but TX present is still "no view of the mandatory stream".
    EXPECT_EQ(na_choose_stream_rate(kEmpty, kCap, kDummyNative, kCap,
                                    NA_STREAM_RATE_PREFERRED),
              48000);
}

// --- The case #84 was filed about --------------------------------------------------

TEST(StreamRate, TakesTheClosestNativeRateWhenTheRequestedOneIsNotOffered) {
    // The rig #84 describes: native rates that simply do not include 48 kHz. Today the
    // bridge asks for 48000 anyway and libhamlib resamples inside the local hop.
    static constexpr int kOnly44k1[] = {44100, 0};
    EXPECT_EQ(na_choose_stream_rate(kOnly44k1, kCap, kOnly44k1, kCap,
                                    NA_STREAM_RATE_PREFERRED),
              44100);

    // Closest, NOT highest. 96000 is native and would cost twice the bytes on the lossy
    // hop this bridge exists to protect, for content a radio does not produce.
    static constexpr int kSpread[] = {32000, 96000, 0};
    EXPECT_EQ(na_choose_stream_rate(kSpread, kCap, kSpread, kCap, NA_STREAM_RATE_PREFERRED),
              32000);
}

TEST(StreamRate, ResolvesAnExactTieToTheHigherRate) {
    static constexpr int kTie[] = {24000, 72000, 0};        // both 24000 away from 48k
    EXPECT_EQ(na_choose_stream_rate(kTie, kCap, kTie, kCap, NA_STREAM_RATE_PREFERRED),
              72000);

    // Same tie, list published in the other order: the policy is an explicit comparison,
    // not an artefact of iteration order. Hamlib documents the channel-count list as
    // ascending and promises nothing of the kind for the rate lists.
    static constexpr int kTieReversed[] = {72000, 24000, 0};
    EXPECT_EQ(na_choose_stream_rate(kTieReversed, kCap, kTieReversed, kCap,
                                    NA_STREAM_RATE_PREFERRED),
              72000);
}

// --- The constraint #84 does not state ---------------------------------------------

TEST(StreamRate, RequiresTheRateToBeNativeOnTheTxDirectionToo) {
    // naudio carries ONE audio format for the whole server, so the rate chosen here is
    // also the rate the operator's TX audio arrives in. 48000 is native on RX and NOT on
    // TX; taking it would leave TX resampling — moving the cost #84 is about rather than
    // removing it, and silently, since nothing on the TX path reports a conversion.
    static constexpr int kRx[] = {44100, 48000, 0};
    static constexpr int kTx[] = {44100, 0};
    EXPECT_EQ(na_choose_stream_rate(kRx, kCap, kTx, kCap, NA_STREAM_RATE_PREFERRED), 44100);
}

TEST(StreamRate, FallsBackToTheRxListWhenTheTwoDirectionsShareNoNativeRate) {
    // Nothing satisfies both. RX wins: it is the mandatory stream and runs continuously,
    // while TX is optional and only carries audio while an operator is keyed.
    static constexpr int kRx[] = {44100, 0};
    static constexpr int kTx[] = {48000, 0};
    EXPECT_EQ(na_choose_stream_rate(kRx, kCap, kTx, kCap, NA_STREAM_RATE_PREFERRED), 44100);
}

TEST(StreamRate, IgnoresTheTxListWhenTxIsNotInUse) {
    // -x (RX only), or a TX stream that failed to open and left the bridge RX-only. A
    // NULL TX list must not constrain the choice, and must not be read.
    static constexpr int kRx[] = {44100, 0};
    static constexpr int kTx[] = {48000, 0};
    EXPECT_EQ(na_choose_stream_rate(kRx, kCap, nullptr, kCap, NA_STREAM_RATE_PREFERRED),
              44100);
    // Sanity: the same call WITH that TX list is the arm above — the difference is the
    // TX argument alone, not the RX list.
    EXPECT_EQ(na_choose_stream_rate(kRx, kCap, kTx, kCap, NA_STREAM_RATE_PREFERRED), 44100);
}

// --- List handling ------------------------------------------------------------------

TEST(StreamRate, StopsAtTheArrayBoundWhenTheListHasNoTerminator) {
    // Hamlib's rate arrays are 0-terminated by contract, but a caps block that fills
    // every slot leaves no terminator to find — the array bound is then the only thing
    // between the scan and a read off the end. A short `cap` must also be honoured: only
    // the first two entries are in bounds here, so 48000 is NOT visible and the closest
    // in-bounds candidate wins.
    static constexpr int kFull[] = {16000, 24000, 48000, 96000};   // no trailing 0
    EXPECT_EQ(na_rate_list_len(kFull, 4), 4);
    EXPECT_EQ(na_choose_stream_rate(kFull, 4, nullptr, 0, NA_STREAM_RATE_PREFERRED), 48000);
    EXPECT_EQ(na_choose_stream_rate(kFull, 2, nullptr, 0, NA_STREAM_RATE_PREFERRED), 24000);
    EXPECT_FALSE(na_rate_list_has(kFull, 2, 48000));
    EXPECT_TRUE(na_rate_list_has(kFull, 4, 48000));
}

TEST(StreamRate, TreatsANonPositiveEntryAsTheEndOfTheList) {
    static constexpr int kNegative[] = {44100, -1, 96000, 0};
    EXPECT_EQ(na_rate_list_len(kNegative, kCap), 1);
    EXPECT_FALSE(na_rate_list_has(kNegative, kCap, 96000));
    EXPECT_FALSE(na_rate_list_has(kNegative, kCap, 0));
    EXPECT_FALSE(na_rate_list_has(kNegative, kCap, -1));
}

TEST(StreamRate, RejectsAPreferredRateThatIsNotARate) {
    // The one input that has no sensible answer. Reported as 0 rather than silently
    // substituting 48000, so a caller that computed a bad preference sees it.
    EXPECT_EQ(na_choose_stream_rate(kDummyNative, kCap, kDummyNative, kCap, 0), 0);
    EXPECT_EQ(na_choose_stream_rate(kDummyNative, kCap, kDummyNative, kCap, -48000), 0);
}
