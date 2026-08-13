# External-consumer verification

An end-to-end check of the **installed C++ library**.
Proves a *throwaway external project* — one that knows nothing about the naudio
source tree — can consume the **installed** package four ways:

| Consumer | Mechanism | Surface exercised | naudio target |
|----------|-----------|-------------------|---------------|
| `consume_c.c`   | `find_package(naudio)` | client **and** server C ABI (`na_client_*` + `na_server_*`) | `naudio::naudio` (shared) |
| `consume_cxx.cpp` | `find_package(naudio)` | the C++ API (`naudio::crc32`, `naudio::net::Socket`) | `naudio::naudio_core` + `naudio::naudio_net` (static) |
| `consume_pa.cpp` | `find_package(naudio)` | the device backend's **link**, incl. the consumer-resolved PortAudio | `naudio::naudio_pa` (static) — *conditional* |
| `consume_c.c`   | **pkg-config** (`naudio.pc`) | client + server C ABI | `-lnaudio` |

All consumers are hardware-free (NULL backends / pure-logic calls), so the gate
runs headless. `consume_pa` is hardware-free by a narrower route worth stating: it
opens no device and never calls `Pa_Initialize`, so what it asserts is that it
**linked** — that `PortAudioBackend.o` came out of the installed archive and its
`Pa_*` references resolved. It is not evidence that device I/O works.

**`consume_pa` is the conditional one, deliberately** (issue #61). The installed
package names no PortAudio target; `naudioConfig.cmake` resolves one on the
consumer's side (pkg-config, else `find_library`/`find_path`) and attaches it to
`naudio::naudio_pa` alone. On a host with no PortAudio the package still loads with
`naudio_PORTAUDIO_FOUND` `FALSE`, and this arm correctly does not build — which is
exactly the case the `fetchcontent` and `windows` CI jobs exercise. Because a
silently vanishing arm is indistinguishable from a passing one, the project prints
the arm's status either way and `run.sh` fails if the line is missing altogether.

## Run it

```sh
tests/external-consumer/run.sh
```

The script (1) configures + builds + installs naudio into a `mktemp` prefix,
(2) configures this project against that prefix via `CMAKE_PREFIX_PATH` and checks
the `naudio_pa` arm reported its status, (3) runs the `find_package` consumers, and
(4) compiles + runs the C consumer again through `pkg-config`. Everything lives in
the temp dir — no source-tree or system residue. Exit 0 = the install/export
surface is sound.

## Design note

The C++ API ships as the internal **static** archives (`naudio_core` /
`naudio_net` / `naudio_pa`), not a second `naudio++` shared lib. Static-link
resolution ignores the hidden symbol visibility the shared C ABI relies on, so the
unexported C++ symbols stay callable from an external TU that links the installed
archive — which `consume_cxx` demonstrates. This preserves the
hidden-visibility-shared-C-ABI + static-internal-C++ design the library
maintains.

## Not wired into ctest — but it is wired into CI

This gate performs a full `install`, which ctest (a build-tree harness) does not
model cleanly, so it stays a standalone script rather than a registered test. It is
**not** optional: the `build-and-test` job runs `run.sh` verbatim on the
system-PortAudio matrix.

The `windows` and `fetchcontent` jobs consume the package too, but without calling
`run.sh` — it builds and installs naudio itself, which on those jobs would mean a
second from-source PortAudio build in the same run. They install once and configure
this project against that prefix directly. Those two are where the interesting case
lives: neither runner has any PortAudio, so `find_package(naudio)` must succeed with
the `naudio_pa` arm skipped, and both jobs assert on that skip.
