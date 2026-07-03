// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — device/audio layer.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#include "naudio.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include "naudio/DeviceBackend.hpp"
#include "naudio/DeviceEnumerator.hpp"
#include "naudio/FormatProbe.hpp"
#include "naudio/PortAudioBackend.hpp"
#include "naudio/ShellRunner.hpp"
#include "naudio/Stream.hpp"
#include "naudio/StreamOpener.hpp"
#include "naudio/Types.hpp"
#include "naudio/VirtualAudioGuide.hpp"
#include "naudio/net/AudioStreamClient.hpp"  // networking client C ABI
#include "naudio/net/AudioStreamServer.hpp"  // networking server C ABI

// The audio-backend context. Owns ONE PortAudioBackend (a single Pa_Initialize /
// Pa_Terminate pair) for the context's lifetime; enumerate/probe/open/diagnostics all
// share it instead of each spinning up a throwaway backend. Streams opened from a context
// borrow this backend, so the context must outlive its streams.
struct na_context {
    naudio::PortAudioBackend backend;  // ctor = Pa_Initialize (throws on failure)
};

// Opaque stream handles. They borrow the context's backend (no per-stream Pa_Initialize),
// so na_close_* must run before na_context_destroy().
struct na_capture_stream {
    std::unique_ptr<naudio::CaptureStream> stream;
};
struct na_playback_stream {
    std::unique_ptr<naudio::PlaybackStream> stream;
};

