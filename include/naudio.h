/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * naudio — public C ABI for the device/audio layer.
 *
 * Copyright (C) 2025-2026 Terrell Deppe
 *
 * C ABI for the naudio device/audio layer. This is the boundary a
 * C / Hamlib-based consumer links against; the implementation is C++ internally.
 *
 * Lifecycle: create one na_context (initializes the audio backend once), pass it
 * to enumerate/probe/open, then destroy it. Errors are reported as negative
 * na_error_t codes (or NULL for handle-returning calls, with na_last_error()
 * giving the cause on the calling thread).
 */
#ifndef NAUDIO_H
#define NAUDIO_H

#include <stddef.h>  /* size_t (the RX audio callback payload length) */

#include "naudio/export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Device type codes (mirror naudio::DeviceType). */
#define NA_TYPE_HARDWARE 0
#define NA_TYPE_VIRTUAL  1
#define NA_TYPE_UNKNOWN  2

/* Capability codes (mirror naudio::Capability). */
#define NA_CAP_CAPTURE  0
#define NA_CAP_PLAYBACK 1
#define NA_CAP_DUPLEX   2

/*
 * Error codes. Functions that return a count return >= 0 on success and one of
 * these (negative) values on failure; handle-returning functions return NULL on
 * failure. The cause of the most recent failed call on the CURRENT THREAD is
 * available via na_last_error(); na_strerror() maps a code to a static string.
 *
 * NA_ERR_BACKEND (-1) and NA_ERR_INVALID (-2) keep their historical numeric
 * values so existing `ret == NA_ERR_*` comparisons remain valid (these were
 * previously #defines; they are now na_error_t enumerators).
 */
typedef enum na_error {
    NA_OK                     =  0,  /* success / no error                                  */
    NA_ERR_BACKEND            = -1,  /* generic backend failure (PortAudio or unknown)      */
    NA_ERR_INVALID            = -2,  /* invalid argument (NULL out-param, non-positive size)*/
    NA_ERR_DEVICE_UNAVAILABLE = -3,  /* device could not be opened in the requested format  */
    NA_ERR_INIT               = -4,  /* audio backend initialization failed (context create)*/
    NA_ERR_NOMEM              = -5,  /* allocation failed                                   */
    NA_ERR_UNSUPPORTED        = -6   /* operation unsupported on this platform              */
} na_error_t;

/* A static, human-readable description of an na_error_t code (never NULL). */
NA_EXPORT const char* na_strerror(na_error_t err);

/* The na_error_t of the most recent failed na_* call on the CALLING THREAD.
 * Cleared to NA_OK at the start of each fallible call, so it reflects the
 * outcome of the immediately preceding call on this thread. */
NA_EXPORT na_error_t na_last_error(void);

/* ---- Context -----------------------------------------------------------------------
 *
 * Opaque audio-backend context. Creating a context initializes the underlying
 * audio backend (PortAudio) exactly ONCE for the context's lifetime; enumerate /
 * probe / open / diagnostics all share that single initialization instead of each
 * spinning up a throwaway backend. Destroy the context when done.
 */
typedef struct na_context na_context;

/* Create a context (one backend initialization). Returns NULL on failure; call
 * na_last_error() for the cause (typically NA_ERR_INIT or NA_ERR_NOMEM). */
NA_EXPORT na_context* na_context_create(void);

/* Destroy a context and release its backend. ALL streams opened from this context
 * must be closed BEFORE destroying it. Safe on NULL. */
NA_EXPORT void na_context_destroy(na_context* ctx);

typedef struct na_device {
    int  backend_id;           /* primary id (== capture id for a capture-capable device)        */
    /* Per-direction ids. For a normal device all three are equal; for a device the backend reports
     * as split capture-only + playback-only records (ALSA-style) they differ. Open capture with
     * capture_backend_id and playback with playback_backend_id. -1 == that direction unsupported. */
    int  capture_backend_id;
    int  playback_backend_id;
    char name[256];
    char host_api[128];
    int  type;        /* NA_TYPE_*  */
    int  capability;  /* NA_CAP_*   */
    int  is_virtual;  /* 0 or 1     */
} na_device;

/*
 * Enumerate audio devices using `ctx`'s backend. Writes up to `max` devices into
 * `out`; returns the number written (>= 0), or a negative na_error_t on failure
 * (NA_ERR_INVALID if ctx/out is NULL or max < 0).
 */
NA_EXPORT int na_enumerate(na_context* ctx, na_device* out, int max);

/*
 * Probe whether device `backend_id` (a na_device.backend_id) supports the given
 * signed-PCM format in one direction, using `ctx`'s backend. `is_capture` selects
 * capture (1) vs playback (0). Returns 1 (supported), 0 (unsupported), or a
 * negative na_error_t on bad arguments / backend failure. Wraps Pa_IsFormatSupported.
 */
