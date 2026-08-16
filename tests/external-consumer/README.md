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
| `consume_skew.c` | **pinned OLD header** + pkg-config | **version skew**: `struct_size` honoured for a consumer compiled before the newest ABI append | `-lnaudio` |
| `consume_zerofill.c` | current header + pkg-config | the other direction: a caller declaring **more** than the library knows gets the excess zero-filled | `-lnaudio` |

## The version-skew arm (issues #32, #88 item 1)

Arms 1–4 above, and every test in the build tree, compile against **the same header as the
library they link**. That configuration structurally cannot observe the hazard `struct_size`
exists to prevent — a newer library writing past the end of a struct an already-compiled
consumer allocated. `tests/c_abi_smoke.c` simulates a differently-sized caller by hand in the
same translation unit, which is good and mutation-verified, but a hand-declared size is not a
separately-compiled consumer.

`consume_skew.c` is compiled against a **pinned older `<naudio.h>` extracted from git history**
(`git show <pin>:include/naudio.h`) and linked against the library just installed, so its
`sizeof(na_client_stats)` is baked in from the old header and the library must honour the size
passed as a parameter.

**Pins are fixed strings, never `HEAD~n`** — a pin that moves with the branch is not a pin:

| Pin | Why |
|---|---|
| `v0.4.0` | The first tagged release — the ABI a packager can actually pin to. **Zero skew today**, and it says so; it starts carrying real load at the first post-tag append with no edit needed. |
| `d4f3c3a^` | The commit before `na_client_stats` gained `fec_pending_discarded`. Its header knows `NA_CLIENT_STATS_SIZE_V1..V3` and a struct 8 bytes shorter than the library's — this is what makes the gate non-vacuous **today**. |

### Why the harness fails when no pin has real skew

The consumer prints the pinned header's `NAUDIO_VERSION_NUMBER` next to the linked library's
`na_version_number()` on every run, and `run.sh` **fails** if every resolved pin reports
`NO SKEW`. That is not defensive decoration. Measured: with the `NA_CLIENT_STATS_SIZE_V4` guard
deleted from `na_client_get_stats`, **the `v0.4.0` arm still passes** — it has no skew, so it
cannot detect the break — while the `d4f3c3a^` arm fails at byte 144. A gate whose only pin has
drifted to equal HEAD would go green against a library that ignores `struct_size` entirely.

If no pin resolves at all (a shallow clone cannot reach tags or history), the arm reports
**SKIPPED with the reason** and does not claim coverage — `CLAUDE.md` Learning 9, a skip that
scrolls past as a pass. CI sets `fetch-depth: 0` on the job that runs this script so the pins
resolve.

### Mutation-verified

All three failure paths were driven RED before being trusted, each singly, against a from-scratch
build:

| Mutation | Result |
|---|---|
| Drop the `struct_size >= NA_CLIENT_STATS_SIZE_V4` guard | **RED** — `consume_skew` names byte 144 holding `0xFF` where the guard `0xA5` belongs |
| Drop the `struct_size > sizeof(...)` zero-fill `memset` | **RED** — `consume_zerofill` names byte 152 holding `0xA5` where `0x00` belongs |
| Reduce the pin set to the zero-skew pin only | **RED** — "the gate has gone vacuous" |

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
