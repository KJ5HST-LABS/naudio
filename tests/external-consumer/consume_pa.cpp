/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio tests — external consumer of the installed device-backend archive.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * Proves naudio::naudio_pa is linkable from an INSTALLED prefix — the one target
 * whose PortAudio dependency the consumer has to resolve for itself (issue #61).
 * The installed naudioTargets.cmake deliberately names no PortAudio target, so
 * naudioConfig.cmake resolves one (pkg-config, else find_library/find_path) and
 * attaches it here. Nothing else in this gate links naudio_pa, so without this
 * binary that whole seam would ship untested: consume_c takes the shared C ABI,
 * which has the backend already inside, and consume_cxx takes naudio_core +
 * naudio_net, neither of which touches PortAudio.
 *
 * HARDWARE-FREE BY CONSTRUCTION, and the honest statement of what that costs:
 * the assertion this binary carries is that it LINKED — that PortAudioBackend.o
 * was pulled from the installed archive and its Pa_* references resolved against
 * a consumer-side PortAudio. It opens no device and calls no Pa_Initialize, so it
 * is not evidence that device I/O works (P3). Pa_GetVersionText needs no
 * initialization and additionally proves the PortAudio HEADER search path
 * naudioConfig.cmake wires up is real, not just the library.
 */
#include <naudio/PortAudioBackend.hpp>

#include <portaudio.h>

#include <cstdio>

// Forces the linker to resolve PortAudioBackend's constructor and destructor —
// and through them the Pa_Initialize / Pa_Terminate the archive references —
// without ever running them. Never called; taking its address is enough to keep
// it, and the reference is what the link step has to satisfy.
static naudio::DeviceBackend* makeBackend() {
    return new naudio::PortAudioBackend();
}

int main() {
    naudio::DeviceBackend* (*factory)() = &makeBackend;
    if (factory == nullptr) {  // never true; keeps `factory` live against -O2 DCE
        std::fprintf(stderr, "consume_pa FAIL: unreachable\n");
        return 1;
    }

    const char* paVersion = Pa_GetVersionText();
    if (paVersion == nullptr || paVersion[0] == '\0') {
        std::fprintf(stderr, "consume_pa FAIL: Pa_GetVersionText returned nothing\n");
        return 1;
    }

    std::printf("consume_pa OK: naudio_pa linked from the installed prefix against \"%s\""
                " (link-only — no device opened)\n",
                paVersion);
    return 0;
}
