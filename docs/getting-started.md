<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Getting started — audio streaming without touching the ABI

This is the operator walkthrough: from an installed naudio toolkit to audio moving over
the network, using only the shipped command-line tools. Nothing here requires writing
code. Install first — **[INSTALL.md](../INSTALL.md)** covers packages, Homebrew, and
from-source; each tool also has a man page (`man na_audio_daemon`).

**What you need:** one machine with an audio input (a radio's USB codec, a sound card, a
laptop microphone) to serve; any machine — same one included — to receive. The wire
between them is the [net-audio `0xAF01` protocol](audio-streaming-protocol-v1.md), so
anything speaking it can join later; here the two ends are the shipped tools
`na_audio_daemon` (serve) and `na_wav_tap` (receive).

## 1. Find your audio device

```bash
na_audio_daemon --list-devices
```

Every capture and playback device prints with a backend **id**. Note the id of the input
you want to stream — the steps below select by `--capture-id`, which skips name matching
entirely (`--capture <substring>` works too; the default pattern is `USB Audio CODEC`,
a radio codec, so anything else needs one of the two flags).

## 2. Prove capture works — no network yet

```bash
na_audio_daemon --mode capture-probe --capture-id <N> --channels 1 --duration-ms 10000
```

Ten seconds of direct capture: the report shows frames read, overflows, and per-channel
RMS. Speak into the device (or open the radio's squelch) and the RMS should move well off
the floor. Exit code 0 means no overruns.

Two flags matter here and everywhere below:

- **`--channels`** must match what the device can open — 1 for a mono source (most
  microphones), 2 for stereo (the default). naudio **refuses** a mismatch rather than
  silently converting.
- **`--rate`** (default 48000) is refused the same way: naudio does not resample, so a
  device that cannot open at exactly the requested rate fails loudly instead of shipping
  wrong-rate audio labeled with the right number.

## 3. Serve it

```bash
na_audio_daemon --mode hardware --capture-id <N> --channels 1 --transport udp --duration-ms 0
```

This starts a naudio streaming server on port 4533 (`--port` to change it), capturing
the device and serving every client that connects, until Ctrl-C. The daemon's own
in-process client prints a throughput/RMS line so you can see audio flowing before any
remote client exists.

`--transport udp` is explicit for a reason: **the transport must match on both ends, and
the two tools' defaults differ** — the daemon defaults to TCP, while `na_wav_tap` (next
step) defaults to the UDP_WAN reliability profile. A mismatch fails cleanly at connect
(`Handshake failed`), it does not half-work. Serve UDP as above, or keep the daemon's
TCP default and pass `--tcp` to the tap.

## 4. Receive it — anywhere

On any machine with naudio installed (the serving machine included):

```bash
na_wav_tap --host <server-host> --seconds 5 --out hello.wav
```

Then play `hello.wav` with anything (`afplay hello.wav` on macOS, `aplay hello.wav` on
Linux). If the negotiated rate divides by 12 kHz — 48/24/12 kHz all do — the file is
written mono at 12 kHz, the rate WSJT-X-family decoders read; that is deliberate, because
recording *what a client actually received* as decoder-ready evidence is this tool's job.

`na_wav_tap` uses the UDP_WAN reliability profile (FEC, reorder, jitter buffering) by
default; `--tcp` is the discriminating run when a UDP recording comes up short, since TCP
turns loss into delay rather than absence. On Windows, `na_wav_tap.exe` works the same —
it needs no audio device, which is why it is the one tool the Windows package ships.

## 5. Listen live

To hear the stream rather than record it, point one of the example clients at the server
— a "play to speakers" client exists in C, C++, Python, Java, and Rust, and the Python
one needs no compile step. They build from the source tree; see
**[examples/README.md](../examples/README.md)**. The same suite includes
`na_audio_source --test-tone`, a hardware-free demo server, so the whole path can be
exercised on machines with no audio input at all.

## 6. When the link is thin

The default format (48 kHz / 16-bit / stereo) costs **1.536 Mbps** on the wire,
unconditionally — naudio provisions statically rather than degrading mid-stream. On a
link that cannot carry that, declare a lower format **at the server**:

```bash
na_audio_daemon --mode hardware --capture-id <N> --rate 12000 --channels 1 --transport udp --duration-ms 0
```

12 kHz mono is 192 kbps — 8× less — and still covers SSB (≤3 kHz) and FT8 (≤3.1 kHz
audio). Clients need no flags: they read the negotiated format after connect. The
rate/bandwidth table is in the README's *Known limitations*; the measured
congested-link case behind it is in
[on-air-verification.md](on-air-verification.md).

## 7. The radio paths

- **Streaming a real radio's audio** is exactly steps 1–4 with the radio's USB codec as
  the capture device. The full evidence-grade procedure — FT8-period alignment, decoding
  the tap, and the negative control without which "it decoded" is not yet evidence — is
  **[on-air-verification.md](on-air-verification.md)**.
- **`na_hamlib_bridge`** re-originates a Hamlib `rig_stream_*` audio stream onto this
  same wire, for rigs whose audio arrives through Hamlib rather than a sound device. The
  binary packages ship it self-contained, and it runs hardware-free against Hamlib's
  Dummy rig today (`na_hamlib_bridge -m 1 -S tone`, then step 4 taps it) — but no
  released Hamlib, and no real radio backend yet, implements the streaming subsystem it
  needs, so it cannot be pointed at a real rig yet. Status and build instructions:
  **[hamlib-streaming-bridge.md](hamlib-streaming-bridge.md)**.

## Where next

- `man na_audio_daemon`, `man na_wav_tap`, `man na_hamlib_bridge` — the complete option
  references, including the caveat about playback-sink patterns that match nothing.
- Writing an application against the stream: the README's C ABI walkthrough, and the
  [protocol docs](protocols.md) for what is on the wire.
