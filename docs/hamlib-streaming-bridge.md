# Building `na_hamlib_bridge` — the Hamlib streaming bridge

`na_hamlib_bridge` re-originates a Hamlib audio stream onto naudio's `0xAF01` wire, so the audio
crosses a lossy/WAN hop with naudio's FEC, adaptive jitter buffer and reorder — which Hamlib's own
transport (trusted-LAN only) does not provide. It runs **at the radio site**, upstream of the lossy
hop: Hamlib carries the reliable local hop (rig → bridge), naudio carries the internet hop
(bridge → operator).

It is **off by default** and needs a libhamlib that most systems do not have. This page is how to
get one.

---

## Quick start

```bash
tools/build-streaming-hamlib.sh                                    # one time

PKG_CONFIG_PATH="$HOME/.local/hamlib-streaming/lib/pkgconfig" \
  cmake -S . -B build -DNAUDIO_BUILD_HAMLIB_BRIDGE=ON
cmake --build build -j

./build/tools/na_hamlib_bridge -h
```

If `build/tools/na_hamlib_bridge` does not exist afterwards, the build **skipped** it — see
[Confirm you actually got the bridge](#confirm-you-actually-got-the-bridge).

---

## Why a special libhamlib

The bridge is written against Hamlib's `rig_stream_*` API, which arrives with
**[Hamlib PR #2116](https://github.com/Hamlib/Hamlib/pull/2116)** ("Add data streaming subsystem for
audio and I/Q") and is not in any released Hamlib. A stock package is 4.x and has no streaming at
all:

| libhamlib on your `PKG_CONFIG_PATH` | `rig_stream_*` | Result of `-DNAUDIO_BUILD_HAMLIB_BRIDGE=ON` |
|---|---|---|
| Homebrew / apt (4.x) | absent | CMake warning, **no binary**, build otherwise succeeds |
| PR #2116 branch (`5.0.0~git`) | present | `na_hamlib_bridge` is built |

`tools/CMakeLists.txt` gates the target on a `check_symbol_exists(rig_stream_open …)` rather than on
a version number, so a wrong libhamlib can never break the rest of the build — it just quietly
produces no bridge. That is deliberate, and it is also why the failure looks like a broken flag
instead of a missing dependency.

### Provenance

| | |
|---|---|
| Repository | `https://github.com/mikaelnousiainen/Hamlib.git` — the head repository of PR #2116 |
| Branch | `streaming-subsystem-pr` |
| Verified at | commit `2ef1e1d` ("Add data streaming subsystem for audio and I/Q") |
| Reports as | `pkg-config --modversion hamlib` → `5.0.0~git` |

PR #2116 is **open**, so the branch head moves as review continues. The script tracks the branch by
default, which is what you want for a review or CI build. Pin it with `--ref <commit>` when you need
a reproducible one — `2ef1e1d` is the commit naudio's bridge has actually been verified against.

---

## Prerequisites

Hamlib builds with autotools, so this needs the full autotools set, not just a compiler:

```bash
# macOS
brew install autoconf automake libtool pkg-config

# Debian / Ubuntu
sudo apt-get install autoconf automake libtool pkg-config build-essential
```

On macOS, Homebrew's `libtool` is what provides GNU `glibtoolize`; Apple's `/usr/bin/libtool` is a
different tool with the same name, and Hamlib's `./bootstrap` already knows to reach for
`glibtoolize` on Darwin. The script checks for either name before starting.

---

## 1. Build and install libhamlib

```bash
tools/build-streaming-hamlib.sh
```

It fetches the PR branch (a depth-1 fetch, so it does not clone Hamlib's full history), bootstraps,
configures, builds, installs, and then verifies the result the same way naudio's build will. Budget
a few minutes and ~120 MB under the prefix — the whole run took 35 s on an 18-core Apple M5 Max, and
Hamlib's ~200 backends parallelise well, so expect that figure to scale with core count.

| Option | Default | |
|---|---|---|
| `--prefix DIR` | `$HOME/.local/hamlib-streaming` | Install prefix. **Must be durable** — see below. |
| `--src DIR` | `<prefix>/src` | Where the checkout lives. Re-running updates it in place. |
| `--ref REF` | `streaming-subsystem-pr` | Branch, tag, or commit. Use a commit to pin. |
| `--repo URL` | PR #2116's head repository | Override for a fork or mirror. |
| `--jobs N` | detected CPU count | Parallel `make` jobs. |

<details>
<summary>Manual equivalent, if you would rather not run the script</summary>

```bash
prefix="$HOME/.local/hamlib-streaming"

git clone --depth 1 --branch streaming-subsystem-pr \
    https://github.com/mikaelnousiainen/Hamlib.git "$prefix/src"
cd "$prefix/src"

./bootstrap
./configure --prefix="$prefix" --without-cxx-binding --disable-static --disable-dependency-tracking
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
make install
```

Only `--prefix` matters. `--without-cxx-binding` and `--disable-static` drop artifacts the bridge
never links — it is pure C against the shared library — and `--disable-dependency-tracking` is a
one-shot-build speedup. Dropping all three still produces a working library, just more slowly.
</details>

---

## 2. Build naudio against it

`PKG_CONFIG_PATH` is how the prefix is found; the CMake flag alone is not enough.

```bash
PKG_CONFIG_PATH="$HOME/.local/hamlib-streaming/lib/pkgconfig" \
  cmake -S . -B build -DNAUDIO_BUILD_HAMLIB_BRIDGE=ON
cmake --build build -j
```

Both are **configure-time** settings. Adding them to a `build/` tree that was already configured
without them requires re-running the `cmake -S . -B build …` line above; `cmake --build` alone will
not pick them up.

### Changing the prefix later needs the cache invalidated

Once a build tree has resolved libhamlib, `pkg_check_modules` **caches the answer** — including the
absolute paths. Re-running the configure line with a different `PKG_CONFIG_PATH` does *not* re-detect:
it silently replays the cached prefix and still prints `na_hamlib_bridge enabled`, so it looks like it
worked. Drop the cached hamlib variables to force a real re-detect:

```bash
PKG_CONFIG_PATH="$HOME/.local/hamlib-streaming/lib/pkgconfig" \
  cmake -U 'HAMLIB*' -U 'pkgcfg_lib_HAMLIB*' -S . -B build -DNAUDIO_BUILD_HAMLIB_BRIDGE=ON
cmake --build build -j
```

The tell that detection really re-ran is `-- Checking for module 'hamlib'` in the output; without the
`-U` flags that line is absent. `cmake --fresh` or deleting `build/` also work, at the cost of
re-resolving every other dependency too. Verify the result against the binary rather than the log:

```bash
otool -L build/tools/na_hamlib_bridge | grep hamlib     # macOS
ldd    build/tools/na_hamlib_bridge | grep hamlib       # Linux
```

---

## Confirm you actually got the bridge

CMake *warns* when it skips the target — it does not fail — so a successful `cmake --build` is not
evidence the bridge exists. Check directly:

```bash
ls build/tools/na_hamlib_bridge          # the target itself
./build/tools/na_hamlib_bridge -h        # and that it runs
```

At configure time, success looks like this line:

```
-- naudio: na_hamlib_bridge enabled (hamlib 5.0.0~git)
```

and a skip looks like one of the `NAUDIO_BUILD_HAMLIB_BRIDGE: …; skipping na_hamlib_bridge`
warnings. To check the dependency on its own, without CMake in the way:

```bash
export PKG_CONFIG_PATH="$HOME/.local/hamlib-streaming/lib/pkgconfig"
pkg-config --modversion hamlib                          # -> 5.0.0~git

printf '#include <hamlib/rig.h>\nint main(void){void(*volatile s)(void)=(void(*)(void))rig_stream_open;(void)s;return 0;}\n' \
  | cc -x c - -o /tmp/na_stream_probe $(pkg-config --cflags --libs hamlib) && echo "streaming API present"
```

That probe is the same test `tools/CMakeLists.txt` applies: it fails to **compile** if the header
has no declaration, and to **link** if the library has no definition. `tools/build-streaming-hamlib.sh`
runs it for you at the end of a build.

---

## The prefix must be durable

`build/CMakeCache.txt` records an **absolute path** into the hamlib prefix
(`pkgcfg_lib_HAMLIB_hamlib`, `HAMLIB_INCLUDE_DIRS`, …). If that directory is moved or deleted, the
bridge cannot be rebuilt, and the failure surfaces later as a confusing link or include error rather
than as "your dependency is gone".

So do not install into `/tmp`, a scratch directory, or anything a reboot clears. This repository
spent its first few bridge sessions with its only streaming libhamlib sitting in a scratch directory,
one reboot away from being unable to rebuild or re-verify the bridge at all — which is why the script
defaults to `$HOME/.local/hamlib-streaming`.
If the prefix does go missing — or you simply want to point an existing build tree at a different
one — rebuild it and then follow
[§ Changing the prefix later](#changing-the-prefix-later-needs-the-cache-invalidated). Re-running the
plain configure line is **not** enough: the old absolute paths are cached, and the reconfigure will
quietly keep using them.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `cmake --build` succeeds, no `na_hamlib_bridge` | The target was skipped; the warning scrolled past | Re-read the configure output for `skipping na_hamlib_bridge`, then work down this table |
| `libhamlib <ver> lacks rig_stream_*` | `PKG_CONFIG_PATH` is resolving your system 4.x libhamlib | Point it at `<prefix>/lib/pkgconfig` **before** re-running the configure step |
| `libhamlib not found via pkg-config` | `PKG_CONFIG_PATH` unset, wrong, or lost between shells | It is not inherited by a later `cmake --build`; it must be set on the `cmake -S . -B build` line |
| Setting the flag changed nothing | Both settings are configure-time | Re-run the full `cmake -S . -B build …` line, not `cmake --build` |
| Changed `PKG_CONFIG_PATH`, reconfigured, still the old prefix | `pkg_check_modules` cached the paths; the reconfigure no-ops and still reports success | [Invalidate the cached hamlib vars](#changing-the-prefix-later-needs-the-cache-invalidated) with `-U 'HAMLIB*' -U 'pkgcfg_lib_HAMLIB*'` |
| `./bootstrap` fails on `libtoolize` | Apple's `/usr/bin/libtool` is not GNU libtool | `brew install libtool` (provides `glibtoolize`) |
| Link/include errors on a rebuild that used to work | The prefix was moved or cleared | Rebuild the prefix, then invalidate the cache as above |

---

## Running it

The bridge is hardware-free against Hamlib's dummy backend, which is what makes it testable without
a radio:

```bash
./build/tools/na_hamlib_bridge -m 1                  # dummy rig -> naudio server on :4533
./build/tools/na_hamlib_bridge -m 1 -S loopback      # dummy feeds TX back out of RX
```

`-S loopback` matters for any TX test: the dummy backend's default `stream_mode` is `tone`, so a
round-trip run without it shows RX bytes flowing from the tone generator and proves nothing about
your TX path. `-h` lists the rest (`-m 2` for netrigctl, `-p` port, `-c` channels, `-R` reliability
profile, `-k` PTT keying, `-x` RX-only).

---

## Known limit: `-c 2` does not work over netrigctl (`-m 2`)

**Use `-c 1` when the source is a remote `rigctld`.** Stereo is silently wrong on that path, and the
cause is upstream in Hamlib's streaming layer, not in naudio — there is nothing the bridge can do
to obtain stereo over netrigctl today.

The `\stream_open` command carries only *type*, *format* and *sample rate*. There is **no channels
field on the wire**, so (all paths below are in **libhamlib**, not naudio):

| Step | What happens |
|---|---|
| Client asks | `netrigctl_stream_open` sends `\stream_open AUDIO_RX PCM_S16 48000` — the channel count is dropped (`rigs/dummy/netrigctl.c`) |
| Server opens | `rigctld_stream_config_from_args` hardcodes `channels = 1`, so the rig is always opened **mono** (`tests/rigctld_stream.c`) |
| Caps still say stereo | `\stream_caps` advertises `channels 1..2`, so the open succeeds and looks honoured |
| Truth is on the wire | The server stamps the real `channels` into every packet header … |
| … and is discarded | The client's receive path (`src/stream_net.c`) never compares that field against the stream it opened |

So a `-c 2` bridge over `-m 2` receives **mono** bytes, hands them to a naudio server configured
for stereo, and naudio re-frames them as stereo — because the C ABI does not resample or convert
(`include/naudio.h`, `na_server_set_audio_format`). The result is mis-framed audio at every client,
not merely quieter or thinner audio.

The visible symptom is a gap counter climbing at roughly one gap per received read while
`link_loss` stays `0`:

```
  rx: clients=0 gaps=718 link_loss=0 overruns=0 underruns=0
  rx: audio 66473 B/s of 192000 expected (35%)
```

That is not packet loss. The two ends disagree about how many bytes make a frame: the sender
advances the wire timestamp by its own frame size, the receiver expects `payload_len` divided by
*ours*, so every packet looks like a forward jump. The bridge detects exactly that combination and
prints a one-time explanation pointing back here.

**Do not read the byte-rate line as the alarm.** It is reported because it is useful, but it
reflects how fast the producer runs as much as whether the framing is right — a dummy in
`stream_mode=loopback` with no TX peer paces itself off `nanosleep` and sits near 70% of nominal
while being completely correct at `-c 1`. The gap-per-read signature is the discriminator.

`-c 2` against a **local** backend (`-m 1`) is unaffected: no `\stream_open` round trip is involved,
and the channel count reaches the backend directly.
