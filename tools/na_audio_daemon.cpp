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
#include <cctype>
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
#include <type_traits>
#include <utility>
#include <vector>

// mkdir, for the control page creating the config file's parent directory.
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

// Set by the build (tools/CMakeLists.txt) so the control page can name the version it is part
// of. Defaulted so the file still compiles if it is ever built outside this project's CMake.
#ifndef NAUDIO_TOOL_VERSION
#define NAUDIO_TOOL_VERSION "unknown"
#endif

#include "naudio/AudioStreamConfig.hpp"
#include "naudio/DeviceEnumerator.hpp"
#include "naudio/FakeBackend.hpp"
#include "naudio/PortAudioBackend.hpp"
#include "naudio/Stream.hpp"
#include "naudio/StreamOpener.hpp"
#include "naudio/Types.hpp"
#include "naudio/net/AudioStreamClient.hpp"
#include "naudio/net/AudioStreamServer.hpp"
#include "naudio/net/Socket.hpp"

// The page, embedded at build time from tools/control_page.html and tools/control_page.js (see
// tools/CMakeLists.txt). Between them they reference NOTHING off this daemon — the machine may
// have no route to the internet, and a page that fetched a stylesheet would render unstyled
// exactly where it matters most. The script is a SECOND asset rather than an inline <script>
// precisely so the Content-Security-Policy in sendResponse() can forbid inline script outright.
//
// Declared OUTSIDE the anonymous namespace, and `extern` is load-bearing: a const array at
// namespace scope has INTERNAL linkage by default in C++, so either mistake makes this a
// declaration of a different object that nothing ever defines (measured — it links no symbol and
// fails at link time with "has internal linkage but is not defined").
extern const char kControlPageHtml[];
extern const char kControlPageJs[];

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
    // --- control mode (issue #96 item 4) ---
    int controlPort = 8737;   // localhost-only HTTP control page; see runControl()
    bool autostart = false;   // control mode: begin capturing at startup rather than on a click

    // Which settings BELONG IN THE FILE — the keys the config file already carried, plus the
    // ones the control page has just been asked to save. Not a setting itself; provenance.
    //
    // It exists because the page rewrites the file whole, and without it the rewrite would
    // persist values that arrived as FLAGS. The service units pass `--mode control` and
    // `--duration-ms 0` on the command line precisely so that no file can undo them (issue #96
    // items 1 and 2), so a page that wrote them back into the file would quietly reverse that
    // decision — and a later plain `na_audio_daemon` would start in control mode because a
    // service had once saved its settings.
    std::vector<std::string> persistKeys;
};

// Mark a setting as one the config file should carry. Idempotent: the same key arriving from
// the file and then from the page must not appear twice.
void markPersisted(Args& a, const std::string& key) {
    if (std::find(a.persistKeys.begin(), a.persistKeys.end(), key) == a.persistKeys.end())
        a.persistKeys.push_back(key);
}

std::string defaultConfigPath();  // defined with the config-file loader below

