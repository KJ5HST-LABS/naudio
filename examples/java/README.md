<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Play to speakers (Java)

Connect to a network-audio server and play whatever it is streaming on your local
speakers. A single Java source file that drives the public `naudio.h` C ABI over the
Java Foreign Function & Memory API (Panama, `java.lang.foreign`) — and only that C ABI —
so it is also a worked example of calling a C library from Java with no JNI shim, no
build tool, and no third-party dependency.

The playback is done by naudio itself: you pick a local output device and the library
renders the received audio to it. By default the client picks your first output device
and starts playing, so the simplest run needs no flags. Because playback is naudio's own
PortAudio backend, the program needs no Java audio library.

## Build

The program needs no separate compile step — the `java` launcher runs the source file
directly — but it loads `libnaudio`, so build the toolkit once (the shared library is on
by default):

```sh
cmake -S . -B build
cmake --build build -j
```

**JDK 22 or newer is required** (the Foreign Function & Memory API is final since JDK 22);
it is developed and tested on JDK 25. There are no other dependencies. The program finds
`libnaudio` via, in order: `--lib PATH`, the `NAUDIO_LIB` environment variable, the sibling
build tree (`build/` and `../../build`), then the platform's default library loader.

## Run

Run the source file directly with `java`. The `--enable-native-access=ALL-UNNAMED` flag
silences the foreign-function access warning (and is required by a future JDK release):

**Hear something with no extra hardware** — pair it with the [demo source](../server):

```sh
build/examples/na_audio_source --test-tone --port 4533                          # one terminal: a test tone
java --enable-native-access=ALL-UNNAMED examples/java/PlayToSpeakers.java --port 4533   # another: plays it
```

**Choose an output device:**

```sh
java --enable-native-access=ALL-UNNAMED examples/java/PlayToSpeakers.java --list-devices
java --enable-native-access=ALL-UNNAMED examples/java/PlayToSpeakers.java --playback-id 2 --host 192.168.1.10 --port 4533
```

With no `--playback-id`, the first output device is selected automatically.

**Run hardware-free** (no sound card needed — the audio is received but not played):

```sh
java --enable-native-access=ALL-UNNAMED examples/java/PlayToSpeakers.java --backend null --port 4533 --seconds 5
```

To use UDP, give the **same** `--transport udp` to both the source and the client — a server
serves the one transport it is configured for (TCP by default), so the two must match.

## Options

```
--host H          server host (default 127.0.0.1)
--port N          server port (default 4533)
--name S          this client's name in the server roster (default na-java-client)
--playback-id N   output device id to play on (default: first output device)
--transport T     tcp (default) | udp
--seconds N       run time; 0 = until Ctrl-C (default 0)
--backend B       system (default; plays to the output device) | null (hardware-free)
--list-devices    print output device ids, then exit
--lib PATH        path to libnaudio (else $NAUDIO_LIB, build tree, system loader)
```

## Expected output

Human-readable logs go to stderr; a one-line machine-readable summary goes to stdout.
A short run against the test-tone source looks like:

```
connected to 127.0.0.1:4533 (backend=null, transport=tcp); receiving for 3s ...
  t= 1s  rx=168960    B  frames=44       166788 B/s  conn=1
RESULT status=PASS connected=true rx_frames=88 ... first_frame_hex=68c569c66ac76bc8 avg_bps=...
```

`first_frame_hex=68c569c66ac76bc8` is the test tone's fixed first frame: seeing it confirms the
audio crossed the wire byte-for-byte. With `--backend system` (the default) you also hear the tone.

Real-speaker playback needs audio hardware, so it is not part of the automated test suite; the
`--backend null` path is the hardware-free check.

## What the C ABI looks like from Java

The program declares the `na_*` functions it uses as Panama downcall handles and mirrors one
struct (`na_device`) as a `MemoryLayout`:

| Area | C ABI used |
|---|---|
| Devices | `na_context_create` → `na_enumerate` → `na_context_destroy` |
| Lifecycle | `na_client_create` → `na_client_connect` → `na_client_disconnect` → `na_client_destroy` |
| RX audio | `na_client_set_audio_cb` — the hot-path PCM sink, passed as an FFM **upcall stub** |
| Config | `na_client_set_transport`, `na_client_set_playback_device` |

This client is **RX-only**: it registers just the audio sink and skips the `na_client_callbacks`
event struct, so the only callback it installs is the single `na_audio_cb` function pointer.

**Threading (from `naudio.h`):** the RX audio callback fires on naudio's receive **worker** thread,
once per frame — keep it short, don't call client lifecycle methods from it, and (the Java-specific
parts) don't let an exception escape it back into native code, and keep the FFM `Arena` that backs
the upcall stub open for the client's lifetime. The FFM runtime attaches naudio's worker thread to
the JVM for the duration of each upcall. The call sequence is identical to the C, C++, and Python
examples.
