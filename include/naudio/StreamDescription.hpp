// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — stream-fact metadata value type.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstdint>

namespace naudio {

// Channel layout for a stream (spec §13.1 proposed format breadth).
enum class ChannelLayout {
    Mono,       // Single channel.
    Stereo,     // Two channels (the 48kHz USB Audio Device native layout).
    VfoALeft,   // VFO-A on the left channel only.
    VfoBRight,  // VFO-B on the right channel only.
    Downmix,    // Stereo downmixed to mono.
};

// Sample format for a stream (spec §13.1 proposed format breadth).
enum class SampleFormat {
    Int16,    // Signed 16-bit PCM — the v1 frozen-wire default.
    Float32,  // 32-bit float PCM.
    Int8,     // Signed 8-bit PCM.
    Uint8,    // Unsigned 8-bit PCM.
};

// The kind of signal a stream carries.
enum class SourceKind {
    Audio,  // Demodulated audio — what net-audio owns.
    IQ,     // Baseband I/Q — Hamlib's domain (#1940), described here only for facts.
};

// Stream-fact metadata: the facts ABOUT a stream, distinct from the frozen
// 0xAF01 wire frame itself.
//
// ⚠ NON-NORMATIVE. This value type is NOT part of the frozen v1 wire contract —
// no conformance vector gates it. It is designed fresh from the spec's §13.5
// "Stream-fact metadata (StreamDescription)" proposed extension and parent-plan
// §4.1, and exists so a future handshake/negotiation phase can carry stream
// facts. It deliberately carries ONLY stream facts (sample rate, channel layout,
// sample format, source kind, optional RF-center, optional precise time anchor)
// and EXCLUDES station topology (receiver count, phase-coherence, transverter
// offsets) — that belongs to Hamlib or the host app, not to this library.
//
// Field set and semantics may change before any release; do not serialize this
// onto the wire until the D6 handshake extension is specified.
struct StreamDescription {
    std::int32_t sampleRate = 48000;                         // Samples per second.
    ChannelLayout channelLayout = ChannelLayout::Stereo;     // Channel arrangement.
    SampleFormat sampleFormat = SampleFormat::Int16;         // PCM sample format.
    SourceKind sourceKind = SourceKind::Audio;               // Audio vs I/Q.

    // Optional RF-center frequency (Hz). Present only when the stream is tied to a
    // tuned receiver; absent for pure soundcard audio.
    bool hasRfCenterHz = false;
    double rfCenterHz = 0.0;

    // Optional precise time anchor (nanoseconds, VITA-49-style UTC convention).
    // Distinct from the per-packet timestamp: this anchors the stream's t=0.
    bool hasTimeAnchorNs = false;
    std::int64_t timeAnchorNs = 0;
};

}  // namespace naudio