namespace {

// Per-thread last-error. Each fallible entry point clears this to NA_OK on entry and sets a
// specific code on failure, so na_last_error() reflects the outcome of the calling thread's
// most recent fallible call — the disambiguation channel for the handle-returning functions
// that can only signal failure with NULL.
thread_local na_error_t g_lastError = NA_OK;

void setError(na_error_t e) { g_lastError = e; }

void copyStr(char* dst, std::size_t cap, const std::string& src) {
    // Honor the documented contract: buf == NULL (or cap == 0) returns the needed length without
    // writing. The text functions use this for the "query length first" idiom — writing here would
    // segfault on a NULL buffer (this previously crashed na_install_instructions(NULL, n)).
    if (dst == nullptr || cap == 0) return;
    const std::size_t n = std::min(src.size(), cap - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

int typeCode(naudio::DeviceType t) {
    switch (t) {
        case naudio::DeviceType::Hardware: return NA_TYPE_HARDWARE;
        case naudio::DeviceType::Virtual:  return NA_TYPE_VIRTUAL;
        case naudio::DeviceType::Unknown:  default: return NA_TYPE_UNKNOWN;
    }
}

int capCode(naudio::Capability c) {
    switch (c) {
        case naudio::Capability::Capture:  return NA_CAP_CAPTURE;
        case naudio::Capability::Playback: return NA_CAP_PLAYBACK;
        case naudio::Capability::Duplex:   default: return NA_CAP_DUPLEX;
    }
}

naudio::AudioFormat makeFormat(int sample_rate, int bits_per_sample, int channels) {
    naudio::AudioFormat f;
    f.sampleRate = sample_rate;
    f.bitsPerSample = bits_per_sample;
    f.channels = channels;
    return f;
}

}  // namespace

// Run a fallible extern "C" body behind the boundary so no C++ exception ever reaches a C
// caller (naudio.h: "No C++ exception ever crosses this boundary"). On entry both forms clear the
// per-thread error to NA_OK — the documented "cleared at the start of each fallible call" contract
// — then run the body. std::bad_alloc surfaces as NA_ERR_NOMEM, any other exception as
// (defaultErr). The body must return on every non-throwing path. (`body` is the variadic tail so
// commas inside its brace block survive preprocessing.) Used for the na_client_* surface; the
// device entry points above keep their pre-existing, more specific try/catch (DeviceUnavailable vs
// NOMEM vs BACKEND), an equivalent guarantee.
//
// NA_GUARD — for functions whose return value IS the na_error_t (the setters, and the text/roster
// functions whose error sentinel is a negative na_error_t). On a caught exception the RETURNED code
// equals the recorded error, so the caller can branch on the return without consulting
// na_last_error().
#define NA_GUARD(defaultErr, ...)                                                     \
    setError(NA_OK);                                                                  \
    try __VA_ARGS__                                                                   \
    catch (const std::bad_alloc&) { setError(NA_ERR_NOMEM); return NA_ERR_NOMEM; }    \
    catch (...) { setError(defaultErr); return (defaultErr); }

// NA_GUARD_VAL — for getters that return a fixed sentinel value (0 / -1) on failure and carry the
// specific cause only in na_last_error(). On a caught exception the error is recorded and (sentinel)
// is returned unchanged.
#define NA_GUARD_VAL(defaultErr, sentinel, ...)                                       \
    setError(NA_OK);                                                                  \
    try __VA_ARGS__                                                                   \
    catch (const std::bad_alloc&) { setError(NA_ERR_NOMEM); return (sentinel); }      \
    catch (...) { setError(defaultErr); return (sentinel); }

// ---- Error model -----------------------------------------------------------------------

extern "C" const char* na_strerror(na_error_t err) {
    switch (err) {
        case NA_OK:                     return "no error";
        case NA_ERR_BACKEND:            return "backend failure";
        case NA_ERR_INVALID:            return "invalid argument";
        case NA_ERR_DEVICE_UNAVAILABLE: return "device unavailable";
        case NA_ERR_INIT:               return "backend initialization failed";
        case NA_ERR_NOMEM:              return "out of memory";
        case NA_ERR_UNSUPPORTED:        return "unsupported operation";
    }
    return "unknown error";
}

extern "C" na_error_t na_last_error(void) { return g_lastError; }

// ---- Context ---------------------------------------------------------------------------

extern "C" na_context* na_context_create(void) {
    setError(NA_OK);
    try {
        return new na_context();  // PortAudioBackend ctor inits PortAudio once
    } catch (const std::bad_alloc&) {
        setError(NA_ERR_NOMEM);
        return nullptr;
    } catch (...) {
        setError(NA_ERR_INIT);  // Pa_Initialize (or backend ctor) failed
        return nullptr;
    }
}

extern "C" void na_context_destroy(na_context* ctx) { delete ctx; }  // Pa_Terminate; safe on NULL

// ---- Enumeration / probe ---------------------------------------------------------------

extern "C" int na_enumerate(na_context* ctx, na_device* out, int max) {
    setError(NA_OK);
    if (ctx == nullptr || out == nullptr || max < 0) {
        setError(NA_ERR_INVALID);
        return NA_ERR_INVALID;
    }
    try {
        naudio::DeviceEnumerator enumerator(ctx->backend);
        const auto devices = enumerator.list();

        int written = 0;
        for (const auto& d : devices) {
            if (written >= max) break;
            na_device& slot = out[written];
            slot.backend_id = d.backendId;
            slot.capture_backend_id = d.captureBackendId;
            slot.playback_backend_id = d.playbackBackendId;
            copyStr(slot.name, sizeof(slot.name), d.name);
            copyStr(slot.host_api, sizeof(slot.host_api), d.hostApi);
            slot.type = typeCode(d.type);
            slot.capability = capCode(d.capability);
            slot.is_virtual = d.isVirtual() ? 1 : 0;
            ++written;
        }
        return written;
    } catch (...) {
        setError(NA_ERR_BACKEND);
        return NA_ERR_BACKEND;
    }
}

extern "C" int na_probe_format(na_context* ctx, int backend_id, int sample_rate,
                               int bits_per_sample, int channels, int is_capture) {
    setError(NA_OK);
    if (ctx == nullptr || sample_rate <= 0 || bits_per_sample <= 0 || channels <= 0) {
        setError(NA_ERR_INVALID);
        return NA_ERR_INVALID;
    }
    try {
        naudio::AudioFormat format = makeFormat(sample_rate, bits_per_sample, channels);
        const naudio::Direction dir =
            is_capture ? naudio::Direction::Capture : naudio::Direction::Playback;
        return ctx->backend.probeFormat(backend_id, format, dir) ? 1 : 0;
    } catch (...) {
        setError(NA_ERR_BACKEND);
        return NA_ERR_BACKEND;
    }
}

// ---- Capture stream --------------------------------------------------------------------

extern "C" na_capture_stream* na_open_capture(na_context* ctx, int backend_id, int sample_rate,
                                              int bits_per_sample, int channels,
                                              int* out_actual_channels) {
    setError(NA_OK);
    if (ctx == nullptr || sample_rate <= 0 || bits_per_sample <= 0 || channels <= 0) {
        setError(NA_ERR_INVALID);
        return nullptr;
    }
    try {
        auto handle = std::make_unique<na_capture_stream>();
        naudio::StreamOpener opener(ctx->backend);
        naudio::DeviceInfo device;
        device.backendId = backend_id;
        handle->stream =
            opener.openCapture(device, makeFormat(sample_rate, bits_per_sample, channels));
        if (out_actual_channels != nullptr) {
            *out_actual_channels = handle->stream->actualFormat().channels;
        }
        return handle.release();
    } catch (const naudio::DeviceUnavailable&) {
        setError(NA_ERR_DEVICE_UNAVAILABLE);  // distinct from generic backend failure
        return nullptr;
    } catch (const std::bad_alloc&) {
        setError(NA_ERR_NOMEM);
        return nullptr;
    } catch (...) {
        setError(NA_ERR_BACKEND);
        return nullptr;
    }
}

extern "C" int na_capture_read(na_capture_stream* stream, void* buf, int frames, int timeout_ms,
                               int* out_overflow) {
    setError(NA_OK);
    if (stream == nullptr || stream->stream == nullptr || buf == nullptr || frames < 0) {
        setError(NA_ERR_INVALID);
        return NA_ERR_INVALID;
    }
    try {
        const naudio::IoResult r = stream->stream->read(buf, frames, timeout_ms);
        if (out_overflow != nullptr) *out_overflow = r.overflowed ? 1 : 0;
        return r.frames;
    } catch (...) {
        setError(NA_ERR_BACKEND);
        return NA_ERR_BACKEND;
    }
}

extern "C" void na_close_capture(na_capture_stream* stream) { delete stream; }

// ---- Playback stream -------------------------------------------------------------------

extern "C" na_playback_stream* na_open_playback(na_context* ctx, int backend_id, int sample_rate,
                                                int bits_per_sample, int channels) {
    setError(NA_OK);
    if (ctx == nullptr || sample_rate <= 0 || bits_per_sample <= 0 || channels <= 0) {
        setError(NA_ERR_INVALID);
        return nullptr;
    }
    try {
        auto handle = std::make_unique<na_playback_stream>();
        naudio::StreamOpener opener(ctx->backend);
        naudio::DeviceInfo device;
        device.backendId = backend_id;
        handle->stream =
            opener.openPlayback(device, makeFormat(sample_rate, bits_per_sample, channels));
        return handle.release();
    } catch (const naudio::DeviceUnavailable&) {
        setError(NA_ERR_DEVICE_UNAVAILABLE);
        return nullptr;
    } catch (const std::bad_alloc&) {
        setError(NA_ERR_NOMEM);
        return nullptr;
    } catch (...) {
        setError(NA_ERR_BACKEND);
        return nullptr;
    }
}

extern "C" int na_playback_write(na_playback_stream* stream, const void* buf, int frames,
                                 int timeout_ms, int* out_underflow) {
    setError(NA_OK);
    if (stream == nullptr || stream->stream == nullptr || buf == nullptr || frames < 0) {
        setError(NA_ERR_INVALID);
        return NA_ERR_INVALID;
    }
    try {
        const naudio::IoResult r = stream->stream->write(buf, frames, timeout_ms);
        if (out_underflow != nullptr) *out_underflow = r.underflowed ? 1 : 0;
        return r.frames;
    } catch (...) {
        setError(NA_ERR_BACKEND);
        return NA_ERR_BACKEND;
    }
}

extern "C" void na_close_playback(na_playback_stream* stream) { delete stream; }

// ---- Guidance / diagnostics / platform auto-config -------------------------------------

extern "C" int na_install_instructions(char* buf, int len) {
    setError(NA_OK);
    if (len < 0) {
        setError(NA_ERR_INVALID);
        return NA_ERR_INVALID;
    }
    try {
        naudio::VirtualAudioGuide guide;  // default format + host platform
        const std::string text = guide.installInstructions();
        copyStr(buf, static_cast<std::size_t>(len), text);
        return static_cast<int>(text.size());
    } catch (...) {
        setError(NA_ERR_BACKEND);
        return NA_ERR_BACKEND;
    }
}

extern "C" int na_diagnostic_report(na_context* ctx, char* buf, int len) {
    setError(NA_OK);
    if (ctx == nullptr || len < 0) {
        setError(NA_ERR_INVALID);
        return NA_ERR_INVALID;
    }
    try {
        naudio::DeviceEnumerator enumerator(ctx->backend);
        naudio::VirtualAudioGuide guide;
        naudio::FormatProbe probe(ctx->backend, &guide);
        const std::string text = naudio::enhancedDiagnosticReport(enumerator, probe, guide);
        copyStr(buf, static_cast<std::size_t>(len), text);
        return static_cast<int>(text.size());
    } catch (...) {
        setError(NA_ERR_BACKEND);
        return NA_ERR_BACKEND;
    }
}

extern "C" int na_blackhole_installed(void) {
    setError(NA_OK);
    try {
        naudio::PosixShellRunner shell;
        naudio::PlatformConfigurator cfg(shell);
        return cfg.isMacOSBlackHoleInstalled() ? 1 : 0;
    } catch (...) {
        setError(NA_ERR_BACKEND);
        return NA_ERR_BACKEND;
    }
}

extern "C" int na_linux_auto_configure(char* msg, int msg_len) {
    setError(NA_OK);
    if (msg_len < 0) {
        setError(NA_ERR_INVALID);
        return NA_ERR_INVALID;
    }
    try {
        naudio::PosixShellRunner shell;
        naudio::PlatformConfigurator cfg(shell);
        const naudio::ConfigurationResult r = cfg.autoConfigureLinux();
        if (msg != nullptr) copyStr(msg, static_cast<std::size_t>(msg_len), r.message);
        return r.success ? 1 : 0;
    } catch (...) {
        setError(NA_ERR_BACKEND);
        return NA_ERR_BACKEND;
    }
}

// ========================================================================================
// Networking audio-streaming client (na_client_*)
//
// A thin C surface over naudio::net::AudioStreamClient. The client is synchronous/std::thread
// (no async runtime), so the ABI maps onto it directly: the wrapper owns the client, the local
// audio backend, and a glue AudioClientListener that fans the C++ virtual events out to the C
// callback struct. Every C++ call is wrapped so no exception crosses the boundary.
// ========================================================================================

namespace {

// A playback stream that accepts and discards all PCM (the NULL backend's RX sink). RX audio is
// still delivered to the client's audio listener in receiveLoop BEFORE playback, so a NULL-backend
// client receives audio at the callback while playing nothing locally (headless / CI / self-test).
class NullPlaybackStream : public naudio::PlaybackStream {
public:
    explicit NullPlaybackStream(naudio::AudioFormat fmt) : fmt_(fmt) {}
    naudio::IoResult write(const void* /*buffer*/, int frames, int /*timeoutMs*/) override {
        naudio::IoResult r;
        r.frames = frames;  // pretend the whole buffer drained
        return r;
    }
    const naudio::AudioFormat& actualFormat() const override { return fmt_; }

private:
    naudio::AudioFormat fmt_;
};

// Hardware-free backend: playback discards, probe always succeeds, capture is unsupported (the
// NULL backend is RX-only by design). No PortAudio, no devices.
class NullBackend : public naudio::DeviceBackend {
public:
    std::vector<naudio::RawDevice> enumerate() override { return {}; }
    bool probeFormat(int, const naudio::AudioFormat&, naudio::Direction) override { return true; }
    std::unique_ptr<naudio::CaptureStream> openCaptureStream(int,
                                                             const naudio::AudioFormat&) override {
        throw naudio::DeviceUnavailable("NULL backend: capture (TX) is unsupported");
    }
    std::unique_ptr<naudio::PlaybackStream> openPlaybackStream(
        int, const naudio::AudioFormat& fmt) override {
        return std::make_unique<NullPlaybackStream>(fmt);
    }
};

}  // namespace

// The handle. Member ORDER is load-bearing: `client` is declared LAST so it is destroyed FIRST
// (reverse declaration order) — its destructor disconnects and joins every worker before `glue`
// and `backend` (which the workers call into / open streams through) are torn down.
struct na_stream_client {
    std::unique_ptr<naudio::DeviceBackend> backend;
    std::unique_ptr<naudio::net::AudioClientListener> glue;
    na_client_callbacks cbs{};
    void* cbUser = nullptr;
    na_audio_cb audioCb = nullptr;
    void* audioUser = nullptr;
    // A3: set once na_client_connect is attempted. The callback pointers (cbs / audioCb) are read
    // by the dispatch + receive workers with no lock, so the documented "set callbacks BEFORE
    // connect" rule must be ENFORCED, not just documented — once this is true, na_client_set_callbacks
    // / na_client_set_audio_cb reject with NA_ERR_INVALID instead of racing a reader. Atomic so a
    // setter on one thread sees a connect() on another.
    std::atomic<bool> connectStarted{false};
    std::unique_ptr<naudio::net::AudioStreamClient> client;
};

namespace {

// Glue: fans the C++ AudioClientListener virtual events out to the C callback struct. Holds a raw
// back-pointer to the owning handle (which outlives it — see the member-order note above) and
// reads owner_->cbs at call time, so callbacks set before connect are picked up.
class CClientListener : public naudio::net::AudioClientListener {
public:
    explicit CClientListener(na_stream_client* owner) : owner_(owner) {}

    void onClientConnected(const std::string& id, const std::string& addr) override {
        if (owner_->cbs.on_connected)
            owner_->cbs.on_connected(id.c_str(), addr.c_str(), owner_->cbUser);
    }
    void onClientDisconnected(const std::string& id) override {
        if (owner_->cbs.on_disconnected) owner_->cbs.on_disconnected(id.c_str(), owner_->cbUser);
    }
    void onStreamStarted(const std::string& id) override {
        if (owner_->cbs.on_stream_started)
            owner_->cbs.on_stream_started(id.c_str(), owner_->cbUser);
    }
    void onStreamStopped(const std::string& id) override {
        if (owner_->cbs.on_stream_stopped)
            owner_->cbs.on_stream_stopped(id.c_str(), owner_->cbUser);
    }
    void onError(const std::string& id, const std::string& error) override {
        if (owner_->cbs.on_error) owner_->cbs.on_error(id.c_str(), error.c_str(), owner_->cbUser);
    }
    void onReconnecting(const std::string& id, int attempt, int max) override {
        if (owner_->cbs.on_reconnecting)
            owner_->cbs.on_reconnecting(id.c_str(), attempt, max, owner_->cbUser);
    }
    void onReconnected(const std::string& id) override {
        if (owner_->cbs.on_reconnected) owner_->cbs.on_reconnected(id.c_str(), owner_->cbUser);
    }
    void onClientsUpdate(int count, int maxClients, const std::string& txOwner,
                         const std::vector<std::string>& clientIds) override {
        if (!owner_->cbs.on_clients_update) return;
        std::vector<const char*> ids;
        ids.reserve(clientIds.size());
        for (const auto& s : clientIds) ids.push_back(s.c_str());
        owner_->cbs.on_clients_update(count, maxClients, txOwner.c_str(),
                                      ids.empty() ? nullptr : ids.data(),
                                      static_cast<int>(ids.size()), owner_->cbUser);
    }
    void onTxGranted() override {
        if (owner_->cbs.on_tx_granted) owner_->cbs.on_tx_granted(owner_->cbUser);
    }
    void onTxDenied(const std::string& holdingClientId) override {
        if (owner_->cbs.on_tx_denied)
            owner_->cbs.on_tx_denied(holdingClientId.c_str(), owner_->cbUser);
    }
    void onTxPreempted(const std::string& preemptingClientId) override {
        if (owner_->cbs.on_tx_preempted)
            owner_->cbs.on_tx_preempted(preemptingClientId.c_str(), owner_->cbUser);
    }
    void onTxReleased() override {
        if (owner_->cbs.on_tx_released) owner_->cbs.on_tx_released(owner_->cbUser);
    }

private:
    na_stream_client* owner_;
};

}  // namespace

// ---- Create / destroy ------------------------------------------------------------------

extern "C" na_stream_client* na_client_create(na_client_backend backend, const char* host,
                                              int port, const char* name) {
    setError(NA_OK);
    if (host == nullptr || port <= 0 || port > 65535) {
        setError(NA_ERR_INVALID);
        return nullptr;
    }
    try {
        auto c = std::make_unique<na_stream_client>();

        switch (backend) {
            case NA_CLIENT_BACKEND_NULL:
                c->backend = std::make_unique<NullBackend>();
                break;
            case NA_CLIENT_BACKEND_SYSTEM:
                c->backend = std::make_unique<naudio::PortAudioBackend>();  // ctor = Pa_Initialize
                break;
            default:
                setError(NA_ERR_INVALID);
                return nullptr;
        }

        c->client = std::make_unique<naudio::net::AudioStreamClient>(
            std::string(host), static_cast<std::uint16_t>(port),
            name != nullptr ? std::string(name) : std::string("naudio-client"));
        c->client->setBackend(c->backend.get());

        c->glue = std::make_unique<CClientListener>(c.get());
        c->client->addStreamListener(c->glue.get());

        // Single RX audio sink: dispatch to the C callback if one is registered. The lambda holds
        // the handle pointer (which owns the client, so it outlives every worker) and reads
        // audioCb at call time, so a callback set before connect is honored.
        na_stream_client* self = c.get();
        c->client->addAudioListener([self](const std::uint8_t* data, std::size_t n) {
            if (self->audioCb) self->audioCb(data, n, self->audioUser);
        });

        return c.release();
    } catch (const std::bad_alloc&) {
        setError(NA_ERR_NOMEM);
        return nullptr;
    } catch (...) {
        setError(NA_ERR_INIT);  // SYSTEM backend's Pa_Initialize (or another ctor) failed
        return nullptr;
    }
}

extern "C" void na_client_destroy(na_stream_client* client) {
    if (client == nullptr) return;
    try {
        if (client->client) client->client->disconnect();  // join workers before teardown
    } catch (...) {
        // best-effort; fall through to free
    }
    // A2: guard the destructor too. ~AudioStreamClient is hardened to swallow teardown errors, but
    // the C ABI boundary must never let a member-destructor exception cross into a C caller.
    try {
        delete client;
    } catch (...) {
        // teardown is best-effort; never abort a C caller
    }
}

// ---- Configuration ---------------------------------------------------------------------

extern "C" na_error_t na_client_set_playback_device(na_stream_client* client, int backend_id) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        client->client->setPlaybackDevice(backend_id);
        return NA_OK;
    });
}

