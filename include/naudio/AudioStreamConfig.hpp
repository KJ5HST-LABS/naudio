// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — audio stream configuration + presets.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "naudio/AudioPacket.hpp"  // UDP_MAX_PAYLOAD / udpMaxAudioPayload — the MTU advisory this
                                   // struct derives its UDP chunk size from, rather than restating
                                   // the header/CRC arithmetic and letting the two drift.
#include "naudio/TransportType.hpp"

namespace naudio {

// Audio-format parameters and buffer/reliability settings for network audio
// streaming.
//
// A flat Copy value struct (public fields, value semantics) — the C-ABI-ready
// shape. The presets are static factories; the struct carries the numeric
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
    // Consent marker for the no-reliability UDP composition (issue #92). A UDP (or, on a server,
    // DUAL) config with fecEnabled false, reorderBufferSize 0, adaptiveJitterEnabled false AND
    // controlReliabilityEnabled false is refused where the pipeline is built — AudioStreamClient::
    // connect / AudioStreamServer::start — unless this is true, because that composition is almost
    // always the "set the transport and nothing else" trap: it discards every parity packet and is
    // indistinguishable from a healthy client on loss-free loopback. udpBare() (and the C ABI's
    // NA_RELIABILITY_UDP_BARE) is the sanctioned way to set it; a C++ caller building a config by
    // hand may set the field directly. Every other preset leaves it false, and it gates nothing
    // when any reliability component is on or the transport is TCP.
    bool explicitBareUdp = false;

    // --- Discovery (spec 1.4, §6.8) ---

    // ON by default: a naudio server answers DISCOVER probes unless told not to.
    //
    // That is a deliberate choice and it has a cost, stated rather than buried:
    // answering tells an unauthenticated stranger on the segment that this server
    // exists, what format it serves and how full it is (§6.8, Disclosure). The
    // alternative default was opt-in, which is safer and which makes discovery a
    // feature almost nobody would ever switch on -- a server you must configure
    // before it can be found does not solve the problem discovery exists to solve.
    //
    // What ON does NOT weaken: a probe still creates no connection, consumes no
    // client slot and starts no stream, replies are rate-limited per source, and
    // the anti-spoof gate is otherwise untouched -- a datagram from an unknown
    // sender that is neither a CONNECT_REQUEST nor a DISCOVER is still dropped.
    // Set false (na_server_set_discoverable(server, 0)) to go silent.
    bool discoverable = true;

    // The RENDEZVOUS port a server also listens on for DISCOVER probes, so a client
    // that knows only this number can find a server serving audio on any other port.
    //
    // 4533 -- the same number as the default service port, because a client that
    // knows nothing else knows this one. A server already serving UDP on 4533 simply
    // fails to bind it a second time and answers through its transport instead, which
    // is why the responder is started best-effort and its failure is not reported.
    //
    // Set 0 to run no rendezvous listener at all (the server is then findable only by
    // a probe aimed at its own service port). `discoverable = false` disables both.
    std::int32_t discoveryPort = 4533;

    // Operator label carried in DISCOVER_REPLY, e.g. "shack" or "K1ABC 40m".
    // Empty is fine and is the default -- a prober then has the address it heard
    // the reply from, which is the actual discovery result. Clamped to 255 bytes
    // by the codec, so a long label is truncated rather than mis-framing the reply.
    std::string serverName;

    // --- Derived calculations ---
    std::int32_t samplesPerFrame() const { return (sampleRate * frameDurationMs) / 1000; }
    // Bytes occupied by one sample across all channels — the unit no chunk boundary
    // may split. bytesPerFrame() is stated in terms of it so the two cannot drift.
    std::int32_t sampleFrameBytes() const { return (bitsPerSample / 8) * channels; }
    std::int32_t bytesPerFrame() const { return samplesPerFrame() * sampleFrameBytes(); }
    // The UDP audio chunk size this format implies (issue #86): the MTU advisory,
    // rounded down to a whole sample frame. See AudioPacket::udpMaxAudioPayload for
    // what a 0 return means and who decides it.
    std::int32_t udpMaxAudioPayload() const {
        const std::int32_t sf = sampleFrameBytes();
        return static_cast<std::int32_t>(
            AudioPacket::udpMaxAudioPayload(sf <= 0 ? 0 : static_cast<std::size_t>(sf)));
    }
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

    // Bare UDP by explicit request (issue #92): the default config with only the transport
    // changed — 20 ms framing, no FEC / reorder / adaptive jitter / control-ARQ — plus the
    // consent marker that lets connect()/start() accept it. This is BYTE-FOR-BYTE the config
    // that "set_transport(Udp) on a default config" used to run implicitly, kept reachable so a
    // measurement-control arm (a deliberately unprotected baseline) stays reproducible. Do not
    // "improve" its settings: its value is that it is exactly the historical bare composition.
    static AudioStreamConfig udpBare() {
        AudioStreamConfig c;
        c.transportType = TransportType::Udp;
        c.explicitBareUdp = true;
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
