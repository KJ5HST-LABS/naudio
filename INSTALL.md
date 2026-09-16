<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Installing naudio

**Just want it to work?** Download the installer for your system from the
[Releases page](https://github.com/KJ5HST-LABS/naudio/releases) and run it: the `.pkg` on
a Mac, the setup `.exe` on Windows, the `.deb` or `.rpm` on Linux. Each one installs the
ready-to-run programs — a demo server and player that put audio across two machines with
no code, the audio-serving daemon, and the stream recorder — along with everything they
need. Then **[docs/getting-started.md](docs/getting-started.md)** walks you from there to
audio moving across your network.

**Writing software that uses naudio?** The tarballs, the Linux packages, and Homebrew
also carry the developer files — the C headers, `naudio.pc` for pkg-config, and the CMake
package for `find_package(naudio)`. The double-click installers deliberately leave those
out; pick any other format. (The optional Hamlib bridge is a from-source build only for
now; see the note under Option C.)

> **Current availability (2026-09).** The current packaged release, **v1.0.0rc9** (a
> prerelease), carries everything on this page: the double-click installers (a macOS `.pkg`
> and a Windows setup `.exe`), TGZ for Linux and macOS, DEB, RPM, a Windows ZIP, and
> `sha256sums.txt` — from the release page, or with
> `gh release download v1.0.0rc9 -R KJ5HST-LABS/naudio`. The repository is public; the
> Homebrew tap is not yet, and installs over ssh (Option B). naudio is ahead of its planned
> Hamlib contribution, and this page is written for the durable state.

## Option A — binary release packages

Packages are attached to tagged releases on the
[Releases page](https://github.com/KJ5HST-LABS/naudio/releases), named
`naudio-<version>-<os>-<arch>` with a `sha256sums.txt` beside them. They are
self-contained: the audio engine is built in, and nothing needs to be installed first.
One thing is deliberately **not** in the packages: the optional Hamlib radio bridge
(`na_hamlib_bridge`), because it needs a version of Hamlib that has not been released
yet. It joins the packages the day one is; until then it builds from source
(`docs/hamlib-streaming-bridge.md`).

### Double-click installers — macOS (.pkg) and Windows (setup .exe)

The "make it just work" path: download `naudio-<version>-macos-<arch>.pkg` or
`naudio-<version>-windows-x86_64.exe` and double-click it. You get everything needed to
run naudio — the programs, the library they share, and (on a Mac) their manual pages.
What the installers leave out are the files for *building software* against naudio;
those are in every other format on this page.

- **macOS** installs under `/usr/local` (`bin/`, `lib/`, `share/man/`), puts **Network Audio
  Service** in `/Applications` (the entry that opens the control page — and the program the
  service runs as, so leave it there), and registers the daemon
  as a background service — listed as **Network Audio Service** under *System Settings › General
  › Login Items & Extensions* on a fresh install. Its
  switch there is the service's switch: it is on, so from your next login the daemon serves the
  control page and captures nothing until you press Start; turn it off there to stop it (see
  *Running the daemon in the background* below). The first time you press Start, macOS asks
  whether to allow **Network Audio Service** to use the microphone — that is the radio's USB
  audio interface; allow it (see *The microphone permission* below for what happens if you
  don't, and how to change your answer). A Mac that already had the service from an
  earlier release keeps listing it under the developer's name: macOS names that entry once, when
  it first sees the service, and reinstalling over it does not rename it. To rename it, make
  macOS drop the old entry first: `sudo rm /Library/LaunchAgents/org.kj5hst.naudio.daemon.plist`,
  open *Login Items & Extensions* (the pane rescans and drops the orphaned entry), then run the
  installer again — no logout needed. **Each release's notes say
  whether that release's `.pkg` is signed and notarized**, and they say it from a check of the
  file itself rather than from what was intended (issue #99) — a signed one opens like any other
  installer, and an unsigned one needs a right-click → *Open* the first time. macOS installers have no
  uninstaller; to remove naudio, delete the installed files and forget the receipts
  (`sudo pkgutil --forget org.kj5hst.naudio.runtime`, same for `….tools`).
- **Windows** installs into `C:\Program Files\naudio`, adds **Network Audio Service** to the
  Start menu (under *naudio*), registers a logon task for the daemon (switched off until you turn
  it on), adds an uninstaller (*Add or remove programs*, or `Uninstall.exe` in the install
  directory), and offers to add the programs to `PATH`. The installer is unsigned, so SmartScreen
  warns: choose *More info* → *Run anyway*.

Once it is installed, open **Network Audio Service** — in the Start menu on Windows, the
applications menu on Linux, or Applications on a Mac. It opens a page in your browser with two tabs:
**Setup**, where you pick the radio's audio device, choose a transport and port, and save; then
**Stream**, where you press Start. The page opens on Setup until a device is chosen, and Start waits
until Setup is saved. Nothing captures audio until you press it.

### Debian / Ubuntu (.deb)

```bash
sudo apt-get install ./naudio-<version>-linux-x86_64.deb
```

Installs under `/usr`; anything it needs (ALSA) is pulled in by `apt` automatically. The
programs and their man pages work immediately, and for developers so does
`pkg-config --modversion naudio` — `/usr/lib/pkgconfig` is on pkg-config's default
search path on Debian-family systems.

### Fedora / RPM distributions (.rpm)

```bash
sudo rpm -i naudio-<version>-linux-x86_64.rpm
```

The programs and man pages work immediately. This is a GitHub convenience RPM, not
distro-official packaging — developers building against it should note it installs under
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
package, and four programs: the daemon (`na_audio_daemon.exe`), the demo pair
(`na_audio_source.exe`, `na_c_play_to_speakers.exe`) and the stream recorder
(`na_wav_tap.exe`). Expand it anywhere:

```powershell
Expand-Archive naudio-<version>-windows-x86_64.zip -DestinationPath C:\naudio
C:\naudio\<unpacked-dir>\bin\na_audio_source.exe --test-tone     # then play it from any machine
```

Consume it from CMake with `-DCMAKE_PREFIX_PATH=<unpacked-dir>`; `naudio.dll` sits in
`bin\` beside the programs. The daemon reads its settings from
`%ProgramData%\naudio\daemon.conf` — the same keys as its command-line options, one
`key = value` per line — or from any file you name with `--config`. You do not have to write
that file by hand: `na_audio_daemon --mode control --open-page` opens the control page in your
browser and writes it for you. The ZIP registers nothing, so nothing starts at logon; the setup
`.exe` above is the path that registers a logon task. (The Hamlib bridge is still not built on
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

## The control page — running naudio without a terminal

Every package installs a shortcut that opens the page, named after the service: **Network Audio
Service** in the Start menu on Windows and the applications menu on Linux, and
`/Applications/Network Audio Service.app` on macOS, where that is also the name Login Items lists
the service under. Opening it starts the daemon in *control mode* and opens a page in your browser
at `http://127.0.0.1:8737/`.

The page has two tabs, **Setup** and **Stream**, in that order because the settings decide what
Start does. On **Setup** you:

- **pick the capture device** — the radio's USB audio interface, chosen from a list rather than
  typed as a name or an index. It is saved by *name*: device numbers move when something is
  plugged in or unplugged, names do not, so the radio is still the radio after a USB microphone
  comes or goes. The picker shows the device your choice resolves to now, says when it has moved
  to another number, and says when it is not connected — in which case Start stays off and names
  it rather than capturing whatever took its number;
- **set the transport, port, sample rate and channel count**, and save them, which writes the
  same configuration file `man na_audio_daemon` describes (so the background service below picks
  up exactly what you chose);
- **see whether the service runs at login** — *Runs at login: On / Off / Not installed*, read
  from the platform's own switch every second. On a Mac the row's **Open Login Items…** button
  takes you to the pane that owns that switch (the page shows it; only System Settings can move
  it); on Linux and Windows the row names the command that flips it (`systemctl --user` and
  `schtasks /Change`, the lines under *Running the daemon in the background* below).

On **Stream** you:

- **start, stop and restart the stream**, and watch it: delivered percentage, connected clients,
  per-channel levels, gaps and error counters. A stream whose levels sit at −120 dBFS while
  audio is being delivered is not a working stream, and after a few seconds the page says so
  and names the likely cause — on a Mac, a Microphone permission that was denied (see *The
  microphone permission* below); anywhere, a muted input or the wrong device;
- **quit the daemon**, since a program started from a shortcut has no window to close.

The page opens on Setup until a capture device has been chosen, and **Start** stays off — with
the reason beside it — until Setup is saved; while Setup holds unsaved changes its tab carries a
dot and Start waits. Settings saved while a stream is running are not that stream's settings until
it restarts: the Stream tab says so and offers **Restart**.

Nothing is captured until you press **Start**, or tick *Start streaming automatically when the
daemon starts*. Opening the shortcut twice is safe: if a control page is already running, the
second one opens it rather than complaining about the port.

The page is served on `127.0.0.1` only. It is not reachable from your network and naudio has no
remote management; there is no password, so any program running on this machine can reach it.

## Running the daemon in the background

`na_audio_daemon` normally runs in a terminal window and stops when you close it. The
packages also install it as a background service, so it starts with your computer and
keeps serving audio on its own — including its control page, so once the service is on, the
shortcut above just opens the page.

Installing software should not start listening to your microphone, and none of the three
does: the service runs the control page and opens no device until you press Start (or tick
*Start streaming automatically*). Where the platforms differ is the switch.

On **macOS** the switch is the system's: the service appears in *System Settings › General ›
Login Items & Extensions › Allow in the Background* as **Network Audio Service**, on. From your next
login the daemon is running and its page is a click away; turning the switch off there stops
it and keeps it off, turning it on starts it again. There is no command to run — a
`launchctl enable`/`disable` from the rc7 and rc8 instructions does not move that switch (the
two are independent), so the installer clears one if you had run it. The service runs the copy
of the daemon inside `/Applications/Network Audio Service.app` — that is what makes macOS call
it by that name — so the app is part of the service, not just a shortcut to its page.

### The microphone permission (macOS)

The radio's USB audio interface is an input device, and macOS treats every input as a
microphone: the first time the service opens it, macOS asks whether to allow **Network Audio
Service** to access your microphone. Allow it. If the stream you had just started reports an
error at that moment, press **Start** again — macOS sometimes fails the open while the question
is on screen rather than waiting for your answer.

If you click **Don't Allow**, nothing errors: the service keeps running, the stream starts,
audio is delivered — and it is all silence, because that is what macOS hands a program it has
denied. The page notices after a few seconds (the levels stay at −120 dBFS while audio arrives)
and says so. To change your answer, open *System Settings › Privacy & Security › Microphone*,
turn **Network Audio Service** on, and restart the stream from the page. Running
`na_audio_daemon` from a terminal is not affected either way: there the permission belongs to
the terminal application.

Earlier releases listed the service there as `na_audio_daemon` — macOS names a program by its
file when it runs outside an application, which is why the service now runs from inside the
app. Upgrading from one of those, your old answer does not carry over: the next Start asks
once more, under the new name, and the old `na_audio_daemon` row can be removed with the pane's
**−** button. launchd also keeps the old definition of the service until you log in again (or
turn its switch off and on in Login Items), so do that once after such an upgrade.

On **Linux** the service arrives **switched off**, and turning it on is a deliberate,
one-time step:

```bash
systemctl --user enable --now naudio-daemon
```

The unit is `naudio-daemon`, which is what the commands take; `systemctl --user status
naudio-daemon` describes it as **Network Audio Service**, the same name the other platforms show.

Both run the daemon as **you**, not as a system account — that is what lets it reach your
microphone and your sound system at all. The settings it uses come from the configuration
file described in `man na_audio_daemon` (*CONFIGURATION FILE*); the service reads that file
once when it starts, so after changing it, restart the service:

```bash
launchctl kickstart -k gui/$(id -u)/org.kj5hst.naudio.daemon   # macOS
systemctl --user restart naudio-daemon                          # Linux
```

If the service will not start, the usual cause is a mistake in the configuration file, and
the message names the line: `systemctl --user status naudio-daemon` on Linux, or
`/usr/local/var/log/naudio/daemon.log` on a Mac. To switch the service off again, turn it off
in Login Items on a Mac, or use `systemctl --user disable --now naudio-daemon` on Linux.

On **Windows** the installer registers a Task Scheduler **logon task** rather than a Windows
service, and for the same reason the other two are session-scoped: a Windows service runs in
session 0, which has no audio devices, so it would install perfectly and then capture nothing.
It arrives switched off, like Linux:

```powershell
schtasks /Change /TN "\naudio\naudio-daemon" /ENABLE
schtasks /Run    /TN "\naudio\naudio-daemon"
```

Turn it off again with `/DISABLE`, or from *Task Scheduler* under the **naudio** folder, where the
task is `naudio-daemon` and its description names it **Network Audio Service**. Windows
does not capture a task's output anywhere, so there is no equivalent of the log file above: a
configuration mistake shows up as `LastTaskResult` 2 on the task, and on the control page.

All three services run the daemon in **control mode**, so a service that is on gives you the
control page at every login and captures nothing until you ask it to. If you want capture to start
automatically, tick *Start streaming automatically* on the page (it writes `autostart = true`
into the configuration file).

## Verify any install

```bash
na_wav_tap --help                     # the programs run (add the prefix's bin/ to PATH if needed)
man na_wav_tap                        # man pages are on the install path
pkg-config --modversion naudio        # developers: prints the installed version
```

And from CMake:

```cmake
find_package(naudio CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE naudio::naudio)   # the C ABI
```

`naudio::naudio_core`, `naudio::naudio_net` and `naudio::naudio_pa` (C++ API) install
alongside — see the README's *Using naudio as a C++ library* section for the
`naudio_PORTAUDIO_FOUND` gate that `naudio::naudio_pa` consumers should check.

---

## What else the package installs

Every package carries the licence, the wire specification and the conformance vectors, so
an installed copy is self-contained — you do not need the repository to implement against
the protocol or to check an implementation.

| Path (under the install prefix) | What it is |
|---|---|
| `share/doc/naudio/copyright` | the full LGPL-2.1 text naudio is licensed under |
| `share/doc/naudio/THIRD_PARTY_NOTICES.md` | third-party components and their licences |
| `share/doc/naudio/audio-streaming-protocol-v1.md` | the normative `0xAF01` wire specification |
| `share/doc/naudio/protocols.md` | the wire format at a glance |
| `share/naudio/conformance/vectors/` | the language-neutral golden vectors |
| `share/naudio/conformance/README.md` | the vector file format, field by field |

**Writing a client in another language?** Those last two are the whole kit. The vectors are
a known-answer test — every expected byte, including the CRCs, is computed independently of
the implementation under test — so passing them is what makes a client conformant rather
than merely compatible with this one. §12 of the specification is the contract; the vector
`README.md` describes the INI format, which is deliberately parseable with any language's
standard library.

On a system-wide install the prefix is `/usr` (`.deb`/`.rpm`) or `/usr/local` (macOS `.pkg`),
so the spec is at `/usr/share/doc/naudio/audio-streaming-protocol-v1.md` and the vectors at
`/usr/share/naudio/conformance/vectors/`. From a tarball or the Windows `.zip` they sit under
the directory you unpacked.

Next: **[docs/getting-started.md](docs/getting-started.md)** — from an installed toolkit
to audio streaming end to end.