extern "C" na_error_t na_client_set_capture_device(na_stream_client* client, int backend_id) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        client->client->setCaptureDevice(backend_id);
        return NA_OK;
    });
}

extern "C" na_error_t na_client_set_transport(na_stream_client* client, na_transport transport) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        naudio::AudioStreamConfig cfg = client->client->config();
        switch (transport) {
            case NA_TRANSPORT_TCP:  cfg.transportType = naudio::TransportType::Tcp;  break;
            case NA_TRANSPORT_UDP:  cfg.transportType = naudio::TransportType::Udp;  break;
            case NA_TRANSPORT_DUAL: cfg.transportType = naudio::TransportType::Dual; break;
            default:
                setError(NA_ERR_INVALID);
                return NA_ERR_INVALID;
        }
        if (!client->client->setConfig(cfg)) {  // false == already connected (InvalidState)
            setError(NA_ERR_INVALID);
            return NA_ERR_INVALID;
        }
        return NA_OK;
    });
}

extern "C" na_error_t na_client_set_identity(na_stream_client* client, const char* callsign,
                                             const char* operator_name, const char* location) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        if (callsign != nullptr) client->client->setCallsign(callsign);
        if (operator_name != nullptr) client->client->setOperatorName(operator_name);
        if (location != nullptr) client->client->setLocation(location);
        return NA_OK;
    });
}

