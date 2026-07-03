<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Play to speakers (Rust)

Connect to a network-audio server and play whatever it is streaming on your local
speakers. The same small program as the [C example](../c), written in Rust: it drives
the public `naudio.h` C ABI over **hand-written `extern "C"` declarations** — no
`bindgen`, no other third-party crate, just the standard library. The bindings are only
the dozen `na_*` functions this client uses, so the whole thing is one `src/main.rs`.

The playback is done by naudio itself: you pick a local output device and the library
renders the received audio to it. By default the client picks your first output device
and starts playing, so the simplest run needs no flags.

## Build

First build the toolkit so the shared `libnaudio` exists (apps are on by default):

```sh
cmake -S . -B build
cmake --build build -j
```

Then build the example with Cargo (run from this directory):

```sh
cd examples/rust
cargo build
```

`build.rs` locates `libnaudio` in the sibling build tree (`../../build`) and bakes an
rpath to it, so `cargo run` finds the library at runtime with no extra environment.
If your build tree is elsewhere, point `NAUDIO_LIB` at the library file or its directory:

```sh
NAUDIO_LIB=/path/to/build/libnaudio.dylib cargo build
```

(As a last resort you can instead set `DYLD_LIBRARY_PATH` on macOS / `LD_LIBRARY_PATH`
on Linux to the directory holding the library when you run the binary.)

## Run

**Hear something with no extra hardware** — pair it with the [demo source](../server):

```sh
build/examples/na_audio_source --test-tone --port 4533   # one terminal: a test tone
cargo run -- --port 4533                                  # another: plays it on your speakers
```

**Choose an output device:**

```sh
cargo run -- --list-devices                              # list output device ids
cargo run -- --playback-id 2 --host 192.168.1.10 --port 4533
```

With no `--playback-id`, the first output device is selected automatically.

**Run hardware-free** (no sound card needed — the audio is received but not played):

```sh
cargo run -- --backend null --port 4533 --seconds 5
```

To use UDP, give the **same** `--transport udp` to both the source and the client — a server
serves the one transport it is configured for (TCP by default), so the two must match.

## Options

```
--host H          server host (default 127.0.0.1)
--port N          server port (default 4533)
--name S          this client's name in the server roster (default na-rust-client)
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
  t= 1s  rx=192000    B  frames=50       767999 B/s  conn=1
RESULT status=PASS connected=true rx_frames=131 ... first_frame_hex=68c569c66ac76bc8 avg_bps=...
```

`first_frame_hex=68c569c66ac76bc8` is the test tone's fixed first frame: seeing it confirms the
audio crossed the wire byte-for-byte. With `--backend system` (the default) you also hear the tone.

Real-speaker playback needs audio hardware, so it is not part of the automated test suite; the
`--backend null` path is the hardware-free check.

## How this works

It is the same client logic as the C version. The Rust-specific parts:

- **No `bindgen`.** The `na_*` functions are declared by hand in one `extern "C"` block, and
  `na_device` is mirrored with a `#[repr(C)]` struct. The bindings cover only what this client
  calls — a deliberately small surface.
- **RX-only callback.** The hot-path audio sink is a plain `extern "C" fn` registered with
  `na_client_set_audio_cb`; the optional `na_client_callbacks` event struct is skipped entirely.
- **No panic across the FFI boundary.** The callback fires on naudio's receive worker thread, so
  its whole body is wrapped in `catch_unwind` — a Rust panic must never unwind back into C.
- **Linking, not loading.** Unlike the Python/Java examples (which `dlopen` `libnaudio` at runtime
  and therefore take a `--lib` flag), this client links `libnaudio` at build time. `build.rs`
  handles finding it and the runtime rpath, so there is no `--lib` flag here.
