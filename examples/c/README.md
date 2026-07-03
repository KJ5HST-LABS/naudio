<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Play to speakers (C)

Connect to a network-audio server and play whatever it is streaming on your local
speakers. A small, standalone C program that uses only the public `naudio.h` C ABI —
so it is also a worked example of writing a full streaming client from plain C.

The playback is done by naudio itself: you pick a local output device and the library
renders the received audio to it. By default the client picks your first output device
and starts playing, so the simplest run needs no flags.

## Build

It builds with the toolkit (apps are on by default):

```sh
cmake -S . -B build
cmake --build build --target na_c_play_to_speakers
```

The program is `build/examples/na_c_play_to_speakers`.

## Run

**Hear something with no extra hardware** — pair it with the [demo source](../server):

```sh
build/examples/na_audio_source --test-tone --port 4533     # one terminal: a test tone
build/examples/na_c_play_to_speakers --port 4533           # another: plays it on your speakers
```

**Choose an output device:**

```sh
build/examples/na_c_play_to_speakers --list-devices        # list output device ids
build/examples/na_c_play_to_speakers --playback-id 2 --host 192.168.1.10 --port 4533
```

With no `--playback-id`, the first output device is selected automatically.

**Run hardware-free** (no sound card needed — the audio is received but not played):

```sh
build/examples/na_c_play_to_speakers --backend null --port 4533 --seconds 5
```

## Options

```
--host H          server host (default 127.0.0.1)
--port N          server port (default 4533)
--name S          this client's name in the server roster (default na-c-client)
--playback-id N   output device id to play on (default: first output device)
--transport T     tcp (default) | udp
--seconds N       run time; 0 = until Ctrl-C (default 0)
--backend B       system (default; plays to the output device) | null (hardware-free)
--list-devices    print output device ids, then exit
```

## Expected output

Human-readable logs go to stderr; a one-line machine-readable summary goes to stdout.
A short run against the test-tone source looks like:

```
connected to 127.0.0.1:4533 (backend=null, transport=tcp); receiving for 3s ...
[client] connected id=... addr=127.0.0.1
  t= 1s  rx=192000   B  frames=50       767999 B/s  conn=1
RESULT status=PASS connected=true rx_frames=114 ... first_frame_hex=68c569c66ac76bc8 avg_bps=...
```

`first_frame_hex=68c569c66ac76bc8` is the test tone's fixed first frame: seeing it confirms the
audio crossed the wire byte-for-byte. With `--backend system` (the default) you also hear the tone.

Real-speaker playback needs audio hardware, so it is not part of the automated test suite; the
`--backend null` path is the hardware-free check.