extern "C" na_error_t na_client_set_callbacks(na_stream_client* client,
                                              const na_client_callbacks* cbs, void* user) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        // A3: the dispatch worker reads cbs/cbUser with no lock once streaming starts. Enforce the
        // documented "set callbacks BEFORE connect" rule rather than racing a reader.
        if (client->connectStarted.load()) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        client->cbs = (cbs != nullptr) ? *cbs : na_client_callbacks{};
        client->cbUser = user;
        return NA_OK;
    });
}

extern "C" na_error_t na_client_set_audio_cb(na_stream_client* client, na_audio_cb cb, void* user) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        // A3: the receive worker reads audioCb/audioUser with no lock once streaming starts.
        if (client->connectStarted.load()) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        client->audioCb = cb;
        client->audioUser = user;
        return NA_OK;
    });
}

extern "C" na_error_t na_client_set_auto_reconnect(na_stream_client* client, int enabled) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        client->client->setAutoReconnect(enabled != 0);
        return NA_OK;
    });
}

extern "C" na_error_t na_client_set_reconnect_policy(na_stream_client* client, int base_delay_ms,
                                                     int max_delay_ms, int max_attempts) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        if (base_delay_ms > 0) client->client->setReconnectDelayMs(base_delay_ms);
        if (max_delay_ms > 0) client->client->setMaxReconnectDelayMs(max_delay_ms);
        if (max_attempts > 0) client->client->setMaxReconnectAttempts(max_attempts);
        return NA_OK;
    });
}

