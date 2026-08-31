// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tools — the HARDWARE-SMOKE driver.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// This is the `na_audio_daemon`: the headless exerciser that runs the real AudioStreamServer /
// AudioStreamClient over a REAL PortAudio device, which the ctest suite (FakeBackend only)
// cannot reach. It is a standalone driver — `main()` + std::threads + a signal handler — and
// lives in tools/, NOT the library (it is an exerciser, not an audio primitive). It links the
// internal static `naudio_net` (server/client/transport) +
// `naudio_pa` (PortAudioBackend) directly — never the public hidden-visibility shared `naudio`.
//
// Modes (device probe / full pipeline) exercise the real capture/playback path that the
// FakeBackend-only ctest suite cannot reach:
//
//   --list-devices             enumerate capture + playback devices, then exit. Use this first
//                              to find the capture device's backendId and any virtual sink.
//
//   --mode capture-probe       open the chosen capture device DIRECTLY (no server) and read
//                              the requested format (--rate/--channels; 48 kHz stereo default)
//                              for --duration-ms. The foundational real-hardware proof: reports
//                              frames transferred, the OVERFLOW count (the "no overruns" gate —
//                              the only place the device's overrun flag is directly observable),
//                              and per-channel RMS (so an asymmetric or single-channel source
//                              shows up per channel). A DECLARED format the device answers with
//                              something else (the mono fallback) is REFUSED — naudio does not
//                              resample, so serving it would mislabel the audio (#91 Phase A).
//
//   --mode hardware (default)  the full pipeline: AudioStreamServer captures from the REAL
//                              device and fans it out over a 127.0.0.1 transport to an
//                              in-process, RX-only AudioStreamClient. The client drains RX
//                              either to a real virtual sink (--playback <pattern>, e.g.
//                              BlackHole — the path an external consumer then opens) or, if
//                              none is given / found, to a hardware-free FakeBackend. An RX
//                              audio listener measures sustained throughput (the dropout proxy)
//                              and per-channel RMS. NB: the server never opens a playback line
//                              back to the capture device, so this smoke is RX-only and cannot
//                              transmit.
//
// Usage:
//   na_audio_daemon --list-devices
//   na_audio_daemon [--mode capture-probe|hardware] [--capture <pat>|--capture-id N]
//                   [--playback <pat>|--playback-id N] [--transport tcp|udp|dual]
//                   [--rate HZ] [--channels 1|2] [--port N] [--duration-ms N]
//                   [--config <file>|--no-config]
//
// Every setting flag also has a config-file key of the same name (issue #96 item 1): `key = value`
// lines read from --config <file>, else from the platform's conventional location; flags override
// the file. See kSettings / loadConfigFile below.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "naudio/AudioStreamConfig.hpp"
#include "naudio/DeviceEnumerator.hpp"
#include "naudio/FakeBackend.hpp"
#include "naudio/PortAudioBackend.hpp"
#include "naudio/Stream.hpp"
#include "naudio/StreamOpener.hpp"
#include "naudio/Types.hpp"
#include "naudio/net/AudioStreamClient.hpp"
#include "naudio/net/AudioStreamServer.hpp"

namespace {

// ---- Ctrl-C handling (run until Ctrl-C) ----------------------------------------------------
std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop.store(true); }

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 16-bit-PCM linear RMS -> dBFS (32768 = full scale). Floors well below the noise of a real ADC.
double dbfs(double rms) { return rms < 1.0 ? -120.0 : 20.0 * std::log10(rms / 32768.0); }

// ---- Per-channel RX signal meter -----------------------------------------------------------
// Fed from the audio-listener / capture-read path (interleaved 16-bit LE PCM). Thread-safe: the
// client's receive worker calls consume() while the monitor thread reads snapshots.
struct SignalMeter {
    mutable std::mutex m;
    std::int64_t bytes = 0;
    std::int64_t frames = 0;
    long double sumSqL = 0.0L;
    long double sumSqR = 0.0L;
    double peakL = 0.0;
    double peakR = 0.0;

    void consume(const std::uint8_t* data, std::size_t len, int channels) {
        std::lock_guard<std::mutex> lock(m);
        bytes += static_cast<std::int64_t>(len);
        const int bytesPerFrame = 2 * channels;
        const std::size_t n = len / static_cast<std::size_t>(bytesPerFrame);
        for (std::size_t i = 0; i < n; ++i) {
            std::int16_t l = 0;
            std::memcpy(&l, data + i * bytesPerFrame, 2);
            const double lv = std::abs(static_cast<double>(l));
            sumSqL += static_cast<long double>(l) * l;
            peakL = std::max(peakL, lv);
            if (channels >= 2) {
                std::int16_t r = 0;
                std::memcpy(&r, data + i * bytesPerFrame + 2, 2);
                const double rv = std::abs(static_cast<double>(r));
                sumSqR += static_cast<long double>(r) * r;
                peakR = std::max(peakR, rv);
            }
        }
        frames += static_cast<std::int64_t>(n);
    }

