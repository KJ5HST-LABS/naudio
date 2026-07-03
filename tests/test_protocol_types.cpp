// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — value types (config presets, transport, stream description).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
//
// The spec requires the 8 presets to produce exact reliability constants. These
// tests pin every preset's transport, reorder, FEC, jitter, control-reliability,
// and buffer settings — there is no conformance vector for the presets, so this
// GTest is the gate.
#include "naudio/AudioStreamConfig.hpp"
#include "naudio/StreamDescription.hpp"
#include "naudio/TransportType.hpp"

#include <gtest/gtest.h>

using naudio::AudioStreamConfig;
using naudio::ChannelLayout;
using naudio::SampleFormat;
using naudio::SourceKind;
using naudio::StreamDescription;
using naudio::TransportType;

TEST(AudioStreamConfig, DefaultIsTcp48kStereo) {
    AudioStreamConfig c;
    EXPECT_EQ(48000, c.sampleRate);
    EXPECT_EQ(16, c.bitsPerSample);
    EXPECT_EQ(2, c.channels);
    EXPECT_EQ(20, c.frameDurationMs);
    EXPECT_EQ(100, c.bufferTargetMs);
    EXPECT_EQ(40, c.bufferMinMs);
    EXPECT_EQ(300, c.bufferMaxMs);
    EXPECT_EQ(4, c.maxClients);
    EXPECT_EQ(500, c.txIdleTimeoutMs);
    EXPECT_EQ(TransportType::Tcp, c.transportType);
    // TCP reliability defaults: everything off, sane fallbacks.
    EXPECT_EQ(0, c.reorderBufferSize);
    EXPECT_EQ(0, c.reorderMaxHoldMs);
    EXPECT_FALSE(c.fecEnabled);
    EXPECT_EQ(5, c.fecBlockSize);
    EXPECT_FALSE(c.adaptiveJitterEnabled);
    EXPECT_DOUBLE_EQ(3.0, c.jitterMultiplier);
    EXPECT_FALSE(c.controlReliabilityEnabled);
    EXPECT_EQ(3, c.controlRetransmitMaxAttempts);
}

TEST(AudioStreamConfig, DerivedMath) {
    AudioStreamConfig c;
    EXPECT_EQ(960, c.samplesPerFrame());       // 48000 * 20ms / 1000
    EXPECT_EQ(3840, c.bytesPerFrame());        // 960 * 2 bytes * 2 ch
    EXPECT_EQ(192000, c.bytesPerSecond());     // 48000 * 2 * 2
    EXPECT_EQ(19200, c.msToBytes(100));        // 100ms @ 192000 B/s
    EXPECT_EQ(0, c.msToBytes(0));
    EXPECT_EQ(100, c.bytesToMs(19200));
    EXPECT_EQ(0, c.bytesToMs(0));
}

TEST(AudioStreamConfig, LowBandwidthPreset) {
    auto c = AudioStreamConfig::lowBandwidth();
    EXPECT_EQ(12000, c.sampleRate);
    EXPECT_EQ(TransportType::Tcp, c.transportType);  // only the rate changes
}

TEST(AudioStreamConfig, Ft8OptimizedPreset) {
    auto c = AudioStreamConfig::ft8Optimized();
    EXPECT_EQ(40, c.bufferTargetMs);
    EXPECT_EQ(20, c.bufferMinMs);
    EXPECT_EQ(100, c.bufferMaxMs);
    EXPECT_TRUE(c.isFt8Optimized());
}

TEST(AudioStreamConfig, VoiceOptimizedPreset) {
    auto c = AudioStreamConfig::voiceOptimized();
    EXPECT_EQ(120, c.bufferTargetMs);
    EXPECT_EQ(60, c.bufferMinMs);
    EXPECT_EQ(300, c.bufferMaxMs);
    EXPECT_TRUE(c.isVoiceOptimized());
}

TEST(AudioStreamConfig, UdpLanPreset) {
    auto c = AudioStreamConfig::udpLan();
    EXPECT_EQ(TransportType::Udp, c.transportType);
    EXPECT_EQ(10, c.frameDurationMs);
    EXPECT_EQ(40, c.bufferTargetMs);
    EXPECT_EQ(20, c.bufferMinMs);
    EXPECT_EQ(150, c.bufferMaxMs);
    EXPECT_EQ(8, c.reorderBufferSize);
    EXPECT_EQ(20, c.reorderMaxHoldMs);
    EXPECT_FALSE(c.fecEnabled);
    EXPECT_FALSE(c.adaptiveJitterEnabled);
    EXPECT_TRUE(c.controlReliabilityEnabled);
    EXPECT_EQ(48000, c.sampleRate);  // unchanged default
    EXPECT_EQ(1920, c.bytesPerFrame());  // 10ms frame = half the default
}