NA_EXPORT int na_probe_format(na_context* ctx, int backend_id, int sample_rate,
                              int bits_per_sample, int channels, int is_capture);

/* ---- Blocking capture/playback streams --------------------------------------------
 *
 * Opaque RAII stream handles, opened from (and bound to) an na_context. The stream borrows
 * the context's backend, so the context must outlive the stream: close every stream with
 * na_close_* BEFORE na_context_destroy(). A C/Hamlib consumer opens a stream, reads/writes
 * blocking PCM frames, and closes it.
 */
typedef struct na_capture_stream na_capture_stream;
typedef struct na_playback_stream na_playback_stream;

/* Negative timeout => block until every requested frame is transferred. A
 * non-negative timeout is a bounded transfer (milliseconds). */
#define NA_BLOCK_FOREVER (-1)

/*
 * Open a blocking capture stream on `backend_id` (via `ctx`) using the capture mono->stereo
 * fallback: if stereo is unsupported the stream opens
 * mono. On success returns a non-NULL handle and, if `out_actual_channels` is non-NULL, writes
 * the ACTUAL channel count (1 if it fell back to mono, even though `channels` was 2) — the
 * caller must up-convert. Returns NULL if the device cannot be opened (na_last_error() gives
 * NA_ERR_DEVICE_UNAVAILABLE vs NA_ERR_BACKEND/NA_ERR_INVALID). Free with na_close_capture().
 */
NA_EXPORT na_capture_stream* na_open_capture(na_context* ctx, int backend_id, int sample_rate,
                                             int bits_per_sample, int channels,
                                             int* out_actual_channels);

/*
 * Blocking read of `frames` frames into `buf`, which must hold at least
 * frames * (bits_per_sample/8) * actual_channels bytes. `timeout_ms` per NA_BLOCK_FOREVER.
 * Returns the number of frames read (>= 0), or a negative na_error_t code. If `out_overflow`
 * is non-NULL it is set to 1 when input was dropped before this read (paInputOverflowed).
 */
NA_EXPORT int na_capture_read(na_capture_stream* stream, void* buf, int frames, int timeout_ms,
                              int* out_overflow);

/* Stop and close a capture stream. Safe on NULL. */
NA_EXPORT void na_close_capture(na_capture_stream* stream);

/*
 * Open a blocking playback stream on `backend_id` (via `ctx`) in exactly the given format (no
 * fallback, matching openPlaybackLine). Returns NULL if the device cannot be opened.
 */
NA_EXPORT na_playback_stream* na_open_playback(na_context* ctx, int backend_id, int sample_rate,
                                               int bits_per_sample, int channels);

/*
 * Blocking write of `frames` frames from `buf`. Returns frames written (>= 0) or a negative
 * na_error_t code. If `out_underflow` is non-NULL it is set to 1 when output was inserted
 * before this write (paOutputUnderflowed).
 */
NA_EXPORT int na_playback_write(na_playback_stream* stream, const void* buf, int frames,
                                int timeout_ms, int* out_underflow);

/* Stop and close a playback stream. Safe on NULL. */
NA_EXPORT void na_close_playback(na_playback_stream* stream);

/* ---- Setup guidance, diagnostics, and platform auto-config -----------------------------
 *
 * Text functions write a NUL-terminated string into `buf` (truncated to `len-1` chars) and
 * return the FULL length of the text — so a caller can detect truncation and re-call with a
 * larger buffer. `len == 0` (or buf == NULL) just returns the needed length without writing.
 * They use the standard 48 kHz / 16-bit / stereo format and the host platform.
 */

/* Platform install instructions for setting up a virtual audio device. (No backend needed,
 * so this does not take an na_context.) */
NA_EXPORT int na_install_instructions(char* buf, int len);

/* Enhanced diagnostic report over `ctx`'s (PortAudio) device list. Returns NA_ERR_INVALID
 * if ctx is NULL, NA_ERR_BACKEND if enumeration fails. */
NA_EXPORT int na_diagnostic_report(na_context* ctx, char* buf, int len);

/* 1 if BlackHole appears installed (macOS only), 0 otherwise, NA_ERR_BACKEND on error. */
NA_EXPORT int na_blackhole_installed(void);

/*
 * Attempt to auto-configure a PulseAudio/PipeWire null sink on Linux (pactl). Returns 1 on
 * success, 0 on failure, NA_ERR_INVALID on bad args. If `msg`/`msg_len` are provided, writes a
 * human-readable result message. NOTE: this MUTATES the system audio configuration and is a
 * no-op (returns 0) off Linux. The sink name is validated against an allow-list before use.
 */
NA_EXPORT int na_linux_auto_configure(char* msg, int msg_len);

