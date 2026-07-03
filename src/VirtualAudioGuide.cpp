// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
//
// Guidance / diagnostic text generation. All app-specific identifiers are
// parameterized through GuideConfig: the templates below carry generic {tokens},
// substituted from the injected config, so this TU emits no app-coupling literals in
// any generated text.
#include "naudio/VirtualAudioGuide.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "naudio/DeviceEnumerator.hpp"
#include "naudio/FormatProbe.hpp"
#include "naudio/PulseCommands.hpp"
#include "naudio/StringUtil.hpp"

namespace naudio {

namespace {

// Replace every occurrence of `from` with `to`. The template blocks below use {token} markers
// for the parameterized values.
std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// Substitute the standard {rate}/{bits}/{channels}/{channelStr}/{app}/{client}/{sink}/{desc}
// tokens from a GuideConfig into a template string.
std::string subst(std::string tmpl, const GuideConfig& c) {
    const std::string rate = std::to_string(c.format.sampleRate);
    const std::string bits = std::to_string(c.format.bitsPerSample);
    const std::string channels = std::to_string(c.format.channels);
    const std::string channelStr = c.format.channels == 1 ? "Mono" : "Stereo";
    tmpl = replaceAll(std::move(tmpl), "{rate}", rate);
    tmpl = replaceAll(std::move(tmpl), "{bits}", bits);
    tmpl = replaceAll(std::move(tmpl), "{channels}", channels);
    tmpl = replaceAll(std::move(tmpl), "{channelStr}", channelStr);
    tmpl = replaceAll(std::move(tmpl), "{app}", c.digitalModeApp);
    tmpl = replaceAll(std::move(tmpl), "{client}", c.clientAppName);
    tmpl = replaceAll(std::move(tmpl), "{sink}", c.sinkName);
    tmpl = replaceAll(std::move(tmpl), "{desc}", c.sinkDescription);
    return tmpl;
}

const char* kMacInstall = R"(=== macOS Virtual Audio Setup ===

STEP 1: Install BlackHole (Recommended, Free)
─────────────────────────────────────────────
Option A - Homebrew:
   brew install blackhole-2ch

Option B - Direct download:
   https://existential.audio/blackhole/

STEP 2: Verify Sample Rate (CRITICAL)
─────────────────────────────────────────────
BlackHole must be set to {rate} Hz for {app} compatibility.

1. Open: /Applications/Utilities/Audio MIDI Setup.app
2. Find "BlackHole 2ch" in the device list (left panel)
3. Click on it to select it
4. In the right panel, check the "Format" dropdown
   - Should show "{rate}.0 Hz" ({rate}.0 Hz is typically the default)
   - If not, select a format that includes {rate} Hz

STEP 3: Create Multi-Output Device (Optional but Recommended)
─────────────────────────────────────────────
This lets you monitor audio while streaming:

1. In Audio MIDI Setup, click "+" at bottom left
2. Select "Create Multi-Output Device"
3. Check both "BlackHole 2ch" and your speakers/headphones
4. Right-click the new device → "Use This Device For Sound Output"

STEP 4: Configure Applications
─────────────────────────────────────────────
{app} Settings > Audio:
  - Soundcard Input:  BlackHole 2ch
  - Soundcard Output: BlackHole 2ch

{client}:
  - Capture:  BlackHole 2ch
  - Playback: BlackHole 2ch

Alternative: Loopback (Commercial, $99)
- More flexible routing: https://rogueamoeba.com/loopback/
)";

const char* kWindowsInstall = R"(=== Windows Virtual Audio Setup ===

STEP 1: Install VB-Cable (Free)
─────────────────────────────────────────────
1. Download from: https://vb-audio.com/Cable/
2. Extract the ZIP file
3. Right-click VBCABLE_Setup_x64.exe → Run as Administrator
4. Reboot when prompted

STEP 2: Configure Sample Rate (CRITICAL)
─────────────────────────────────────────────
VB-Cable defaults to 44100 Hz but {app} requires {rate} Hz.

Configure CABLE Output (Recording):
1. Right-click speaker icon in taskbar → Sounds
2. Go to "Recording" tab
3. Right-click "CABLE Output" → Properties
4. Go to "Advanced" tab
5. Set "Default Format" to: {rate} Hz, {bits} bit, {channelStr}
6. Uncheck both "Exclusive Mode" options
7. Click OK

Configure CABLE Input (Playback):
1. Go to "Playback" tab
2. Right-click "CABLE Input" → Properties
3. Go to "Advanced" tab
4. Set "Default Format" to: {rate} Hz, {bits} bit, {channelStr}
5. Uncheck both "Exclusive Mode" options
6. Click OK

STEP 3: Configure Applications
─────────────────────────────────────────────
{app} Settings > Audio:
  - Soundcard Input:  CABLE Output (VB-Audio Virtual Cable)
  - Soundcard Output: CABLE Input (VB-Audio Virtual Cable)

{client}:
  - Capture:  CABLE Input (captures {app} TX audio)
  - Playback: CABLE Output (sends RX audio to {app})

Alternative: VoiceMeeter (Free, more features)
- Download from: https://vb-audio.com/Voicemeeter/
)";

