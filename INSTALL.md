<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Installing naudio

Every install path below ends in the same package: the shared `naudio` library (the
stable `na_*` C ABI), the C++ static archives and headers, `naudio.pc` for pkg-config,
the CMake package for `find_package(naudio)`, and the command-line programs — the demo
pair (`na_audio_source`, `na_c_play_to_speakers`: hear audio across two machines with no
code), the device-serving daemon (`na_audio_daemon`), and the stream recorder
(`na_wav_tap`) — with man pages on the platforms that read them. (The optional Hamlib
bridge is a from-source build only for now; see the note under Option C.)

Once installed, **[docs/getting-started.md](docs/getting-started.md)** is the walkthrough
from an installed toolkit to audio streaming end to end.

> **Current availability (2026-08).** naudio is not yet public — the repository is
> private ahead of its planned Hamlib contribution. The current packaged release,
> **v1.0.0rc2** (a prerelease), carries the full installer set: TGZ for Linux and macOS,
> DEB, RPM, a Windows ZIP, and `sha256sums.txt`. While the repository is private,
> collaborators with access fetch packages with
> `gh release download v1.0.0rc2 -R KJ5HST-LABS/naudio` rather than anonymous URLs, and
> the Homebrew tap installs over ssh. Everything else on this page is written for the
> durable, public state and works unchanged once the repository is public.

## Option A — binary release packages

Packages are attached to tagged releases on the
[Releases page](https://github.com/KJ5HST-LABS/naudio/releases), named
`naudio-<version>-<os>-<arch>` with a `sha256sums.txt` beside them. They are
self-contained — PortAudio is built into the library, and nothing in a package depends on
anything a package manager cannot satisfy. Deliberately **not** in the packages:
`na_hamlib_bridge`, which needs a streaming-capable libhamlib that no released Hamlib
provides yet. Shipping it would mean embedding a frozen pre-release hamlib beside
whatever hamlib your other applications carry; instead it joins the packages — linked
against the system's real libhamlib — once a released Hamlib ships the streaming API.

### Debian / Ubuntu (.deb)

```bash
sudo apt-get install ./naudio-<version>-linux-x86_64.deb
```

Installs under `/usr`; runtime dependencies (ALSA) are declared by the package and
resolved by `apt`. `pkg-config --modversion naudio` and `man na_wav_tap` work
immediately — `/usr/lib/pkgconfig` is on pkg-config's default search path on
Debian-family systems.

### Fedora / RPM distributions (.rpm)

```bash
sudo rpm -i naudio-<version>-linux-x86_64.rpm
```

This is a GitHub convenience RPM, not distro-official packaging: it installs under
`/usr` with `lib/` (not `lib64/`), so tell pkg-config where the `.pc` file is:

```bash
PKG_CONFIG_PATH=/usr/lib/pkgconfig pkg-config --modversion naudio
```

### Linux / macOS tarball (.tar.gz)

The tarballs are **relocatable** — unpack anywhere; the tools find `libnaudio` through a
relative rpath, no environment needed:

```bash
mkdir -p ~/naudio && tar xzf naudio-<version>-<os>-<arch>.tar.gz -C ~/naudio --strip-components 1
~/naudio/bin/na_wav_tap --help
man ~/naudio/share/man/man1/na_wav_tap.1        # macOS: man <path>; Linux: man -l <path>
```

To build **against** an unpacked tarball, point your build system at it:

```bash
# pkg-config: --define-prefix rewrites the baked paths to wherever you unpacked
PKG_CONFIG_PATH=~/naudio/lib/pkgconfig pkg-config --define-prefix --cflags --libs naudio

# CMake: find_package(naudio) via the prefix
cmake -S . -B build -DCMAKE_PREFIX_PATH=~/naudio
```

> **macOS:** if Gatekeeper blocks a tool downloaded through a browser (the binaries are
> not notarized), clear the quarantine attribute on the unpacked tree:
> `xattr -dr com.apple.quarantine ~/naudio`.

### Windows (.zip)

The ZIP carries the library (`naudio.dll` + import library), the headers, the CMake
package, and three programs: the demo pair (`na_audio_source.exe`,
`na_c_play_to_speakers.exe`) and the stream recorder (`na_wav_tap.exe`). Expand it
anywhere:

```powershell
Expand-Archive naudio-<version>-windows-x86_64.zip -DestinationPath C:\naudio
C:\naudio\<unpacked-dir>\bin\na_audio_source.exe --test-tone     # then play it from any machine
```

Consume it from CMake with `-DCMAKE_PREFIX_PATH=<unpacked-dir>`; `naudio.dll` sits in
`bin\` beside the programs. (`na_audio_daemon` and the Hamlib bridge are not built on
Windows.)

## Option B — Homebrew (macOS)

```bash
brew tap kj5hst-labs/naudio git@github.com:KJ5HST-LABS/homebrew-naudio.git
brew install naudio
```

The formula builds the tagged release from source against Homebrew's PortAudio (the
explicit tap URL is only needed while the tap is private). Like the binary packages, it
does not build `na_hamlib_bridge` (no released Hamlib provides the streaming API it
needs). The programs and their man pages install from the formula from v1.0.0rc1 onward.

## Option C — from source

### Prerequisites

| Platform | Commands |
|---|---|
| Debian / Ubuntu | `sudo apt-get install build-essential cmake pkg-config portaudio19-dev` |
| macOS | Xcode Command Line Tools, then `brew install cmake pkg-config portaudio` |
| Windows | Visual Studio 2019+ (C++ workload) and CMake 3.20+ |

CMake ≥ 3.20 and a C++17 compiler are the hard requirements. PortAudio is the only
system dependency, and it is optional: without it, the first configure fetches and
builds PortAudio v19.7.0 from source (as it does GoogleTest for the test suite).

### Build, test, install

```bash
git clone https://github.com/KJ5HST-LABS/naudio.git
cd naudio
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local   # set the prefix at CONFIGURE time
cmake --build build -j
ctest --test-dir build --output-on-failure               # hardware-free; optional but cheap
sudo cmake --install build
```

> Set `-DCMAKE_INSTALL_PREFIX` at **configure** time (not only on
> `cmake --install --prefix`): `naudio.pc` bakes the prefix when the build tree is
> configured, so a configure-time prefix keeps the pkg-config paths correct.

The tools build and install by default (`NAUDIO_BUILD_APPS=ON`); `na_hamlib_bridge` is
the one exception — it is opt-in and needs a streaming-capable libhamlib first. Building
that dependency, enabling the bridge, and confirming you actually got it are covered in
**[docs/hamlib-streaming-bridge.md](docs/hamlib-streaming-bridge.md)**.

A from-source install can be removed with the manifest CMake writes:
`xargs rm < build/install_manifest.txt` (the package installs above are removed by their
package manager: `apt-get remove naudio`, `rpm -e naudio`, `brew uninstall naudio`).

## Verify any install

```bash
pkg-config --modversion naudio        # prints the installed version
na_wav_tap --help                     # the tools run (add the prefix's bin/ to PATH if needed)
man na_wav_tap                        # man pages are on the install path
```

And from CMake:

```cmake
find_package(naudio CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE naudio::naudio)   # the C ABI
```

`naudio::naudio_core`, `naudio::naudio_net` and `naudio::naudio_pa` (C++ API) install
alongside — see the README's *Using naudio as a C++ library* section for the
`naudio_PORTAUDIO_FOUND` gate that `naudio::naudio_pa` consumers should check.

Next: **[docs/getting-started.md](docs/getting-started.md)** — from an installed toolkit
to audio streaming end to end.