extern "C" na_error_t na_client_set_ptt(na_stream_client* client, int tx_active) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        client->client->setPTT(tx_active != 0);
        return NA_OK;
    });
}

extern "C" na_error_t na_client_set_capture_muted(na_stream_client* client, int muted) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        client->client->setCaptureMuted(muted != 0);
        return NA_OK;
    });
}

extern "C" na_error_t na_client_set_playback_muted(na_stream_client* client, int muted) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        client->client->setPlaybackMuted(muted != 0);
        return NA_OK;
    });
}

// ---- Lifecycle -------------------------------------------------------------------------

extern "C" na_error_t na_client_connect(na_stream_client* client, char* errbuf, int errlen) {
    setError(NA_OK);
    if (client == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
    // A3: freeze the callback config — from here on the dispatch/receive workers may read it, so
    // na_client_set_callbacks / na_client_set_audio_cb refuse further changes. Set on ATTEMPT (not
    // success) because even a failed connect can post an onError the dispatcher reads.
    client->connectStarted.store(true);
    if (errbuf != nullptr && errlen > 0) errbuf[0] = '\0';
    try {
        std::string err;
        if (!client->client->connect(&err)) {
            if (errbuf != nullptr && errlen > 0)
                copyStr(errbuf, static_cast<std::size_t>(errlen), err);
            setError(NA_ERR_BACKEND);
            return NA_ERR_BACKEND;
        }
        return NA_OK;
    } catch (...) {
        setError(NA_ERR_BACKEND);
        return NA_ERR_BACKEND;
    }
}

extern "C" void na_client_disconnect(na_stream_client* client) {
    if (client == nullptr) return;
    try {
        client->client->disconnect();
    } catch (...) {
        // best-effort
    }
}

extern "C" int na_client_is_connected(na_stream_client* client) {
    NA_GUARD_VAL(NA_ERR_BACKEND, 0, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return 0; }  // doc: 0 on NULL
        return client->client->isConnected() ? 1 : 0;
    });
}

