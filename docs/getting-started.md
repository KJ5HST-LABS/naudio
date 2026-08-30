<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Getting started

naudio streams live audio between computers. This walkthrough goes from an installed
toolkit to audio moving across your network, using only the programs that come with it —
no code, and for the first step, no microphone. Install first: **[INSTALL.md](../INSTALL.md)**.
Every program here also has a manual page (`man na_audio_source`).

## 1. Hear it work — one machine, about a minute

Open two terminals.

```bash
# terminal 1 — start a server broadcasting the built-in test tone:
na_audio_source --test-tone

# terminal 2 — connect a player and listen:
na_c_play_to_speakers
```

You hear a steady tone. Stop both with Ctrl-C.

What just happened: the first program is a real streaming server (the tone stands in for
a sound source); the second connected to it over the network stack — the same path a
connection from another machine takes — received the stream, and played it on your
default output.

## 2. Now across two machines

On the machine that will broadcast:

```bash
na_audio_source --test-tone
```

On any other machine on the network:

```bash
na_c_play_to_speakers --host <address-of-the-first-machine>
```

The tone comes out of the second machine's speakers. Any number of machines can listen at
once (the server admits 4 by default — `--max-clients` raises it). If nothing arrives,
check that TCP port 4533 is open on the broadcasting machine; `--port` changes it on both
ends.

## 3. Stream something real

Replace the test tone with an actual audio input — a microphone, a line-in, a USB audio
device:

```bash
na_audio_source --list-devices        # shows each input with an id number
na_audio_source --capture-id <N>
```

Listen from anywhere exactly as before. Two rules save confusion here:

- **Mono and stereo are explicit.** The default is stereo; most microphones are mono.
  naudio never converts silently — if the device can't open as asked, the start fails
  with a clear message instead of streaming something mislabeled. Pass `--channels 1`
  for a mono source.
- **The sample rate is explicit too** (default 48000). naudio never resamples; a device
  that can't do the requested rate is refused, not approximated.

## 4. Record what a listener hears

`na_wav_tap` is a client that saves the stream to a WAV file instead of playing it:

```bash
na_wav_tap --host <server> --seconds 5 --out hello.wav --tcp
```

Play `hello.wav` with anything. The `--tcp` matters: **the transport must match on both
ends, and the defaults differ** — the servers default to TCP while `na_wav_tap` defaults
to UDP with loss recovery (its usual habitat is imperfect links). A mismatch fails
cleanly at connect (`Handshake failed`); it never half-works. Either pass `--tcp` to the
recorder, or run the server with `--transport udp --reliability wan` and let the
recorder's default match it.

## 5. UDP, and networks that lose packets

TCP is the friction-free default on a LAN. On links that drop or reorder packets, UDP
with a loss-recovery profile keeps latency low while repairing the damage (forward error
correction, reordering, jitter buffering):

```bash
na_audio_source --test-tone --transport udp --reliability wan
na_c_play_to_speakers --host <server> --transport udp --reliability wan
```

Plain UDP with no recovery profile is refused on purpose — unprotected UDP audio on a
real network sounds like torn paper, so naudio makes the protection explicit rather than
the failure silent.

## 6. When the connection is slow

The default format (48 kHz, 16-bit, stereo) costs about 1.5 Mbit/s, and naudio provisions
bandwidth up front rather than degrading quality mid-stream. On a connection that can't
carry that, declare a smaller format at the server — clients adapt automatically:

```bash
na_audio_source --capture-id <N> --rate 12000 --channels 1    # ~192 kbit/s
```

12 kHz mono still covers speech fully. The rate/bandwidth table is in the README under
*Known limitations*.

## 7. The device-serving daemon

`na_audio_daemon` is the heavier sibling of `na_audio_source`, for permanent
installations and diagnostics: a `capture-probe` mode that tests an input device with no
network involved (frame counts, overflow detection, signal levels), and a `hardware` mode
that serves the device while continuously measuring throughput. Its manual page covers
both. If you only want audio on the network, `na_audio_source` is enough.

## 8. For radio amateurs

naudio's original habitat: put a receiver's audio on the network by streaming its USB
audio codec (steps 3–4, with the radio as the capture device), and record decoder-ready
WAV files with `na_wav_tap` — it writes 12 kHz mono, aligned to FT8 periods with
`--align15`, and the evidence-grade verification procedure lives in
**[on-air-verification.md](on-air-verification.md)**. The optional `na_hamlib_bridge`
connects naudio to Hamlib's rig-audio streaming API for rigs whose audio arrives through
Hamlib rather than a sound device; it works hardware-free against Hamlib's test rig
today, but no released Hamlib (and no real rig backend yet) supports the API it needs —
status and build instructions in **[hamlib-streaming-bridge.md](hamlib-streaming-bridge.md)**.

## Where next

- The manual pages: `na_audio_source(1)`, `na_c_play_to_speakers(1)`,
  `na_audio_daemon(1)`, `na_wav_tap(1)`, `na_hamlib_bridge(1)`.
- Building an application on the library: the README's *C ABI* walkthrough, and the
  worked example clients in five languages under `examples/` in the source tree.
- What's on the wire: the [protocol overview](protocols.md) and the
  [full specification](audio-streaming-protocol-v1.md).
