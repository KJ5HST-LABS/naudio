# External-consumer verification

An end-to-end check of the **installed C++ library**.
Proves a *throwaway external project* — one that knows nothing about the naudio
source tree — can consume the **installed** package three ways:

| Consumer | Mechanism | Surface exercised | naudio target |
|----------|-----------|-------------------|---------------|
| `consume_c.c`   | `find_package(naudio)` | client **and** server C ABI (`na_client_*` + `na_server_*`) | `naudio::naudio` (shared) |
| `consume_cxx.cpp` | `find_package(naudio)` | the C++ API (`naudio::crc32`, `naudio::net::Socket`) | `naudio::naudio_core` + `naudio::naudio_net` (static) |
| `consume_c.c`   | **pkg-config** (`naudio.pc`) | client + server C ABI | `-lnaudio` |

All consumers are hardware-free (NULL backends / pure-logic calls), so the gate
runs headless.

## Run it

```sh
tests/external-consumer/run.sh
```

The script (1) configures + builds + installs naudio into a `mktemp` prefix,
(2) configures this project against that prefix via `CMAKE_PREFIX_PATH`,
(3) runs both `find_package` consumers, and (4) compiles + runs the C consumer
again through `pkg-config`. Everything lives in the temp dir — no source-tree or
system residue. Exit 0 = the install/export surface is sound.

## Design note

The C++ API ships as the internal **static** archives (`naudio_core` /
`naudio_net` / `naudio_pa`), not a second `naudio++` shared lib. Static-link
resolution ignores the hidden symbol visibility the shared C ABI relies on, so the
unexported C++ symbols stay callable from an external TU that links the installed
archive — which `consume_cxx` demonstrates. This preserves the
hidden-visibility-shared-C-ABI + static-internal-C++ design the library
maintains.

## Not wired into ctest

This gate performs a full `install`, which ctest (a build-tree harness) does not
model cleanly. It is a standalone script intended to be invoked as its own CI job
(a natural CI addition).