void usage() {
    const std::string confPath = defaultConfigPath();
    std::fprintf(
        stderr,
        "usage: na_audio_daemon [--mode capture-probe|hardware|control]\n"
        "                       [--capture <pat>|--capture-id N]\n"
        "                       [--playback <pat>|--playback-id N] [--transport tcp|udp|dual]\n"
        "                       [--port N] [--duration-ms N] [--config <file>|--no-config]\n"
        "                       [--control-port N] [--autostart true|false] [--open-page]\n"
        "       na_audio_daemon --list-devices\n\n"
        "  modes:\n"
        "    capture-probe  open the capture device directly; report frames/overflow/RMS\n"
        "                   (the real-hardware capture + no-overrun gate; capture only)\n"
        "    hardware       (default) server captures the radio RX -> 127.0.0.1 transport ->\n"
        "                   in-process RX-only client; measures sustained throughput + RMS\n"
        "    control        serve the localhost control page: pick a device, save settings,\n"
        "                   start/stop the stream and watch it, all from a browser. Captures\n"
        "                   nothing until you press Start (or set autostart = true)\n\n"
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
        "  --control-port N  control mode: the localhost port for the page (default %d)\n"
        "  --autostart B     control mode: true starts capturing at launch instead of waiting\n"
        "                    for the page's Start button (default false)\n"
        "  --open-page       control mode: open the page in the default browser at startup\n"
        "  --list-devices    enumerate capture + playback devices, then exit\n"
        "  -h, --help        print this message\n\n"
        "  config file:      one `key = value` per line, '#' starts a comment line; keys are\n"
        "                    the setting names above without their leading dashes (e.g.\n"
        "                    `transport = udp`, `capture-id = 3`). Read from --config <file>,\n"
        "                    else from %s\n"
        "                    when that exists. Flags always override the file.\n",
        naudio::AudioStreamConfig::DEFAULT_PORT, Args{}.controlPort,
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

// ---- Live status (issue #96 item 4) --------------------------------------------------------
// What a run in progress publishes for the control page. Every field is a number the hardware
// loop already computes for its own per-second stdout line — this struct is a window onto that
// loop, not a second measurement of it.
//
// The -1 fields are passed through as -1 deliberately. ClientStats is emphatic that -1 means
// "this connection does not measure it" and never "none happened": sequenceGaps is live on the
// UDP profiles and unmeasured on TCP, and rendering that as 0 would tell a TCP operator their
// link is perfect when nothing was counted at all. The page prints an em dash for -1.
struct LiveStatus {
    mutable std::mutex m;
    std::string state = "idle";  // idle | starting | running | stopping | error
    std::string error;           // set with state == "error"; cleared on the next start
    std::string transport;
    std::string captureName, sinkName;
    bool realSink = false;
    int port = 0;
    int clients = 0;
    std::int64_t startedMs = 0;
    std::int64_t rxBytes = 0, bps = 0, expectedBps = 0;
    double rmsL = 0.0, rmsR = 0.0;
    int clientErrors = 0, crcErrors = 0;
    std::int64_t queueDrops = 0, sequenceGaps = -1, fecRecovered = 0;
};

// Record a failure reason for the control page. runHardware's error paths already print to
// stderr (where the service log or the terminal shows them); this puts the same sentence
// somewhere a browser can read it, because a control page whose Start button just goes back to
// "idle" with no reason is the failure mode this whole item exists to remove.
void liveFail(LiveStatus* live, const std::string& why) {
    if (!live) return;
    std::lock_guard<std::mutex> lock(live->m);
    live->error = why;
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

// `stop` replaces the file-scope g_stop so control mode can stop THIS run without signalling the
// process, and `live` (nullable) is the control page's window into a run in progress — every
// number it publishes is one the loop below already computed for its own per-second line.
int runHardware(const Args& a, std::atomic<bool>& stop, LiveStatus* live) {
    naudio::PortAudioBackend backend;
    naudio::DeviceEnumerator enumerator(backend);

    // --- Resolve the radio's capture device (required). ---
    auto capDev = resolveCapture(enumerator, a);
    if (!capDev) {
        std::fprintf(stderr, "error: no capture device matched (try --list-devices / --capture-id)\n");
        liveFail(live, "no capture device matched — pick one from the device list");
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
        liveFail(live, "server.start failed: " + err +
                           (a.formatDeclared ? " (the device may not support the declared rate or "
                                               "channel count — naudio does not resample)"
                                             : ""));
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
        liveFail(live, "client.connect failed: " + err);
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
    if (live) {
        std::lock_guard<std::mutex> lock(live->m);
        live->state = "running";
        live->error.clear();
        live->transport = a.transport;
        live->port = boundPort;
        live->captureName = capDev->name;
        live->sinkName = realSink ? sinkDev->name : "";
        live->realSink = realSink;
        live->expectedBps = expectedBps;
        live->startedMs = start;
        live->rxBytes = live->bps = 0;
    }
    const std::int64_t deadline = a.durationMs > 0 ? start + a.durationMs : 0;
    std::int64_t prevBytes = 0, prevMs = start, minBps = -1;
    while (!stop.load() && (deadline == 0 || nowMs() < deadline)) {
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
        if (live) {
            // Both snapshots are taken here, on the monitor thread, so the page never reaches
            // into the server or the client itself.
            const naudio::net::ServerStats ss = server.stats();
            const naudio::net::ClientStats cs = client.stats();
            std::lock_guard<std::mutex> lock(live->m);
            live->rxBytes = s.bytes;
            live->bps = bps;
            live->rmsL = s.rmsL;
            live->rmsR = s.rmsR;
            live->clients = ss.clientsConnected;
            live->crcErrors = ss.crcErrors;
            live->queueDrops = ss.queueDrops;
            live->clientErrors = listener.errors.load();
            live->sequenceGaps = cs.sequenceGaps;   // -1 on TCP: unmeasured, NOT zero
            live->fecRecovered = cs.packetsRecoveredByFec;
        }
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
// must stop the run, not quietly select a different radio. `where` names the source in the
// message: the flag itself on the command line, "<file>:<line>: <key>" from a config file.
//
// It REPORTS rather than exits (issue #96 item 4). It used to call std::exit(2) directly, which
// is right for a command line and fatal for a control page: the same table validates a value
// arriving over HTTP, where a typo owes the operator a 400 and a message, not a daemon that
// vanishes mid-session. The exit still happens — at the two call sites that are a command line
// (the flag loop and the config-file reader), which print `error: <msg>` and exit 2 exactly as
// before, byte for byte. The message text lives here so all three paths speak with one voice.
bool parseInt(const std::string& where, const std::string& text, long long lo, long long hi,
              long long& out, std::string& err) {
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    if (text.empty() || end == text.c_str() || *end != '\0') {
        err = where + " expects an integer, got '" + text + "'";
        return false;
    }
    if (errno == ERANGE || v < lo || v > hi) {
        err = where + " must be in [" + std::to_string(lo) + ", " + std::to_string(hi) +
              "], got '" + text + "'";
        return false;
    }
    out = v;
    return true;
}

// The bool settings (issue #96 item 4's `autostart`). Deliberately strict and deliberately NOT
// "anything non-empty is true": a config file saying `autostart = flase` must refuse, not
// silently mean false, for the same reason `--capture-id usb` must not mean device 0.
bool parseBool(const std::string& where, const std::string& text, bool& out, std::string& err) {
    if (text == "true" || text == "yes" || text == "on" || text == "1") { out = true; return true; }
    if (text == "false" || text == "no" || text == "off" || text == "0") { out = false; return true; }
    err = where + " expects true or false, got '" + text + "'";
    return false;
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
    // Returns false and fills `err` on a bad value; the two command-line callers turn that into
    // `error: <err>` + exit 2, and the control page turns it into a 400. Captureless lambdas, so
    // these convert to plain function pointers — a capture would break the table's type.
    bool (*apply)(Args&, const std::string& value, const std::string& where, std::string& err);
    // What the control page needs to render an input for this key without a second table that
    // could drift from this one (issue #96 item 4). `kind` picks the widget; lo/hi bound the
    // number inputs and are the SAME literals the apply lambda validates against, one line above.
    const char* kind;  // "text" | "int" | "bool" | "enum:a,b,c"
    long long lo, hi;  // meaningful for kind == "int"
};

// Small helpers so each row states its range exactly once (the lambda and the metadata below it
// would otherwise be two copies of the same numbers, free to drift — L118's shape in a table).
#define NA_INT_ROW(key, field, lo, hi, cast)                                                     \
    {key,                                                                                        \
     [](Args& a, const std::string& v, const std::string& w, std::string& e) {                   \
         long long n = 0;                                                                        \
         if (!parseInt(w, v, lo, hi, n, e)) return false;                                         \
         a.field = static_cast<cast>(n);                                                          \
         return true;                                                                            \
     },                                                                                          \
     "int", lo, hi}

const Setting kSettings[] = {
    {"mode",
     [](Args& a, const std::string& v, const std::string&, std::string&) { a.mode = v; return true; },
     "enum:hardware,capture-probe,control", 0, 0},
    {"capture",
     [](Args& a, const std::string& v, const std::string&, std::string&) { a.capturePattern = v; return true; },
     "text", 0, 0},
    NA_INT_ROW("capture-id", captureId, 0, INT_MAX, int),
    {"playback",
     [](Args& a, const std::string& v, const std::string&, std::string&) { a.playbackPattern = v; return true; },
     "text", 0, 0},
    NA_INT_ROW("playback-id", playbackId, 0, INT_MAX, int),
    {"transport",
     [](Args& a, const std::string& v, const std::string&, std::string&) { a.transport = v; return true; },
     "enum:tcp,udp,dual", 0, 0},
    // rate/channels arm the #91 format-declaration refusal from the file exactly as from the
    // flag: a declared format is declared wherever the operator wrote it down, and the silent
    // mono/rate fallback would mislabel the audio either way.
    {"rate",
     [](Args& a, const std::string& v, const std::string& w, std::string& e) {
         long long n = 0;
         if (!parseInt(w, v, 8000, 192000, n, e)) return false;
         a.rate = static_cast<int>(n);
         a.formatDeclared = true;
         return true;
     },
     "int", 8000, 192000},
    {"channels",
     [](Args& a, const std::string& v, const std::string& w, std::string& e) {
         long long n = 0;
         if (!parseInt(w, v, 1, 2, n, e)) return false;
         a.channels = static_cast<int>(n);
         a.formatDeclared = true;
         return true;
     },
     "int", 1, 2},
    NA_INT_ROW("port", port, 0, 65535, int),
    NA_INT_ROW("duration-ms", durationMs, 0, LLONG_MAX, std::int64_t),
    // --- issue #96 item 4 -------------------------------------------------------------------
    // The control page's own two settings, and they are settings rather than flags-only for the
    // same reason every other row is: the service unit's argv is fixed at install time, so
    // anything an operator can change has to live in the file the page writes.
    // 0 is legal and means "ask the OS for a free port", the same spelling --port already uses.
    // A service would not want it (the URL then changes at every start), but a second daemon on
    // one machine, and the hardware-free gate, both need it.
    NA_INT_ROW("control-port", controlPort, 0, 65535, int),
    // "Start capturing as soon as the daemon starts." This is how "it just works at every logon"
    // is expressed now that the service runs `--mode control`: the unit stays inert-by-default
    // and the OPERATOR opts in, in the file, through a checkbox — rather than the package
    // deciding for them. Ignored outside control mode (hardware mode always captures).
    {"autostart",
     [](Args& a, const std::string& v, const std::string& w, std::string& e) {
         return parseBool(w, v, a.autostart, e);
     },
     "bool", 0, 0},
};
#undef NA_INT_ROW

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
        std::string verr;
        if (!s->apply(args, value, where + ": " + key, verr)) {
            std::fprintf(stderr, "error: %s\n", verr.c_str());
            std::exit(2);
        }
        // The file said this key, so a rewrite keeps saying it.
        markPersisted(args, key);
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


// ============================================================================================
// The control page (issue #96 item 4)
// ============================================================================================
//
// `--mode control` serves a LOCALHOST-ONLY HTTP page: a device picker fed by the same
// DeviceEnumerator --list-devices uses, the settings from item 1 rendered as a form that writes
// the config file, start / stop / restart for the streaming pipeline, and the live numbers the
// hardware loop already computes. One implementation for all three platforms, no GUI toolkit, no
// dependency: it is ~400 lines of HTTP over naudio's own Socket, which the daemon already links.
//
// WHY THE DAEMON AND NOT A SEPARATE PROGRAM. The page's whole job is to configure and drive THIS
// process. A separate GUI would have to locate, launch, signal and monitor the daemon across
// three platforms' process models — every one of which is a thing that can be wrong — to end up
// where an in-process page starts.
//
// THE THREAT MODEL, stated rather than implied.
//   * The listener binds 127.0.0.1 and nothing else, so it is not reachable from the network.
//     Remote management is an explicit non-goal of #96.
//   * A LOCAL user on this machine can reach it. There is no token: on a single-user desktop it
//     would be friction with nothing behind it, and on a shared machine the audio device is
//     already reachable by any local process. Said plainly here so nobody infers otherwise.
//   * The real exposure a localhost service has is the OPERATOR'S OWN BROWSER, and it is
//     defended, because a page on the open internet can make a browser send requests here:
//       - Host: must name 127.0.0.1 or localhost. This is the DNS-rebinding defence — an
//         attacker's hostname resolving to 127.0.0.1 arrives with THEIR name in Host.
//       - Origin:, when present, must be our own. A cross-site fetch carries the attacker's.
//       - Every mutating request must be Content-Type: application/json, which an HTML form
//         cannot produce and a cross-origin fetch cannot set without a preflight we refuse.
//     The first two are cheap; the third is the one that stops a plain <form> POST.

// --- JSON out ------------------------------------------------------------------------------
// Hand-rolled and deliberately so: this writes flat objects of strings and numbers, which is
// less code than any dependency's initialisation, and the daemon takes no new dependency.
std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                // Control characters are the only other thing JSON forbids raw. A device name
                // is whatever the host API said it was, so this is not hypothetical.
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof esc, "\\u%04x", static_cast<unsigned char>(c));
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    return out;
}
std::string jstr(const std::string& v) { return "\"" + jsonEscape(v) + "\""; }
// ONE template rather than an overload per integer type, and the reason is a portability trap
// worth naming: `int`, `long` and `long long` are three DISTINCT types, and which of them
// std::int64_t names is platform-dependent — `long` on LP64 Linux, `long long` on macOS and
// Windows. Overloads for `int` and `long long` are therefore exactly right on macOS and
// AMBIGUOUS on Linux, where an int64_t argument matches neither exactly and `long -> double`
// is as good a conversion as `long -> long long` (measured: GCC refused seven call sites that
// clang had accepted). A constrained template has no such gap, and `double` still binds to its
// own non-template overload exactly.
template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
std::string jnum(T v) { return std::to_string(v); }
std::string jnum(double v) {
    if (!std::isfinite(v)) return "null";  // JSON has no NaN/Infinity; dbfs() can floor, not blow up
    char b[40];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

// --- JSON in -------------------------------------------------------------------------------
// A STRICT parser for exactly one shape: a flat object whose every value is a string
// ({"transport":"udp","port":"4533"}). Anything else — nesting, numbers, arrays, trailing
// commas — is refused rather than coerced. That is not a limitation to apologise for: the only
// producer is the page below, the settings table takes strings anyway, and a parser that
// accepts more than its one caller emits is surface with no test behind it.
bool parseFlatJsonObject(const std::string& in,
                         std::vector<std::pair<std::string, std::string>>& out, std::string& err) {
    std::size_t i = 0;
    auto skipWs = [&] { while (i < in.size() && std::isspace(static_cast<unsigned char>(in[i]))) ++i; };
    auto parseString = [&](std::string& v) -> bool {
        if (i >= in.size() || in[i] != '"') { err = "expected a string at byte " + std::to_string(i); return false; }
        ++i;
        v.clear();
        while (i < in.size()) {
            const char c = in[i++];
            if (c == '"') return true;
            if (c != '\\') { v += c; continue; }
            if (i >= in.size()) break;
            const char e = in[i++];
            switch (e) {
                case '"': v += '"'; break;
                case '\\': v += '\\'; break;
                case '/': v += '/'; break;
                case 'b': v += '\b'; break;
                case 'f': v += '\f'; break;
                case 'n': v += '\n'; break;
                case 'r': v += '\r'; break;
                case 't': v += '\t'; break;
                case 'u': {
                    // Only the ASCII range is decoded. Every value this page sends is a setting
                    // value the daemon will compare against ASCII keywords or parse as an
                    // integer, so a \u escape outside ASCII is refused rather than mangled.
                    if (i + 4 > in.size()) { err = "truncated \\u escape"; return false; }
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = in[i + static_cast<std::size_t>(k)];
                        const int d = (h >= '0' && h <= '9')   ? h - '0'
                                      : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                                      : (h >= 'A' && h <= 'F') ? h - 'A' + 10
                                                               : -1;
                        if (d < 0) { err = "bad \\u escape"; return false; }
                        code = code * 16 + static_cast<unsigned>(d);
                    }
                    if (code == 0 || code > 0x7f) { err = "only ASCII \\u escapes are accepted"; return false; }
                    i += 4;
                    v += static_cast<char>(code);
                    break;
                }
                default: err = "unknown escape '\\"; err += e; err += "'"; return false;
            }
        }
        err = "unterminated string";
        return false;
    };

    skipWs();
    if (i >= in.size() || in[i] != '{') { err = "body is not a JSON object"; return false; }
    ++i;
    skipWs();
    if (i < in.size() && in[i] == '}') return true;  // {} is legal and means "change nothing"
    for (;;) {
        skipWs();
        std::string k, v;
        if (!parseString(k)) return false;
        skipWs();
        if (i >= in.size() || in[i] != ':') { err = "expected ':' after key '" + k + "'"; return false; }
        ++i;
        skipWs();
        if (!parseString(v)) { err = "value for '" + k + "' must be a string (" + err + ")"; return false; }
        out.emplace_back(k, v);
        skipWs();
        if (i < in.size() && in[i] == ',') { ++i; continue; }
        if (i < in.size() && in[i] == '}') { ++i; break; }
        err = "expected ',' or '}'";
        return false;
    }
    skipWs();
    if (i != in.size()) { err = "trailing bytes after the object"; return false; }
    return true;
}

// --- rendering the current settings ----------------------------------------------------------
// Every value below is produced FROM Args by the same key name the settings table parses, so a
// round trip (read file -> render -> write file -> read file) is the identity. There is no
// second list of key names anywhere: the order here follows kSettings and a new row shows up in
// the page and the written file without touching this function's callers.
std::string settingValue(const Args& a, const std::string& key) {
    if (key == "mode") return a.mode;
    if (key == "capture") return a.capturePattern;
    if (key == "capture-id") return a.captureId < 0 ? "" : std::to_string(a.captureId);
    if (key == "playback") return a.playbackPattern;
    if (key == "playback-id") return a.playbackId < 0 ? "" : std::to_string(a.playbackId);
    if (key == "transport") return a.transport;
    if (key == "rate") return std::to_string(a.rate);
    if (key == "channels") return std::to_string(a.channels);
    if (key == "port") return std::to_string(a.port);
    if (key == "duration-ms") return std::to_string(a.durationMs);
    if (key == "control-port") return std::to_string(a.controlPort);
    if (key == "autostart") return a.autostart ? "true" : "false";
    return "";
}

// The config file the page writes.
//
// TWO filters, and each one exists to stop a different wrong line appearing:
//   * Only keys in `persistKeys` — what the file already carried, plus what the page just saved.
//     Without this the rewrite would persist FLAGS, and the two flags the service units pass
//     (`--mode control`, `--duration-ms 0`) are exactly the ones that must never become file
//     settings: they are on the command line so that no file can undo them.
//   * Only values that differ from the default. A file full of defaults freezes them, so a
//     later naudio that changed one would be silently overridden by a value the operator never
//     chose. A setting returned to its default therefore leaves the file, which is the same
//     thing as saying it.
std::string renderConfigFile(const Args& a) {
    const Args defaults;
    std::string out =
        "# na_audio_daemon configuration — written by the naudio control page.\n"
        "# One `key = value` per line; '#' starts a comment. Keys are the command-line flags\n"
        "# without their leading dashes, and a flag on the command line still overrides this\n"
        "# file. See na_audio_daemon(1).\n"
        "#\n"
        "# Hand edits survive a rewrite only as VALUES: this file is regenerated whenever the\n"
        "# control page saves, so comments added below are lost. Edit here or there, not both.\n";
    // kSettings order, not persistKeys order, so the file's layout is stable across saves
    // rather than reflecting the order in which keys happened to be touched.
    for (const Setting& st : kSettings) {
        const std::string key = st.name;
        if (std::find(a.persistKeys.begin(), a.persistKeys.end(), key) == a.persistKeys.end())
            continue;
        const std::string v = settingValue(a, key);
        if (v.empty() || v == settingValue(defaults, key)) continue;
        out += key + " = " + v + "\n";
    }
    return out;
}

// --- writing the config file -----------------------------------------------------------------
bool makeParentDirs(const std::string& path, std::string& err) {
    // Walk the path creating each component. EEXIST is success — a race with another writer
    // creating the same directory is the expected case, not a failure.
    for (std::size_t i = 1; i < path.size(); ++i) {
        const char c = path[i];
        if (c != '/' && c != '\\') continue;
        const std::string dir = path.substr(0, i);
        if (dir.empty()) continue;
#if defined(_WIN32)
        // "C:" is a drive, not a directory to create.
        if (dir.size() == 2 && dir[1] == ':') continue;
        if (_mkdir(dir.c_str()) != 0 && errno != EEXIST) {
#else
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
#endif
            err = "cannot create directory '" + dir + "': " + std::strerror(errno);
            return false;
        }
    }
    return true;
}

// Write via a temporary in the SAME directory, then rename over the target. A control page that
// truncated the config file and then failed to write it would leave the daemon unable to start
// with nothing to restore; rename is the only step that is atomic on both platforms.
bool writeConfigFile(const std::string& path, const std::string& text, std::string& err) {
    if (!makeParentDirs(path, err)) return false;
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { err = "cannot open '" + tmp + "' for writing"; return false; }
        out << text;
        out.flush();
        if (!out) { err = "write to '" + tmp + "' failed"; return false; }
    }
#if defined(_WIN32)
    // POSIX rename() replaces an existing target; the Windows CRT's does NOT, it fails with
    // EEXIST. So the old file must go first — which is why the temporary is written and flushed
    // above before anything is removed.
    std::remove(path.c_str());
#endif
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        err = "cannot replace '" + path + "': " + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// --- the pipeline, started and stopped by the page --------------------------------------------
// Owns at most one hardware run at a time on its own thread. runHardware() is unchanged in what
// it does; all this adds is a per-run stop flag (so a click stops the run without signalling the
// process) and a joinable thread.
class Pipeline {
public:
    LiveStatus live;

    bool running() const { return running_.load(); }

    // `a` is copied: a run must not observe a settings change made while it is in flight, and the
    // control page can save the config file at any moment.
    bool start(Args a, std::string& err) {
        std::lock_guard<std::mutex> lock(m_);
        if (running_.load()) { err = "already streaming"; return false; }
        reap();
        {
            std::lock_guard<std::mutex> ls(live.m);
            live.state = "starting";
            live.error.clear();
        }
        stop_.store(false);
        running_.store(true);
        a.mode = "hardware";      // control mode's own value must not recurse into itself
        a.durationMs = 0;         // a run started from the page ends when the page says so
        thread_ = std::thread([this, a] {
            const int rc = runHardware(a, stop_, &live);
            std::lock_guard<std::mutex> ls(live.m);
            // An error string already set by runHardware wins — it is the specific one.
            if (rc != 0 && live.error.empty()) live.error = "the pipeline exited with status " +
                                                            std::to_string(rc);
            live.state = live.error.empty() ? "idle" : "error";
            live.clients = 0;
            live.bps = 0;
            running_.store(false);
        });
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(m_);
        if (thread_.joinable()) {
            {
                std::lock_guard<std::mutex> ls(live.m);
                if (live.state == "running" || live.state == "starting") live.state = "stopping";
            }
            stop_.store(true);
            thread_.join();
        }
        running_.store(false);
    }

    ~Pipeline() { stop(); }

private:
    // A finished run leaves a joinable thread behind; join it before starting the next one, or
    // std::thread::operator= terminates the process.
    void reap() { if (thread_.joinable()) thread_.join(); }

    mutable std::mutex m_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
};

// --- HTTP --------------------------------------------------------------------------------------
struct HttpRequest {
    std::string method, target, body;
    std::string host, origin, contentType;
};

constexpr std::size_t kMaxHead = 8 * 1024;
constexpr std::size_t kMaxBody = 64 * 1024;

std::string lower(std::string v) {
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
}

// Reads one request. Returns false on anything malformed, oversized, or truncated — the caller
// closes the connection without a response, which is the right answer to a peer that is not
// speaking HTTP.
bool readRequest(naudio::net::Socket& sock, HttpRequest& req) {
    std::string buf;
    char chunk[2048];
    std::size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos) {
        if (buf.size() > kMaxHead) return false;
        const naudio::net::RecvResult r = sock.recv(chunk, sizeof chunk);
        if (r.status != naudio::net::IoStatus::Ok || r.bytes == 0) return false;
        buf.append(chunk, r.bytes);
        headEnd = buf.find("\r\n\r\n");
    }
    const std::string head = buf.substr(0, headEnd);
    std::size_t lineEnd = head.find("\r\n");
    if (lineEnd == std::string::npos) lineEnd = head.size();
    {
        const std::string line = head.substr(0, lineEnd);
        const std::size_t sp1 = line.find(' ');
        if (sp1 == std::string::npos) return false;
        const std::size_t sp2 = line.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return false;
        req.method = line.substr(0, sp1);
        req.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    }
    std::size_t contentLength = 0;
    std::size_t pos = lineEnd + 2;
    while (pos < head.size()) {
        std::size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) e = head.size();
        const std::string line = head.substr(pos, e - pos);
        pos = e + 2;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) value.pop_back();
        if (name == "host") req.host = value;
        else if (name == "origin") req.origin = value;
        else if (name == "content-type") req.contentType = value;
        else if (name == "content-length") {
            long long n = 0;
            std::string e2;
            if (!parseInt("content-length", value, 0, static_cast<long long>(kMaxBody), n, e2))
                return false;
            contentLength = static_cast<std::size_t>(n);
        }
    }
    req.body = buf.substr(headEnd + 4);
    while (req.body.size() < contentLength) {
        const naudio::net::RecvResult r = sock.recv(chunk, sizeof chunk);
        if (r.status != naudio::net::IoStatus::Ok || r.bytes == 0) return false;
        req.body.append(chunk, r.bytes);
        if (req.body.size() > kMaxBody) return false;
    }
    req.body.resize(contentLength);
    return true;
}

void sendResponse(naudio::net::Socket& sock, int status, const std::string& reason,
                  const std::string& contentType, const std::string& body) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    head += "Content-Type: " + contentType + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Connection: close\r\n";
    // The page loads nothing from anywhere, so it may be forbidden from trying. These also make
    // the answer useless to an attacker's page that somehow reaches it: no framing, no sniffing,
    // and nothing cached to a disk another program can read.
    head += "Content-Security-Policy: default-src 'none'; script-src 'self'; "
            "style-src 'self' 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; "
            "frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n";
    head += "X-Content-Type-Options: nosniff\r\n";
    head += "Cache-Control: no-store\r\n";
    head += "Referrer-Policy: no-referrer\r\n";
    head += "\r\n";
    sock.sendAll(head.data(), head.size());
    if (!body.empty()) sock.sendAll(body.data(), body.size());
}

