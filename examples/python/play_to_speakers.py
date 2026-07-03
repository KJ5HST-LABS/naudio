#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio examples — play a network audio stream to your speakers (Python).
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# A dead-simple client: connect to a network-audio server and play whatever it
# is streaming on your local speakers. It drives the public `naudio.h` C ABI over
# Python's standard-library `ctypes` — and only that C ABI — so it is also a worked
# example of writing a full streaming client from Python with no compile step.
#
# The playback is done by naudio itself: you pick a local output device and the
# library renders the received audio to it. By default the client picks your first
# output device and starts playing — no flags required:
#
#     play_to_speakers.py --host 127.0.0.1 --port 4533
#
# Pair it with the demo source to hear something without a radio or a second
# machine (see examples/server):
#
#     na_audio_source --test-tone --port 4533       # one terminal: a test tone
#     play_to_speakers.py --port 4533               # another — you hear the tone
#
# Use `--list-devices` to see the output-device ids, then `--playback-id N` to
# choose one. `--backend null` runs hardware-free (the audio is still received,
# it just is not played) — handy on a headless box, in CI, or to verify a server
# end to end without a sound card.
#
# THREADING (from naudio.h): the RX audio callback fires on naudio's receive
# WORKER thread, once per frame. Keep it SHORT and non-blocking, do not call client
# lifecycle methods from it, and do not let the ctypes callback objects be
# garbage-collected while the client is alive (we keep references in _KEEPALIVE).
#
# The library is found via (in order): --lib PATH, $NAUDIO_LIB, the sibling build
# tree (../../build), then the system loader (an installed libnaudio).
#
# Output: the machine-readable `RESULT ...` line goes to STDOUT; all human logs
# (events, the throughput meter, the summary) go to STDERR.

import argparse
import ctypes as C
import os
import sys
import time
from ctypes.util import find_library


def log(*a):
    """Human-readable output -> stderr (stdout is reserved for the RESULT line)."""
    print(*a, file=sys.stderr, flush=True)


# ----------------------------------------------------------------------------------
# Locate + load libnaudio
# ----------------------------------------------------------------------------------

def _lib_filenames():
    if sys.platform == "darwin":
        return ["libnaudio.dylib"]
    if sys.platform == "win32":
        return ["naudio.dll", "libnaudio.dll"]
    return ["libnaudio.so"]


def load_naudio(explicit_path=None):
    """Load libnaudio and return the CDLL, trying explicit path, env, build tree, loader."""
    candidates = []
    if explicit_path:
        candidates.append(explicit_path)
    if os.environ.get("NAUDIO_LIB"):
        candidates.append(os.environ["NAUDIO_LIB"])
    here = os.path.dirname(os.path.abspath(__file__))
    build = os.path.normpath(os.path.join(here, "..", "..", "build"))
    for name in _lib_filenames():
        candidates.append(os.path.join(build, name))
    found = find_library("naudio")  # installed (handles lib prefix / soname)
    if found:
        candidates.append(found)
    candidates.extend(_lib_filenames())  # bare name -> default loader search

    tried = []
    for path in candidates:
        try:
            return C.CDLL(path)
        except OSError as exc:
            tried.append(f"  {path}: {exc}")
    sys.exit(
        "could not load libnaudio. Build it first:\n"
        "  (cd naudio && cmake -S . -B build && cmake --build build -j)\n"
        "or pass --lib /path/to/libnaudio, or set NAUDIO_LIB.\nTried:\n" + "\n".join(tried)
    )


# ----------------------------------------------------------------------------------
# C ABI types (mirror include/naudio.h)
# ----------------------------------------------------------------------------------

NA_OK = 0
NA_CLIENT_BACKEND_SYSTEM = 0
NA_CLIENT_BACKEND_NULL = 1
NA_TRANSPORT_TCP = 0
NA_TRANSPORT_UDP = 1


class NaDevice(C.Structure):
    _fields_ = [
        ("backend_id", C.c_int),
        ("capture_backend_id", C.c_int),
        ("playback_backend_id", C.c_int),
        ("name", C.c_char * 256),
        ("host_api", C.c_char * 128),
        ("type", C.c_int),
        ("capability", C.c_int),
        ("is_virtual", C.c_int),
    ]


