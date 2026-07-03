<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Demo audio source

A tiny streaming server so you can run one of the example clients and actually
**hear something** — no radio and no second machine required. It broadcasts audio
to every connected client over the network-audio protocol, using only the public
`naudio.h` C ABI.

It runs in two ways:

- **Capture mode (default)** — opens a local audio input device and broadcasts the
  live audio. The input can be a real microphone / line-in, or a *virtual loopback*
  (BlackHole on macOS, a PulseAudio/PipeWire null sink on Linux) so you can stream
  whatever the machine is already playing.
- **Test-tone mode (`--test-tone`)** — opens no device at all and broadcasts a fixed
  tone. Use it when there is no capture device (a headless box, CI) or to confirm a
  client works end to end. The stream is fully deterministic.

## Build

It builds with the toolkit (apps are on by default):

```sh
cmake -S . -B build
cmake --build build --target na_audio_source
```

The program is `build/examples/na_audio_source`.

## Run

**Hear a test tone (no hardware needed):**

```sh
build/examples/na_audio_source --test-tone --port 4533
# then, in another terminal, connect a client and play it to your speakers:
build/examples/na_c_play_to_speakers --backend system --playback-id N --host 127.0.0.1 --port 4533
```

Find a playback device id `N` for the client with that client's `--list-devices`.

**Broadcast a local input device:**

```sh
build/examples/na_audio_source --list-devices       # list capture-capable device ids
build/examples/na_audio_source --capture-id 3 --port 4533
```

With no `--capture-id`, the first capture-capable device is selected automatically.

## Options

```
--port N          listen port; 0 = OS-assigned ephemeral (default 4533)
--capture-id N    capture device id to broadcast (default: first capture device)
--transport T     tcp (default) | udp
--test-tone       hardware-free: broadcast a deterministic tone (no device)
--max-clients N   maximum simultaneous clients (default 4)
--seconds N       run time; 0 = until Ctrl-C (default 0)
--list-devices    print capture-capable device ids, then exit
```

## Expected output

The bound port is printed to stdout as `LISTENING port=N` (useful with `--port 0`);
human-readable logs go to stderr. A short test-tone run with one client looks like:

```
LISTENING port=4533
[source] test-tone source on tcp:4533 (max 4 clients); tone_first_frame_hex=68c569c66ac76bc8
[source] client connected id=... addr=127.0.0.1 (now 1)
SUMMARY port=4533 peak_clients=1 frames_injected=345 mode=test-tone
```

On the client side you will hear a steady tone, and its result line reports the same
first-frame fingerprint — the audio crossed the wire byte-for-byte:

```
RESULT status=PASS connected=true rx_frames=114 first_frame_hex=68c569c66ac76bc8 ...
```

Real-device capture and real-speaker playback need audio hardware, so they are not
part of the automated test suite; the test-tone path is the hardware-free check.