void sendJson(naudio::net::Socket& sock, int status, const std::string& reason,
              const std::string& json) {
    sendResponse(sock, status, reason, "application/json; charset=utf-8", json);
}
void sendError(naudio::net::Socket& sock, int status, const std::string& reason,
               const std::string& message) {
    sendJson(sock, status, reason, "{\"error\":" + jstr(message) + "}");
}



// --- the server -------------------------------------------------------------------------------
class ControlServer {
public:
    ControlServer(Args settings, std::string configPath, Pipeline& pipeline)
        : settings_(std::move(settings)), configPath_(std::move(configPath)), pipeline_(pipeline) {}

    // Binds and starts accepting. `port` is in/out: 0 asks the OS for an ephemeral one, and the
    // bound port is written back so the caller can print (and open) the real URL.
    bool start(int& port, std::string& err) {
        listener_ = naudio::net::Socket::listenTcp("127.0.0.1", static_cast<std::uint16_t>(port),
                                                   /*ownsPort=*/true, &err);
        if (!listener_.valid()) return false;
        port = listener_.localPort();
        port_ = port;
        accepter_ = std::thread([this] { acceptLoop(); });
        return true;
    }

    void stop() {
        stopping_.store(true);
        listener_.close();
        if (accepter_.joinable()) accepter_.join();
        std::vector<Worker> workers;
        {
            std::lock_guard<std::mutex> lock(workersMutex_);
            workers.swap(workers_);
        }
        for (Worker& w : workers)
            if (w.first.joinable()) w.first.join();
    }

