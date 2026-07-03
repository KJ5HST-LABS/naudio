# Contributing to naudio

Thanks for your interest. naudio is the C/C++ reference toolkit for the net-audio audio-streaming
protocol. A few conventions keep it clean and cross-platform.

## Build & test before you push

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

All tests must pass (the suite is hardware-free, so it runs anywhere). CI runs the same on Linux,
macOS, and Windows — see [.github/workflows/ci.yml](.github/workflows/ci.yml).

## Code conventions

- **C++17**, no compiler extensions (`CMAKE_CXX_EXTENSIONS OFF`). Build warning-free.
- **Keep `naudio_core` pure.** No fork/exec, no PortAudio, no sockets, no threads in the core library.
  Process/device/socket/thread code goes in `naudio_pa` / `naudio_net` / `tools/` — see the target
  layout table in the [README](README.md).
- **Public C ABI changes** must tag new symbols with `NA_EXPORT` and keep `include/naudio.h`
  C-callable (the `naudio_c_abi_smoke` test enforces this). Honor the length-probe / NULL-buffer
  contract on text functions.

## Licensing & provenance (please read)

naudio is **LGPL-2.1-or-later**. Contributions are accepted under that license.

- **Do not paste or link GPL/copyleft source.** naudio's shipped artifacts must stay free of any
  copyleft obligation. See [LICENSE](LICENSE).
- Only contribute code you have the right to license under LGPL-2.1+. Do not port code from projects
  with incompatible licenses.

## Changing the wire protocol

The `0xAF01` wire is **frozen at v1** (`docs/audio-streaming-protocol-v1.md`). A wire change
requires: (1) a spec revision, (2) regenerating `conformance/vectors/vectors.ini` via
`conformance/tools/gen_vectors.py`, and (3) re-checking the codec and every example client against
the regenerated golden vectors. The byte-identity tell for a correctly received stream is
`first_frame_hex=68c569c66ac76bc8`.

## Pull requests

Keep PRs focused. Describe what changed and why, and confirm `ctest` is green. New behavior should
come with a test (a `FakeBackend`/loopback unit test, or a conformance vector for wire changes).
