// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — audio stream configuration + presets.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstdint>

#include "naudio/TransportType.hpp"

namespace naudio {

// Audio-format parameters and buffer/reliability settings for network audio
// streaming.
//
// A flat Copy value struct (public fields, value semantics) — the C-ABI-ready
// shape. The 8 presets are static factories; the struct carries the numeric
// format facts directly.
struct AudioStreamConfig {
    // --- Format constants ---
    static constexpr std::int32_t DEFAULT_SAMPLE_RATE = 48000;      // 48 kHz default.
    static constexpr std::int32_t LOW_BANDWIDTH_SAMPLE_RATE = 12000;
    static constexpr std::int32_t DEFAULT_BITS_PER_SAMPLE = 16;
    static constexpr std::int32_t DEFAULT_CHANNELS = 2;             // Stereo (USB Audio Device).
    static constexpr std::int32_t DEFAULT_FRAME_MS = 20;
    static constexpr std::int32_t DEFAULT_BUFFER_TARGET_MS = 100;
    static constexpr std::int32_t DEFAULT_BUFFER_MIN_MS = 40;
    static constexpr std::int32_t DEFAULT_BUFFER_MAX_MS = 300;
    static constexpr std::int32_t FT8_BUFFER_TARGET_MS = 40;        // Low latency for digital modes.
    static constexpr std::int32_t FT8_BUFFER_MIN_MS = 20;
    static constexpr std::int32_t FT8_BUFFER_MAX_MS = 100;
    static constexpr std::int32_t VOICE_BUFFER_TARGET_MS = 120;     // Balanced for SSB.
    static constexpr std::int32_t VOICE_BUFFER_MIN_MS = 60;
    static constexpr std::int32_t VOICE_BUFFER_MAX_MS = 300;
    static constexpr std::int32_t MAX_INITIAL_BUFFERING_MS = 500;
    static constexpr std::int32_t DEFAULT_PORT = 4533;
    static constexpr std::int32_t DEFAULT_MAX_CLIENTS = 4;
    static constexpr std::int64_t DEFAULT_TX_IDLE_TIMEOUT_MS = 500;

    // --- UDP-specific preset constants ---
    static constexpr std::int32_t UDP_FRAME_MS = 10;
    static constexpr std::int32_t UDP_LAN_BUFFER_TARGET_MS = 40;
    static constexpr std::int32_t UDP_LAN_BUFFER_MIN_MS = 20;
    static constexpr std::int32_t UDP_LAN_BUFFER_MAX_MS = 150;
    static constexpr std::int32_t UDP_WAN_BUFFER_TARGET_MS = 120;
    static constexpr std::int32_t UDP_WAN_BUFFER_MIN_MS = 60;
    static constexpr std::int32_t UDP_WAN_BUFFER_MAX_MS = 400;
    static constexpr std::int32_t UDP_FT8_BUFFER_TARGET_MS = 30;
    static constexpr std::int32_t UDP_FT8_BUFFER_MIN_MS = 15;
    static constexpr std::int32_t UDP_FT8_BUFFER_MAX_MS = 80;
    static constexpr std::int32_t UDP_IQ_BUFFER_TARGET_MS = 60;
    static constexpr std::int32_t UDP_IQ_BUFFER_MIN_MS = 30;
    static constexpr std::int32_t UDP_IQ_BUFFER_MAX_MS = 200;
    static constexpr std::int32_t UDP_IQ_SAMPLE_RATE = 192000;

    // --- Fields (defaults = the TCP 48kHz/16-bit/stereo default config) ---
    std::int32_t sampleRate = DEFAULT_SAMPLE_RATE;
    std::int32_t maxClients = DEFAULT_MAX_CLIENTS;
    std::int64_t txIdleTimeoutMs = DEFAULT_TX_IDLE_TIMEOUT_MS;
    std::int32_t bitsPerSample = DEFAULT_BITS_PER_SAMPLE;
    std::int32_t channels = DEFAULT_CHANNELS;
    std::int32_t frameDurationMs = DEFAULT_FRAME_MS;
    std::int32_t bufferTargetMs = DEFAULT_BUFFER_TARGET_MS;
    std::int32_t bufferMinMs = DEFAULT_BUFFER_MIN_MS;
    std::int32_t bufferMaxMs = DEFAULT_BUFFER_MAX_MS;
    TransportType transportType = TransportType::Tcp;
    std::int32_t reorderBufferSize = 0;                  // Disabled for TCP.
    std::int32_t reorderMaxHoldMs = 0;
    bool fecEnabled = false;                             // Disabled for TCP.
    std::int32_t fecBlockSize = 5;                       // 1 parity per 5 audio packets.
    bool adaptiveJitterEnabled = false;                  // Disabled for TCP.
    double jitterMultiplier = 3.0;                       // RFC 3550 typical multiplier.
    bool controlReliabilityEnabled = false;             // Disabled for TCP.
    std::int32_t controlRetransmitMaxAttempts = 3;