/* ---- Networking audio-streaming client (na_client_*) -----------------------------------
 *
 * A C consumer — Hamlib, a standalone C client, or a Python ctypes/cffi binding — drives the
 * SAME bidirectional audio-streaming client the example clients use: it connects
 * to an AudioStreamServer, receives RX PCM (delivered to a C callback) into a local playback
 * device, and OPTIONALLY captures TX PCM from a local device to send to the server. The wire is
 * the frozen 0xAF01 v1 protocol; this is a thin C surface over the C++ naudio::net::
 * AudioStreamClient, not a re-implementation.
 *
 * THREADING / LIFETIME CONTRACT — read before using callbacks:
 *   - EVENT callbacks (the na_client_callbacks struct: connect / disconnect / stream / error /
 *     reconnect / roster / TX) fire on a single dedicated DISPATCH thread, one at a time and in
 *     order — never on a network worker and never under an internal lock. You MAY call any client
 *     method from inside an event callback, INCLUDING na_client_disconnect, without deadlocking;
 *     the call is honored and the remaining events still drain. (The one exception is
 *     na_client_destroy: never destroy a client from within its own callback — that frees an
 *     object that is still running.)
 *   - The RX audio callback (na_audio_cb) fires on the receive WORKER thread — the data plane, one
 *     call per RX frame. Keep it SHORT and non-blocking, and do NOT call client lifecycle methods
 *     (connect / disconnect / destroy) from it; copy the pcm and hand off to your own thread. The
 *     `pcm` buffer is valid ONLY for the duration of the call.
 *   - String arguments to the event callbacks are valid only for the duration of the call.
 *   - Set callbacks (na_client_set_callbacks / na_client_set_audio_cb) and all configuration
 *     BEFORE na_client_connect; they are read once streaming starts.
 *   - na_client_destroy() disconnects, JOINS every worker, then drains + joins the dispatch thread
 *     before freeing — so no callback (event or audio) can be in flight once it returns. Do not
 *     free user-data a callback references until afterward.
 *   - No C++ exception ever crosses this boundary; failures surface as na_error_t / NULL +
 *     na_last_error(), exactly like the device ABI above.
 */

/* Local audio backend for the client's RX playback (required) and TX capture (optional). */
typedef enum na_client_backend {
    NA_CLIENT_BACKEND_SYSTEM = 0,  /* PortAudio: real / virtual audio devices (production).        */
    NA_CLIENT_BACKEND_NULL   = 1   /* Hardware-free: RX is still delivered to na_audio_cb but is
                                      discarded locally, and TX capture is unsupported. For headless
                                      relays, CI, and the loopback self-test — no PortAudio init.  */
} na_client_backend;

/* Transport selection (mirrors naudio::TransportType). The client picks ONE; a DUAL server serves
 * TCP+UDP on one port and the client connects with whichever it selects. */
typedef enum na_transport {
    NA_TRANSPORT_TCP  = 0,
    NA_TRANSPORT_UDP  = 1,
    NA_TRANSPORT_DUAL = 2   /* treated as TCP on the client side */
} na_transport;

/* UDP reliability profile applied by na_client_set_reliability_profile and
 * na_server_set_reliability_profile: transport + framing + the FEC / reorder / adaptive-jitter /
 * control-ARQ knobs, as one named bundle. Mirrors the C++ AudioStreamConfig UDP presets. Each
 * setter documents below exactly which of its own settings the profile replaces — the two are
 * deliberately not identical, because the client and the server own different settings.
 *
 * BOTH ENDS MUST AGREE on the transport, and it is the profile that carries FEC: a server on
 * NA_RELIABILITY_UDP_WAN sends parity packets that a client left on any other profile receives and
 * discards, silently getting no loss recovery at all. */
typedef enum na_reliability_profile {
    NA_RELIABILITY_DEFAULT = 0,  /* Plain TCP defaults: no FEC / reorder / jitter / control-ARQ.    */
    NA_RELIABILITY_UDP_LAN = 1,  /* UDP, low-latency LAN buffers, reorder + control-ARQ.            */
    NA_RELIABILITY_UDP_WAN = 2,  /* UDP, Internet buffers: XOR FEC + adaptive jitter + reorder +    */
                                 /*   control-ARQ. The resilient remote-operating profile.          */
    NA_RELIABILITY_UDP_FT8 = 3   /* UDP, FT8/digital: tight buffers, reorder + control-ARQ.         */
} na_reliability_profile;

/* Opaque streaming-client handle. Create with na_client_create, free with na_client_destroy. */
typedef struct na_stream_client na_stream_client;

/* Hot-path RX PCM callback: `pcm` / `n_bytes` is one received audio frame's payload, valid only
 * for the duration of the call. Fires on an internal worker thread. */
typedef void (*na_audio_cb)(const unsigned char* pcm, size_t n_bytes, void* user);

