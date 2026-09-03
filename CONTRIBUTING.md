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
- **Do not tell consumers to gate a NEW SYMBOL on `na_version_number()`.** It cannot work: the
  reference is resolved when the image loads, so a missing symbol aborts the process before any
  check of yours runs, and the version of an unreleased cycle does not distinguish the build that
  has the symbol from the one that does not. Document new symbols as configure-time detections
  (`check_symbol_exists`). A version comparison is for **struct fields and behaviour** of symbols
  that already exist — that use is correct and stays.

## Licensing & provenance (please read)

naudio is **LGPL-2.1-or-later**. Contributions are accepted under that license.

- **Do not paste or link GPL/copyleft source.** naudio's shipped artifacts must stay free of any
  copyleft obligation. See [LICENSE](LICENSE).
- Only contribute code you have the right to license under LGPL-2.1+. Do not port code from projects
  with incompatible licenses.

## Changing the wire protocol

The `0xAF01` wire is **frozen at v1** (`docs/audio-streaming-protocol-v1.md`). A wire change
requires: (1) a spec revision, (2) regenerating the golden vectors via
`conformance/tools/gen_vectors.py` — which writes `conformance/vectors/vectors.ini`,
`conformance/vectors/vectors-v1_2.ini` (spec-1.2/1.3 forms, `Conformance.GoldenVectorsV12`)
**and** `conformance/vectors/vectors-v1_4.ini` (spec-1.4 discovery, `Conformance.GoldenVectorsV14`),
each under its own fail-closed 0-skipped gate; see spec §12. Adding a record to the generator
means raising that gate's count literal deliberately — it is what stops a vector going missing
from an `.ini` and the suite reporting green over a smaller corpus — and
(3) re-checking the codec and every example client against the regenerated vectors.
The byte-identity tell for a correctly received stream is `first_frame_hex=68c569c66ac76bc8`.

## Cutting a release

**First, set `NAUDIO_VERSION_PRERELEASE` in `include/naudio.h`** — `""` for a final `X.Y.Z`, or
the SemVer tag *including its leading dash* (`"-rc6"`) for a pre-release. It is what separates a
release from the builds around it: the three numeric macros cannot, because every build of an
unreleased `X.Y.Z` reports `X.Y.Z`. Getting this wrong is how `v1.0.0rc5` and the main branch
after it both came to answer `1.0.0` from `na_version_string()`, which made a consumer's version
gate true against a library that lacked the symbol it was gating (see the header's *Library
version* note). The CMake version gate does **not** check it — a pre-release tag is not part of
the SONAME — so nothing but this step and `c_abi_smoke`'s shape check stands behind it.

Then push a `v*` tag. `.github/workflows/release.yml` builds every platform, runs the full suite,
gates the **packages themselves** (a package-based install must pass the same external-consumer
commands a build-tree install does), and attaches the artifacts to a GitHub Release — prerelease
automatically for a `v*rc*` tag. `workflow_dispatch` runs the byte-identical dry run without
touching a release, which is how the pipeline is proven before a tag exists.

**The macOS `.pkg` is signed, notarized and stapled by the release workflow itself.** The macOS
job imports the org's `DEVELOPER_ID_CERTIFICATE_P12` and `DEVELOPER_ID_INSTALLER_P12` into a
throwaway keychain; `cmake/CodesignPayload.cmake` signs every payload binary at
`CPACK_PRE_BUILD_SCRIPTS` (hardened runtime, secure timestamp, and the audio-input entitlement on
`na_audio_daemon` and `na_audio_source` — the two tools that open an input device, which the
hardened runtime otherwise silences when launchd starts them); and once the installer gate has
passed, the wrapper is `productsign`ed, submitted with `notarytool` (`APPLE_ID`, `APPLE_TEAM_ID`,
`APPLE_APP_SPECIFIC_PASSWORD`) and stapled. None of that is trusted: `packaging/macos/signing-status.sh`
measures the artifact afterwards and the release note is written from that measurement, never
assumed (issue #99: the note used to assert "signed and notarized" unconditionally, which was false
at the moment it published and true only if a human followed up). A run with the secrets present
must measure `signed` or it fails; a run without them publishes an honest "unsigned" sentence.

**If a release was published without the secrets**, the fallback is still one command:

```
packaging/macos/publish-signed-pkg.sh v1.0.0rc4          # add --dry-run to verify and publish nothing
```

It signs, notarizes, staples, re-uploads, regenerates `sha256sums.txt` from the published assets,
and rewrites that sentence — together, because doing them separately is what left one release
describing a file it had already replaced. It refuses to upload anything that does not measure as
signed first, and re-downloads the published assets to check the result. It needs a Developer ID
Installer identity and a `notarytool` keychain profile (run it with `--help` for the one-time
setup), and a `.pkg` whose payload CI signed — an unsigned payload cannot be retrofitted, so re-run
the release instead.

The Windows installer is unsigned (no certificate) and SmartScreen warns about it.

## Pull requests

Keep PRs focused. Describe what changed and why, and confirm `ctest` is green. New behavior should
come with a test (a `FakeBackend`/loopback unit test, or a conformance vector for wire changes).
