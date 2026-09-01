# naudio — the C/C++ network-audio streaming toolkit

**naudio** sends live audio from one computer to another over an ordinary network. One machine
runs a small server that broadcasts a sound source — a microphone, a line-in, any audio device,
or audio an application feeds in. Any number of machines connect and listen: play the stream on
their speakers, record it to a file, or hand it to another program. The connection is built for
imperfect networks — lost, late, and out-of-order packets are corrected rather than heard.

Three things ship here:

- **Ready-to-run programs.** A demo server and player that put audio across two machines in
  about a minute with no code and (using the built-in test tone) no microphone; a device-serving
  daemon, configured and driven from a local web page rather than a command line; a stream
  recorder. Start at **[docs/getting-started.md](docs/getting-started.md)**.
- **A small, stable C library** (`libnaudio`) that any application — in C, C++, Python, Java,
  Rust, or anything with a C foreign-function interface — links to send or receive streams. A
  worked example client exists in each of those five languages.
- **An open wire protocol** (*net-audio*, magic `0xAF01`), fully specified and frozen, so
  independent implementations can interoperate.

naudio grew out of amateur radio — streaming a receiver's audio to operators and digital-mode
software elsewhere on a network — and that remains a first-class use. But nothing in the library
is radio-specific: it is a general-purpose audio transport. The one radio-specific piece, an
optional bridge to the [Hamlib](https://github.com/Hamlib/Hamlib) rig-control project, is a
separate bolt-on tool (off by default, built only on request) documented in
**[docs/hamlib-streaming-bridge.md](docs/hamlib-streaming-bridge.md)**.

> **On the names:** *net-audio* is the wire **protocol** — the frozen `0xAF01` frame contract defined in [the spec](docs/audio-streaming-protocol-v1.md). *naudio* is this C/C++ **toolkit** that implements it. There is no separate "net-audio" package to find or install; the protocol name and this repository are the whole story.

- **Install:** **[INSTALL.md](INSTALL.md)** — binary packages, Homebrew, and from-source, per platform.
- **Getting started:** **[docs/getting-started.md](docs/getting-started.md)** — audio streaming end to end with the shipped programs, no code required.
- **License:** LGPL-2.1-or-later — see **[LICENSE](LICENSE)**. (Links cleanly from proprietary and GPL applications alike.)
- **Wire spec:** **[docs/audio-streaming-protocol-v1.md](docs/audio-streaming-protocol-v1.md)** — the frozen `0xAF01` v1 contract.
- **Protocols overview:** **[docs/protocols.md](docs/protocols.md)** — the wire format at a glance.
- **Conformance:** **[conformance/README.md](conformance/README.md)** — language-neutral golden vectors that pin every normative value in the wire spec.
- **For radio amateurs:** **[docs/on-air-verification.md](docs/on-air-verification.md)** — verifying the toolkit against a real radio.


---

## How it fits together

```
audio in ──▶ server ──▶ network (TCP, or UDP with loss recovery) ──▶ clients ──▶ speakers / file / your app
```

A **server** owns the audio source and accepts clients; every **client** receives the same
stream. Audio is carried as 16-bit PCM at a sample rate you choose (48 kHz stereo by default),
framed by the `0xAF01` protocol, over TCP or UDP — the UDP path adds forward error correction,
reordering, and jitter buffering so brief network trouble is repaired instead of audible.
Everything above the socket lives in the library; the shipped programs are thin wrappers over
the same public C API any application would use.

---

## Status

The streaming stack is implemented, tested, and packaged: the device layer, the `0xAF01` codec,
the loss-recovery machinery, the multi-tenant TCP/UDP server and client, and the complete C API
(both the client and server surfaces). It installs as a normal package — `find_package(naudio)`
or pkg-config — and binary installers exist for Linux, macOS, and Windows
(**[INSTALL.md](INSTALL.md)**). The automated test suite needs no audio hardware and runs on all
three platforms on every change; it includes the protocol conformance vectors, compile checks
for every code sample in this file, and cross-language checks of the example clients. What does
require real audio hardware — live capture and playback — is exercised by the shipped tools and
documented procedures rather than by CI; see **Known limitations** below.

---

## Layout

| Target | Kind | What it is |
|---|---|---|
| `naudio_core` | static lib | **Pure, portable logic** — no fork/exec, no PortAudio, no sockets. Device classification/merge, format probing, the mono→stereo open policy, the `0xAF01` codec, and the reliability primitives (jitter / FEC / reorder / control-ACK). Fully unit-tested via a fake backend. |
| `naudio_pa` | static lib | `naudio_core` + the PortAudio device backend. |
| `naudio_net` | static lib | The transport layer — TCP/UDP sockets, per-client threads, the multi-tenant `AudioStreamServer`, and `AudioStreamClient`. Backend-agnostic (takes a `DeviceBackend*`). |
| `naudio` | **shared lib** | The public artifact: the **C ABI** (`include/naudio.h`) over the internal backends, exporting **only** the `na_*` surface (hidden visibility + `SOVERSION`). This is what a C / Hamlib consumer links and what `make install` ships, alongside `naudio.pc`. |
| `tools/` | apps | The **device-serving daemon** (`na_audio_daemon`), which streams a real capture device, serves a localhost control page for configuring and running itself, and doubles as the hardware diagnostic (it exercises the live-device path the hardware-free test suite cannot reach); the **stream recorder** (`na_wav_tap`), which saves what a client receives as a WAV file; and the optional **Hamlib streaming bridge** (`na_hamlib_bridge`) — a radio-specific bolt-on, off by default, needing a libhamlib no release ships yet: see **[docs/hamlib-streaming-bridge.md](docs/hamlib-streaming-bridge.md)**. |
| `examples/` | apps | The **examples suite** — a "play to speakers" client in C, C++, Python, Java, and Rust, plus `na_audio_source`, the demo server. The C client and the demo server **ship in the binary packages** as the out-of-the-box demo pair. Each example links the public shared `naudio` and uses **only** `naudio.h`, proving the C ABI is self-sufficient from C and from any FFI consumer. See **[examples/README.md](examples/README.md)**. |

---

## Quick start

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure      # logic + codec + C-ABI tests, no hardware
ctest --test-dir build -N | tail -1             # how many this configuration registers
```

Dependencies resolve from a system / Homebrew install when present (PortAudio via `pkg-config`,
GoogleTest via `find_package`), falling back to CMake `FetchContent` otherwise. First configure may
fetch GoogleTest (and, if no system PortAudio, PortAudio v19.7.0) from the network.

**System dependency:** PortAudio. On Debian/Ubuntu `sudo apt-get install portaudio19-dev`; on macOS
`brew install portaudio pkg-config`. Without it, CMake fetches and builds PortAudio from source.

To **install** — the shared `naudio` C ABI, the C++ archives and headers, `naudio.pc` /
`find_package(naudio)`, and the tools with their man pages — see **[INSTALL.md](INSTALL.md)**: it
covers the binary release packages, the Homebrew tap, and the from-source path (including the
configure-time-prefix rule that keeps `naudio.pc` truthful). Once installed,
**[docs/getting-started.md](docs/getting-started.md)** streams audio end to end using only the
shipped tools.

---

## The C ABI (`include/naudio.h`)

For C / Hamlib / Python(+ffi) consumers. The implementation is C++ internally, so link the shared
`naudio` (the `naudio_c_abi_smoke` and `naudio_c_net_smoke` ctests are the C-callability proofs). The
ABI has two surfaces.

### Device I/O

```c
#include "naudio.h"

na_context* ctx = na_context_create();                   // one backend init per context
na_device devs[32];
int n = na_enumerate(ctx, devs, 32, sizeof devs[0]);     // count, or a negative na_error_t
na_capture_stream* s =
    na_open_capture(ctx, devs[0].capture_backend_id, 48000, 16, 2, /*out_actual_channels=*/NULL);
int16_t buf[480 * 2];
int frames = na_capture_read(s, buf, 480, NA_BLOCK_FOREVER, /*out_overflow=*/NULL);
na_close_capture(s);                                     // close streams BEFORE the context
na_context_destroy(ctx);
```

Also exposed: `na_probe_format`, `na_open_playback` / `na_playback_write` / `na_close_playback`, and
the guidance/diagnostics functions (`na_install_instructions`, `na_diagnostic_report`,
`na_blackhole_installed`, `na_linux_auto_configure`). Errors are negative `na_error_t` codes
(count-returning calls) or `NULL` (handle-returning calls), with the cause on the calling thread via
`na_last_error()`; `na_strerror()` maps a code to a string. Text functions follow a length-probe
contract — pass `buf == NULL` (or `len == 0`) to get the required length without writing.

### Networking client (`na_client_*`)

```c
char errbuf[256];
na_stream_client* c =                                    // host, port, and roster name go in create()
    na_client_create(NA_CLIENT_BACKEND_SYSTEM, "192.168.1.20", 4533, "my-client");
na_client_set_transport(c, NA_TRANSPORT_TCP);            // NA_CLIENT_BACKEND_NULL = headless RX, no PortAudio
/* For UDP, call na_client_set_reliability_profile instead — it selects UDP itself and enables the
   loss-recovery layer (FEC/reorder/jitter/control-ARQ) that set_transport alone leaves off. Since
   0.5.0 a bare-UDP connect is refused unless NA_RELIABILITY_UDP_BARE explicitly requests it. */
na_client_set_audio_cb(c, on_pcm, user);                 // the hot-path RX PCM sink
events.struct_size = sizeof events;                      // REQUIRED — see "Binary compatibility"
na_client_set_callbacks(c, &events, user);               // connected / stream / roster / error events
na_client_set_playback_device(c, playback_id);           // REQUIRED for RX (any id on the NULL backend)
if (na_client_connect(c, errbuf, sizeof errbuf) != NA_OK) {
    /* connect failed — errbuf holds the reason (also via na_last_error()) */
}
/* ... receive PCM through on_pcm until done ... */
na_client_stats st;                                      // reliability counters for THIS connection
na_client_get_stats(c, &st, sizeof st);                  // packets_recovered_by_fec, reordered, jitter...
na_client_disconnect(c);
na_client_destroy(c);
```

The networking client wraps the C++ `AudioStreamClient`; the matching server is the C++
`AudioStreamServer` (in `naudio_net`), exposed to C through the symmetric **`na_server_*`** surface
(create / configure / start / inject-RX / extract-TX / roster + TX callbacks). See
**[examples/README.md](examples/README.md)** for the example clients (play to speakers, in five
languages) and the demo source.

### Binary compatibility

Read this if you package naudio, or link it as a shared library you did not build yourself.

**Source compatibility is promised; binary compatibility begins at the first tag — and that tag is
now cut.** `v0.4.0` is the first tagged release, so the rules below are what the C ABI commits to
from here rather than an intention. `SOVERSION` tracks the major version (`1` since 1.0.0), so every
release within a major shares one soname — which is exactly why the declared-size mechanism described
below, and not a soname bump, is what keeps an appended field from breaking an already-compiled
consumer.

**Every caller-allocated struct tells the library how large the caller believes it to be.** Without
that, appending a single field to a future release would make the library read or write past the end
of a struct allocated by a consumer compiled against the older header — silently, with no symbol
rename and no link error. How the size travels depends on which side *writes* the struct:

| Struct | Direction | How its size travels | Floor the library enforces |
|---|---|---|---|
| `na_client_callbacks` | library **reads** | in-band: `cbs.struct_size = sizeof cbs` | `NA_CLIENT_CALLBACKS_SIZE_V1` |
| `na_server_callbacks` | library **reads** | in-band: `cbs.struct_size = sizeof cbs` | `NA_SERVER_CALLBACKS_SIZE_V1` |
| `na_client_stats` | library **writes** | parameter: `na_client_get_stats(c, &st, sizeof st)` | `NA_CLIENT_STATS_SIZE_V1` |
| `na_device` | library **writes** (array) | parameter: `na_enumerate(ctx, devs, max, sizeof devs[0])` | `NA_DEVICE_SIZE_V1` |

The split is not stylistic. A table the library only reads can carry its own size as a first member —
Win32's `cbSize` idiom — and callers already `memset` these, so it is one line beside the existing
one. An *out*-parameter cannot: requiring a caller to pre-initialize a struct the library fills would
invert the contract these have always had, which is that nothing needs pre-zeroing. For `na_device`
it is not even a preference — `na_enumerate` fills an **array** and its `max` is an element *count*,
so only the caller's own element size can say where element *k* begins.

```c
#include <string.h>

na_client_callbacks cbs;                 /* library READS it -> size travels in-band  */
memset(&cbs, 0, sizeof cbs);
cbs.struct_size = sizeof cbs;            /* NA_ERR_INVALID without this — 0 is below the floor */

na_context* ctx = na_context_create();
na_device devs[32];                      /* library WRITES it -> size is a parameter  */
int n = na_enumerate(ctx, devs, 32, sizeof devs[0]);
na_context_destroy(ctx);
```

**What the declared size buys, in both directions.** The library treats it as a hard bound:

- A library **newer** than the consumer writes only the prefix the consumer allocated, and reads only
  the fields it declared. An appended field cannot scribble past the end of an already-compiled
  consumer's struct — the hazard this exists for. For `na_enumerate` it also means each element lands
  in the consumer's own slot instead of the array walking off its end after the first one.
- A library **older** than the consumer fills the fields it knows and **zero-fills** the remainder, so
  the tail is defined rather than indeterminate. On its own that makes the tail *defined*, not
  *informative* — a zero there reads exactly like a measured zero. The version accessors below are
  what separate them.

A size below the struct's `_SIZE_V1` floor is rejected with `NA_ERR_INVALID`, which is what an
accidentally-zero size gives you — the common mistake fails loudly at the call rather than quietly in
memory.

**Telling a zero-fill from a zero.** Two versions exist and they are not the same number: the one you
**compiled** against (`NAUDIO_VERSION_MAJOR` / `_MINOR` / `_PATCH`, and the derived
`NAUDIO_VERSION_NUMBER` and `NAUDIO_VERSION_STRING`) and the one you **loaded**
(`na_version_number()` and `na_version_string()`, answered by the shared library itself). A packager,
a distro upgrade, or an `LD_LIBRARY_PATH` can make them differ. This matters most for
`na_client_stats`, which deliberately encodes *"the library is not measuring this"* as `-1`, so the
zero-fill and the sentinel conventions collide exactly in that tail.

```c
#include <stdio.h>

/* At startup: is the library you LOADED as new as the header you COMPILED against? */
if (na_version_number() < NAUDIO_VERSION_NUMBER)
    printf("naudio %s loaded, built against %s — newer fields read as zero-fill\n",
           na_version_string(), NAUDIO_VERSION_STRING);

/* Per field: sequence_gaps arrived in 0.2.0, so it carries a measurement only on 0.2.0 or
   later. On anything older, na_client_get_stats zero-filled that tail and the 0 means
   nothing. The -1 check is a second, separate question: this library IS new enough, but
   this connection may still not be measuring (no reorder buffer engaged). */
na_stream_client* c = na_client_create(NA_CLIENT_BACKEND_NULL, "127.0.0.1", 4533, "demo");
na_client_stats st;
na_client_get_stats(c, &st, sizeof st);

if (na_version_number() >= NA_VERSION_ENCODE(0, 2, 0) && st.sequence_gaps >= 0)
    printf("%lld sequence gaps\n", st.sequence_gaps);

/* socket_rx_drops arrived in 0.3.0 and asks the same two questions in the same order — but
   its -1 is a statement about the PLATFORM, not the connection: the mechanism is Linux-only
   (see include/naudio.h). Note a 0 here is weaker than it looks; the counter lags by a whole
   receive buffer, so it means "no loss reported yet", not "no loss". */
if (na_version_number() >= NA_VERSION_ENCODE(0, 3, 0) && st.socket_rx_drops >= 0)
    printf("%lld datagrams dropped by the kernel receive buffer\n", st.socket_rx_drops);
```

Always compare **through** `NA_VERSION_ENCODE` rather than spelling the arithmetic — the packing is
an implementation detail, and only that macro and `na_version_number()` are promised to agree on it.
The ordering is total for components within `NA_VERSION_MAX_COMPONENT` (so `0.999.999 < 1.0.0`).
Both accessors are infallible: no allocation, no backend, callable before `na_context_create()` and
from any thread, and they leave `na_last_error()` untouched. The header's macros cannot drift from
the SONAME — CMake refuses to configure a build where they disagree with `project(VERSION)`.

**The growth rule these constants encode:** fields are only ever **appended**, each one **naming the
version it arrived in**, so the comparison above is always writable; and each `_SIZE_V1` is
frozen as `offsetof(<v1's last field>) + sizeof(<its type>)` rather than a byte literal, so appending
does not move it and the value stays correct on a 32-bit build where these mostly-pointer structs are
smaller. Any change that is *not* an append — reordering, resizing, or removing a field — is an soname
break, and `SOVERSION` tracks the major version for exactly that.

**Outside this promise:** the `0xAF01` wire format is frozen separately by
[the spec](docs/audio-streaming-protocol-v1.md) and is unaffected by any of the above; and the C++ API
under `naudio/**` ships as **static archives** with no binary-stability promise — link the shared
`naudio` C ABI if you need one.

---

## Using naudio as a C++ library

After `make install`, the C++ API is reachable via `find_package(naudio)` —
`target_link_libraries(your_app PRIVATE naudio::naudio_pa)` (device I/O + PortAudio backend),
`naudio::naudio_core` (pure logic only), or `naudio::naudio_net` (the transport/server/client). The
full `naudio/**` header tree installs alongside. (The internal C++ classes ship as static archives,
so the hidden-visibility the shared `naudio` C ABI relies on does not strip them from a static link.)

> **`naudio::naudio_pa` is the one target that needs PortAudio on *your* side.** The installed
> package names no PortAudio target, so `find_package(naudio)` resolves one itself — pkg-config
> first, then a plain `find_library`/`find_path` — and attaches it to `naudio::naudio_pa` alone. If
> your host has none, `find_package(naudio)` still succeeds and sets `naudio_PORTAUDIO_FOUND` to
> `FALSE`: `naudio::naudio`, `naudio::naudio_core` and `naudio::naudio_net` all remain usable, and
> only linking `naudio::naudio_pa` fails. Gate on that variable if you consume the device layer:
>
> ```cmake
> find_package(naudio CONFIG REQUIRED)
> if(NOT naudio_PORTAUDIO_FOUND)
>     message(FATAL_ERROR "this app needs naudio::naudio_pa, which needs a PortAudio")
> endif()
> ```

```cpp
#include "naudio/PortAudioBackend.hpp"
#include "naudio/DeviceEnumerator.hpp"
#include "naudio/StreamOpener.hpp"
using namespace naudio;

PortAudioBackend backend;
DeviceEnumerator enumerator(backend);
auto caps = enumerator.captureDevices();                 // classified, merged
StreamOpener opener(backend);
auto cap = opener.openCapture(caps.front(), AudioFormat{48000, 16, 2});
std::vector<int16_t> buf(960 * cap->actualFormat().channels);
IoResult r = cap->read(buf.data(), 960, kBlockForever);  // kBlockForever == -1
```

The `DeviceBackend` interface is the test seam — inject `FakeBackend`
to unit-test device/stream logic with no hardware. The streaming server/client live in `naudio_net`
(`AudioStreamServer`, `AudioStreamClient`).

---

## Testing & conformance

```bash
ctest --test-dir build --output-on-failure
```

The unit suite (driven by `FakeBackend` + fake stream/shell doubles — no hardware) covers device
classification/merge, format probing, the open/fallback policy, the
`0xAF01` frame/control codec, and the reliability primitives (FEC parity + recovery, reorder ordering,
jitter, control-ACK). `naudio_c_abi_smoke` proves the C ABI is C-callable; `naudio_c_net_smoke` drives
a full pure-C connect → RX-via-callback → disconnect against an in-process server over loopback.

The **conformance** test loads the language-neutral golden vectors from `conformance/vectors/` (a
known-answer test with CRCs computed independently, not a round-trip-against-self). See
**[conformance/README.md](conformance/README.md)**.

---

## Example clients

naudio is the **C/C++** implementation of the `0xAF01` protocol. The [`examples/`](examples/) suite
ships a "play to speakers" client in C, C++, Python, Java, and Rust — each linking only the public
shared `naudio` and `naudio.h`, so the C ABI is exercised from C and from every supported FFI
consumer. The byte-identity tell for a correctly received stream is
`first_frame_hex=68c569c66ac76bc8`.

---

## Known limitations

- **Host-API duplication (carried forward).** PortAudio enumerates the same physical device once per
  host API (WASAPI/MME/DirectSound on Windows) under different names. The DUPLEX merge keys on
  `name + hostApi`, so it does **not** yet de-duplicate the same device across host APIs; deferred
  until it can be tested more thoroughly by others.
- **`verifyConfiguration` "detected" values are PortAudio's defaults.** PortAudio has no format list,
  so the detected rate/channels come from the device's reported default rate + max channel counts —
  faithful, but device-default numbers rather than an enumerated best match.
- **Virtual-device paths not runtime-verified.** With no BlackHole/VB-CABLE installed in CI,
  `bestVirtual` selection and the virtual-mismatch report are unit-tested only, with limited local hardware testing; enumeration + the
  probe primitive **are** runtime-exercised on real Core Audio via `na_audio_daemon --list-devices`.
- **The Hamlib streaming bridge cannot be pointed at a radio yet, and that is upstream.** Measured
  against Hamlib master `0839c03`: **1 of 313 models advertises `stream_caps`, and it is the Dummy.**
  No real radio backend implements the audio-streaming subsystem `na_hamlib_bridge` is built on, and
  `netrigctl` only relays a remote backend that does not have it either — so the bridge's
  real-backend behaviour (format negotiation against real caps, `-k` keying around TX bursts, a
  short write from a radio that will not take a full buffer) is **unverified**, not merely untested.
  The device layer *can* be verified with a radio today via its USB-audio interface; both the
  measurement and that procedure are in
  **[docs/on-air-verification.md](docs/on-air-verification.md)**.
- **No IP multicast; IPv4 only.** RX fan-out to multiple clients is **unicast replication** (O(N)
  egress), not IP multicast, and the transport is IPv4 (`AF_INET`) only. Fan-out should not be read as
  a multicast group.
- **Discovery is local to one segment, and one server per host holds the rendezvous port.**
  Since wire spec 1.4 (§6.8) a client broadcasts a `DISCOVER` and every server that hears it
  answers by unicast: `na_discover()` in C, `naudio::net::discoverServers()` in C++. Probe the
  **rendezvous port** — 4533 by default — and the reply carries the port that server actually
  serves on, so a server on any port is found by a client that was told nothing. That works for
  TCP, UDP and DUAL alike, because the rendezvous listener owns a UDP socket whatever the server
  serves audio over. The exchange is side-effect-free: no connection, no slot against
  `maxClients`, no stream — which is what distinguishes it from the broadcast-`CONNECT_REQUEST`
  trick it replaces. Two limits remain. Only **one server per host** can hold a given rendezvous
  port; where several run, the first to start wins it and the rest are found only by a probe
  aimed at their own service port (running several already meant choosing those ports). And it is
  **local**: routers do not forward a broadcast, and naudio implements no multicast or
  cross-subnet discovery.
- **Servers answer discovery by default**, and also bind the rendezvous port by default.
  `na_server_set_discoverable(server, 0)` turns discovery off entirely;
  `na_server_set_discovery_port(server, 0)` keeps direct-port probes working but runs no
  rendezvous listener.
  Leaving it on means anyone on the segment can learn that the server exists, what audio format
  it serves and how full it is. That is the point of the feature, and it is worth being explicit
  that it is a *discoverability* switch and not access control: a server that answers nothing
  still accepts connections from anyone who already knows its address.

- **Device selection is local, not remote.** A client or server selects its own capture / playback
  device locally; there is no control message to enumerate or choose the *peer's* device over the wire.
- **Conformance vectors gate C/C++ only.** The language-neutral golden vectors are loaded and checked
  by the C/C++ suite; the Python / Java / Rust example clients are validated by the runtime byte-tell
  (`first_frame_hex=68c569c66ac76bc8`), not by loading the vectors. There is no TypeScript / Node
  client yet.
- **No arbitrary sample-rate conversion.** naudio carries audio at the negotiated rate (48 kHz is the
  first-class default) and does **not** resample between arbitrary rates in v1; rate conversion is left
  to the application or a companion DSP stage.
- **No graceful degradation on a constrained link — provision the rate instead.** The default
  48 kHz / 16-bit / stereo format costs **1.536 Mbps** on the wire, unconditionally: nothing adapts
  mid-stream, and deeper buffers do not help (measured on a congested 2.4 GHz WiFi hop: 33 % of the
  audio delivered over UDP — and over TCP too, so the shortfall was sustained undercapacity, which
  no buffer depth converts into throughput). What exists today is **static provisioning**: declare a
  lower server-wide format with `na_server_set_audio_format` (or `--rate`/`--channels` on
  `na_audio_source` and `na_audio_daemon`) before start. Clients discover it via
  `na_client_get_audio_format` after connect. **Since 0.5.0 a client may also ask for less than
  the server broadcasts**, for its own subscription only: `na_client_request_format`
  (spec 1.2 §6.2.1) requests an integer divisor of the server's rate and/or a reduced channel
  layout, while every other client keeps the native stream. **Since spec 1.3 the two are granted
  independently**, so an unservable layout no longer withdraws a servable rate and a request is
  safe to make speculatively; read what you actually got from `na_client_get_audio_format`,
  which is the only authority for what the callbacks carry. What still does not exist is mid-stream adaptation and resampling
  between arbitrary rates; the remaining format breadth is §13.1 of the spec. Against that measured
  link (~0.5 Mbps effective — **derived** from 33 % of 1.536 Mbps, not itself measured):

  | Format | Rate on wire | On that link |
  |---|---|---|
  | 48 kHz / 16 / stereo | 1.536 Mbps | 3× over — 33 % delivered |
  | 48 kHz / 16 / mono | 768 kbps | marginal |
  | 16 kHz / 16 / mono | 256 kbps | fits, 2× headroom |
  | 12 kHz / 16 / mono | 192 kbps | fits, 2.6× headroom — covers SSB (≤3 kHz) and FT8 (≤3.1 kHz audio) |
  | 8 kHz / 16 / mono | 128 kbps | fits; voice-grade only |