    struct Snap {
        std::int64_t bytes, frames;
        double rmsL, rmsR, peakL, peakR;
    };
    Snap snapshot() const {
        std::lock_guard<std::mutex> lock(m);
        const double rmsL = frames ? std::sqrt(static_cast<double>(sumSqL / frames)) : 0.0;
        const double rmsR = frames ? std::sqrt(static_cast<double>(sumSqR / frames)) : 0.0;
        return {bytes, frames, rmsL, rmsR, peakL, peakR};
    }
};

// ---- CLI -----------------------------------------------------------------------------------
struct Args {
    std::string mode = "hardware";  // hardware | capture-probe
    std::string capturePattern;
    int captureId = -1;
    std::string playbackPattern;
    int playbackId = -1;
    std::string transport = "tcp";  // tcp | udp | dual
    int port = naudio::AudioStreamConfig::DEFAULT_PORT;
    std::int64_t durationMs = 30000;  // 0 == until Ctrl-C
    bool listDevices = false;
    int rate = 48000;      // #91 Phase A: the declared capture/server format
    int channels = 2;
    bool formatDeclared = false;  // set by --rate/--channels; arms the capture-probe refusal
};

std::string defaultConfigPath();  // defined with the config-file loader below

void usage() {
    const std::string confPath = defaultConfigPath();
    std::fprintf(
        stderr,
        "usage: na_audio_daemon [--mode capture-probe|hardware] [--capture <pat>|--capture-id N]\n"
        "                       [--playback <pat>|--playback-id N] [--transport tcp|udp|dual]\n"
        "                       [--port N] [--duration-ms N] [--config <file>|--no-config]\n"
        "       na_audio_daemon --list-devices\n\n"
        "  modes:\n"
        "    capture-probe  open the capture device directly; report frames/overflow/RMS\n"
        "                   (the real-hardware capture + no-overrun gate; capture only)\n"
        "    hardware       (default) server captures the radio RX -> 127.0.0.1 transport ->\n"
        "                   in-process RX-only client; measures sustained throughput + RMS\n\n"
        "  --capture <pat>   capture device name substring (default: USB Audio CODEC)\n"
        "  --capture-id N    force capture device backendId (skips name lookup)\n"
        "  --playback <pat>  hardware mode: client RX sink, e.g. BlackHole (the digital-mode feed);\n"
        "                    omitted/not-found => hardware-free FakeBackend drain\n"
        "                    WARNING: a pattern that matches NOTHING takes that same drain and the\n"
        "                    run can still exit 0 -- so a misspelled sink name reports a PASS for a\n"
        "                    check that never fed your digital-mode app. Confirm the 'client sink'\n"
        "                    line names a real device before trusting a hardware-mode pass.\n"
        "  --playback-id N   force playback device backendId\n"
        "  --transport       tcp (default) | udp | dual\n"
        "  --rate HZ         capture/server sample rate, 8000..192000, divisible by 50 for an\n"
        "                    exact 20 ms frame (default 48000). naudio does NOT resample: the\n"
        "                    device must open at exactly this rate or the run refuses -- a\n"
        "                    silent fallback would ship wrong-rate audio labeled with this rate\n"
        "  --channels N      1 (mono) | 2 (stereo, default); refused like --rate on mismatch\n"
        "  --port N          server port; 0 = ephemeral (default %d)\n"
        "  --duration-ms N   run time; 0 = until Ctrl-C (default 30000)\n"
        "  --config <file>   read settings from <file>; flags given here still override it\n"
        "  --no-config       skip the default config file\n"
        "  --list-devices    enumerate capture + playback devices, then exit\n"
        "  -h, --help        print this message\n\n"
        "  config file:      one `key = value` per line, '#' starts a comment line; keys are\n"
        "                    the setting names above without their leading dashes (e.g.\n"
        "                    `transport = udp`, `capture-id = 3`). Read from --config <file>,\n"
        "                    else from %s\n"
        "                    when that exists. Flags always override the file.\n",
        naudio::AudioStreamConfig::DEFAULT_PORT,
        confPath.empty() ? "(the default location is unresolvable: HOME is not set)"
                         : confPath.c_str());
}

// Default substrings for the radio's USB-audio capture device (macOS shows "USB Audio CODEC").
const std::vector<std::string> kDefaultCapturePatterns = {"USB Audio CODEC",
                                                          "USB Audio"};