const char* kLinuxInstall = R"(=== Linux Virtual Audio Setup ===

STEP 1: Create Virtual Audio Device
─────────────────────────────────────────────

Option A - PulseAudio (most distros):

# Create null sink with correct sample rate
pactl load-module module-null-sink \
    sink_name={sink} \
    sink_properties=device.description={desc} \
    rate={rate} \
    channels={channels} \
    format=s{bits}le

# Create loopback for bidirectional audio
pactl load-module module-loopback \
    source={sink}.monitor \
    sink={sink} \
    latency_msec=20

Option B - PipeWire (Fedora, Ubuntu 22.10+):

# PipeWire is PulseAudio-compatible, same commands work
# Or use pw-cli for native PipeWire:
pw-cli create-node adapter \
    factory.name=support.null-audio-sink \
    node.name={sink} \
    media.class=Audio/Sink \
    audio.rate={rate} \
    audio.channels={channels}

STEP 2: Make Persistent (Optional)
─────────────────────────────────────────────
Add to ~/.config/pulse/default.pa or /etc/pulse/default.pa:

load-module module-null-sink sink_name={sink} rate={rate} channels={channels} format=s{bits}le sink_properties=device.description={desc}
load-module module-loopback source={sink}.monitor sink={sink} latency_msec=20

STEP 3: Verify with pavucontrol
─────────────────────────────────────────────
sudo apt install pavucontrol  # or dnf/pacman
pavucontrol
# Check "Output Devices" and "Input Devices" tabs

STEP 4: Configure Applications
─────────────────────────────────────────────
{app} Settings > Audio:
  - Soundcard Input:  {desc} Monitor
  - Soundcard Output: {desc}

{client}:
  - Capture:  Monitor of {desc}
  - Playback: {desc}

TIP: Use this client's auto-configure feature to set up automatically
)";

}  // namespace

std::string VirtualAudioGuide::installInstructions() const {
    switch (platform_) {
        case Platform::MacOS:   return subst(kMacInstall, config_);
        case Platform::Windows: return subst(kWindowsInstall, config_);
        case Platform::Linux:   return subst(kLinuxInstall, config_);
        case Platform::Unknown:
        default:
            return "Unknown platform. Please set up a virtual audio device manually.";
    }
}

std::string VirtualAudioGuide::digitalModeInstructions() const {
    // Digital-mode guidance: the audio path describes the generic
    // server <-> client <-> virtual-audio <-> digital-mode-app chain rather than app-specific names.
    const std::string dev = preferredDeviceName();
    std::string tmpl = R"(=== {app} Configuration ===

1. Open {app}
2. Go to File > Settings > Audio

3. Configure audio devices:
   - Soundcard Input:  {dev}
   - Soundcard Output: {dev}

4. Go to File > Settings > Radio

5. Configure radio connection:
   - Rig: Hamlib NET rigctl
   - Network Server: [rig control server IP]:4532
   - Example: 192.168.1.100:4532

6. Click "Test CAT" to verify connection

7. Start your audio client and connect to:
   [audio streaming server IP]:4533

The audio path will be:
Radio <-> Audio Server <-> Audio Client <-> Virtual Audio <-> {app}
)";
    tmpl = replaceAll(std::move(tmpl), "{dev}", dev);
    return subst(std::move(tmpl), config_);
}

