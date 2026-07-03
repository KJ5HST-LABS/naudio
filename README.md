# naudio — the C/C++ network-audio streaming toolkit

**naudio** is an open, cross-platform toolkit for moving radio **audio** over the network: a versioned wire **spec**, a reference **server** and **clients**, a reliability stack (jitter / FEC / reorder / ARQ), virtual-audio device plumbing, and a stable **C ABI** so C / Hamlib / Python consumers can link a single versioned artifact. It is the C/C++ home of the **net-audio** audio-streaming protocol (magic `0xAF01`), built so the ham ecosystem — fldigi, Quisk, digital-mode apps, and Hamlib apps — can reuse one audio transport instead of each re-inventing it.

> **On the names:** *net-audio* is the wire **protocol** — the frozen `0xAF01` frame contract defined in [the spec](docs/audio-streaming-protocol-v1.md). *naudio* is this C/C++ **toolkit** that implements it. There is no separate "net-audio" package to find or install; the protocol name and this repository are the whole story.

- **License:** LGPL-2.1-or-later — see **[LICENSE](LICENSE)**. (Matches Hamlib; links cleanly from proprietary apps and from GPL apps alike.)
- **Wire spec:** **[docs/audio-streaming-protocol-v1.md](docs/audio-streaming-protocol-v1.md)** — the frozen `0xAF01` v1 contract.
- **Protocols overview:** **[docs/protocols.md](docs/protocols.md)** — the `0xAF01` network audio wire format at a glance.
- **Conformance:** **[conformance/README.md](conformance/README.md)** — language-neutral golden vectors that pin every normative value in the wire spec.


---

## Status

The audio streaming stack is **implemented and tested** (the device layer, the `0xAF01` codec, the reliability primitives, the multi-tenant TCP/UDP server + client, and the full networking C ABI — both the `na_client_*` and `na_server_*` surfaces). It installs as a package: `find_package(naudio)` or pkg-config resolves the C ABI **and** the C++ API. The build runs **286 hardware-free ctests** green, including the language-neutral conformance golden vectors and the README snippet compile-gate. What is hardware-gated (real PortAudio capture/playback, on-air decode) is exercised by opt-in smokes and the `na_audio_daemon` driver, not by CI; see **Known limitations** below.

---

## Layout

| Target | Kind | What it is |
|---|---|---|
| `naudio_core` | static lib | **Pure, portable logic** — no fork/exec, no PortAudio, no sockets. Device classification/merge, format probing, the mono→stereo open policy, the `0xAF01` codec, and the reliability primitives (jitter / FEC / reorder / control-ACK). Fully unit-tested via a fake backend. |
| `naudio_pa` | static lib | `naudio_core` + the PortAudio device backend. |
| `naudio_net` | static lib | The transport layer — TCP/UDP sockets, per-client threads, the multi-tenant `AudioStreamServer`, and `AudioStreamClient`. Backend-agnostic (takes a `DeviceBackend*`). |
| `naudio` | **shared lib** | The public artifact: the **C ABI** (`include/naudio.h`) over the internal backends, exporting **only** the `na_*` surface (hidden visibility + `SOVERSION`). This is what a C / Hamlib consumer links and what `make install` ships, alongside `naudio.pc`. |
| `tools/` | app | The **hardware smoke daemon** (`na_audio_daemon`) — runs the real `AudioStreamServer` / `AudioStreamClient` over a live PortAudio device, the path ctest (fake backend only) cannot reach. An exerciser, **not** an audio primitive: it lives outside the libraries to keep the core pure + portable. |
| `examples/` | apps | The **examples suite** — a "play to speakers" client in C, C++, Python, Java, and Rust, plus a small demo source. Each links the public shared `naudio` and uses **only** `naudio.h`, proving the C ABI is self-sufficient from C and from any FFI consumer. See **[examples/README.md](examples/README.md)**. |

---

## Quick start

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure      # 286 logic + codec + C-ABI tests, no hardware
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
int n = na_enumerate(ctx, devs, 32);                     // count, or a negative na_error_t
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
na_client_set_callbacks(c, &events, user);               // connected / stream / roster / error events
na_client_set_playback_device(c, playback_id);           // REQUIRED for RX (any id on the NULL backend)
if (na_client_connect(c, errbuf, sizeof errbuf) != NA_OK) {
    /* connect failed — errbuf holds the reason (also via na_last_error()) */
}
/* ... receive PCM through on_pcm until done ... */
na_client_disconnect(c);
na_client_destroy(c);
```

The networking client wraps the C++ `AudioStreamClient`; the matching server is the C++
`AudioStreamServer` (in `naudio_net`), exposed to C through the symmetric **`na_server_*`** surface
(create / configure / start / inject-RX / extract-TX / roster + TX callbacks). See
**[examples/README.md](examples/README.md)** for the example clients (play to speakers, in five
languages) and the demo source.

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