// Default substrings for a virtual sink the client can feed (and a digital-mode app can read).
const std::vector<std::string> kDefaultVirtualSinkPatterns = {"BlackHole", "VB-Cable", "VB-Audio",
                                                             "Soundflower", "Loopback"};

void printDeviceList(const char* label, const std::vector<naudio::DeviceInfo>& devs) {
    std::printf("\n%s (%zu):\n", label, devs.size());
    for (const auto& d : devs) {
        const char* type = d.type == naudio::DeviceType::Virtual    ? "virtual"
                           : d.type == naudio::DeviceType::Hardware ? "hardware"
                                                                    : "unknown";
        std::printf("  [%2d] %-40s  in=%d out=%d  %.0f Hz  %-8s  %s\n", d.backendId,
                    d.name.c_str(), d.maxInputChannels, d.maxOutputChannels, d.defaultSampleRate,
                    type, d.hostApi.c_str());
    }
}

int runListDevices() {
    naudio::PortAudioBackend backend;
    naudio::DeviceEnumerator enumerator(backend);
    printDeviceList("CAPTURE devices", enumerator.captureDevices());
    printDeviceList("PLAYBACK devices", enumerator.playbackDevices());
    std::printf(
        "\nPick the radio's USB-audio device for --capture (or --capture-id), and a virtual\n"
        "sink (e.g. BlackHole) for --playback if you want to feed an external app.\n");
    return 0;
}

naudio::AudioStreamConfig configFor(const std::string& transport) {
    if (transport == "udp") return naudio::AudioStreamConfig::udpLan();
    if (transport == "dual") return naudio::AudioStreamConfig::dualDefault();
    return naudio::AudioStreamConfig{};  // tcp default
}

// Resolve the capture device into a DeviceInfo (for StreamOpener's mono-fallback policy).
std::optional<naudio::DeviceInfo> resolveCapture(naudio::DeviceEnumerator& en, const Args& a) {
    if (a.captureId >= 0) {
        naudio::DeviceInfo d;
        d.backendId = a.captureId;
        d.captureBackendId = a.captureId;
        d.name = "device #" + std::to_string(a.captureId);
        return d;
    }
    auto caps = en.captureDevices();
    const auto& pats = a.capturePattern.empty() ? kDefaultCapturePatterns
                                                : std::vector<std::string>{a.capturePattern};
    return en.find(caps, pats);
}