extern "C" int na_client_is_streaming(na_stream_client* client) {
    NA_GUARD_VAL(NA_ERR_BACKEND, 0, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return 0; }  // doc: 0 on NULL
        return client->client->isStreaming() ? 1 : 0;
    });
}

// ---- Server roster ---------------------------------------------------------------------
// -1 is the documented "no data yet" value (before the first CLIENTS_UPDATE) AND the NULL/error
// sentinel; na_last_error() disambiguates: NA_OK -> no update yet, NA_ERR_INVALID -> NULL client,
// NA_ERR_BACKEND -> the C++ accessor threw.

extern "C" int na_client_server_client_count(na_stream_client* client) {
    NA_GUARD_VAL(NA_ERR_BACKEND, -1, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return -1; }
        return client->client->serverClientCount();
    });
}

extern "C" int na_client_server_max_clients(na_stream_client* client) {
    NA_GUARD_VAL(NA_ERR_BACKEND, -1, {
        if (client == nullptr) { setError(NA_ERR_INVALID); return -1; }
        return client->client->serverMaxClients();
    });
}

extern "C" int na_client_server_tx_owner(na_stream_client* client, char* buf, int len) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (client == nullptr || len < 0) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        const std::string owner = client->client->serverTxOwner();  // by value -> may bad_alloc
        copyStr(buf, static_cast<std::size_t>(len), owner);
        return static_cast<int>(owner.size());
    });
}

// ========================================================================================
// Networking audio-streaming server (na_server_*)
//
// A thin C surface over naudio::net::AudioStreamServer (+ AudioBroadcaster / AudioMixer), the
// mirror of the na_client_* surface above. AudioStreamServer takes its AudioStreamConfig at
// CONSTRUCTION (no setConfig), so the handle holds a PENDING config + device ids + callbacks that
// the configuration setters mutate, and the AudioStreamServer is built lazily in na_server_start
// with the final config. The same exception-tightness (NA_GUARD / NA_GUARD_VAL) and the
// startAttempted gate (the server-side analog of the client's connectStarted) apply.
// ========================================================================================

// The handle. Member ORDER is load-bearing (same discipline as na_stream_client): `server` is
// declared LAST so it is destroyed FIRST — ~AudioStreamServer stops + joins every worker and
// drains+joins the dispatch thread before `glue` (the listener the dispatch thread fans into) and
// `backend` (whose ForwardingPlaybackStream the mixer playback thread calls) are torn down.
struct na_audio_server {
    na_server_backend backendKind = NA_SERVER_BACKEND_NULL;
    int port = 0;
    naudio::AudioStreamConfig pendingConfig{};  // mutated by the config setters before start
    std::optional<int> captureDevice;           // SYSTEM only; absent => inject-only RX
    std::optional<int> playbackDevice;          // SYSTEM only; NULL backend uses the forwarding sink
    na_server_callbacks cbs{};
    void* cbUser = nullptr;
    na_server_tx_audio_cb txAudioCb = nullptr;  // NULL backend: mixer playback -> this callback
    void* txAudioUser = nullptr;
    // Set once na_server_start is attempted. The dispatch worker reads cbs/cbUser and the mixer
    // playback thread reads txAudioCb/txAudioUser with no lock once running, so the documented "set
    // before start" rule is ENFORCED (mirror of na_stream_client::connectStarted): the config /
    // callback setters then reject with NA_ERR_INVALID instead of racing a reader.
    std::atomic<bool> startAttempted{false};

    std::unique_ptr<naudio::DeviceBackend> backend;             // created in na_server_create
    std::unique_ptr<naudio::net::AudioStreamListener> glue;     // created in na_server_start
    std::unique_ptr<naudio::net::AudioStreamServer> server;     // created in na_server_start; first to die
};

namespace {

// The NULL backend's playback sink: forwards each mixed-TX frame the mixer playback loop writes to
// the C tx-audio callback (read at call time so a callback set before start is honored), then
// reports the buffer fully drained. Discards if no callback is registered.
class ForwardingPlaybackStream : public naudio::PlaybackStream {
public:
    ForwardingPlaybackStream(na_audio_server* owner, naudio::AudioFormat fmt)
        : owner_(owner), fmt_(fmt) {}
    naudio::IoResult write(const void* buffer, int frames, int /*timeoutMs*/) override {
        if (owner_->txAudioCb != nullptr && buffer != nullptr && frames > 0) {
            const std::size_t n = static_cast<std::size_t>(frames) * fmt_.frameSize();
            owner_->txAudioCb(static_cast<const unsigned char*>(buffer), n, owner_->txAudioUser);
        }
        naudio::IoResult r;
        r.frames = frames;  // pretend the whole buffer drained
        return r;
    }
    const naudio::AudioFormat& actualFormat() const override { return fmt_; }

private:
    na_audio_server* owner_;
    naudio::AudioFormat fmt_;
};

// Hardware-free server backend: RX never comes from a capture device (it is injected), and mixed TX
// is extracted through the ForwardingPlaybackStream above. No PortAudio, no devices.
class ServerNullBackend : public naudio::DeviceBackend {
public:
    explicit ServerNullBackend(na_audio_server* owner) : owner_(owner) {}
    std::vector<naudio::RawDevice> enumerate() override { return {}; }
    bool probeFormat(int, const naudio::AudioFormat&, naudio::Direction) override { return true; }
    std::unique_ptr<naudio::CaptureStream> openCaptureStream(int,
                                                             const naudio::AudioFormat&) override {
        throw naudio::DeviceUnavailable("NULL server backend: RX is via na_server_inject_audio");
    }
    std::unique_ptr<naudio::PlaybackStream> openPlaybackStream(
        int, const naudio::AudioFormat& fmt) override {
        return std::make_unique<ForwardingPlaybackStream>(owner_, fmt);
    }

private:
    na_audio_server* owner_;
};

// Glue: fans the C++ AudioStreamListener virtual events out to the C na_server_callbacks struct.
// Holds a raw back-pointer to the owning handle (which outlives it) and reads owner_->cbs at call
// time. Fires on the server's dispatch thread, so a callback may re-enter the server.
class CServerListener : public naudio::net::AudioStreamListener {
public:
    explicit CServerListener(na_audio_server* owner) : owner_(owner) {}

