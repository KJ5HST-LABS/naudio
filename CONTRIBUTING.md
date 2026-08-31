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
- **C11** (`CMAKE_C_STANDARD 11`, `_REQUIRED ON`) — and, unlike C++, compiler extensions are
  deliberately left **on**. Strict `-std=c11` defines `__STRICT_ANSI__`, which on glibc hides the
  POSIX declarations `tools/na_hamlib_bridge.c` depends on; it compiles on macOS and fails on
  Linux, so please do not "restore the symmetry" with `CMAKE_C_EXTENSIONS OFF`. Note this is the
  level the *tests* compile at: `include/naudio.h` itself is C99-clean, so a consumer of the C ABI
  is not obliged to build at C11. **That is enforced, not merely asserted** — the
  `naudio_c99_header_gate` ctest arm compiles a TU including the header at
  `-std=c99 -pedantic-errors` on every run (`tests/c99-header-gate/`). So a `_Static_assert`,
  `_Alignas` or anonymous union added to the **public header** will fail the suite even though the
  rest of the build accepts it; that is deliberate, and the fix is to use a C99 construct — or, if
  the floor genuinely has to rise, to change this sentence and the one in `CMakeLists.txt` with it
  and say why.
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
requires: (1) a spec revision, (2) regenerating the golden vectors via
`conformance/tools/gen_vectors.py` — which writes `conformance/vectors/vectors.ini` **and**
`conformance/vectors/vectors-v1_2.ini` (spec-1.2 forms, loaded by the suite's
`Conformance.GoldenVectorsV12` under its own fail-closed gate; see spec §12) — and
(3) re-checking the codec and every example client against the regenerated vectors.
The byte-identity tell for a correctly received stream is `first_frame_hex=68c569c66ac76bc8`.

## Cutting a release

Push a `v*` tag. `.github/workflows/release.yml` builds every platform, runs the full suite,
gates the **packages themselves** (a package-based install must pass the same external-consumer
commands a build-tree install does), and attaches the artifacts to a GitHub Release — prerelease
automatically for a `v*rc*` tag. `workflow_dispatch` runs the byte-identical dry run without
touching a release, which is how the pipeline is proven before a tag exists.

**Signing the macOS `.pkg` is a manual step, and it is one command:**

```
packaging/macos/publish-signed-pkg.sh v1.0.0rc4          # add --dry-run to verify and publish nothing
```

The workflow has no signing identity, so the `.pkg` it publishes is unsigned and the release note
says so — measured on the artifact by `packaging/macos/signing-status.sh`, never assumed (issue
#99: the note used to assert "signed and notarized" unconditionally, which was false at the moment
it published and true only if a human followed up). The script above signs, notarizes, staples,
re-uploads, regenerates `sha256sums.txt` from the published assets, and rewrites that sentence —
together, because doing them separately is what left one release describing a file it had already
replaced. It refuses to upload anything that does not measure as signed first, and re-downloads
the published assets to check the result. It needs a Developer ID Installer identity and a
`notarytool` keychain profile; run it with `--help` for the one-time setup.

The Windows installer is unsigned (no certificate) and SmartScreen warns about it.

## Pull requests

Keep PRs focused. Describe what changed and why, and confirm `ctest` is green. New behavior should
come with a test (a `FakeBackend`/loopback unit test, or a conformance vector for wire changes).