// ---- capture-probe mode --------------------------------------------------------------------
int runCaptureProbe(const Args& a) {
    naudio::PortAudioBackend backend;
    naudio::DeviceEnumerator enumerator(backend);
    naudio::StreamOpener opener(backend);

    auto dev = resolveCapture(enumerator, a);
    if (!dev) {
        std::fprintf(stderr, "error: no capture device matched (try --list-devices / --capture-id)\n");
        return 1;
    }
    // backendIdFor: StreamOpener opens the per-direction id, so print that one rather than the
    // merged record's `backendId`, which on a split pair can name a different record entirely.
    std::printf("capture-probe: device [%d] %s\n", dev->backendIdFor(naudio::Direction::Capture),
                dev->name.c_str());

    naudio::AudioFormat requested;  // 48 kHz / 16-bit / stereo unless --rate/--channels declared
    requested.sampleRate = a.rate;
    requested.channels = a.channels;
    std::unique_ptr<naudio::CaptureStream> stream;
    try {
        stream = opener.openCapture(*dev, requested);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: openCapture failed: %s\n", e.what());
        if (a.formatDeclared)
            std::fprintf(stderr, "hint: the device may not support %d Hz / %d ch — naudio does "
                                 "not resample. Pick a rate/channel count the device supports.\n",
                         a.rate, a.channels);
        return 1;
    }
    const naudio::AudioFormat fmt = stream->actualFormat();
    std::printf("opened: %d Hz / %d-bit / %d ch%s\n", fmt.sampleRate, fmt.bitsPerSample,
                fmt.channels, fmt.channels == 1 ? "  (mono fallback)" : "");
    // The refusal (#91 Phase A). StreamOpener's one silent divergence is the mono fallback —
    // the backend otherwise opens EXACTLY the requested format or throws (PortAudioBackend
    // stores `requested` verbatim as actualFormat). A declared format that came back different
    // must not be served: naudio has no resampler, so proceeding would measure — and in
    // hardware mode ship — audio in a format other than the one the operator declared. The
    // undeclared default keeps the labeled fallback above: the probe then reports what it
    // actually opened, which lies to no one.
    if (a.formatDeclared &&
        (fmt.sampleRate != requested.sampleRate || fmt.channels != requested.channels ||
         fmt.bitsPerSample != requested.bitsPerSample)) {
        std::fprintf(stderr,
                     "error: capture opened %d Hz / %d ch but %d Hz / %d ch was declared — "
                     "refusing.\nnaudio does not resample; serving this would label %d Hz / %d ch "
                     "audio as the declared format.\nDrop --rate/--channels to accept the device's "
                     "fallback, or pick a device that supports the declared format.\n",
                     fmt.sampleRate, fmt.channels, requested.sampleRate, requested.channels,
                     fmt.sampleRate, fmt.channels);
        return 1;
    }

    const int chunkFrames = (fmt.sampleRate / 10);  // ~100 ms per read
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(chunkFrames) * fmt.frameSize());
    SignalMeter meter;
    std::int64_t overflowReads = 0, totalReads = 0, framesRead = 0;

    const std::int64_t start = nowMs();
    const std::int64_t deadline = a.durationMs > 0 ? start + a.durationMs : 0;
    std::int64_t nextTick = start + 1000;
    std::printf("reading for %s ... (Ctrl-C to stop)\n",
                a.durationMs > 0 ? (std::to_string(a.durationMs) + " ms").c_str() : "ever");

    while (!g_stop.load() && (deadline == 0 || nowMs() < deadline)) {
        const naudio::IoResult r = stream->read(buf.data(), chunkFrames, /*timeoutMs=*/2000);
        ++totalReads;
        if (r.overflowed) ++overflowReads;
        if (r.frames > 0) {
            meter.consume(buf.data(), static_cast<std::size_t>(r.frames) * fmt.frameSize(),
                          fmt.channels);
            framesRead += r.frames;
        }
        if (nowMs() >= nextTick) {
            const auto s = meter.snapshot();
            std::printf("  t=%2llds  frames=%-9lld  L=%.1f dBFS  R=%.1f dBFS  overflows=%lld\n",
                        static_cast<long long>((nowMs() - start) / 1000),
                        static_cast<long long>(framesRead), dbfs(s.rmsL), dbfs(s.rmsR),
                        static_cast<long long>(overflowReads));
            nextTick += 1000;
        }
    }

    const std::int64_t elapsed = nowMs() - start;
    const auto s = meter.snapshot();
    const double expectedFrames = (static_cast<double>(fmt.sampleRate) * elapsed) / 1000.0;
    const double capturePct = expectedFrames > 0 ? 100.0 * framesRead / expectedFrames : 0.0;
    std::printf("\n=== capture-probe summary ===\n");
    std::printf("  elapsed        : %lld ms\n", static_cast<long long>(elapsed));
    std::printf("  frames         : %lld (%.1f%% of %0.f expected @ %d Hz)\n",
                static_cast<long long>(framesRead), capturePct, expectedFrames, fmt.sampleRate);
    std::printf("  reads          : %lld, overflowed: %lld\n", static_cast<long long>(totalReads),
                static_cast<long long>(overflowReads));
    std::printf("  LEFT  (ch 0)   : RMS %.1f dBFS, peak %.1f dBFS\n", dbfs(s.rmsL), dbfs(s.peakL));
    if (fmt.channels >= 2)
        std::printf("  RIGHT (ch 1)   : RMS %.1f dBFS, peak %.1f dBFS\n", dbfs(s.rmsR),
                    dbfs(s.peakR));
    const bool noOverruns = overflowReads == 0;
    const bool signalLeft = s.rmsL > 30.0;  // ~ -60 dBFS; an active signal is well above this
    std::printf("  no-overrun gate: %s\n", noOverruns ? "PASS" : "FAIL (overruns seen)");
    std::printf("  LEFT signal    : %s\n",
                signalLeft ? "present (real RX audio captured)"
                           : "below threshold (band quiet? wrong device? check level)");
    return noOverruns ? 0 : 1;
}

// ---- hardware mode: client lifecycle listener ----------------------------------------------
class SmokeClientListener : public naudio::net::AudioClientListener {
public:
    std::atomic<int> errors{0};
    std::atomic<bool> connected{false};
    void onClientConnected(const std::string& id, const std::string& addr) override {
        std::printf("[client] connected id=%s addr=%s\n", id.c_str(), addr.c_str());
        connected.store(true);
    }
    void onClientDisconnected(const std::string& id) override {
        std::printf("[client] disconnected id=%s\n", id.c_str());
        connected.store(false);
    }
    void onStreamStarted(const std::string& id) override {
        std::printf("[client] stream started id=%s\n", id.c_str());
    }
    void onError(const std::string& id, const std::string& err) override {
        std::printf("[client] ERROR id=%s: %s\n", id.c_str(), err.c_str());
        errors.fetch_add(1);
    }
    void onClientsUpdate(int count, int maxClients, const std::string& txOwner,
                         const std::vector<std::string>&) override {
        std::printf("[roster] clients=%d/%d txOwner=%s\n", count, maxClients,
                    txOwner.empty() ? "(none)" : txOwner.c_str());
    }
};