    void onServerStarted(int port) override {
        if (owner_->cbs.on_started) owner_->cbs.on_started(port, owner_->cbUser);
    }
    void onServerStopped() override {
        if (owner_->cbs.on_stopped) owner_->cbs.on_stopped(owner_->cbUser);
    }
    void onClientConnected(const std::string& id, const std::string& addr) override {
        if (owner_->cbs.on_client_connected)
            owner_->cbs.on_client_connected(id.c_str(), addr.c_str(), owner_->cbUser);
    }
    void onClientDisconnected(const std::string& id) override {
        if (owner_->cbs.on_client_disconnected)
            owner_->cbs.on_client_disconnected(id.c_str(), owner_->cbUser);
    }
    void onStreamStarted(const std::string& id) override {
        if (owner_->cbs.on_stream_started)
            owner_->cbs.on_stream_started(id.c_str(), owner_->cbUser);
    }
    void onStreamStopped(const std::string& id) override {
        if (owner_->cbs.on_stream_stopped)
            owner_->cbs.on_stream_stopped(id.c_str(), owner_->cbUser);
    }
    void onError(const std::string& id, const std::string& error) override {
        if (owner_->cbs.on_error) owner_->cbs.on_error(id.c_str(), error.c_str(), owner_->cbUser);
    }

private:
    na_audio_server* owner_;
};

}  // namespace

// ---- Create / destroy ------------------------------------------------------------------

extern "C" na_audio_server* na_server_create(na_server_backend backend, int port) {
    setError(NA_OK);
    if (port < 0 || port > 65535) {  // 0 is allowed (OS-assigned ephemeral port)
        setError(NA_ERR_INVALID);
        return nullptr;
    }
    try {
        auto s = std::make_unique<na_audio_server>();
        s->backendKind = backend;
        s->port = port;

        switch (backend) {
            case NA_SERVER_BACKEND_NULL:
                s->backend = std::make_unique<ServerNullBackend>(s.get());
                break;
            case NA_SERVER_BACKEND_SYSTEM:
                s->backend = std::make_unique<naudio::PortAudioBackend>();  // ctor = Pa_Initialize
                break;
            default:
                setError(NA_ERR_INVALID);
                return nullptr;
        }
        return s.release();
    } catch (const std::bad_alloc&) {
        setError(NA_ERR_NOMEM);
        return nullptr;
    } catch (...) {
        setError(NA_ERR_INIT);  // SYSTEM backend's Pa_Initialize (or another ctor) failed
        return nullptr;
    }
}

extern "C" void na_server_destroy(na_audio_server* server) {
    if (server == nullptr) return;
    try {
        if (server->server) server->server->stop();  // join workers before teardown
    } catch (...) {
        // best-effort; fall through to free
    }
    // A2-style belt: ~AudioStreamServer is hardened to swallow teardown errors, but the C ABI
    // boundary must never let a member-destructor exception cross into a C caller.
    try {
        delete server;
    } catch (...) {
        // teardown is best-effort; never abort a C caller
    }
}

// ---- Configuration ---------------------------------------------------------------------

extern "C" na_error_t na_server_set_transport(na_audio_server* server, na_transport transport) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (server == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        if (server->startAttempted.load()) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        switch (transport) {
            case NA_TRANSPORT_TCP:  server->pendingConfig.transportType = naudio::TransportType::Tcp;  break;
            case NA_TRANSPORT_UDP:  server->pendingConfig.transportType = naudio::TransportType::Udp;  break;
            case NA_TRANSPORT_DUAL: server->pendingConfig.transportType = naudio::TransportType::Dual; break;
            default:
                setError(NA_ERR_INVALID);
                return NA_ERR_INVALID;
        }
        return NA_OK;
    });
}

extern "C" na_error_t na_server_set_max_clients(na_audio_server* server, int max_clients) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (server == nullptr || max_clients <= 0) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        if (server->startAttempted.load()) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        server->pendingConfig.maxClients = max_clients;
        return NA_OK;
    });
}

extern "C" na_error_t na_server_set_capture_device(na_audio_server* server, int backend_id) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (server == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        if (server->startAttempted.load()) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        // RX-from-device requires PortAudio; the NULL backend's RX is injected.
        if (server->backendKind == NA_SERVER_BACKEND_NULL) {
            setError(NA_ERR_UNSUPPORTED);
            return NA_ERR_UNSUPPORTED;
        }
        server->captureDevice = backend_id;
        return NA_OK;
    });
}

extern "C" na_error_t na_server_set_playback_device(na_audio_server* server, int backend_id) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (server == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        if (server->startAttempted.load()) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        // TX-to-device requires PortAudio; the NULL backend extracts TX via na_server_tx_audio_cb.
        if (server->backendKind == NA_SERVER_BACKEND_NULL) {
            setError(NA_ERR_UNSUPPORTED);
            return NA_ERR_UNSUPPORTED;
        }
        server->playbackDevice = backend_id;
        return NA_OK;
    });
}