/* Lifecycle / roster / TX-arbitration events. EVERY field may be NULL (that event is ignored).
 * All fire on a single dedicated dispatch thread, one at a time and in order (the THREADING
 * CONTRACT above); `user` is the pointer passed to na_client_set_callbacks.
 * String arguments are valid only for the duration of the call. */
typedef struct na_client_callbacks {
    void (*on_connected)(const char* client_id, const char* server_addr, void* user);
    void (*on_disconnected)(const char* client_id, void* user);
    void (*on_stream_started)(const char* client_id, void* user);
    void (*on_stream_stopped)(const char* client_id, void* user);
    void (*on_error)(const char* client_id, const char* message, void* user);
    void (*on_reconnecting)(const char* client_id, int attempt, int max_attempts, void* user);
    void (*on_reconnected)(const char* client_id, void* user);
    void (*on_clients_update)(int count, int max_clients, const char* tx_owner,
                              const char* const* client_ids, int n_client_ids, void* user);
    void (*on_tx_granted)(void* user);
    void (*on_tx_denied)(const char* holding_client_id, void* user);
    void (*on_tx_preempted)(const char* preempting_client_id, void* user);
    void (*on_tx_released)(void* user);
} na_client_callbacks;

/* Create a streaming client for `host`:`port` (port 1..65535). `name` (may be NULL -> a default)
 * identifies the client in the server's roster. `backend` selects the local audio backend.
 * Returns NULL on failure; na_last_error() gives the cause (NA_ERR_INVALID for a bad host/port,
 * NA_ERR_INIT if the SYSTEM backend's PortAudio could not initialize, NA_ERR_NOMEM). */
NA_EXPORT na_stream_client* na_client_create(na_client_backend backend, const char* host,
                                             int port, const char* name);

/* Disconnect (joining all worker threads) and free. Safe on NULL. After this returns, no callback
 * can fire. */
NA_EXPORT void na_client_destroy(na_stream_client* client);

/* --- Configuration (set BEFORE na_client_connect; each returns NA_ERR_INVALID on a NULL client) --- */

/* The local playback device id (REQUIRED for RX — connect fails without it). The NULL backend
 * accepts any id. */
NA_EXPORT na_error_t na_client_set_playback_device(na_stream_client* client, int backend_id);
/* The local capture device id (OPTIONAL — only needed for TX). Unsupported on the NULL backend. */
NA_EXPORT na_error_t na_client_set_capture_device(na_stream_client* client, int backend_id);
/* Enable TX audio from na_client_inject_tx_audio instead of (or alongside) a capture device — the
 * headless TX path, and the ONLY way for a NULL-backend client to transmit, since that backend
 * cannot capture. MUST be set before connect: it decides whether the send worker starts, and
 * connect starts the workers once. NA_ERR_INVALID on a NULL client or after connect. */
NA_EXPORT na_error_t na_client_set_tx_inject(na_stream_client* client, int enabled);
/* Select the transport. No effect once connected (returns NA_ERR_INVALID). */
NA_EXPORT na_error_t na_client_set_transport(na_stream_client* client, na_transport transport);
/* Apply a reliability profile (transport + framing + FEC / reorder / adaptive-jitter / control-ARQ)
 * in one call; see na_reliability_profile. This is the ONLY way to enable the client's loss-recovery
 * layer — na_client_set_transport selects the transport and nothing else, so a client configured with
 * it alone runs UDP with FEC, reordering, adaptive jitter and control-ARQ all OFF and discards every
 * parity packet the server sends.
 *
 * Selecting a UDP profile makes a separate na_client_set_transport call unnecessary. The two setters
 * both write the transport and the LAST ONE WINS, so calling na_client_set_transport afterwards
 * changes the transport while leaving the rest of the profile in force.
 *
 * Unlike the server's setter this replaces the client's WHOLE transport/framing/reliability set,
 * including buffer targets: the na_client_* surface has no audio-format or max-clients setting of its
 * own to preserve, and the server pushes the negotiated format to the client during the handshake.
 *
 * MUST be called before na_client_connect — the reliability pipeline is built at connect, from the
 * config as it stands then. NA_ERR_INVALID on a NULL client, an unknown profile, or once connected. */
NA_EXPORT na_error_t na_client_set_reliability_profile(na_stream_client* client,
                                                       na_reliability_profile profile);
/* Identify to the server's roster. Any argument may be NULL to leave that field unset. */
NA_EXPORT na_error_t na_client_set_identity(na_stream_client* client, const char* callsign,
                                            const char* operator_name, const char* location);
/* Register lifecycle/roster/TX callbacks (the struct is COPIED). `user` is passed back to each.
 * `cbs` may be NULL to clear all. Call before connect. */
NA_EXPORT na_error_t na_client_set_callbacks(na_stream_client* client,
                                             const na_client_callbacks* cbs, void* user);
/* Register the hot-path RX PCM callback (a single sink). `cb` may be NULL to clear. Call before
 * connect. */