int runHardware(const Args& a) {
    naudio::PortAudioBackend backend;
    naudio::DeviceEnumerator enumerator(backend);

    // --- Resolve the radio's capture device (required). ---
    auto capDev = resolveCapture(enumerator, a);
    if (!capDev) {
        std::fprintf(stderr, "error: no capture device matched (try --list-devices / --capture-id)\n");
        return 1;
    }
    // backendIdFor, never backendId: on an ALSA-style split record the two directions live on
    // DIFFERENT backend ids and `backendId` is only the first-seen record's. Print the id that
    // is actually opened, so the log names the device the server really uses.
    const int captureId = capDev->backendIdFor(naudio::Direction::Capture);
    std::printf("server capture: device [%d] %s\n", captureId, capDev->name.c_str());

    naudio::AudioStreamConfig cfg = configFor(a.transport);
    // #91 Phase A: the declared server-wide format (defaults match cfg's own, so this is a no-op
    // without --rate/--channels). No refusal check is needed on this path: the server opens the
    // capture device DIRECTLY on the backend, which opens exactly formatFromConfig() or throws —
    // there is no fallback here to diverge silently, so a device that cannot do the declared
    // format fails server.start loudly instead.
    cfg.sampleRate = a.rate;
    cfg.channels = a.channels;

    // --- Resolve the client's RX sink: a real virtual device, else a FakeBackend drain. ---
    naudio::AudioFormat fmt;  // matches cfg (48 kHz / 16-bit / stereo at the defaults)
    fmt.sampleRate = cfg.sampleRate;
    fmt.bitsPerSample = cfg.bitsPerSample;
    fmt.channels = cfg.channels;

    std::optional<naudio::DeviceInfo> sinkDev;
    if (a.playbackId >= 0) {
        naudio::DeviceInfo d;
        d.backendId = a.playbackId;
        d.playbackBackendId = a.playbackId;
        d.name = "device #" + std::to_string(a.playbackId);
        sinkDev = d;
    } else {
        auto plays = enumerator.playbackDevices();
        const auto& pats = a.playbackPattern.empty()
                               ? kDefaultVirtualSinkPatterns
                               : std::vector<std::string>{a.playbackPattern};
        sinkDev = enumerator.find(plays, pats);
    }

    // The client is backend-agnostic; pick the backend for its REQUIRED playback line.
    naudio::FakeBackend fakeBackend;
    naudio::DeviceBackend* clientBackend = nullptr;
    int clientPlaybackId = -1;
    bool realSink = false;
    if (sinkDev) {
        clientBackend = &backend;  // share the one PortAudio init
        clientPlaybackId = sinkDev->backendIdFor(naudio::Direction::Playback);
        realSink = true;
        std::printf("client sink  : device [%d] %s (real — external apps can read this)\n",
                    clientPlaybackId, sinkDev->name.c_str());
    } else {
        // Hardware-free drain: register the cfg playback format the client will open (the
        // negotiated format IS cfg's — the server announces it in AUDIO_CONFIG).
        fakeBackend.add(naudio::RawDevice{/*backendId=*/0, "fake-sink", "fake", 0, cfg.channels,
                                          static_cast<double>(cfg.sampleRate)});
        fakeBackend.addSupportedFormat(0, naudio::Direction::Playback, fmt);
        clientBackend = &fakeBackend;
        clientPlaybackId = 0;
        std::printf("client sink  : FakeBackend drain (no virtual sink found; pipeline-only)\n");
        // An EXPLICIT --playback that matched nothing takes this same silent fallback, and the run
        // can still exit 0 — so a misspelled sink name reports a pass for a check that never fed
        // the digital-mode app. Name the pattern that missed, so the drain is not mistaken for the
        // "no sink configured" case.
        if (!a.playbackPattern.empty()) {
            std::printf("               NOTE: --playback '%s' matched NO device; this run does NOT\n"
                        "               feed any external consumer, and a pass here says nothing\n"
                        "               about your digital-mode app.\n",
                        a.playbackPattern.c_str());
        }
    }

    // --- Start the server (capture-only; NEVER opens a playback line to the radio). ---
    naudio::net::AudioStreamServer server(static_cast<std::uint16_t>(a.port), cfg, "127.0.0.1");
    server.setBackend(&backend);
    server.setCaptureDevice(captureId);
    std::string err;
    if (!server.start(&err)) {
        std::fprintf(stderr, "error: server.start failed: %s\n", err.c_str());
        if (a.formatDeclared)
            std::fprintf(stderr, "hint: the capture device may not support %d Hz / %d ch — naudio "
                                 "does not resample. Pick a rate the device supports.\n",
                         cfg.sampleRate, cfg.channels);
        return 1;
    }
    const int boundPort = server.port();
    std::printf("server up    : %s 127.0.0.1:%d\n", a.transport.c_str(), boundPort);

    // --- In-process RX-only client over loopback. ---
    naudio::net::AudioStreamClient client("127.0.0.1", static_cast<std::uint16_t>(boundPort),
                                          "phase3e-smoke");
    client.setConfig(cfg);
    client.setBackend(clientBackend);
    client.setPlaybackDevice(clientPlaybackId);
    // RX-only: no capture device on the client (no TX). Deterministic single connection.
    client.setAutoReconnect(false);

    SmokeClientListener listener;
    client.addStreamListener(&listener);
    SignalMeter meter;
    client.addAudioListener([&meter, ch = cfg.channels](const std::uint8_t* d, std::size_t n) {
        meter.consume(d, n, ch);
    });

    if (!client.connect(&err)) {
        std::fprintf(stderr, "error: client.connect failed: %s\n", err.c_str());
        server.stop();
        return 1;
    }

    // --- Monitor loop: per-second throughput + RMS; min-throughput is the dropout proxy. ---
    const std::int64_t expectedBps = cfg.bytesPerSecond();
    std::printf("expected RX  : %lld bytes/s (%d Hz / %d-bit / %d ch)\n",
                static_cast<long long>(expectedBps), cfg.sampleRate, cfg.bitsPerSample,
                cfg.channels);
    std::printf("streaming for %s ... (Ctrl-C to stop)\n\n",
                a.durationMs > 0 ? (std::to_string(a.durationMs) + " ms").c_str() : "ever");

    const std::int64_t start = nowMs();
    const std::int64_t deadline = a.durationMs > 0 ? start + a.durationMs : 0;
    std::int64_t prevBytes = 0, prevMs = start, minBps = -1;
    while (!g_stop.load() && (deadline == 0 || nowMs() < deadline)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const std::int64_t t = nowMs();
        if (t - prevMs < 1000) continue;
        const auto s = meter.snapshot();
        const std::int64_t bps = (s.bytes - prevBytes) * 1000 / (t - prevMs);
        if (minBps < 0 || bps < minBps) minBps = bps;
        std::printf("  t=%2llds  rx=%-9lld B  %lld B/s (%.0f%%)  L=%.1f dBFS  R=%.1f dBFS  conn=%d\n",
                    static_cast<long long>((t - start) / 1000), static_cast<long long>(s.bytes),
                    static_cast<long long>(bps), expectedBps ? 100.0 * bps / expectedBps : 0.0,
                    dbfs(s.rmsL), dbfs(s.rmsR), listener.connected.load() ? 1 : 0);
        prevBytes = s.bytes;
        prevMs = t;
    }

    // --- Teardown: client first (no reconnect storm), then server. ---
    const std::int64_t elapsed = nowMs() - start;
    client.disconnect();
    server.stop();

    const auto s = meter.snapshot();
    const std::int64_t avgBps = elapsed > 0 ? s.bytes * 1000 / elapsed : 0;
    const double avgPct = expectedBps ? 100.0 * avgBps / expectedBps : 0.0;
    std::printf("\n=== hardware smoke summary ===\n");
    std::printf("  transport      : %s, sink: %s\n", a.transport.c_str(),
                realSink ? "real virtual device" : "FakeBackend drain");
    std::printf("  elapsed        : %lld ms\n", static_cast<long long>(elapsed));
    std::printf("  RX total       : %lld bytes\n", static_cast<long long>(s.bytes));
    std::printf("  RX throughput  : avg %lld B/s (%.1f%%), min %lld B/s of %lld expected\n",
                static_cast<long long>(avgBps), avgPct, static_cast<long long>(minBps < 0 ? 0 : minBps),
                static_cast<long long>(expectedBps));
    std::printf("  LEFT  (ch 0)   : RMS %.1f dBFS, peak %.1f dBFS\n", dbfs(s.rmsL), dbfs(s.peakL));
    if (cfg.channels >= 2)
        std::printf("  RIGHT (ch 1)   : RMS %.1f dBFS, peak %.1f dBFS\n", dbfs(s.rmsR),
                    dbfs(s.peakR));
    std::printf("  client errors  : %d\n", listener.errors.load());

    const bool gotStream = s.bytes > 0;
    const bool steady = avgPct >= 85.0;  // loopback should sit near 100%; <85% => dropouts
    const bool noErrors = listener.errors.load() == 0;
    const bool signalLeft = s.rmsL > 30.0;
    std::printf("  pipeline       : %s\n", gotStream && noErrors ? "PASS" : "FAIL");
    std::printf("  steady-rate    : %s\n", steady ? "PASS (no sustained dropouts)" : "WARN (rate dipped)");
    std::printf("  LEFT signal    : %s\n",
                signalLeft ? "present (real RX audio through the pipeline)"
                           : "below threshold (band quiet? check device/level)");
    if (realSink)
        std::printf("  virtual sink   : server RX is now flowing to '%s' — open it as the input\n"
                    "                   in your digital-mode app (or any consumer) to complete the bridge check\n",
                    sinkDev->name.c_str());
    return (gotStream && noErrors) ? 0 : 1;
}

