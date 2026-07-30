# naudio — the C/C++ network-audio streaming toolkit

**naudio** is an open, cross-platform toolkit for moving radio **audio** over the network: a versioned wire **spec**, a reference **server** and **clients**, a reliability stack (jitter / FEC / reorder / ARQ), virtual-audio device plumbing, and a stable **C ABI** so C / Hamlib / Python consumers can link a single versioned artifact. It is the C/C++ home of the **net-audio** audio-streaming protocol (magic `0xAF01`), built so the ham ecosystem — fldigi, Quisk, digital-mode apps, and Hamlib apps — can reuse one audio transport instead of each re-inventing it.

> **On the names:** *net-audio* is the wire **protocol** — the frozen `0xAF01` frame contract defined in [the spec](docs/audio-streaming-protocol-v1.md). *naudio* is this C/C++ **toolkit** that implements it. There is no separate "net-audio" package to find or install; the protocol name and this repository are the whole story.

- **License:** LGPL-2.1-or-later — see **[LICENSE](LICENSE)**. (Matches Hamlib; links cleanly from proprietary apps and from GPL apps alike.)
- **Wire spec:** **[docs/audio-streaming-protocol-v1.md](docs/audio-streaming-protocol-v1.md)** — the frozen `0xAF01` v1 contract.
- **Protocols overview:** **[docs/protocols.md](docs/protocols.md)** — the `0xAF01` network audio wire format at a glance.
- **Conformance:** **[conformance/README.md](conformance/README.md)** — language-neutral golden vectors that pin every normative value in the wire spec.


---

## Status

The audio streaming stack is **implemented and tested** (the device layer, the `0xAF01` codec, the reliability primitives, the multi-tenant TCP/UDP server + client, and the full networking C ABI — both the `na_client_*` and `na_server_*` surfaces). It installs as a package: `find_package(naudio)` or pkg-config resolves the C ABI **and** the C++ API. The build runs **291 hardware-free ctests** green, including the language-neutral conformance golden vectors and the documentation snippet compile-gate (every fenced `c`/`cpp` block in this file and in `docs/hamlib-streaming-bridge.md` is compiled against the real headers). A separate CI job runs the Python, Java and Rust example clients and diffs each one's hand-declared `na_device` against a C reference, so a layout change cannot silently break them. What is hardware-gated (real PortAudio capture/playback, on-air decode) is exercised by opt-in smokes and the `na_audio_daemon` driver, not by CI; see **Known limitations** below.

---

## Layout

| Target | Kind | What it is |
|---|---|---|
| `naudio_core` | static lib | **Pure, portable logic** — no fork/exec, no PortAudio, no sockets. Device classification/merge, format probing, the mono→stereo open policy, the `0xAF01` codec, and the reliability primitives (jitter / FEC / reorder / control-ACK). Fully unit-tested via a fake backend. |
| `naudio_pa` | static lib | `naudio_core` + the PortAudio device backend. |
| `naudio_net` | static lib | The transport layer — TCP/UDP sockets, per-client threads, the multi-tenant `AudioStreamServer`, and `AudioStreamClient`. Backend-agnostic (takes a `DeviceBackend*`). |
| `naudio` | **shared lib** | The public artifact: the **C ABI** (`include/naudio.h`) over the internal backends, exporting **only** the `na_*` surface (hidden visibility + `SOVERSION`). This is what a C / Hamlib consumer links and what `make install` ships, alongside `naudio.pc`. |
| `tools/` | apps | The **hardware smoke daemon** (`na_audio_daemon`) — runs the real `AudioStreamServer` / `AudioStreamClient` over a live PortAudio device, the path ctest (fake backend only) cannot reach. An exerciser, **not** an audio primitive: it lives outside the libraries to keep the core pure + portable. Also the optional **Hamlib streaming bridge** (`na_hamlib_bridge`), which re-originates a Hamlib `rig_stream_*` audio stream onto naudio's wire so it survives a lossy/WAN hop — off by default, and needs a libhamlib that no release ships yet: see **[docs/hamlib-streaming-bridge.md](docs/hamlib-streaming-bridge.md)**. |
| `examples/` | apps | The **examples suite** — a "play to speakers" client in C, C++, Python, Java, and Rust, plus a small demo source. Each links the public shared `naudio` and uses **only** `naudio.h`, proving the C ABI is self-sufficient from C and from any FFI consumer. See **[examples/README.md](examples/README.md)**. |

---

## Quick start

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure      # 291 logic + codec + C-ABI tests, no hardware
```

Dependencies resolve from a system / Homebrew install when present (PortAudio via `pkg-config`,
GoogleTest via `find_package`), falling back to CMake `FetchContent` otherwise. First configure may
fetch GoogleTest (and, if no system PortAudio, PortAudio v19.7.0) from the network.

**System dependency:** PortAudio. On Debian/Ubuntu `sudo apt-get install portaudio19-dev`; on macOS
`brew install portaudio pkg-config`. Without it, CMake fetches and builds PortAudio from source.

To **install** the package — the shared `naudio` C ABI, the static C++ archives
(`naudio_core` / `naudio_net` / `naudio_pa`), the `naudio.h` + `naudio/**` headers, `naudio.pc`, and
the CMake package config (`find_package(naudio)`):

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local   # set the prefix at CONFIGURE time
cmake --build build
cmake --install build
pkg-config --cflags --libs naudio                       # or: find_package(naudio) in CMake
```

> Set `-DCMAKE_INSTALL_PREFIX` at **configure** time (not only on `cmake --install --prefix`):
> `naudio.pc` bakes the prefix when the build tree is configured, so a configure-time prefix keeps
> the pkg-config paths correct.

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

**Source compatibility is promised; binary compatibility begins at the first tag.** Nothing has been
tagged, `SOVERSION` is `0`, and no released consumer exists — so the C ABI is still free to change
shape. The rules below are what it commits to *from* the first tagged release, and the mechanism
they rest on is already in place.

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

/* Per field: a counter appended in 0.2.0 carries a measurement only on 0.2.0 or later.
   On anything older, na_client_get_stats zero-filled that tail and the 0 means nothing. */
if (na_version_number() >= NA_VERSION_ENCODE(0, 2, 0)) {
    /* safe to read the field that arrived in 0.2.0 */
}
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
- **No IP multicast; IPv4 only.** RX fan-out to multiple clients is **unicast replication** (O(N)
  egress), not IP multicast, and the transport is IPv4 (`AF_INET`) only. Fan-out should not be read as
  a multicast group.
- **Device selection is local, not remote.** A client or server selects its own capture / playback
  device locally; there is no control message to enumerate or choose the *peer's* device over the wire.
- **Conformance vectors gate C/C++ only.** The language-neutral golden vectors are loaded and checked
  by the C/C++ suite; the Python / Java / Rust example clients are validated by the runtime byte-tell
  (`first_frame_hex=68c569c66ac76bc8`), not by loading the vectors. There is no TypeScript / Node
  client yet.
- **No arbitrary sample-rate conversion.** naudio carries audio at the negotiated rate (48 kHz is the
  first-class default) and does **not** resample between arbitrary rates in v1; rate conversion is left
  to the application or a companion DSP stage.
