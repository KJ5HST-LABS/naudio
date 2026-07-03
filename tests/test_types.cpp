// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — device/audio value types (AudioFormat, AudioDeviceInfo).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include <gtest/gtest.h>

#include "naudio/Types.hpp"

using namespace naudio;

TEST(AudioFormat, DefaultsAre48kStereo) {
    AudioFormat f;
    EXPECT_EQ(f.sampleRate, 48000);
    EXPECT_EQ(f.bitsPerSample, 16);
    EXPECT_EQ(f.channels, 2);
    EXPECT_EQ(f.frameSize(), 4);          // 16-bit * 2ch = 4 bytes/frame
    EXPECT_EQ(f.bytesPerSecond(), 192000);  // 48000 Hz x 2 ch x 2 bytes = 192 KB/s
}

TEST(AudioFormat, EqualityComparesAllFields) {
    AudioFormat a;
    AudioFormat b;
    EXPECT_EQ(a, b);
    b.channels = 1;
    EXPECT_NE(a, b);
}

TEST(DeviceInfo, CapabilityHelpers) {
    DeviceInfo d;

    d.capability = Capability::Duplex;
    EXPECT_TRUE(d.supportsCapture());
    EXPECT_TRUE(d.supportsPlayback());

    d.capability = Capability::Capture;
    EXPECT_TRUE(d.supportsCapture());
    EXPECT_FALSE(d.supportsPlayback());

    d.capability = Capability::Playback;
    EXPECT_FALSE(d.supportsCapture());
    EXPECT_TRUE(d.supportsPlayback());
}

TEST(DeviceInfo, IsVirtual) {
    DeviceInfo d;
    d.type = DeviceType::Virtual;
    EXPECT_TRUE(d.isVirtual());
    d.type = DeviceType::Hardware;
    EXPECT_FALSE(d.isVirtual());
}
