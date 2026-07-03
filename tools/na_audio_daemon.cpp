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
//                              48 kHz stereo for --duration-ms. The foundational real-hardware
//                              proof: reports frames transferred, the OVERFLOW count (the "no
//                              overruns" gate — the only place the device's overrun flag is
//                              directly observable), and per-channel RMS (so an asymmetric or
//                              single-channel source shows up per channel).
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
//                   [--port N] [--duration-ms N]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
};

void usage() {
    std::fprintf(
        stderr,
        "usage: na_audio_daemon [--mode capture-probe|hardware] [--capture <pat>|--capture-id N]\n"
        "                       [--playback <pat>|--playback-id N] [--transport tcp|udp|dual]\n"
        "                       [--port N] [--duration-ms N]\n"
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
        "  --playback-id N   force playback device backendId\n"
        "  --transport       tcp (default) | udp | dual\n"
        "  --port N          server port; 0 = ephemeral (default %d)\n"
        "  --duration-ms N   run time; 0 = until Ctrl-C (default 30000)\n"
        "  --list-devices    enumerate capture + playback devices, then exit\n"
        "  -h, --help        print this message\n",
        naudio::AudioStreamConfig::DEFAULT_PORT);
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
    std::printf("capture-probe: device [%d] %s\n", dev->backendId, dev->name.c_str());

    naudio::AudioFormat requested;  // 48 kHz / 16-bit / stereo
    std::unique_ptr<naudio::CaptureStream> stream;
    try {
        stream = opener.openCapture(*dev, requested);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: openCapture failed: %s\n", e.what());
        return 1;
    }
    const naudio::AudioFormat fmt = stream->actualFormat();
    std::printf("opened: %d Hz / %d-bit / %d ch%s\n", fmt.sampleRate, fmt.bitsPerSample,
                fmt.channels, fmt.channels == 1 ? "  (mono fallback)" : "");

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
    std::printf("server capture: device [%d] %s\n", capDev->backendId, capDev->name.c_str());

    const naudio::AudioStreamConfig cfg = configFor(a.transport);

    // --- Resolve the client's RX sink: a real virtual device, else a FakeBackend drain. ---
    naudio::AudioFormat fmt;  // 48 kHz / 16-bit / stereo (matches cfg)
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
        clientPlaybackId = sinkDev->backendId;
        realSink = true;
        std::printf("client sink  : device [%d] %s (real — external apps can read this)\n",
                    sinkDev->backendId, sinkDev->name.c_str());
    } else {
        // Hardware-free drain: register the 48k/16/stereo playback format the client will open.
        fakeBackend.add(naudio::RawDevice{/*backendId=*/0, "fake-sink", "fake", 0, 2, 48000.0});
        fakeBackend.addSupportedFormat(0, naudio::Direction::Playback, fmt);
        clientBackend = &fakeBackend;
        clientPlaybackId = 0;
        std::printf("client sink  : FakeBackend drain (no virtual sink found; pipeline-only)\n");
    }

    // --- Start the server (capture-only; NEVER opens a playback line to the radio). ---
    naudio::net::AudioStreamServer server(static_cast<std::uint16_t>(a.port), cfg, "127.0.0.1");
    server.setBackend(&backend);
    server.setCaptureDevice(capDev->backendId);
    std::string err;
    if (!server.start(&err)) {
        std::fprintf(stderr, "error: server.start failed: %s\n", err.c_str());
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
    std::printf("expected RX  : %lld bytes/s (48k/16/stereo)\n", static_cast<long long>(expectedBps));
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
    std::printf("  RIGHT (ch 1)   : RMS %.1f dBFS, peak %.1f dBFS\n", dbfs(s.rmsR), dbfs(s.peakR));
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

}  // namespace

int main(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--mode") args.mode = next("--mode");
        else if (a == "--capture") args.capturePattern = next("--capture");
        else if (a == "--capture-id") args.captureId = std::atoi(next("--capture-id").c_str());
        else if (a == "--playback") args.playbackPattern = next("--playback");
        else if (a == "--playback-id") args.playbackId = std::atoi(next("--playback-id").c_str());
        else if (a == "--transport") args.transport = next("--transport");
        else if (a == "--port") args.port = std::atoi(next("--port").c_str());
        else if (a == "--duration-ms") args.durationMs = std::atoll(next("--duration-ms").c_str());
        else if (a == "--list-devices") args.listDevices = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(); return 2; }
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