    ~ControlServer() { stop(); }

private:
    void acceptLoop() {
        while (!stopping_.load()) {
            naudio::net::Socket conn;
            std::string err;
            // A 200 ms deadline rather than a blocking accept: closing the listener from another
            // thread to wake a blocked accept() is exactly the use-after-close race Socket.hpp
            // warns about, so the loop wakes on its own instead.
            const naudio::net::IoStatus st = listener_.acceptTcp(200, conn, &err);
            if (st == naudio::net::IoStatus::TimedOut) { reapWorkers(); continue; }
            if (st != naudio::net::IoStatus::Ok) {
                if (stopping_.load()) return;
                reapWorkers();
                continue;
            }
            reapWorkers();
            std::size_t live = 0;
            {
                std::lock_guard<std::mutex> lock(workersMutex_);
                live = workers_.size();
            }
            if (live >= kMaxConnections) {
                // Browsers open speculative connections they never use; a cap keeps one page
                // from parking an unbounded number of threads here.
                sendError(conn, 503, "Service Unavailable", "too many concurrent connections");
                continue;
            }
            // Neither deadline is optional: without them one connection that opens and then says
            // nothing parks a thread until the process exits.
            conn.setRecvTimeout(5000);
            conn.setSendTimeout(5000);
            std::lock_guard<std::mutex> lock(workersMutex_);
            auto sock = std::make_shared<naudio::net::Socket>(std::move(conn));
            // Each worker carries its OWN done flag. A single shared counter is not enough:
            // it says how MANY workers finished, never WHICH, so the reaper below would join
            // the first joinable thread in the vector — quite possibly one still mid-request —
            // and block the accept loop behind it for up to the 5 s recv deadline.
            auto done = std::make_shared<std::atomic<bool>>(false);
            workers_.emplace_back(std::thread([sock, done, this] {
                                      HttpRequest req;
                                      if (readRequest(*sock, req)) handle(*sock, req);
                                      sock->close();
                                      done->store(true);
                                  }),
                                  done);
        }
    }