NA_EXPORT na_error_t na_client_set_audio_cb(na_stream_client* client, na_audio_cb cb, void* user);

/* --- Auto-reconnect (optional; defaults: enabled, 1s base / 30s max delay, 10 attempts) --- */
NA_EXPORT na_error_t na_client_set_auto_reconnect(na_stream_client* client, int enabled);
NA_EXPORT na_error_t na_client_set_reconnect_policy(na_stream_client* client, int base_delay_ms,
                                                    int max_delay_ms, int max_attempts);

/* --- Mute / PTT --- */
/* PTT active => capture unmuted (send voice) + playback muted (no feedback); inactive => reverse. */
NA_EXPORT na_error_t na_client_set_ptt(na_stream_client* client, int tx_active);
NA_EXPORT na_error_t na_client_set_capture_muted(na_stream_client* client, int muted);
NA_EXPORT na_error_t na_client_set_playback_muted(na_stream_client* client, int muted);

/* --- TX audio inject (the client-side mirror of na_server_inject_audio) --- */
/* Queue `n_bytes` of TX PCM for the server without a capture device. Requires
 * na_client_set_tx_inject(client, 1) BEFORE connect, and a connected client.
 *
 * `pcm` MUST already match the negotiated format (48000 / 16-bit / the server's channel count) —
 * nothing here resamples or converts, exactly as with na_server_inject_audio. Non-blocking: the TX
 * ring overwrites its oldest bytes on overrun, the same as captured audio.
 *
 * Gated on PTT: a client that has not called na_client_set_ptt(client, 1) transmits nothing, so
 * injected and captured audio obey identical keying rules.
 *
 * LEN-RETURN CONVENTION: returns the bytes accepted (>= 0), or a negative na_error_t. A 0 return is
 * not an error — it means not connected, TX inject not enabled, or PTT inactive. */
NA_EXPORT int na_client_inject_tx_audio(na_stream_client* client, const unsigned char* pcm,
                                        int n_bytes);

/* --- Lifecycle --- */
/* Connect, handshake, open audio lines, start streaming. Returns NA_OK on success; on failure
 * returns a negative na_error_t and, if `errbuf`/`errlen` are provided, writes the reason
 * (truncated to errlen-1 chars, always NUL-terminated). */
NA_EXPORT na_error_t na_client_connect(na_stream_client* client, char* errbuf, int errlen);
/* Best-effort DISCONNECT to the server, stop reconnection, and join workers. Idempotent. */
NA_EXPORT void na_client_disconnect(na_stream_client* client);
/* 1 if connected, else 0 (also 0 on NULL). */
NA_EXPORT int na_client_is_connected(na_stream_client* client);
/* 1 if streaming, else 0 (also 0 on NULL). */
NA_EXPORT int na_client_is_streaming(na_stream_client* client);

/* --- Server roster (from the server's CLIENTS_UPDATE; -1 / empty before the first update) --- */
NA_EXPORT int na_client_server_client_count(na_stream_client* client);
NA_EXPORT int na_client_server_max_clients(na_stream_client* client);
/* Write the current TX owner id into `buf` using the text functions' length-return-for-truncation
 * convention (len==0 / buf==NULL just returns the needed length). "" (length 0) if no owner. */
NA_EXPORT int na_client_server_tx_owner(na_stream_client* client, char* buf, int len);

/* --- Reliability / transport counters --------------------------------------------------
 *
 * The observability half of na_client_set_reliability_profile: that setter turns FEC,
 * reordering, adaptive jitter and control-ARQ ON, and these counters are how a consumer sees
 * them work. Without them, showing that FEC repaired anything means inferring it from
 * delivered-byte parity against a separate no-loss control run — which can show that delivery
 * survived loss, but not how many packets were repaired.
 *
 * Every field is a CUMULATIVE TOTAL for the CURRENT connection, not a rate. An auto-reconnect
 * installs a fresh connection, so the counters restart from zero; sample them and difference
 * the samples yourself if you want a rate, and treat a decrease as "this is a new connection".
 *
 * WHICH COUNTERS MOVE depends on the profile, because each is owned by a subsystem the profile
 * either configures or leaves off. A counter reading 0 because its subsystem is off is NOT
 * distinguishable here from one reading 0 because nothing happened:
 *
 *   NA_RELIABILITY_DEFAULT (TCP)  packets_/bytes_ + crc_errors only; every reliability
 *                                 counter below stays 0 (TCP has none of the subsystems).
 *   NA_RELIABILITY_UDP_LAN/_FT8   + packets_reordered. FEC is off in these profiles, so both
 *                                 FEC counters stay 0.
 *   NA_RELIABILITY_UDP_WAN        + packets_recovered_by_fec, fec_blocks_unreconciled,
 *                                 jitter_ms, buffer_target_ms.
 *
 * TWO COUNTERS CANNOT MOVE ON A CLIENT AT ALL — control_retransmits and queue_drops. They are not
 * dead code and they are not off: both are written by live paths that only a SERVER-side connection
 * reaches, and this struct reports a client's. Each field says why below. They read 0 rather than the
 * -1 the three unavailable counters use, because -1 is this struct's frozen encoding for "the library
 * is not measuring this", and these two ARE measured — the client simply never produces the event.
 * Treat a 0 in either as carrying no information about the connection.
 */