std::vector<std::string> VirtualAudioGuide::sampleRateConfigurationSuggestions(
        const std::string& deviceName) const {
    std::vector<std::string> suggestions;
    const std::string rate = std::to_string(config_.format.sampleRate);
    const std::string bits = std::to_string(config_.format.bitsPerSample);
    const std::string channels = std::to_string(config_.format.channels);

    // lowercase copy for the "cable" substring test.
    const std::string lower = toLower(deviceName);

    switch (platform_) {
        case Platform::MacOS:
            suggestions.push_back(
                "Open Audio MIDI Setup (/Applications/Utilities/Audio MIDI Setup.app)");
            suggestions.push_back("Select '" + deviceName + "' and set Format to " + rate + " Hz");
            break;

        case Platform::Windows:
            suggestions.push_back("Open Sound settings (right-click speaker icon → Sounds)");
            if (lower.find("cable") != std::string::npos) {
                suggestions.push_back("Go to Recording tab → CABLE Output → Properties → Advanced");
                suggestions.push_back("Set Default Format to: " + rate + " Hz, " + bits + " bit");
                suggestions.push_back("Also configure Playback tab → CABLE Input with same settings");
            } else {
                suggestions.push_back("Find '" + deviceName + "' → Properties → Advanced");
                suggestions.push_back("Set Default Format to: " + rate + " Hz");
            }
            break;

        case Platform::Linux:
            suggestions.push_back("Recreate the virtual sink with correct sample rate:");
            suggestions.push_back("  pactl unload-module module-null-sink");
            suggestions.push_back("  pactl load-module module-null-sink sink_name=" +
                                  config_.sinkName + " rate=" + rate + " channels=" + channels);
            break;

        case Platform::Unknown:
        default:
            break;
    }
    return suggestions;
}

std::vector<std::string> VirtualAudioGuide::linuxConfigurationCommands() const {
    // The substantive commands come from the shared pulse:: builder used by
    // PlatformConfigurator::autoConfigureLinux, so the documented commands cannot drift from
    // the ones auto-config actually runs.
    return {
        "# Remove existing sink if present",
        "pactl unload-module module-null-sink 2>/dev/null || true",
        "",
        "# Create null sink with correct sample rate",
        pulse::nullSinkCommand(config_.sinkName, config_.sinkDescription, config_.format),
        "",
        "# Create loopback for bidirectional audio",
        pulse::loopbackCommand(config_.sinkName),
        "",
        "# Verify creation",
        pulse::checkSinkCommand(config_.sinkName),
    };
}

std::string VirtualAudioGuide::linuxPersistentConfig() const {
    std::string tmpl = R"(# Add these lines to ~/.config/pulse/default.pa or /etc/pulse/default.pa
# Then restart PulseAudio: pulseaudio -k && pulseaudio --start

# Virtual Audio Device
load-module module-null-sink sink_name={sink} rate={rate} channels={channels} format=s{bits}le sink_properties=device.description={desc}
load-module module-loopback source={sink}.monitor sink={sink} latency_msec=20
)";
    return subst(std::move(tmpl), config_);
}

std::vector<std::string> VirtualAudioGuide::macOSDiagnosticCommands() const {
    return {
        "# Check if BlackHole is installed",
        "system_profiler SPAudioDataType 2>/dev/null | grep -A 5 -i blackhole",
        "",
        "# List all audio devices with sample rates",
        "system_profiler SPAudioDataType",
        "",
        "# Check Audio MIDI Setup (opens GUI)",
        "# open /Applications/Utilities/Audio\\ MIDI\\ Setup.app",
    };
}

