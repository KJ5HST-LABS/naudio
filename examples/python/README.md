<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Play to speakers (Python)

Connect to a network-audio server and play whatever it is streaming on your local
speakers. A small, standalone Python script that drives the public `naudio.h` C ABI
over the standard-library `ctypes` module — and only that C ABI — so it is also a
worked example of writing a full streaming client from Python with no compile step.

The playback is done by naudio itself: you pick a local output device and the library
renders the received audio to it. By default the client picks your first output device
and starts playing, so the simplest run needs no flags. Because playback is naudio's
own PortAudio backend, the script needs no third-party Python audio package.

## Build

The script itself needs no build, but it loads `libnaudio`, so build the toolkit once
(the shared library is on by default):

```sh
cmake -S . -B build
cmake --build build -j
```

The script finds `libnaudio` via, in order: `--lib PATH`, the `NAUDIO_LIB` environment
variable, the sibling build tree (`../../build`), then the system loader (an installed
`libnaudio`). Python 3.7+ is required; there are no other dependencies.

## Run

**Hear something with no extra hardware** — pair it with the [demo source](../server):

```sh
build/examples/na_audio_source --test-tone --port 4533              # one terminal: a test tone
python3 examples/python/play_to_speakers.py --port 4533             # another: plays it on your speakers
```

**Choose an output device:**

```sh
python3 examples/python/play_to_speakers.py --list-devices          # list device ids
python3 examples/python/play_to_speakers.py --playback-id 2 --host 192.168.1.10 --port 4533
```

With no `--playback-id`, the first output device is selected automatically.

**Run hardware-free** (no sound card needed — the audio is received but not played):

```sh
python3 examples/python/play_to_speakers.py --backend null --port 4533 --seconds 5
```

To use UDP, give the **same** `--transport udp` to both the source and the client — a server
serves the one transport it is configured for (TCP by default), so the two must match.

## Options

```
--host H          server host (default 127.0.0.1)
--port N          server port (default 4533)
--name S          this client's name in the server roster (default na-py-client)
--playback-id N   output device id to play on (default: first output device)
--transport T     tcp (default) | udp
--seconds N       run time; 0 = until Ctrl-C (default 0)
--backend B       system (default; plays to the output device) | null (hardware-free)
--list-devices    print device ids, then exit
--lib PATH        path to libnaudio (else $NAUDIO_LIB, build tree, system loader)
```

## Expected output

Human-readable logs go to stderr; a one-line machine-readable summary goes to stdout.
A short run against the test-tone source looks like:

```
connected to 127.0.0.1:4533 (backend=null, transport=tcp); receiving for 3s ...
[client] connected id=local addr=127.0.0.1:4533
  t=1s  rx=192000 B  frames=50  151996 B/s  conn=1
RESULT status=PASS connected=true rx_frames=115 ... first_frame_hex=68c569c66ac76bc8 avg_bps=...
```

`first_frame_hex=68c569c66ac76bc8` is the test tone's fixed first frame: seeing it confirms the
audio crossed the wire byte-for-byte. With `--backend system` (the default) you also hear the tone.

Real-speaker playback needs audio hardware, so it is not part of the automated test suite; the
`--backend null` path is the hardware-free check.

## What the C ABI looks like from Python

The script declares the `na_*` functions it uses with `ctypes` and mirrors a couple of structs
(`na_device`, `na_client_callbacks`):

| Area | C ABI used |
|---|---|
| Devices | `na_context_create` → `na_enumerate` → `na_context_destroy` |
| Lifecycle | `na_client_create` → `na_client_connect` → `na_client_disconnect` → `na_client_destroy` |
| RX audio | `na_client_set_audio_cb` — the hot-path PCM sink (a short frame meter here; a real app consumes the PCM) |
| Events | `na_client_set_callbacks` — connected / stream-started / error / disconnected |
| Config | `na_client_set_transport`, `na_client_set_playback_device` |

**Threading (from `naudio.h`):** the RX audio callback fires on naudio's receive **worker**
thread, once per frame — keep it short, don't call client lifecycle methods from it, and don't
let the `ctypes` callback objects be garbage-collected while the client is alive (the script keeps
references in `_KEEPALIVE`). The call sequence is identical to the C and C++ examples.