typedef struct na_client_stats {
    /* 1 if a live connection supplied these numbers. 0 means there is none (before connect,
     * between reconnect attempts, after disconnect) and EVERY field below is its default
     * rather than a reading — check this before believing a zero. */
    int connected;

    long long packets_sent;
    long long packets_received;
    long long bytes_sent;
    long long bytes_received;
    int       crc_errors;         /* undeserializable datagrams (bad CRC / truncated header) */

    long long packets_reordered;         /* delivered in order by the reorder buffer          */
    long long packets_recovered_by_fec;  /* rebuilt from an XOR parity packet — loss repaired */
    /* FEC blocks whose parity could not be reconciled against a contiguous run of audio
     * packets, so recovery was declined rather than run over the wrong member set. This is a
     * lost opportunity to recover, never corrupted audio: the packet stays lost exactly as it
     * would with FEC off. It is what separates "no parity ever arrived" from "the parity
     * arrived and was declined", and it moves whenever control traffic interleaves with an
     * audio block — so a non-zero value here alongside a low packets_recovered_by_fec is the
     * expected shape on a busy roster, not a defect. */
    long long fec_blocks_unreconciled;
    /* Control-ARQ resends (reliability layer, not audio). ALWAYS 0 ON A CLIENT: only a critical
     * control type is tracked for retransmission, and of the four control messages a client sends
     * (CONNECT_REQUEST, HEARTBEAT_ACK, LATENCY_PROBE, DISCONNECT) only DISCONNECT is critical — and
     * it is sent during close, after the heartbeat thread that pumps the retransmit sweep has already
     * exited, so nothing is ever pending when a sweep runs. The counter moves on the SERVER side,
     * which sends CONNECT_ACCEPT / AUDIO_CONFIG / TX_GRANTED / CLIENTS_UPDATE and retransmits them. */
    long long control_retransmits;
    /* Packets discarded from the ordered queue because it was full — audio lost LOCALLY, after the
     * network delivered it successfully. ALWAYS 0 ON A CLIENT: the cap is 2048 packets (~20 s of
     * audio), and on a client the same thread both fills the queue and drains it — the receive path
     * empties the queue before it reads the socket, so a slow consumer stalls the producer with it
     * and the depth never exceeds one reorder burst. A consumer too slow to keep up loses audio in
     * the kernel's socket buffer instead, which no counter here reports. The counter moves on the
     * SERVER side, where a demux thread fills the queue and the application thread drains it. */
    long long queue_drops;
    double    jitter_ms;                 /* current inter-arrival jitter estimate; 0 if off    */
    int       buffer_target_ms;          /* adaptive buffer target; -1 when adaptive jitter is off */

    /* UNAVAILABLE IS NOT ZERO. These three are -1 when the library is not measuring them,
     * which is the case on EVERY profile selectable here: the sequence-gap tracker runs only
     * when no reorder buffer is engaged, and every UDP profile configures one (TCP never
     * tracks gaps at all). -1 means "not measured" and never means "nothing was lost" — to
     * see loss recovery, read packets_recovered_by_fec. They are present, and specified as
     * -1 rather than 0, so that they can begin carrying real values without this struct
     * changing shape if post-reorder loss accounting is ever added. */
    long long packets_lost;
    long long packets_out_of_order;
    double    packet_loss_rate;
} na_client_stats;

/* Fill *out with a snapshot of the client's counters. Safe to call from any thread at any
 * time, including while streaming and before connect (which yields connected == 0 and
 * defaults). NA_ERR_INVALID on a NULL client or a NULL out; NA_OK otherwise — a
 * not-connected client is NOT an error, it is `connected == 0`. */
NA_EXPORT na_error_t na_client_get_stats(na_stream_client* client, na_client_stats* out);

