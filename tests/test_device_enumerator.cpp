// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — device classification / DUPLEX-merge / virtual-matching.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Unit tests for the classification / DUPLEX-merge / virtual-matching logic. Platform is
// injected so results are deterministic on any host.
#include <gtest/gtest.h>

#include "naudio/DeviceEnumerator.hpp"
#include "naudio/FakeBackend.hpp"

using namespace naudio;

namespace {

RawDevice dev(int id, std::string name, std::string hostApi, int in, int out) {
    RawDevice d;
    d.backendId = id;
    d.name = std::move(name);
    d.hostApi = std::move(hostApi);
    d.maxInputChannels = in;
    d.maxOutputChannels = out;
    return d;
}

}  // namespace

TEST(DeviceEnumerator, ClassifiesVirtualByNameOnMac) {
    FakeBackend be;
    be.add(dev(0, "BlackHole 2ch", "Core Audio", 2, 2));
    DeviceEnumerator e(be, Platform::MacOS);

    auto devs = e.list();
    ASSERT_EQ(devs.size(), 1u);
    EXPECT_EQ(devs[0].type, DeviceType::Virtual);
    EXPECT_TRUE(devs[0].isVirtual());
}

TEST(DeviceEnumerator, ClassifiesHardware) {
    FakeBackend be;
    be.add(dev(0, "MacBook Pro Microphone", "Core Audio", 1, 0));
    DeviceEnumerator e(be, Platform::MacOS);

    auto devs = e.list();
    ASSERT_EQ(devs.size(), 1u);
    EXPECT_EQ(devs[0].type, DeviceType::Hardware);
}

TEST(DeviceEnumerator, CapabilityDerivedFromChannels) {
    FakeBackend be;
    be.add(dev(0, "Mic", "Core Audio", 1, 0));
    be.add(dev(1, "Speakers", "Core Audio", 0, 2));
    DeviceEnumerator e(be, Platform::MacOS);

    auto devs = e.list();
    ASSERT_EQ(devs.size(), 2u);
    EXPECT_EQ(devs[0].capability, Capability::Capture);
    EXPECT_EQ(devs[1].capability, Capability::Playback);
}

TEST(DeviceEnumerator, DuplexMergeOfSeparateRecords) {
    // Same identity reported once capture-only and once playback-only -> merged DUPLEX.
    FakeBackend be;
    be.add(dev(0, "USB Audio CODEC", "ALSA", 2, 0));
    be.add(dev(1, "USB Audio CODEC", "ALSA", 0, 2));
    DeviceEnumerator e(be, Platform::Linux);

    auto devs = e.list();
    ASSERT_EQ(devs.size(), 1u);
    EXPECT_EQ(devs[0].capability, Capability::Duplex);
    EXPECT_TRUE(devs[0].supportsCapture());
    EXPECT_TRUE(devs[0].supportsPlayback());
}

TEST(DeviceEnumerator, DuplexFromSingleRecord) {
    FakeBackend be;
    be.add(dev(0, "Scarlett 2i2", "Core Audio", 2, 2));
    DeviceEnumerator e(be, Platform::MacOS);

    auto devs = e.list();
    ASSERT_EQ(devs.size(), 1u);
    EXPECT_EQ(devs[0].capability, Capability::Duplex);
}

// A split capture-only + playback-only merge must keep BOTH direction ids, so each direction
// stays openable (the merged record must not collapse onto the first record's single id).
TEST(DeviceEnumerator, SplitDuplexMergeKeepsBothBackendIds) {
    FakeBackend be;
    be.add(dev(0, "USB Audio CODEC", "ALSA", 2, 0));   // capture-only, id 0
    be.add(dev(1, "USB Audio CODEC", "ALSA", 0, 2));   // playback-only, id 1
    DeviceEnumerator e(be, Platform::Linux);

    auto devs = e.list();
    ASSERT_EQ(devs.size(), 1u);
    EXPECT_EQ(devs[0].captureBackendId, 0);
    EXPECT_EQ(devs[0].playbackBackendId, 1);
    EXPECT_EQ(devs[0].backendIdFor(Direction::Capture), 0);
    EXPECT_EQ(devs[0].backendIdFor(Direction::Playback), 1);
}

// Merge is order-independent: playback record first, capture second -> same per-direction ids.
TEST(DeviceEnumerator, SplitDuplexMergeOrderIndependent) {
    FakeBackend be;
    be.add(dev(7, "CODEC", "ALSA", 0, 2));   // playback FIRST, id 7
    be.add(dev(3, "CODEC", "ALSA", 2, 0));   // capture SECOND, id 3
    DeviceEnumerator e(be, Platform::Linux);

    auto devs = e.list();
    ASSERT_EQ(devs.size(), 1u);
    EXPECT_EQ(devs[0].backendIdFor(Direction::Capture), 3);
    EXPECT_EQ(devs[0].backendIdFor(Direction::Playback), 7);
}

// A single duplex record: both directions share the one backend id.
TEST(DeviceEnumerator, SingleDuplexRecordSharesBackendId) {
    FakeBackend be;
    be.add(dev(5, "Scarlett 2i2", "Core Audio", 2, 2));
    DeviceEnumerator e(be, Platform::MacOS);

    auto devs = e.list();
    ASSERT_EQ(devs.size(), 1u);
    EXPECT_EQ(devs[0].captureBackendId, 5);
    EXPECT_EQ(devs[0].playbackBackendId, 5);
}

TEST(DeviceEnumerator, VirtualMatchingFindsCaptureDevice) {
    FakeBackend be;
    be.add(dev(0, "MacBook Pro Microphone", "Core Audio", 1, 0));
    be.add(dev(1, "BlackHole 2ch", "Core Audio", 2, 2));
    DeviceEnumerator e(be, Platform::MacOS);

    auto v = e.findVirtualCapture();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->name, "BlackHole 2ch");
}

TEST(DeviceEnumerator, VirtualMatchingReturnsNulloptWhenNone) {
    FakeBackend be;
    be.add(dev(0, "MacBook Pro Microphone", "Core Audio", 1, 0));
    DeviceEnumerator e(be, Platform::MacOS);

    EXPECT_FALSE(e.findVirtualCapture().has_value());
}

TEST(DeviceEnumerator, LinuxHostApiPatternClassifiesVirtual) {
    // On Linux "pulse"/"jack" surface as device or host-API names -> virtual.
    FakeBackend be;
    be.add(dev(0, "pulse", "ALSA", 32, 32));                          // name match
    be.add(dev(1, "system", "JACK Audio Connection Kit", 2, 2));      // hostApi match
    DeviceEnumerator e(be, Platform::Linux);

    auto devs = e.list();
    ASSERT_EQ(devs.size(), 2u);
    EXPECT_TRUE(devs[0].isVirtual());
    EXPECT_TRUE(devs[1].isVirtual());
}

TEST(DeviceEnumerator, MacPatternsDoNotMatchLinuxNames) {
    // "pulse" is a Linux pattern, not a macOS one — must stay Hardware on macOS.
    FakeBackend be;
    be.add(dev(0, "pulse", "Core Audio", 2, 2));
    DeviceEnumerator e(be, Platform::MacOS);

    auto devs = e.list();
    ASSERT_EQ(devs.size(), 1u);
    EXPECT_EQ(devs[0].type, DeviceType::Hardware);
}

TEST(DeviceEnumerator, SkipsDevicesWithNoChannels) {
    FakeBackend be;
    be.add(dev(0, "Some Control Device", "Core Audio", 0, 0));
    DeviceEnumerator e(be, Platform::MacOS);

    EXPECT_TRUE(e.list().empty());
}