// Parse an integer option value STRICTLY: the whole token must be consumed and the result must sit
// in [lo, hi]. std::atoi returns 0 for a non-numeric string and gives the caller no way to tell
// that from a genuine "0" — and 0 is a VALID PortAudio device index, so `--capture-id usb` used to
// open device 0 and the RMS gate could then pass on whatever that device happened to hear. A typo
// must stop the run, not quietly select a different radio. Exits 2 (the usage-error code the rest
// of the parser uses) rather than returning a sentinel, because there is no in-band value left to
// mean "invalid". `where` names the source in the message: the flag itself on the command line,
// "<file>:<line>: <key>" from a config file.
long long requireInt(const std::string& where, const std::string& text, long long lo,
                     long long hi) {
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    if (text.empty() || end == text.c_str() || *end != '\0') {
        std::fprintf(stderr, "error: %s expects an integer, got '%s'\n", where.c_str(),
                     text.c_str());
        std::exit(2);
    }
    if (errno == ERANGE || v < lo || v > hi) {
        std::fprintf(stderr, "error: %s must be in [%lld, %lld], got '%s'\n", where.c_str(), lo, hi,
                     text.c_str());
        std::exit(2);
    }
    return v;
}

// ---- Config file (issue #96 item 1) --------------------------------------------------------
// The daemon's whole configuration surface used to be argv; a service or a control page needs
// the same settings to live in a file a clickable thing can write. Precedence is defaults <
// config file < flags, implemented by ordering alone: the file is applied to Args first and the
// flag pass then overwrites whatever it also names.