/* ---- Networking audio-streaming server (na_server_*) -----------------------------------
 *
 * The server side of the SAME frozen 0xAF01 v1 protocol the na_client_* surface speaks. A C
 * consumer — Hamlib, a standalone C server, or a Python ctypes/cffi binding —
 * runs an AudioStreamServer: it accepts multiple clients, BROADCASTS radio RX audio to all of
 * them, and RECEIVES TX audio from clients under priority-based arbitration to play to the radio.
 * This is a thin C surface over the C++ naudio::net::AudioStreamServer (+ AudioBroadcaster /
 * AudioMixer), not a re-implementation.
 *
 * TWO BACKENDS — pick the one that matches how YOUR program owns the radio audio:
 *   - NA_SERVER_BACKEND_NULL (hardware-free, the Hamlib-bridge / CI / self-test path): naudio
 *     touches no audio device. RX audio comes IN through na_server_inject_audio() (you read it from
 *     the radio and hand it to naudio to broadcast); mixed TX audio goes OUT through the
 *     na_server_tx_audio_cb (naudio hands you the arbitrated TX stream and you write it to the
 *     radio). No PortAudio init.
 *   - NA_SERVER_BACKEND_SYSTEM (production, naudio owns the devices): RX is CAPTURED from the
 *     device set with na_server_set_capture_device(); mixed TX is PLAYED to the device set with
 *     na_server_set_playback_device(). The na_server_tx_audio_cb is NOT delivered on this backend
 *     (TX goes to the playback device instead); na_server_inject_audio() is also unused.
 *
 * THREADING / LIFETIME CONTRACT — read before using callbacks:
 *   - LIFECYCLE/roster callbacks (na_server_callbacks: started / stopped / client connect-disconnect
 *     / stream start-stop / error) fire on a single dedicated DISPATCH thread, one at a time and in
 *     order — never on an accept/session worker and never under an internal lock. You MAY call any
 *     server method from inside one, INCLUDING na_server_stop, without deadlocking. (The one
 *     exception is na_server_destroy: never destroy a server from within its own callback.)
 *   - The TX audio callback (na_server_tx_audio_cb) fires on the mixer PLAYBACK thread — the data
 *     plane, one call per mixed TX frame (silence frames when no client is transmitting). Keep it
 *     SHORT and non-blocking; do NOT call server lifecycle methods from it; the `pcm` buffer is
 *     valid ONLY for the duration of the call.
 *   - String arguments to the lifecycle callbacks are valid only for the duration of the call.
 *   - Set callbacks AND all configuration (transport / max-clients / devices) BEFORE
 *     na_server_start; they are frozen once it is called (later set_* calls return NA_ERR_INVALID).
 *   - na_server_destroy() stops the server, JOINS every worker, then drains + joins the dispatch
 *     thread before freeing — so no callback can be in flight once it returns.
 *   - No C++ exception ever crosses this boundary; failures surface as na_error_t / NULL +
 *     na_last_error(), exactly like the client and device ABIs above.
 */

/* Local audio backend for the server's RX capture and TX playback. */
typedef enum na_server_backend {
    NA_SERVER_BACKEND_SYSTEM = 0,  /* PortAudio: capture RX from / play TX to real devices.        */
    NA_SERVER_BACKEND_NULL   = 1   /* Hardware-free: RX via na_server_inject_audio(), TX via the
                                      na_server_tx_audio_cb. No PortAudio, no devices.             */
} na_server_backend;

/* na_reliability_profile is declared with the client surface above — both ends select a profile,
 * and both must select a matching one. */

/* Opaque streaming-server handle. Create with na_server_create, free with na_server_destroy. */
typedef struct na_audio_server na_audio_server;

/* Mixed TX PCM destined for the radio: `pcm` / `n_bytes` is one arbitrated TX frame's payload,
 * valid only for the duration of the call. Fires on the mixer playback thread (NULL backend only).
 * Silence frames are delivered when no client is transmitting (the radio expects a continuous
 * stream). */
typedef void (*na_server_tx_audio_cb)(const unsigned char* pcm, size_t n_bytes, void* user);

/* Lifecycle / roster events (mirror AudioStreamListener). EVERY field may be NULL (that event is
 * ignored). All fire on the dispatch thread; `user` is the pointer passed to na_server_set_callbacks.
 * String arguments are valid only for the duration of the call. */
typedef struct na_server_callbacks {
    void (*on_started)(int port, void* user);
    void (*on_stopped)(void* user);
    void (*on_client_connected)(const char* client_id, const char* addr, void* user);
    void (*on_client_disconnected)(const char* client_id, void* user);
    void (*on_stream_started)(const char* client_id, void* user);
    void (*on_stream_stopped)(const char* client_id, void* user);
    void (*on_error)(const char* client_id, const char* message, void* user);
} na_server_callbacks;

/* Create a streaming server bound to `port` (0 => an OS-assigned ephemeral port, readable after
 * start via na_server_port; 1..65535 for a fixed port). `backend` selects the audio backend.
 * Returns NULL on failure; na_last_error() gives the cause (NA_ERR_INVALID for a bad port,
 * NA_ERR_INIT if the SYSTEM backend's PortAudio could not initialize, NA_ERR_NOMEM). */
NA_EXPORT na_audio_server* na_server_create(na_server_backend backend, int port);

/* Stop the server (joining all workers + the dispatch thread) and free. Safe on NULL. After this
 * returns, no callback can fire. */