TEST(AudioStreamConfig, UdpWanPreset) {
    auto c = AudioStreamConfig::udpWan();
    EXPECT_EQ(TransportType::Udp, c.transportType);
    EXPECT_EQ(10, c.frameDurationMs);
    EXPECT_EQ(120, c.bufferTargetMs);
    EXPECT_EQ(60, c.bufferMinMs);
    EXPECT_EQ(400, c.bufferMaxMs);
    EXPECT_EQ(8, c.reorderBufferSize);
    EXPECT_EQ(40, c.reorderMaxHoldMs);
    EXPECT_TRUE(c.fecEnabled);
    EXPECT_EQ(5, c.fecBlockSize);
    EXPECT_TRUE(c.adaptiveJitterEnabled);
    EXPECT_TRUE(c.controlReliabilityEnabled);
}

TEST(AudioStreamConfig, UdpFt8Preset) {
    auto c = AudioStreamConfig::udpFt8();
    EXPECT_EQ(TransportType::Udp, c.transportType);
    EXPECT_EQ(10, c.frameDurationMs);
    EXPECT_EQ(30, c.bufferTargetMs);
    EXPECT_EQ(15, c.bufferMinMs);
    EXPECT_EQ(80, c.bufferMaxMs);
    EXPECT_EQ(8, c.reorderBufferSize);
    EXPECT_EQ(15, c.reorderMaxHoldMs);
    EXPECT_FALSE(c.fecEnabled);
    EXPECT_FALSE(c.adaptiveJitterEnabled);
    EXPECT_TRUE(c.controlReliabilityEnabled);
}

TEST(AudioStreamConfig, UdpIqPreset) {
    auto c = AudioStreamConfig::udpIq();
    EXPECT_EQ(TransportType::Udp, c.transportType);
    EXPECT_EQ(10, c.frameDurationMs);
    EXPECT_EQ(192000, c.sampleRate);
    EXPECT_EQ(60, c.bufferTargetMs);
    EXPECT_EQ(30, c.bufferMinMs);
    EXPECT_EQ(200, c.bufferMaxMs);
    EXPECT_EQ(8, c.reorderBufferSize);
    EXPECT_EQ(30, c.reorderMaxHoldMs);
    EXPECT_FALSE(c.fecEnabled);
    EXPECT_FALSE(c.adaptiveJitterEnabled);
    EXPECT_TRUE(c.controlReliabilityEnabled);
    EXPECT_EQ(768000, c.bytesPerSecond());  // 192kHz * 2 * 2
}

TEST(AudioStreamConfig, DualDefaultPreset) {
    auto c = AudioStreamConfig::dualDefault();
    EXPECT_EQ(TransportType::Dual, c.transportType);
    EXPECT_EQ(8, c.reorderBufferSize);
    EXPECT_EQ(20, c.reorderMaxHoldMs);
    EXPECT_TRUE(c.controlReliabilityEnabled);
    // Uses FT8-optimized buffer settings.
    EXPECT_EQ(AudioStreamConfig::FT8_BUFFER_TARGET_MS, c.bufferTargetMs);
    EXPECT_EQ(AudioStreamConfig::FT8_BUFFER_MIN_MS, c.bufferMinMs);
    EXPECT_EQ(AudioStreamConfig::FT8_BUFFER_MAX_MS, c.bufferMaxMs);
    EXPECT_EQ(20, c.frameDurationMs);
    EXPECT_EQ(48000, c.sampleRate);
}

TEST(TransportType, NameAndValueOf) {
    EXPECT_STREQ("TCP", naudio::transportTypeName(TransportType::Tcp));
    EXPECT_STREQ("UDP", naudio::transportTypeName(TransportType::Udp));
    EXPECT_STREQ("DUAL", naudio::transportTypeName(TransportType::Dual));
    EXPECT_EQ(TransportType::Tcp, naudio::transportTypeFromName("TCP").value());
    EXPECT_EQ(TransportType::Udp, naudio::transportTypeFromName("UDP").value());
    EXPECT_EQ(TransportType::Dual, naudio::transportTypeFromName("DUAL").value());
    EXPECT_FALSE(naudio::transportTypeFromName("SCTP").has_value());
}

TEST(StreamDescription, DefaultsAreNativeAudio) {
    StreamDescription d;
    EXPECT_EQ(48000, d.sampleRate);
    EXPECT_EQ(ChannelLayout::Stereo, d.channelLayout);
    EXPECT_EQ(SampleFormat::Int16, d.sampleFormat);
    EXPECT_EQ(SourceKind::Audio, d.sourceKind);
    EXPECT_FALSE(d.hasRfCenterHz);
    EXPECT_FALSE(d.hasTimeAnchorNs);
}

TEST(StreamDescription, CarriesOptionalRfCenterAndAnchor) {
    StreamDescription d;
    d.sourceKind = SourceKind::IQ;
    d.sampleFormat = SampleFormat::Float32;
    d.hasRfCenterHz = true;
    d.rfCenterHz = 14'074'000.0;
    d.hasTimeAnchorNs = true;
    d.timeAnchorNs = 1234567890;
    EXPECT_EQ(SourceKind::IQ, d.sourceKind);
    EXPECT_DOUBLE_EQ(14'074'000.0, d.rfCenterHz);
    EXPECT_EQ(1234567890, d.timeAnchorNs);
}