// The single settings table — one row per setting, shared by the config-file parser and the
// flag loop, so a flag cannot gain, lose, or re-range a setting without its config key doing
// the same ("every daemon flag becomes a key", #96). Deliberately NOT settings: --list-devices
// and --help command an action for one invocation, and a file that latently listed devices on
// every service start would be a trap; --config/--no-config locate the file and cannot sensibly
// live inside it.
struct Setting {
    const char* name;  // the long flag without its "--"; identical to the config-file key
    void (*apply)(Args&, const std::string& value, const std::string& where);
};
const Setting kSettings[] = {
    {"mode", [](Args& a, const std::string& v, const std::string&) { a.mode = v; }},
    {"capture", [](Args& a, const std::string& v, const std::string&) { a.capturePattern = v; }},
    {"capture-id",
     [](Args& a, const std::string& v, const std::string& w) {
         a.captureId = static_cast<int>(requireInt(w, v, 0, INT_MAX));
     }},
    {"playback", [](Args& a, const std::string& v, const std::string&) { a.playbackPattern = v; }},
    {"playback-id",
     [](Args& a, const std::string& v, const std::string& w) {
         a.playbackId = static_cast<int>(requireInt(w, v, 0, INT_MAX));
     }},
    {"transport", [](Args& a, const std::string& v, const std::string&) { a.transport = v; }},
    // rate/channels arm the #91 format-declaration refusal from the file exactly as from the
    // flag: a declared format is declared wherever the operator wrote it down, and the silent
    // mono/rate fallback would mislabel the audio either way.
    {"rate",
     [](Args& a, const std::string& v, const std::string& w) {
         a.rate = static_cast<int>(requireInt(w, v, 8000, 192000));
         a.formatDeclared = true;
     }},
    {"channels",
     [](Args& a, const std::string& v, const std::string& w) {
         a.channels = static_cast<int>(requireInt(w, v, 1, 2));
         a.formatDeclared = true;
     }},
    {"port",
     [](Args& a, const std::string& v, const std::string& w) {
         a.port = static_cast<int>(requireInt(w, v, 0, 65535));
     }},
    {"duration-ms",
     [](Args& a, const std::string& v, const std::string& w) {
         a.durationMs = requireInt(w, v, 0, LLONG_MAX);
     }},
};

const Setting* findSetting(const std::string& name) {
    for (const Setting& s : kSettings)
        if (name == s.name) return &s;
    return nullptr;
}

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