    // Joining finished workers from the accept loop keeps the vector from growing for the life
    // of the process. A worker is joined ONLY once its own flag says it has returned, so this
    // never blocks the accept loop behind a request still in flight — which, with a 5 s recv
    // deadline on every connection, would otherwise stall the whole server for five seconds
    // because one browser opened a speculative socket and said nothing on it.
    void reapWorkers() {
        std::lock_guard<std::mutex> lock(workersMutex_);
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (it->second->load()) {
                if (it->first.joinable()) it->first.join();
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // The three browser defences from the header comment. Returns false having already answered.
    bool authorize(naudio::net::Socket& sock, const HttpRequest& req) {
        const std::string expect1 = "127.0.0.1:" + std::to_string(port_);
        const std::string expect2 = "localhost:" + std::to_string(port_);
        if (req.host != expect1 && req.host != expect2) {
            sendError(sock, 421, "Misdirected Request",
                      "this page is served on 127.0.0.1 only; the request named host '" +
                          req.host + "'");
            return false;
        }
        if (!req.origin.empty() && req.origin != "http://" + expect1 &&
            req.origin != "http://" + expect2) {
            sendError(sock, 403, "Forbidden", "cross-origin request refused");
            return false;
        }
        if (req.method != "GET" && lower(req.contentType).rfind("application/json", 0) != 0) {
            sendError(sock, 415, "Unsupported Media Type",
                      "this endpoint accepts application/json only");
            return false;
        }
        return true;
    }

    void handle(naudio::net::Socket& sock, const HttpRequest& req) {
        if (!authorize(sock, req)) return;
        // Query strings and fragments are not used by any endpoint; strip so "/api/state?x=1"
        // routes rather than 404s.
        std::string path = req.target.substr(0, req.target.find_first_of("?#"));

        if (req.method == "GET" && (path == "/" || path == "/index.html")) {
            sendResponse(sock, 200, "OK", "text/html; charset=utf-8", kControlPageHtml);
            return;
        }
        if (req.method == "GET" && path == "/app.js") {
            sendResponse(sock, 200, "OK", "text/javascript; charset=utf-8", kControlPageJs);
            return;
        }
        if (req.method == "GET" && path == "/api/state")   { getState(sock);   return; }
        if (req.method == "GET" && path == "/api/devices") { getDevices(sock); return; }
        if (req.method == "POST" && path == "/api/config") { postConfig(sock, req); return; }
        if (req.method == "POST" && path == "/api/stream") { postStream(sock, req); return; }
        if (req.method == "POST" && path == "/api/quit")   { postQuit(sock);        return; }
        sendError(sock, 404, "Not Found", "no such endpoint: " + path);
    }

    // --- endpoints ---------------------------------------------------------------------------
    std::string settingsJson() {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        std::string out = "{";
        bool first = true;
        for (const Setting& st : kSettings) {
            if (!first) out += ",";
            first = false;
            out += jstr(st.name) + ":{" + "\"value\":" + jstr(settingValue(settings_, st.name)) +
                   ",\"kind\":" + jstr(st.kind);
            if (std::string(st.kind) == "int")
                out += ",\"lo\":" + jnum(st.lo) + ",\"hi\":" + jnum(st.hi);
            out += "}";
        }
        return out + "}";
    }

    std::string liveJson() {
        std::lock_guard<std::mutex> lock(pipeline_.live.m);
        const LiveStatus& l = pipeline_.live;
        const double pct = l.expectedBps > 0
                               ? 100.0 * static_cast<double>(l.bps) / static_cast<double>(l.expectedBps)
                               : 0.0;
        std::string out = "{";
        out += "\"state\":" + jstr(l.state);
        out += ",\"error\":" + jstr(l.error);
        out += ",\"transport\":" + jstr(l.transport);
        out += ",\"captureName\":" + jstr(l.captureName);
        out += ",\"sinkName\":" + jstr(l.sinkName);
        out += ",\"realSink\":" + std::string(l.realSink ? "true" : "false");
        out += ",\"port\":" + jnum(l.port);
        out += ",\"clients\":" + jnum(l.clients);
        out += ",\"uptimeMs\":" + jnum(l.startedMs > 0 && l.state == "running" ? nowMs() - l.startedMs : 0);
        out += ",\"rxBytes\":" + jnum(l.rxBytes);
        out += ",\"bps\":" + jnum(l.bps);
        out += ",\"expectedBps\":" + jnum(l.expectedBps);
        out += ",\"deliveryPct\":" + jnum(pct);
        out += ",\"rmsL\":" + jnum(dbfs(l.rmsL));
        out += ",\"rmsR\":" + jnum(dbfs(l.rmsR));
        out += ",\"clientErrors\":" + jnum(l.clientErrors);
        out += ",\"crcErrors\":" + jnum(l.crcErrors);
        out += ",\"queueDrops\":" + jnum(l.queueDrops);
        // -1 travels as -1. The page renders it as an em dash, never as zero: on TCP nothing
        // counts sequence gaps, and "0 gaps" would be a claim no one measured.
        out += ",\"sequenceGaps\":" + jnum(l.sequenceGaps);
        out += ",\"fecRecovered\":" + jnum(l.fecRecovered);
        return out + "}";
    }

    void getState(naudio::net::Socket& sock) {
        std::string out = "{";
        out += "\"version\":" + jstr(NAUDIO_TOOL_VERSION);
        out += ",\"configPath\":" + jstr(configPath_);
        out += ",\"settings\":" + settingsJson();
        out += ",\"stream\":" + liveJson();
        {
            std::lock_guard<std::mutex> lock(devicesMutex_);
            out += ",\"devices\":" + (devicesJson_.empty() ? std::string("null") : devicesJson_);
            out += ",\"devicesStale\":" + std::string(devicesStale_ ? "true" : "false");
        }
        out += "}";
        sendJson(sock, 200, "OK", out);
    }

    static std::string deviceArrayJson(const std::vector<naudio::DeviceInfo>& devs,
                                       naudio::Direction dir) {
        std::string out = "[";
        for (std::size_t i = 0; i < devs.size(); ++i) {
            const naudio::DeviceInfo& d = devs[i];
            if (i) out += ",";
            // backendIdFor, never backendId: on an ALSA-style split record the two directions
            // live on different ids, and the id the picker writes into the config file must be
            // the one StreamOpener will actually open.
            out += "{\"id\":" + jnum(d.backendIdFor(dir));
            out += ",\"name\":" + jstr(d.name);
            out += ",\"hostApi\":" + jstr(d.hostApi);
            out += ",\"inputs\":" + jnum(d.maxInputChannels);
            out += ",\"outputs\":" + jnum(d.maxOutputChannels);
            out += ",\"defaultRate\":" + jnum(d.defaultSampleRate);
            out += ",\"virtual\":" +
                   std::string(d.type == naudio::DeviceType::Virtual ? "true" : "false");
            out += "}";
        }
        return out + "]";
    }

    // Enumerating means Pa_Initialize / Pa_Terminate, which PortAudio does not make thread-safe
    // against a run that is opening streams on another thread. So the device list is refreshed
    // only while the pipeline is STOPPED, and the last list is served (marked stale) otherwise.
    // That is a real limit, stated in the page rather than hidden: a device plugged in while
    // streaming appears after a stop.
    void getDevices(naudio::net::Socket& sock) {
        if (pipeline_.running()) {
            std::lock_guard<std::mutex> lock(devicesMutex_);
            devicesStale_ = true;
            sendJson(sock, 200, "OK",
                     "{\"devices\":" + (devicesJson_.empty() ? std::string("null") : devicesJson_) +
                         ",\"stale\":true}");
            return;
        }
        std::string json;
        try {
            naudio::PortAudioBackend backend;
            naudio::DeviceEnumerator en(backend);
            json = "{\"capture\":" + deviceArrayJson(en.captureDevices(), naudio::Direction::Capture) +
                   ",\"playback\":" +
                   deviceArrayJson(en.playbackDevices(), naudio::Direction::Playback) + "}";
        } catch (const std::exception& e) {
            sendError(sock, 500, "Internal Server Error",
                      std::string("device enumeration failed: ") + e.what());
            return;
        }
        {
            std::lock_guard<std::mutex> lock(devicesMutex_);
            devicesJson_ = json;
            devicesStale_ = false;
        }
        sendJson(sock, 200, "OK", "{\"devices\":" + json + ",\"stale\":false}");
    }

    void postConfig(naudio::net::Socket& sock, const HttpRequest& req) {
        std::vector<std::pair<std::string, std::string>> kv;
        std::string err;
        if (!parseFlatJsonObject(req.body, kv, err)) {
            sendError(sock, 400, "Bad Request", "malformed request body: " + err);
            return;
        }
        // Validate into a COPY first, so a body whose third key is bad leaves the daemon's
        // settings exactly as they were rather than half-applied.
        Args candidate;
        {
            std::lock_guard<std::mutex> lock(settingsMutex_);
            candidate = settings_;
        }
        for (const auto& [key, value] : kv) {
            const Setting* st = findSetting(key);
            if (!st) {
                sendError(sock, 400, "Bad Request", "unknown setting '" + key + "'");
                return;
            }
            // An empty value clears a text setting (no capture pattern, no forced id) — the
            // page's way of saying "unset". The numeric rows have no empty spelling, so they are
            // refused by the same parser the command line uses.
            if (value.empty() && std::string(st->kind) == "text") {
                if (!st->apply(candidate, "", "--" + key, err)) {
                    sendError(sock, 400, "Bad Request", err);
                    return;
                }
                // Cleared, not unset-from-the-file: renderConfigFile drops empty values, so
                // marking it keeps the bookkeeping uniform without emitting a blank line.
                markPersisted(candidate, key);
                continue;
            }
            if (value.empty() && key == "capture-id") { candidate.captureId = -1; continue; }
            if (value.empty() && key == "playback-id") { candidate.playbackId = -1; continue; }
            if (!st->apply(candidate, value, "--" + key, err)) {
                sendError(sock, 400, "Bad Request", err);
                return;
            }
            // The page asked for this key, so the file should carry it from now on.
            markPersisted(candidate, key);
        }
        // The same two whole-config checks main() applies to a command line. Without them the
        // page could write a file that makes the daemon refuse to start at the next logon —
        // exactly the "installs clean, fails later" shape this arc keeps removing.
        if (candidate.rate % 50 != 0) {
            sendError(sock, 400, "Bad Request",
                      "rate " + std::to_string(candidate.rate) +
                          " does not make an exact 20 ms frame (must be divisible by 50)");
            return;
        }
        if (candidate.transport != "tcp" && candidate.transport != "udp" &&
            candidate.transport != "dual") {
            sendError(sock, 400, "Bad Request",
                      "invalid transport '" + candidate.transport + "' (tcp|udp|dual)");
            return;
        }
        if (configPath_.empty()) {
            sendError(sock, 409, "Conflict",
                      "there is no config-file location on this machine (HOME is not set), so "
                      "settings cannot be saved");
            return;
        }
        const std::string text = renderConfigFile(candidate);
        if (!writeConfigFile(configPath_, text, err)) {
            sendError(sock, 500, "Internal Server Error", err);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(settingsMutex_);
            settings_ = candidate;
        }
        std::printf("[control] wrote %s\n", configPath_.c_str());
        std::fflush(stdout);
        sendJson(sock, 200, "OK",
                 "{\"ok\":true,\"configPath\":" + jstr(configPath_) +
                     ",\"restartNeeded\":" + (pipeline_.running() ? "true" : "false") + "}");
    }

    void postStream(naudio::net::Socket& sock, const HttpRequest& req) {
        std::vector<std::pair<std::string, std::string>> kv;
        std::string err;
        if (!parseFlatJsonObject(req.body, kv, err)) {
            sendError(sock, 400, "Bad Request", "malformed request body: " + err);
            return;
        }
        std::string action;
        for (const auto& [k, v] : kv)
            if (k == "action") action = v;
        if (action != "start" && action != "stop" && action != "restart") {
            sendError(sock, 400, "Bad Request", "action must be start, stop or restart");
            return;
        }
        if (action == "stop" || action == "restart") {
            pipeline_.stop();
            std::printf("[control] pipeline stopped\n");
            std::fflush(stdout);
        }
        if (action == "start" || action == "restart") {
            Args a;
            {
                std::lock_guard<std::mutex> lock(settingsMutex_);
                a = settings_;
            }
            if (!pipeline_.start(a, err)) {
                sendError(sock, 409, "Conflict", err);
                return;
            }
            std::printf("[control] pipeline starting (%s, capture '%s')\n", a.transport.c_str(),
                        a.captureId >= 0 ? ("#" + std::to_string(a.captureId)).c_str()
                                         : a.capturePattern.c_str());
            std::fflush(stdout);
        }
        sendJson(sock, 200, "OK", "{\"ok\":true,\"stream\":" + liveJson() + "}");
    }

    // Stop the whole daemon, not just the stream. Without this, a daemon started by clicking the
    // desktop shortcut has no visible way to stop — there is no window to close and no terminal
    // to Ctrl-C, and telling an operator to open Activity Monitor is exactly the terminal this
    // item exists to remove. It sets the same flag SIGINT sets, so the shutdown path is the one
    // already exercised rather than a second one.
    //
    // Safe under every service manager this arc ships: launchd's KeepAlive is SuccessfulExit
    // false, systemd's is Restart=on-failure, and a Task Scheduler logon task does not restart —
    // so a clean exit stays exited rather than coming straight back.
    void postQuit(naudio::net::Socket& sock) {
        // Answer BEFORE stopping: the page is waiting on this response, and a daemon that tore
        // its listener down first would leave the browser showing a network error for a
        // shutdown that worked.
        sendJson(sock, 200, "OK", "{\"ok\":true,\"quitting\":true}");
        std::printf("[control] quit requested from the control page\n");
        std::fflush(stdout);
        g_stop.store(true);
    }

    static constexpr std::size_t kMaxConnections = 12;

    Args settings_;
    std::mutex settingsMutex_;
    std::string configPath_;
    Pipeline& pipeline_;
    int port_ = 0;

    std::string devicesJson_;
    bool devicesStale_ = true;
    std::mutex devicesMutex_;

    // The thread plus the flag it sets on its way out; see reapWorkers().
    using Worker = std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>;

    naudio::net::Socket listener_;
    std::thread accepter_;
    std::vector<Worker> workers_;
    std::mutex workersMutex_;
    std::atomic<bool> stopping_{false};
};

// Hand the URL to whatever the desktop uses for http://. Best-effort by design: a machine with
// no browser, or a service context with no session, simply prints the URL instead — which is
// why the URL is printed first, unconditionally.
void openInBrowser(const std::string& url) {
#if defined(_WIN32)
    // The empty "" is start's TITLE argument; without it start treats a quoted URL as the title
    // and opens a console window instead.
    const std::string cmd = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
    const std::string cmd = "open '" + url + "'";
#else
    const std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1";
#endif
    if (std::system(cmd.c_str()) != 0)
        std::fprintf(stderr, "note: could not open a browser; visit %s yourself\n", url.c_str());
}

// Is a naudio control page ALREADY answering on this port? Asked when the bind fails, because
// the overwhelmingly likely cause is the operator clicking the desktop shortcut a second time —
// or clicking it while the login service is already running. Without this, the second click gets
// "address already in use" and nothing else, which is a terrible answer to "show me the page".
//
// It is a real HTTP GET rather than a bare connect(): something else entirely may hold the port,
// and opening a browser at an unrelated local service would be worse than the error.
bool naudioControlPageAnswers(int port) {
    std::string err;
    naudio::net::Socket sock = naudio::net::Socket::connectTcp("127.0.0.1",
                                                               static_cast<std::uint16_t>(port),
                                                               /*timeoutMs=*/1000, &err);
    if (!sock.valid()) return false;
    sock.setSendTimeout(1000);
    sock.setRecvTimeout(1000);
    const std::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                            "\r\nConnection: close\r\n\r\n";
    if (!sock.sendAll(req.data(), req.size())) return false;
    std::string got;
    char buf[2048];
    // The marker sits in the page's <title>, well inside the first read; a couple of reads is
    // plenty and bounds this against a peer that dribbles bytes forever.
    for (int i = 0; i < 8 && got.find("naudio control") == std::string::npos; ++i) {
        const naudio::net::RecvResult r = sock.recv(buf, sizeof buf);
        if (r.status != naudio::net::IoStatus::Ok || r.bytes == 0) break;
        got.append(buf, r.bytes);
    }
    return got.rfind("HTTP/1.1 200", 0) == 0 && got.find("naudio control") != std::string::npos;
}

int runControl(const Args& a, const std::string& configPath, bool openPage) {
    Pipeline pipeline;
    ControlServer server(a, configPath, pipeline);
    int port = a.controlPort;
    std::string err;
    if (!server.start(port, err)) {
        // The desktop shortcut on all three platforms is literally
        // `na_audio_daemon --mode control --duration-ms 0 --open-page`, so clicking it twice —
        // or clicking it while the login service is running — lands here. That is not an error:
        // the operator asked for a control page on this port and there is one, so point at it
        // and succeed. Exit 0 keeps a launchd/systemd/Task Scheduler retry from treating a
        // duplicate start as a fault.
        if (a.controlPort != 0 && naudioControlPageAnswers(a.controlPort)) {
            const std::string running = "http://127.0.0.1:" + std::to_string(a.controlPort) + "/";
            std::printf("a naudio control page is already running at %s\n", running.c_str());
            std::fflush(stdout);
            if (openPage) openInBrowser(running);
            return 0;
        }
        std::fprintf(stderr, "error: cannot serve the control page on 127.0.0.1:%d: %s\n",
                     a.controlPort, err.c_str());
        std::fprintf(stderr, "hint: something else is using that port; set a different one with "
                             "--control-port, or 0 to pick a free one.\n");
        return 1;
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    std::printf("control page : %s\n", url.c_str());
    std::printf("config file  : %s\n",
                configPath.empty() ? "(none — HOME is not set, so settings cannot be saved)"
                                   : configPath.c_str());
    std::printf("streaming    : %s\n", a.autostart ? "starting now (autostart = true)"
                                                   : "stopped — press Start on the page");
    std::fflush(stdout);

    if (openPage) openInBrowser(url);
    if (a.autostart && !pipeline.start(a, err))
        std::fprintf(stderr, "error: autostart failed: %s\n", err.c_str());

    // The control page's own run loop does nothing but wait for a signal: every other thread is
    // doing the work. --duration-ms still applies, so a bounded control session is expressible
    // (the gate below uses it) even though a service sets 0.
    const std::int64_t start = nowMs();
    const std::int64_t deadline = a.durationMs > 0 ? start + a.durationMs : 0;
    while (!g_stop.load() && (deadline == 0 || nowMs() < deadline))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::printf("\nstopping ...\n");
    std::fflush(stdout);
    pipeline.stop();
    server.stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;

    // --- Pass 1: locate and apply the config file BEFORE the flag pass — that ordering IS the
    // precedence rule (defaults < config file < flags). --help suppresses config loading
    // entirely, so usage still prints on a machine whose config file is broken.
    std::string configPath;
    bool noConfig = false, explicitConfig = false, wantHelp = false, openPage = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") wantHelp = true;
        else if (a == "--no-config") noConfig = true;
        else if (a == "--open-page") openPage = true;
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
    // The file control mode will WRITE, which is the file this invocation reads: an operator who
    // ran with --config <file> expects the page to edit that file, not the default one. Empty
    // only when --no-config was given or the default location is unresolvable; the page then
    // refuses to save rather than writing somewhere nobody asked for.
    std::string effectiveConfigPath;
    if (!wantHelp && !noConfig) {
        if (explicitConfig) {
            loadConfigIfPresent(args, configPath, /*required=*/true);
            effectiveConfigPath = configPath;
        } else if (const std::string def = defaultConfigPath(); !def.empty()) {
            loadConfigIfPresent(args, def, /*required=*/false);
            effectiveConfigPath = def;
        }
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
        else if (a == "--open-page") {}                   // consumed in pass 1
        else if (a == "--list-devices") args.listDevices = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (const Setting* s =
                     a.compare(0, 2, "--") == 0 ? findSetting(a.substr(2)) : nullptr) {
            std::string verr;
            if (!s->apply(args, next(a.c_str()), a, verr)) {
                std::fprintf(stderr, "error: %s\n", verr.c_str());
                return 2;
            }
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
    // --open-page is meaningless without a page to open, and silently ignoring it would leave an
    // operator staring at a terminal wondering where their browser went.
    if (openPage && args.mode != "control") {
        std::fprintf(stderr, "error: --open-page needs --mode control (there is no page to open "
                             "in '%s' mode)\n", args.mode.c_str());
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        if (args.listDevices) return runListDevices();
        if (args.mode == "capture-probe") return runCaptureProbe(args);
        if (args.mode == "hardware") return runHardware(args, g_stop, nullptr);
        if (args.mode == "control") return runControl(args, effectiveConfigPath, openPage);
        std::fprintf(stderr, "error: invalid --mode '%s' (capture-probe|hardware|control)\n",
                     args.mode.c_str());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
