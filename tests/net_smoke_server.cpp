// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — in-process server fixture for the C net-smoke.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Test-only fixture for the C net-smoke (c_net_smoke.c). It wraps an in-process
// naudio::net::AudioStreamServer behind a tiny extern "C" surface so the PURE-C smoke can spin up
// a loopback server, inject RX audio, observe the roster, and tear down — without the C side ever
// naming a C++ type. The server runs INJECT-ONLY (audio comes from injectAudio(), so no capture
// device / hardware is needed — the same hardware-free path the C++ e2e gate uses). NOT part of
// the public ABI; it exists only so the C client ABI has a real server to talk to over loopback.

#include <cstdint>
#include <string>
#include <vector>

#include "naudio/AudioStreamConfig.hpp"
#include "naudio/net/AudioStreamServer.hpp"

extern "C" {

// Start an inject-only loopback server on an ephemeral port (0 -> OS-assigned). Writes the bound
// port to *out_port. Returns an opaque handle, or nullptr on failure.
void* nasmoke_server_start(int* out_port) {
    naudio::AudioStreamConfig cfg{};
    cfg.maxClients = 4;
    cfg.txIdleTimeoutMs = 5000;  // keep any TX grant through the smoke
    auto* server = new naudio::net::AudioStreamServer(0, cfg);
    server->setInjectOnlyMode(true);  // audio from injectAudio(), not a capture device
    std::string err;
    if (!server->start(&err)) {
        delete server;
        return nullptr;
    }
    if (out_port != nullptr) *out_port = server->port();
    return server;
}

// Broadcast `len` bytes of RX PCM to all connected clients (the radio-RX-audio analog).
void nasmoke_server_inject(void* handle, const unsigned char* data, int len) {
    if (handle == nullptr || data == nullptr || len <= 0) return;
    auto* server = static_cast<naudio::net::AudioStreamServer*>(handle);
    server->injectAudio(std::vector<std::uint8_t>(data, data + len));
}

// Number of currently-connected clients (-1 on a NULL handle).
int nasmoke_server_client_count(void* handle) {
    if (handle == nullptr) return -1;
    return static_cast<naudio::net::AudioStreamServer*>(handle)->clientCount();
}

// Stop the server (joins all session threads) and free the handle. Safe on NULL.
void nasmoke_server_stop(void* handle) {
    if (handle == nullptr) return;
    auto* server = static_cast<naudio::net::AudioStreamServer*>(handle);
    server->stop();
    delete server;
}

}  // extern "C"