# Hot-path RX PCM callback: void(const unsigned char* pcm, size_t n_bytes, void* user)
NA_AUDIO_CB = C.CFUNCTYPE(None, C.POINTER(C.c_ubyte), C.c_size_t, C.c_void_p)

# Event-callback function-pointer types (every field of na_client_callbacks).
_CB_ID = C.CFUNCTYPE(None, C.c_char_p, C.c_void_p)                       # (client_id, user)
_CB_ID2 = C.CFUNCTYPE(None, C.c_char_p, C.c_char_p, C.c_void_p)          # (id, str, user)
_CB_RECON = C.CFUNCTYPE(None, C.c_char_p, C.c_int, C.c_int, C.c_void_p)  # (id, attempt, max, user)
_CB_ROSTER = C.CFUNCTYPE(None, C.c_int, C.c_int, C.c_char_p,
                         C.POINTER(C.c_char_p), C.c_int, C.c_void_p)
_CB_VOID = C.CFUNCTYPE(None, C.c_void_p)                                 # (user)


class NaClientCallbacks(C.Structure):
    _fields_ = [
        ("on_connected", _CB_ID2),       # (client_id, server_addr, user)
        ("on_disconnected", _CB_ID),
        ("on_stream_started", _CB_ID),
        ("on_stream_stopped", _CB_ID),
        ("on_error", _CB_ID2),           # (client_id, message, user)
        ("on_reconnecting", _CB_RECON),
        ("on_reconnected", _CB_ID),
        ("on_clients_update", _CB_ROSTER),
        ("on_tx_granted", _CB_VOID),
        ("on_tx_denied", _CB_ID),
        ("on_tx_preempted", _CB_ID),
        ("on_tx_released", _CB_VOID),
    ]


def bind(lib):
    """Declare argtypes/restypes for the na_* functions this example uses."""
    lib.na_strerror.argtypes = [C.c_int]
    lib.na_strerror.restype = C.c_char_p
    lib.na_last_error.argtypes = []
    lib.na_last_error.restype = C.c_int

    lib.na_context_create.argtypes = []
    lib.na_context_create.restype = C.c_void_p
    lib.na_context_destroy.argtypes = [C.c_void_p]
    lib.na_enumerate.argtypes = [C.c_void_p, C.POINTER(NaDevice), C.c_int]
    lib.na_enumerate.restype = C.c_int

    lib.na_client_create.argtypes = [C.c_int, C.c_char_p, C.c_int, C.c_char_p]
    lib.na_client_create.restype = C.c_void_p
    lib.na_client_destroy.argtypes = [C.c_void_p]
    lib.na_client_set_transport.argtypes = [C.c_void_p, C.c_int]
    lib.na_client_set_playback_device.argtypes = [C.c_void_p, C.c_int]
    lib.na_client_set_audio_cb.argtypes = [C.c_void_p, NA_AUDIO_CB, C.c_void_p]
    lib.na_client_set_callbacks.argtypes = [C.c_void_p, C.POINTER(NaClientCallbacks), C.c_void_p]
    lib.na_client_connect.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
    lib.na_client_connect.restype = C.c_int
    lib.na_client_disconnect.argtypes = [C.c_void_p]
    lib.na_client_is_connected.argtypes = [C.c_void_p]
    lib.na_client_is_connected.restype = C.c_int
    lib.na_client_is_streaming.argtypes = [C.c_void_p]
    lib.na_client_is_streaming.restype = C.c_int


# Keep ctypes callback trampolines alive for the client's lifetime (else GC frees them).
_KEEPALIVE = []


# ----------------------------------------------------------------------------------
# Device enumeration
# ----------------------------------------------------------------------------------

def enumerate_devices(lib):
    ctx = lib.na_context_create()
    if not ctx:
        sys.exit("na_context_create failed: " + lib.na_strerror(lib.na_last_error()).decode())
    try:
        arr = (NaDevice * 64)()
        n = lib.na_enumerate(ctx, arr, 64)
        if n < 0:
            sys.exit("na_enumerate failed: " + lib.na_strerror(n).decode())
        return [arr[i] for i in range(n)]
    finally:
        lib.na_context_destroy(ctx)


def pick_playback_device(devices):
    for d in devices:
        if d.playback_backend_id >= 0:
            return d
    return None