std::vector<std::string> VirtualAudioGuide::windowsDiagnosticCommands() const {
    return {
        "# PowerShell commands to check audio devices",
        "Get-WmiObject Win32_SoundDevice | Select-Object Name, Status",
        "",
        "# Check for VB-Cable specifically",
        "Get-WmiObject Win32_SoundDevice | Where-Object { $_.Name -like '*VB-Audio*' -or "
            "$_.Name -like '*CABLE*' }",
        "",
        "# Open Sound settings (run in PowerShell)",
        "# Start-Process mmsys.cpl",
    };
}

std::string VirtualAudioGuide::preferredDeviceName() const {
    switch (platform_) {
        case Platform::MacOS:   return "BlackHole 2ch";
        case Platform::Windows: return "CABLE Input/Output (VB-Audio)";
        case Platform::Linux:   return "Virtual Audio";
        case Platform::Unknown:
        default:                return "[Virtual Audio Device]";
    }
}

// ===== Diagnostic reports (need live device data) =====

namespace {

const char* platformName(Platform p) {
    switch (p) {
        case Platform::MacOS:   return "macOS";
        case Platform::Windows: return "Windows";
        case Platform::Linux:   return "Linux";
        case Platform::Unknown:
        default:                return "Unknown";
    }
}

}  // namespace

std::string diagnosticReport(DeviceEnumerator& enumerator, const VirtualAudioGuide& guide) {
    std::string sb;
    sb += "=== Virtual Audio Diagnostic Report ===\n\n";
    sb += std::string("Platform: ") + platformName(guide.platform()) + "\n\n";

    const auto captureDevices = enumerator.captureDevices();
    const auto playbackDevices = enumerator.playbackDevices();

    sb += "Capture Devices (" + std::to_string(captureDevices.size()) + "):\n";
    for (const auto& d : captureDevices) {
        sb += "  - " + d.name + (d.isVirtual() ? " [VIRTUAL]" : "") + "\n";
    }

    sb += "\nPlayback Devices (" + std::to_string(playbackDevices.size()) + "):\n";
    for (const auto& d : playbackDevices) {
        sb += "  - " + d.name + (d.isVirtual() ? " [VIRTUAL]" : "") + "\n";
    }

    // A virtual capture AND a virtual playback exist.
    const bool virtualAvailable = enumerator.findVirtualCapture().has_value() &&
                                  enumerator.findVirtualPlayback().has_value();
    sb += "\nVirtual Audio Available: ";
    sb += virtualAvailable ? "YES" : "NO";
    sb += "\n";

    if (!virtualAvailable) {
        sb += "\n" + guide.installInstructions();
    }
    return sb;
}