    // --- Derived calculations ---
    std::int32_t samplesPerFrame() const { return (sampleRate * frameDurationMs) / 1000; }
    std::int32_t bytesPerFrame() const { return samplesPerFrame() * (bitsPerSample / 8) * channels; }
    std::int32_t bytesPerSecond() const { return sampleRate * (bitsPerSample / 8) * channels; }
    std::int32_t msToBytes(std::int32_t ms) const { return (bytesPerSecond() * ms) / 1000; }
    std::int32_t bytesToMs(std::int32_t bytes) const {
        std::int32_t bps = bytesPerSecond();
        return bps == 0 ? 0 : (bytes * 1000) / bps;
    }
    bool isFt8Optimized() const { return bufferTargetMs <= FT8_BUFFER_TARGET_MS; }
    bool isVoiceOptimized() const { return bufferTargetMs >= VOICE_BUFFER_TARGET_MS; }

    // --- Presets (return owned, fully-built configs) ---

    // Low bandwidth mode (12kHz).
    static AudioStreamConfig lowBandwidth() {
        AudioStreamConfig c;
        c.sampleRate = LOW_BANDWIDTH_SAMPLE_RATE;
        return c;
    }

    // FT8 / digital modes (40ms target buffer to minimize latency).
    static AudioStreamConfig ft8Optimized() {
        AudioStreamConfig c;
        c.bufferTargetMs = FT8_BUFFER_TARGET_MS;
        c.bufferMinMs = FT8_BUFFER_MIN_MS;
        c.bufferMaxMs = FT8_BUFFER_MAX_MS;
        return c;
    }

    // SSB voice operation (120ms target buffer).
    static AudioStreamConfig voiceOptimized() {
        AudioStreamConfig c;
        c.bufferTargetMs = VOICE_BUFFER_TARGET_MS;
        c.bufferMinMs = VOICE_BUFFER_MIN_MS;
        c.bufferMaxMs = VOICE_BUFFER_MAX_MS;
        return c;
    }

    // UDP optimized for LAN operation.
    static AudioStreamConfig udpLan() {
        AudioStreamConfig c;
        c.transportType = TransportType::Udp;
        c.frameDurationMs = UDP_FRAME_MS;
        c.bufferTargetMs = UDP_LAN_BUFFER_TARGET_MS;
        c.bufferMinMs = UDP_LAN_BUFFER_MIN_MS;
        c.bufferMaxMs = UDP_LAN_BUFFER_MAX_MS;
        c.reorderBufferSize = 8;
        c.reorderMaxHoldMs = 20;
        c.fecEnabled = false;
        c.adaptiveJitterEnabled = false;
        c.controlReliabilityEnabled = true;
        return c;
    }

    // UDP optimized for WAN/Internet operation.
    static AudioStreamConfig udpWan() {
        AudioStreamConfig c;
        c.transportType = TransportType::Udp;
        c.frameDurationMs = UDP_FRAME_MS;
        c.bufferTargetMs = UDP_WAN_BUFFER_TARGET_MS;
        c.bufferMinMs = UDP_WAN_BUFFER_MIN_MS;
        c.bufferMaxMs = UDP_WAN_BUFFER_MAX_MS;
        c.reorderBufferSize = 8;
        c.reorderMaxHoldMs = 40;
        c.fecEnabled = true;
        c.fecBlockSize = 5;
        c.adaptiveJitterEnabled = true;
        c.controlReliabilityEnabled = true;
        return c;
    }

    // UDP optimized for FT8 and digital modes.
    static AudioStreamConfig udpFt8() {
        AudioStreamConfig c;
        c.transportType = TransportType::Udp;
        c.frameDurationMs = UDP_FRAME_MS;
        c.bufferTargetMs = UDP_FT8_BUFFER_TARGET_MS;
        c.bufferMinMs = UDP_FT8_BUFFER_MIN_MS;
        c.bufferMaxMs = UDP_FT8_BUFFER_MAX_MS;
        c.reorderBufferSize = 8;
        c.reorderMaxHoldMs = 15;
        c.fecEnabled = false;
        c.adaptiveJitterEnabled = false;
        c.controlReliabilityEnabled = true;
        return c;
    }

    // UDP for IQ streaming (SDR data at 192kHz).
    static AudioStreamConfig udpIq() {
        AudioStreamConfig c;
        c.transportType = TransportType::Udp;
        c.frameDurationMs = UDP_FRAME_MS;
        c.sampleRate = UDP_IQ_SAMPLE_RATE;
        c.bufferTargetMs = UDP_IQ_BUFFER_TARGET_MS;
        c.bufferMinMs = UDP_IQ_BUFFER_MIN_MS;
        c.bufferMaxMs = UDP_IQ_BUFFER_MAX_MS;
        c.reorderBufferSize = 8;
        c.reorderMaxHoldMs = 30;
        c.fecEnabled = false;
        c.adaptiveJitterEnabled = false;
        c.controlReliabilityEnabled = true;
        return c;
    }

    // Dual TCP+UDP transport (conservative FT8-optimized UDP settings).
    static AudioStreamConfig dualDefault() {
        AudioStreamConfig c;
        c.transportType = TransportType::Dual;
        c.bufferTargetMs = FT8_BUFFER_TARGET_MS;
        c.bufferMinMs = FT8_BUFFER_MIN_MS;
        c.bufferMaxMs = FT8_BUFFER_MAX_MS;
        c.reorderBufferSize = 8;
        c.reorderMaxHoldMs = 20;
        c.controlReliabilityEnabled = true;
        return c;
    }
};

}  // namespace naudio