def list_devices(devices):
    log(f"{len(devices)} device(s):")
    for d in devices:
        dirs = []
        if d.capture_backend_id >= 0:
            dirs.append(f"cap={d.capture_backend_id}")
        if d.playback_backend_id >= 0:
            dirs.append(f"play={d.playback_backend_id}")
        tag = " [virtual]" if d.is_virtual else ""
        log(f"  [{d.backend_id}] {d.name.decode(errors='replace')} "
            f"({d.host_api.decode(errors='replace')}) {' '.join(dirs)}{tag}")


# ----------------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Play a remote naudio stream on local speakers (Python/ctypes).")
    ap.add_argument("--host", default="127.0.0.1", help="server host (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=4533, help="server port (default 4533)")
    ap.add_argument("--name", default="na-py-client",
                    help="this client's name in the server roster (default na-py-client)")
    ap.add_argument("--playback-id", type=int, default=-1,
                    help="output device id to play on; -1 (default) auto-picks the first one")
    ap.add_argument("--transport", choices=["tcp", "udp"], default="tcp",
                    help="tcp (default) | udp")
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="run time; 0 (default) = until Ctrl-C")
    ap.add_argument("--backend", choices=["system", "null"], default="system",
                    help="system (default; plays RX to the output device) | null (hardware-free)")
    ap.add_argument("--list-devices", action="store_true", help="list audio devices and exit")
    ap.add_argument("--lib", help="path to libnaudio (else env NAUDIO_LIB, build tree, system loader)")
    args = ap.parse_args()

    lib = load_naudio(args.lib)
    bind(lib)

    devices = enumerate_devices(lib)

    if args.list_devices:
        list_devices(devices)
        return 0

    backend = NA_CLIENT_BACKEND_SYSTEM if args.backend == "system" else NA_CLIENT_BACKEND_NULL
    transport = NA_TRANSPORT_TCP if args.transport == "tcp" else NA_TRANSPORT_UDP

    # Resolve the playback device up front so we can fail early with guidance. The SYSTEM
    # backend needs a real output device; the NULL backend accepts any id (it plays nothing).
    playback_id = args.playback_id
    if backend == NA_CLIENT_BACKEND_SYSTEM:
        if playback_id < 0:
            dev = pick_playback_device(devices)
            if dev is None:
                sys.exit("error: no output device found. Run --list-devices, pass "
                         "--playback-id N, or use --backend null to run without one.")
            playback_id = dev.playback_backend_id
            log(f"[client] auto-selected output device id={playback_id} "
                f"({dev.name.decode(errors='replace')}) — run --list-devices to choose another")
    elif playback_id < 0:
        playback_id = 0  # null backend ignores the value but the field is required

    # --- RX frame meter: written on the receive worker thread, read from main ---------------
    # The hot-path audio callback fires on naudio's receive worker thread (one call per RX
    # frame). Keep it short: copy the frame once with string_at(), then count zero bytes in C
    # (a frame of underrun silence is all-zero, so non-zero bytes prove real content arrived).
    # Only one worker thread writes these counters; main reads them (the CPython GIL makes each
    # read/write atomic, and the final read happens after disconnect() has joined the worker).
    stats = {"frames": 0, "bytes": 0, "nonzero": 0, "first_hex": None}

    def on_pcm(pcm, n_bytes, user):
        buf = C.string_at(pcm, n_bytes)
        stats["frames"] += 1
        stats["bytes"] += n_bytes
        stats["nonzero"] += n_bytes - buf.count(0)
        if stats["first_hex"] is None and n_bytes >= 8:
            stats["first_hex"] = buf[:8].hex()
    audio_cb = NA_AUDIO_CB(on_pcm)
    _KEEPALIVE.append(audio_cb)

    # --- A few lifecycle events for friendly output (all to stderr, all kept short) ---
    def on_connected(cid, addr, user):
        log(f"[client] connected id={cid.decode() if cid else ''} addr={addr.decode() if addr else ''}")
    def on_stream_started(cid, user):
        log("[client] stream started")
    def on_error(cid, msg, user):
        log(f"[client] ERROR: {msg.decode() if msg else '?'}")
    def on_disconnected(cid, user):
        log("[client] disconnected")

    cbs = NaClientCallbacks()
    cbs.on_connected = _CB_ID2(on_connected)
    cbs.on_stream_started = _CB_ID(on_stream_started)
    cbs.on_error = _CB_ID2(on_error)
    cbs.on_disconnected = _CB_ID(on_disconnected)
    _KEEPALIVE.extend([cbs.on_connected, cbs.on_stream_started, cbs.on_error, cbs.on_disconnected])

    client = lib.na_client_create(backend, args.host.encode(), args.port, args.name.encode())
    if not client:
        sys.exit("na_client_create failed: " + lib.na_strerror(lib.na_last_error()).decode())

    connected_end = False
    try:
        # Configure BEFORE connect — the worker threads read callbacks/config once streaming starts.
        lib.na_client_set_callbacks(client, C.byref(cbs), None)
        lib.na_client_set_audio_cb(client, audio_cb, None)
        lib.na_client_set_transport(client, transport)
        lib.na_client_set_playback_device(client, playback_id)  # REQUIRED for RX

        backend_name = "system" if backend == NA_CLIENT_BACKEND_SYSTEM else "null"

        errbuf = C.create_string_buffer(256)
        rc = lib.na_client_connect(client, errbuf, len(errbuf))
        if rc != NA_OK:
            sys.exit(f"error: connect to {args.host}:{args.port} failed "
                     f"({lib.na_strerror(rc).decode()}): {errbuf.value.decode()}")

        verb = "playing" if backend == NA_CLIENT_BACKEND_SYSTEM else "receiving"
        span = f"for {args.seconds:g}s" if args.seconds > 0 else "until Ctrl-C"
        log(f"connected to {args.host}:{args.port} (backend={backend_name}, "
            f"transport={args.transport}); {verb} {span} ...")

        # Monitor loop: a per-second throughput meter so you can see audio arriving.
        start = time.monotonic()
        deadline = (start + args.seconds) if args.seconds > 0 else None
        prev_bytes, prev_t = 0, start
        try:
            while True:
                time.sleep(0.2)
                t = time.monotonic()
                if t - prev_t >= 1.0:
                    b = stats["bytes"]
                    bps = int((b - prev_bytes) / (t - prev_t))
                    log(f"  t={int(t - start)}s  rx={b} B  frames={stats['frames']}  "
                        f"{bps} B/s  conn={lib.na_client_is_connected(client)}")
                    prev_bytes, prev_t = b, t
                if deadline and t >= deadline:
                    break
                if not lib.na_client_is_connected(client):
                    log("connection lost")
                    break
        except KeyboardInterrupt:
            log("interrupted")

        connected_end = bool(lib.na_client_is_connected(client))
        elapsed = time.monotonic() - start
        lib.na_client_disconnect(client)  # stop reconnection + join workers; no callback after this
    finally:
        lib.na_client_destroy(client)

    frames = stats["frames"]
    rx_bytes = stats["bytes"]
    nonzero = stats["nonzero"]
    first_hex = stats["first_hex"] or ""
    avg_bps = int(rx_bytes / elapsed) if elapsed > 0 else 0

    # PASS = received real (non-silent) audio. A bounded run (--seconds) that got nothing FAILS
    # (exit 1); an interactive (Ctrl-C) run always exits 0 — you chose when to stop.
    gate = args.seconds > 0
    passed = frames > 0 and nonzero > 0

    log("\n=== play to speakers — summary ===")
    log(f"  server         : {args.host}:{args.port} ({args.transport})")
    log(f"  rx             : {frames} frames, {rx_bytes} bytes ({nonzero} non-zero)")
    log(f"  throughput     : avg {avg_bps} B/s over {elapsed * 1000:.0f} ms")
    log(f"  first_frame_hex: {first_hex or '(none)'}")
    log(f"  connected      : {'yes' if connected_end else 'no'}")

    # Machine-readable result on STDOUT (handy for a scripted check).
    print(f"RESULT status={'PASS' if passed else 'FAIL'} "
          f"connected={'true' if connected_end else 'false'} "
          f"rx_frames={frames} rx_bytes={rx_bytes} nonzero_bytes={nonzero} "
          f"first_frame_hex={first_hex} avg_bps={avg_bps}", flush=True)

    return 1 if (gate and not passed) else 0


if __name__ == "__main__":
    sys.exit(main())