// The platform-conventional location (#96 names all three). The _WIN32 branch is written now,
// ahead of the Windows daemon port (#96 item 3), so lifting the NOT WIN32 build gate does not
// rework configuration. Empty when the location cannot be resolved (no HOME), in which case no
// default is read.
std::string defaultConfigPath() {
#if defined(_WIN32)
    const char* base = std::getenv("ProgramData");
    return std::string(base && *base ? base : "C:\\ProgramData") + "\\naudio\\daemon.conf";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (!home || !*home) return "";
    return std::string(home) + "/Library/Application Support/naudio/daemon.conf";
#else
    return "/etc/naudio/daemon.conf";
#endif
}

// Parse `path` into args: one `key = value` per line, '#' starts a comment line, outer
// whitespace trimmed (inner spaces survive, so device patterns need no quoting). Exits 2 on any
// malformed line, unknown key, or bad value, naming file:line — a typo'd key skipped silently
// would be the config-file version of the atoi defect requireInt exists to prevent: the daemon
// would run, on the wrong device or port, and report green.
void loadConfigFile(Args& args, const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "error: cannot read config file '%s'\n", path.c_str());
        std::exit(2);
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const std::string where = path + ":" + std::to_string(lineNo);
        const auto eq = t.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "error: %s: expected 'key = value', got '%s'\n", where.c_str(),
                         t.c_str());
            std::exit(2);
        }
        const std::string key = trim(t.substr(0, eq));
        const std::string value = trim(t.substr(eq + 1));
        const Setting* s = key.empty() ? nullptr : findSetting(key);
        if (!s) {
            std::fprintf(stderr, "error: %s: unknown key '%s'\n", where.c_str(), key.c_str());
            std::exit(2);
        }
        s->apply(args, value, where + ": " + key);
    }
}

// `required` distinguishes the operator-named --config file (absence is an error — they asked
// for THIS file) from the default location (absence means defaults apply). Any failure other
// than absence is loud either way: a config file that exists but cannot be read, skipped
// silently, would configure the daemon differently than the file the operator can see says.
void loadConfigIfPresent(Args& args, const std::string& path, bool required) {
    errno = 0;
    std::FILE* probe = std::fopen(path.c_str(), "r");
    if (!probe) {
        if (!required && (errno == ENOENT || errno == ENOTDIR)) return;
        std::fprintf(stderr, "error: cannot read config file '%s': %s\n", path.c_str(),
                     std::strerror(errno));
        std::exit(2);
    }
    std::fclose(probe);
    loadConfigFile(args, path);
}

}  // namespace

int main(int argc, char** argv) {
    Args args;

    // --- Pass 1: locate and apply the config file BEFORE the flag pass — that ordering IS the
    // precedence rule (defaults < config file < flags). --help suppresses config loading
    // entirely, so usage still prints on a machine whose config file is broken.
    std::string configPath;
    bool noConfig = false, explicitConfig = false, wantHelp = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") wantHelp = true;
        else if (a == "--no-config") noConfig = true;
        else if (a == "--config") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --config needs a value\n");
                return 2;
            }
            configPath = argv[++i];
            explicitConfig = true;
        }
    }
    if (explicitConfig && noConfig) {
        std::fprintf(stderr, "error: --config and --no-config are mutually exclusive\n");
        return 2;
    }
    if (!wantHelp && !noConfig) {
        if (explicitConfig) loadConfigIfPresent(args, configPath, /*required=*/true);
        else if (const std::string def = defaultConfigPath(); !def.empty())
            loadConfigIfPresent(args, def, /*required=*/false);
    }

    // --- Pass 2: flags overwrite whatever the config file set.
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--config") { (void)next("--config"); }  // consumed in pass 1
        else if (a == "--no-config") {}                   // consumed in pass 1
        else if (a == "--list-devices") args.listDevices = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (const Setting* s =
                     a.compare(0, 2, "--") == 0 ? findSetting(a.substr(2)) : nullptr) {
            s->apply(args, next(a.c_str()), a);
        }
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(); return 2; }
    }

    // Before the transport check so a bad rate is named as such even when the argcheck harness's
    // --transport guard is also on the command line. Still before any device work.
    if (args.rate % 50 != 0) {
        std::fprintf(stderr, "error: --rate %d does not make an exact 20 ms frame "
                             "(must be divisible by 50)\n", args.rate);
        return 2;
    }
    if (args.transport != "tcp" && args.transport != "udp" && args.transport != "dual") {
        std::fprintf(stderr, "error: invalid --transport '%s' (tcp|udp|dual)\n", args.transport.c_str());
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        if (args.listDevices) return runListDevices();
        if (args.mode == "capture-probe") return runCaptureProbe(args);
        if (args.mode == "hardware") return runHardware(args);
        std::fprintf(stderr, "error: invalid --mode '%s' (capture-probe|hardware)\n", args.mode.c_str());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