NA_EXPORT void na_server_destroy(na_audio_server* server);

/* --- Configuration (set BEFORE na_server_start; each returns NA_ERR_INVALID on a NULL server or
 *     if the server has already been started) --- */

/* Select the transport served (TCP / UDP / DUAL). */
NA_EXPORT na_error_t na_server_set_transport(na_audio_server* server, na_transport transport);
/* Maximum simultaneous clients (must be > 0). Default 4. */
NA_EXPORT na_error_t na_server_set_max_clients(na_audio_server* server, int max_clients);
/* Audio wire format the server advertises and broadcasts. `bits_per_sample` must be 16 (the v1 wire
 * carries signed 16-bit PCM), `channels` 1 or 2, `sample_rate` > 0. Bytes fed to na_server_inject_audio
 * (and delivered to na_server_tx_audio_cb) MUST match this layout — naudio does not resample or convert.
 * Default is 48000 / 16 / 2. NA_ERR_INVALID on a bad value / NULL server / after start. */
NA_EXPORT na_error_t na_server_set_audio_format(na_audio_server* server, int sample_rate,
                                                int bits_per_sample, int channels);
/* Apply a UDP reliability profile (transport + framing + FEC/reorder/jitter/control-ARQ) in one call;
 * see na_reliability_profile. Leaves audio format and max-clients untouched, so it composes freely with
 * na_server_set_audio_format / na_server_set_max_clients. Selecting a UDP profile makes a separate
 * na_server_set_transport call unnecessary. NA_ERR_INVALID on a NULL server / bad profile / after start. */
NA_EXPORT na_error_t na_server_set_reliability_profile(na_audio_server* server,
                                                       na_reliability_profile profile);
/* RX capture device id (SYSTEM backend only; NA_ERR_UNSUPPORTED on the NULL backend). If no capture
 * device is set the server runs inject-only (RX comes from na_server_inject_audio). */
NA_EXPORT na_error_t na_server_set_capture_device(na_audio_server* server, int backend_id);
/* TX playback device id (SYSTEM backend only; NA_ERR_UNSUPPORTED on the NULL backend, which extracts
 * TX through na_server_tx_audio_cb instead). */
NA_EXPORT na_error_t na_server_set_playback_device(na_audio_server* server, int backend_id);
/* Register lifecycle/roster callbacks (the struct is COPIED). `user` is passed back to each. `cbs`
 * may be NULL to clear all. Call before start. */
NA_EXPORT na_error_t na_server_set_callbacks(na_audio_server* server,
                                             const na_server_callbacks* cbs, void* user);
/* Register the mixed-TX-audio callback (NULL backend only; a single sink). `cb` may be NULL to
 * clear. Call before start. */
NA_EXPORT na_error_t na_server_set_tx_audio_cb(na_audio_server* server, na_server_tx_audio_cb cb,
                                               void* user);

/* --- Lifecycle --- */
/* Bind, init audio, and start accepting clients. Returns NA_OK on success; on failure returns a
 * negative na_error_t and, if `errbuf`/`errlen` are provided, writes the reason (truncated to
 * errlen-1 chars, always NUL-terminated). Call exactly once per server (a second call after a
 * successful start returns NA_ERR_INVALID). */
NA_EXPORT na_error_t na_server_start(na_audio_server* server, char* errbuf, int errlen);
/* Stop accepting, close all sessions, join workers, tear down audio. Idempotent. */
NA_EXPORT void na_server_stop(na_audio_server* server);
/* 1 if running, else 0 (also 0 on NULL). */
NA_EXPORT int na_server_is_running(na_audio_server* server);
/* The bound port (>0) once started, or -1 before start / on NULL. */
NA_EXPORT int na_server_port(na_audio_server* server);

/* --- Audio I/O --- */
/* Broadcast `n_bytes` of RX PCM to all connected clients (the radio-RX-audio analog; NULL backend /
 * inject-only). Returns NA_OK, or NA_ERR_INVALID on a NULL server / NULL buffer / n_bytes <= 0. */
NA_EXPORT na_error_t na_server_inject_audio(na_audio_server* server, const unsigned char* pcm,
                                            int n_bytes);

/* --- Roster --- */
/* Currently-connected client count (>= 0), or -1 on NULL / error (na_last_error disambiguates). */
NA_EXPORT int na_server_client_count(na_audio_server* server);
/* The configured maximum client count (> 0), or -1 on NULL / error. */
NA_EXPORT int na_server_max_clients(na_audio_server* server);
/* Write the current TX owner id into `buf` using the text functions' length-return-for-truncation
 * convention (len==0 / buf==NULL just returns the needed length). "" (length 0) if no owner. */
NA_EXPORT int na_server_tx_owner(na_audio_server* server, char* buf, int len);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* NAUDIO_H */
