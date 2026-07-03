<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# net-audio examples

Worked examples that connect to a network-audio server and **play whatever it is
streaming on your local speakers** — one client per language, plus a small demo
server so you can hear something with no radio and no second machine.

Every client uses **only** the public `naudio.h` C ABI
([`../include/naudio.h`](../include/naudio.h)) — never an internal C++ type. Together
they are the proof that a C consumer (and an FFI consumer in any language) can write a
full streaming client from that one header alone.

## The clients

Each is the *same* small program — connect, receive RX PCM through a C callback, play
it on the default output device, disconnect cleanly on Ctrl-C — written idiomatically
for its language and binding the C ABI a different way.

| Language | Directory | How it reaches the C ABI |
|---|---|---|
| **C** | [`c/`](c) | Compiles against `naudio.h`, links the shared `naudio`. The reference template the others follow. |
| **C++** | [`cpp/`](cpp) | The same program compiled as C++ — names no naudio C++ class, links only `naudio`; shows the C ABI is sufficient from C++ too. |
| **Python** | [`python/`](python) | Standard-library `ctypes` — no compile step, no third-party dependency. |
| **Java** | [`java/`](java) | The Foreign Function & Memory API (Panama, `java.lang.foreign`, JDK 22+) — no JNI shim, no Maven, single source file. |
| **Rust** | [`rust/`](rust) | Hand-written `extern "C"` declarations — no `bindgen`, no third-party crate. |

## The demo source

[`server/`](server) — `na_audio_source`, a tiny streaming server over the `na_server_*`
ABI. It can broadcast a local audio input, or (with `--test-tone`) a fixed,
hardware-free tone so a client works end to end on a headless box. Start it, point a
client at it, and you hear what it sends.

## Quick start

Build the toolkit (apps are on by default), then run the demo source and a client:

```bash
cd naudio
cmake -S . -B build && cmake --build build -j

# terminal 1 — broadcast a test tone (no hardware needed):
build/examples/na_audio_source --test-tone --port 4533

# terminal 2 — connect the C client and play it to your speakers:
build/examples/na_c_play_to_speakers --host 127.0.0.1 --port 4533
```

Swap in any other client — see each directory's `README.md` for its exact build/run
command (Python, Java, and Rust drive the same server the same way).

## Shared command-line options

All five clients accept the same core options, so one habit transfers across the suite:

```
--host H            server host (default 127.0.0.1)
--port N            server port (default 4533)
--name S            client display name (default na-<lang>-client)
--playback-id N     output device id (default: first output device)
--transport tcp|udp transport (default tcp) — must match the server's
--seconds N         run time; 0 = until Ctrl-C (default 0)
--backend system|null  system (default) plays to a device; null discards (hardware-free)
--list-devices      print output device ids, then exit
```

The default `system` backend plays to a real device, so a no-flag run just works. The
`null` backend receives frames but discards them — the hardware-free path used to check
a client anywhere (CI, a headless host) with no audio device. The Python and Java
clients also take `--lib PATH` to locate `libnaudio` at runtime; the Rust client links
it at build time and so does not.

## Notes

- **Links the public shared library only.** Each client depends on `libnaudio` (and the
  C++ runtime it carries) — never the internal static libraries the `tools/` daemon
  uses. That is the "the C ABI is self-sufficient" guarantee.
- **The wire is the `0xAF01` protocol, v1** — the frozen contract in
  [`../docs/audio-streaming-protocol-v1.md`](../docs/audio-streaming-protocol-v1.md).
  Clients speak it *through* the library codec; none of them re-implement the wire.
- **Real-device capture and audible playback need audio hardware**, so they are not part
  of the automated test suite; the `--test-tone` source plus the `null` backend are the
  hardware-free end-to-end check.