extern "C" na_error_t na_server_set_callbacks(na_audio_server* server,
                                              const na_server_callbacks* cbs, void* user) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (server == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        // The dispatch worker reads cbs/cbUser with no lock once running. Enforce "set before start".
        if (server->startAttempted.load()) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        server->cbs = (cbs != nullptr) ? *cbs : na_server_callbacks{};
        server->cbUser = user;
        return NA_OK;
    });
}

extern "C" na_error_t na_server_set_tx_audio_cb(na_audio_server* server, na_server_tx_audio_cb cb,
                                                void* user) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (server == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        // The mixer playback thread reads txAudioCb/txAudioUser with no lock once running.
        if (server->startAttempted.load()) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        server->txAudioCb = cb;
        server->txAudioUser = user;
        return NA_OK;
    });
}

// ---- Lifecycle -------------------------------------------------------------------------

extern "C" na_error_t na_server_start(na_audio_server* server, char* errbuf, int errlen) {
    setError(NA_OK);
    if (server == nullptr) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
    if (errbuf != nullptr && errlen > 0) errbuf[0] = '\0';
    // Freeze config/callbacks on ATTEMPT (mirror connectStarted). exchange() also makes a second
    // start a clean NA_ERR_INVALID rather than constructing a second AudioStreamServer.
    if (server->startAttempted.exchange(true)) {
        setError(NA_ERR_INVALID);
        return NA_ERR_INVALID;
    }
    try {
        server->server = std::make_unique<naudio::net::AudioStreamServer>(
            static_cast<std::uint16_t>(server->port), server->pendingConfig);
        server->server->setBackend(server->backend.get());

        // RX: a capture device captures from hardware (SYSTEM only); without one the server runs
        // inject-only so clients aren't rejected for a missing capture device (RX from injectAudio).
        if (server->captureDevice.has_value()) {
            server->server->setCaptureDevice(*server->captureDevice);
        } else {
            server->server->setInjectOnlyMode(true);
        }
        // TX: the NULL backend always opens the ForwardingPlaybackStream (device id is a placeholder
        // — the backend ignores it) so the mixer playback loop drains arbitrated TX to the tx
        // callback; SYSTEM plays to a user-set device if one was provided.
        if (server->backendKind == NA_SERVER_BACKEND_NULL) {
            server->server->setPlaybackDevice(0);
        } else if (server->playbackDevice.has_value()) {
            server->server->setPlaybackDevice(*server->playbackDevice);
        }

        server->glue = std::make_unique<CServerListener>(server);
        server->server->addStreamListener(server->glue.get());

        std::string err;
        if (!server->server->start(&err)) {
            if (errbuf != nullptr && errlen > 0)
                copyStr(errbuf, static_cast<std::size_t>(errlen), err);
            setError(NA_ERR_BACKEND);
            return NA_ERR_BACKEND;
        }
        return NA_OK;
    } catch (const std::bad_alloc&) {
        setError(NA_ERR_NOMEM);
        return NA_ERR_NOMEM;
    } catch (...) {
        setError(NA_ERR_BACKEND);
        return NA_ERR_BACKEND;
    }
}

extern "C" void na_server_stop(na_audio_server* server) {
    if (server == nullptr) return;
    try {
        if (server->server) server->server->stop();
    } catch (...) {
        // best-effort
    }
}

extern "C" int na_server_is_running(na_audio_server* server) {
    NA_GUARD_VAL(NA_ERR_BACKEND, 0, {
        if (server == nullptr) { setError(NA_ERR_INVALID); return 0; }  // doc: 0 on NULL
        if (!server->server) return 0;                                  // not started yet
        return server->server->isRunning() ? 1 : 0;
    });
}

extern "C" int na_server_port(na_audio_server* server) {
    NA_GUARD_VAL(NA_ERR_BACKEND, -1, {
        if (server == nullptr) { setError(NA_ERR_INVALID); return -1; }
        if (!server->server) return -1;  // not started yet
        return server->server->port();
    });
}

// ---- Audio I/O -------------------------------------------------------------------------

extern "C" na_error_t na_server_inject_audio(na_audio_server* server, const unsigned char* pcm,
                                             int n_bytes) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (server == nullptr || pcm == nullptr || n_bytes <= 0) {
            setError(NA_ERR_INVALID);
            return NA_ERR_INVALID;
        }
        if (!server->server) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }  // not started
        server->server->injectAudio(
            std::vector<std::uint8_t>(pcm, pcm + n_bytes));  // may bad_alloc
        return NA_OK;
    });
}

// ---- Roster ----------------------------------------------------------------------------

extern "C" int na_server_client_count(na_audio_server* server) {
    NA_GUARD_VAL(NA_ERR_BACKEND, -1, {
        if (server == nullptr) { setError(NA_ERR_INVALID); return -1; }
        if (!server->server) return 0;  // not started => no clients (NA_OK, not an error)
        return server->server->clientCount();
    });
}

extern "C" int na_server_max_clients(na_audio_server* server) {
    NA_GUARD_VAL(NA_ERR_BACKEND, -1, {
        if (server == nullptr) { setError(NA_ERR_INVALID); return -1; }
        // The configured value, readable before and after start (pendingConfig is the source of
        // truth pre-start; the running server's config() is an identical copy of it).
        return server->server ? server->server->config().maxClients
                              : server->pendingConfig.maxClients;
    });
}

extern "C" int na_server_tx_owner(na_audio_server* server, char* buf, int len) {
    NA_GUARD(NA_ERR_BACKEND, {
        if (server == nullptr || len < 0) { setError(NA_ERR_INVALID); return NA_ERR_INVALID; }
        const std::string owner =
            server->server ? server->server->txOwner() : std::string();  // by value -> may bad_alloc
        copyStr(buf, static_cast<std::size_t>(len), owner);
        return static_cast<int>(owner.size());
    });
}
