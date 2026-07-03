// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio/DeviceEnumerator.hpp"

#include <algorithm>
#include <unordered_map>

#include "naudio/FormatProbe.hpp"
#include "naudio/StringUtil.hpp"

namespace naudio {
namespace {

// Classification matches on `name + " " + description` (lowercased). PortAudio has
// no per-device description; the host-API name is its closest analog AND carries the
// Linux virtual markers ("jack"/"pulse" are host APIs / devices under PortAudio), so it
// is the right second field to match against.
std::string matchKey(const std::string& name, const std::string& hostApi) {
    return toLower(name + " " + hostApi);
}

bool containsAny(const std::string& haystack, const std::vector<std::string>& patterns) {
    for (const auto& p : patterns) {
        if (!p.empty() && haystack.find(toLower(p)) != std::string::npos) return true;
    }
    return false;
}

// Strip a leading "Port " prefix when building the display name.
// PortAudio has no vendor field, so the host API is appended for disambiguation instead.
std::string displayNameFor(const std::string& name, const std::string& hostApi) {
    std::string n = name;
    const std::string prefix = "Port ";
    if (n.rfind(prefix, 0) == 0) n = n.substr(prefix.size());
    if (!hostApi.empty()) return n + " (" + hostApi + ")";
    return n;
}

Capability capabilityFor(bool canCapture, bool canPlayback) {
    if (canCapture && canPlayback) return Capability::Duplex;
    return canCapture ? Capability::Capture : Capability::Playback;
}

}  // namespace

DeviceEnumerator::DeviceEnumerator(DeviceBackend& backend, Platform platform)
    : backend_(backend), platform_(platform) {}

DeviceType DeviceEnumerator::classify(const RawDevice& raw) const {
    // Substring match against virtual patterns, default Hardware. (Classification
    // never returns UNKNOWN.)
    if (containsAny(matchKey(raw.name, raw.hostApi), virtualPatterns(platform_))) {
        return DeviceType::Virtual;
    }
    return DeviceType::Hardware;
}

std::vector<DeviceInfo> DeviceEnumerator::list() {
    const std::vector<RawDevice> raw = backend_.enumerate();

    std::vector<DeviceInfo> out;
    out.reserve(raw.size());
    std::unordered_map<std::string, std::size_t> byIdentity;  // name+hostApi -> index in `out`

    for (const auto& r : raw) {
        const bool canCapture = r.maxInputChannels > 0;
        const bool canPlayback = r.maxOutputChannels > 0;
        if (!canCapture && !canPlayback) continue;  // not a usable audio endpoint

        const std::string identity = matchKey(r.name, r.hostApi);
        auto it = byIdentity.find(identity);
        if (it != byIdentity.end()) {
            // DUPLEX merge: the same device seen as both capture and playback is
            // upgraded to DUPLEX. Order-independent (the merge
            // adds capture first, then upgrades; this unions either order).
            DeviceInfo& existing = out[it->second];
            existing.capability = capabilityFor(existing.supportsCapture() || canCapture,
                                                existing.supportsPlayback() || canPlayback);
            // Union the per-direction channel counts / default rate so the merged record
            // reports the real capture+playback capability. Element-wise max keeps this
            // order-independent (like the capability union above).
            existing.maxInputChannels = std::max(existing.maxInputChannels, r.maxInputChannels);
            existing.maxOutputChannels = std::max(existing.maxOutputChannels, r.maxOutputChannels);
            existing.defaultSampleRate = std::max(existing.defaultSampleRate, r.defaultSampleRate);
            // Record the incoming record's backend id for the direction(s) it adds, so a split
            // capture-only + playback-only pair (ALSA-style) stays openable in BOTH directions
            // rather than collapsing onto the first record's single id.
            if (canCapture && existing.captureBackendId < 0) existing.captureBackendId = r.backendId;
            if (canPlayback && existing.playbackBackendId < 0) existing.playbackBackendId = r.backendId;
            continue;
        }

        DeviceInfo info;
        info.backendId = r.backendId;
        info.captureBackendId = canCapture ? r.backendId : -1;
        info.playbackBackendId = canPlayback ? r.backendId : -1;
        info.name = r.name;
        info.hostApi = r.hostApi;
        info.displayName = displayNameFor(r.name, r.hostApi);
        info.type = classify(r);
        info.capability = capabilityFor(canCapture, canPlayback);
        info.defaultSampleRate = r.defaultSampleRate;
        info.maxInputChannels = r.maxInputChannels;
        info.maxOutputChannels = r.maxOutputChannels;
        byIdentity.emplace(identity, out.size());
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<DeviceInfo> DeviceEnumerator::captureDevices() {
    std::vector<DeviceInfo> result;
    for (const auto& d : list()) {
        if (d.supportsCapture()) result.push_back(d);
    }
    return result;
}

std::vector<DeviceInfo> DeviceEnumerator::playbackDevices() {
    std::vector<DeviceInfo> result;
    for (const auto& d : list()) {
        if (d.supportsPlayback()) result.push_back(d);
    }
    return result;
}

std::optional<DeviceInfo> DeviceEnumerator::find(const std::vector<DeviceInfo>& devices,
                                                 const std::vector<std::string>& patterns) const {
    for (const auto& d : devices) {
        if (containsAny(matchKey(d.name, d.hostApi), patterns)) return d;
    }
    return std::nullopt;
}

std::optional<DeviceInfo> DeviceEnumerator::findVirtualCapture() {
    return find(captureDevices(), virtualPatterns(platform_));
}

std::optional<DeviceInfo> DeviceEnumerator::findVirtualPlayback() {
    return find(playbackDevices(), virtualPatterns(platform_));
}

std::optional<DeviceInfo> DeviceEnumerator::bestVirtual(const AudioFormat& format, Direction dir) {
    // The set of virtual devices for this direction: the classified-virtual subset of the
    // direction's devices, in enumeration order.
    std::vector<DeviceInfo> devices;
    for (const auto& d : (dir == Direction::Capture ? captureDevices() : playbackDevices())) {
        if (d.isVirtual()) devices.push_back(d);
    }
    if (devices.empty()) return std::nullopt;

    FormatProbe probe(backend_);

    // 1) Platform-preferred patterns, validated. The preferred match is on the device
    //    NAME only, unlike classification which also weighs the host API.
    for (const auto& pattern : preferredPatterns(platform_)) {
        const std::string needle = toLower(pattern);
        for (const auto& device : devices) {
            if (toLower(device.name).find(needle) != std::string::npos &&
                probe.supports(device, format, dir)) {
                return device;
            }
        }
    }

    // 2) First device that validates, regardless of name.
    for (const auto& device : devices) {
        if (probe.supports(device, format, dir)) return device;
    }

    // 3) Last resort: first virtual device even without validation.
    return devices.front();
}

std::optional<DeviceInfo> DeviceEnumerator::bestVirtualCapture(const AudioFormat& format) {
    return bestVirtual(format, Direction::Capture);
}

std::optional<DeviceInfo> DeviceEnumerator::bestVirtualPlayback(const AudioFormat& format) {
    return bestVirtual(format, Direction::Playback);
}

}  // namespace naudio