std::string enhancedDiagnosticReport(DeviceEnumerator& enumerator, FormatProbe& probe,
                                     const VirtualAudioGuide& guide) {
    const AudioFormat& fmt = guide.config().format;

    // Enumerate + probe the recommended devices ONCE and reuse below — the report previously
    // re-ran the full PortAudio scan ~6x and re-verified the same device repeatedly.
    const std::vector<DeviceInfo> captureDevices = enumerator.captureDevices();
    const std::vector<DeviceInfo> playbackDevices = enumerator.playbackDevices();
    const std::optional<DeviceInfo> bestCapture = enumerator.bestVirtualCapture(fmt);
    const std::optional<DeviceInfo> bestPlayback = enumerator.bestVirtualPlayback(fmt);
    const std::optional<VerificationResult> captureVerify =
        bestCapture ? std::optional<VerificationResult>(
                          probe.verifyConfiguration(&*bestCapture, fmt, Direction::Capture))
                    : std::nullopt;
    const std::optional<VerificationResult> playbackVerify =
        bestPlayback ? std::optional<VerificationResult>(
                           probe.verifyConfiguration(&*bestPlayback, fmt, Direction::Playback))
                     : std::nullopt;

    std::string sb;
    sb += "+==============================================================+\n";
    sb += "|         Virtual Audio Diagnostic Report                      |\n";
    sb += "+==============================================================+\n\n";
    sb += std::string("Platform: ") + platformName(guide.platform()) + "\n";
    sb += "Required Format: " + std::to_string(fmt.sampleRate) + " Hz, " +
          std::to_string(fmt.bitsPerSample) + "-bit, " + std::to_string(fmt.channels) +
          " channel(s)\n\n";

    // One device-listing section (capture or playback): mark the recommended device and surface
    // each virtual device's verification issues inline. `boxHeader` is the exact 3-line box.
    auto section = [&](const char* boxHeader, const std::vector<DeviceInfo>& devices,
                       const std::optional<DeviceInfo>& best, Direction dir) {
        sb += boxHeader;
        for (const auto& d : devices) {
            const bool recommended =
                best && best->backendId == d.backendId && best->name == d.name;
            sb += "  * " + d.name + (d.isVirtual() ? " [VIRTUAL]" : "") +
                  (recommended ? " * RECOMMENDED" : "") + "\n";
            if (d.isVirtual()) {
                const auto r = probe.verifyConfiguration(&d, fmt, dir);
                if (!r.success) {
                    for (const auto& issue : r.issues) sb += "      ! " + issue + "\n";
                }
            }
        }
    };

    section("+-------------------------------------------------------------+\n"
            "| CAPTURE DEVICES                                             |\n"
            "+-------------------------------------------------------------+\n",
            captureDevices, bestCapture, Direction::Capture);
    sb += "\n";
    section("+-------------------------------------------------------------+\n"
            "| PLAYBACK DEVICES                                            |\n"
            "+-------------------------------------------------------------+\n",
            playbackDevices, bestPlayback, Direction::Playback);

    // ---- Status ----
    sb += "\n+-------------------------------------------------------------+\n";
    sb += "| STATUS                                                      |\n";
    sb += "+-------------------------------------------------------------+\n";
    // isVirtualAudioAvailable: a virtual capture AND a virtual playback device exist. Derived from
    // the already-enumerated lists (classify() and findVirtual* use the same virtual predicate).
    auto anyVirtual = [](const std::vector<DeviceInfo>& v) {
        return std::any_of(v.begin(), v.end(), [](const DeviceInfo& d) { return d.isVirtual(); });
    };
    const bool virtualAvailable = anyVirtual(captureDevices) && anyVirtual(playbackDevices);
    sb += std::string("  Virtual Audio Available: ") + (virtualAvailable ? "YES" : "NO") + "\n";
    if (captureVerify) {
        sb += std::string("  Capture Device Ready: ") +
              (captureVerify->success ? "YES" : "NEEDS CONFIGURATION") + "\n";
    }
    if (playbackVerify) {
        sb += std::string("  Playback Device Ready: ") +
              (playbackVerify->success ? "YES" : "NEEDS CONFIGURATION") + "\n";
    }

    // ---- Platform-specific notes ----
    if (!virtualAvailable) {
        sb += "\n" + guide.installInstructions();
    } else {
        // Emit suggestions for EVERY recommended device that needs configuration — a healthy
        // capture must not suppress a misconfigured playback's remediation steps (and vice versa).
        std::vector<std::string> suggestions;
        auto addSuggestions = [&](const std::optional<VerificationResult>& v) {
            if (!v || v->success) return;
            for (const auto& s : v->suggestions) {
                if (std::find(suggestions.begin(), suggestions.end(), s) == suggestions.end()) {
                    suggestions.push_back(s);  // de-dup identical capture/playback suggestions
                }
            }
        };
        addSuggestions(captureVerify);
        addSuggestions(playbackVerify);
        if (!suggestions.empty()) {
            sb += "\n+-------------------------------------------------------------+\n";
            sb += "| CONFIGURATION NEEDED                                        |\n";
            sb += "+-------------------------------------------------------------+\n";
            for (const auto& s : suggestions) sb += "  -> " + s + "\n";
        }
    }
    return sb;
}

}  // namespace naudio
