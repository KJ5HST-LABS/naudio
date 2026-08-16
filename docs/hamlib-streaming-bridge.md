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

The bridge is written against Hamlib's `rig_stream_*` API, which arrived with
**[Hamlib PR #2116](https://github.com/Hamlib/Hamlib/pull/2116)** ("Add data streaming subsystem for
audio and I/Q"), **merged upstream on 2026-08-15**. It is in Hamlib `master` but **not in any
released Hamlib** — those are different things, and the distinction is why this page still exists. A
stock package is 4.x and has no streaming at all:

| libhamlib on your `PKG_CONFIG_PATH` | `rig_stream_*` | Result of `-DNAUDIO_BUILD_HAMLIB_BRIDGE=ON` |
|---|---|---|
| Homebrew / apt (4.x) | absent | CMake warning, **no binary**, build otherwise succeeds |
| Hamlib `master`, built from source (`5.0.0~git`) | present | `na_hamlib_bridge` is built |

`tools/CMakeLists.txt` gates the target on a `check_symbol_exists(rig_stream_open …)` rather than on
a version number, so a wrong libhamlib can never break the rest of the build — it just quietly
produces no bridge. That is deliberate, and it is also why the failure looks like a broken flag
instead of a missing dependency.

The API moved twice during review and can still move now that it is in `master`, so that presence
gate is not enough on its own: two further checks detect *which* revision you have and compile the
matching code, rather than assuming the newest shape. They also remain necessary because a prefix
built before the merge is still perfectly usable — the checks describe revisions, not a timeline.
Both are capability checks, never version comparisons.

| Check | Arrived in | What it selects |
|---|---|---|
| `rig_stream_get_conversions` | `961093f2` | whether the bridge can report the conversion stages libhamlib is running for it |
| `struct rig_stream_caps.channels` | `b538567b` | whether openable channel counts are an exact 0-terminated **list** or a `channels_min`/`channels_max` **range** |

The second is a struct-field change, so `check_symbol_exists` cannot see it — a prefix on either side
of that commit exports identical symbols, and the mismatch would surface only as a compile error.
`check_struct_has_member` is used instead.

> ⚠️ **Refreshing a prefix in place? Delete your build directory.** CMake **caches** `check_*`
> results and does not re-run them when the header underneath changes. An existing build tree
> therefore keeps compiling the code path selected for the *old* libhamlib against the *new*
> headers, and fails with something that reads like a naudio bug:
>
> ```
> na_hamlib_bridge.c: error: no member named 'channels_min' in 'struct rig_stream_caps'
> ```
>
> Measured, not hypothesised — this is exactly what happened on 2026-08-16 when the prefix was
> refreshed from the pre-merge fork commit to upstream `master` under an existing tree. The cached
> values were `NAUDIO_HAMLIB_HAS_STREAM_CHANNEL_LIST:INTERNAL=` and
> `NAUDIO_HAMLIB_HAS_STREAM_CONV:INTERNAL=` — both empty, both correct for the *previous* prefix.
> `rm -rf build` and reconfigure; the same probes then report `Success`.
>
> **Because the default ref is a moving branch, this is the normal consequence of re-running the
> build script, not an exceptional one.** Note also that a plain `cmake -S . -B build` after
> deleting the tree does **not** re-enable the bridge — `-DNAUDIO_BUILD_HAMLIB_BRIDGE=ON` lived in
> the cache you just deleted, so pass it again.

### Provenance

| | |
|---|---|
| Repository | `https://github.com/Hamlib/Hamlib.git` — upstream, since PR #2116 merged |
| Branch | `master` |
| Verified at | commits `2ef1e1d`, `961093f2` and `b538567b` on the pre-merge fork branch, and upstream `master` after the merge — the bridge builds and runs against all of them |
| Reports as | `pkg-config --modversion hamlib` → `5.0.0~git` |

naudio tracks upstream `master`'s head rather than pinning a commit. That is a decision, not an
oversight (issue #81): the streaming API is new, naudio follows where upstream takes it, and
noticing a move early is worth more than a build that cannot be surprised. The cost is accepted with
it — an upstream change can turn the `hamlib-bridge` CI job red without any naudio commit, and twice
now it has.

The countermeasure is not a pin, it is that nothing here assumes a shape: the capability checks
above compile whichever revision you actually have, so a moving head produces a *diagnosed* build
rather than a broken one. Pin with `--ref <commit>` if you need a reproducible build of your own.

**This page previously pointed at `mikaelnousiainen/Hamlib` @ `streaming-subsystem-pr`,** the PR
author's fork, and said that when #2116 merged the defaults should become upstream `master`. **That
happened on 2026-08-15 and the defaults were changed on 2026-08-16.** The reason for moving promptly
rather than waiting: a merged PR's branch is deletion-eligible, and it was naudio's only source for
this dependency — so the fork was a single point of failure for the `hamlib-bridge` job the moment
the merge landed. Repointing keeps the tracking property and removes that risk. The fork remains a
valid `--repo` if you need its history; nothing about the build requires it.

**Until a Hamlib *release* ships the streaming API, this script is still required.** Merged into
`master` is not packaged by a distro, so `apt install libhamlib-dev` will keep producing a 4.x
without `rig_stream_*` for some time yet.

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

It fetches upstream `master` (a depth-1 fetch, so it does not clone Hamlib's full history), bootstraps,
configures, builds, installs, and then verifies the result the same way naudio's build will. Budget
a few minutes and ~120 MB under the prefix — the whole run took 35 s on an 18-core Apple M5 Max, and
Hamlib's ~200 backends parallelise well, so expect that figure to scale with core count.

| Option | Default | |
|---|---|---|
| `--prefix DIR` | `$HOME/.local/hamlib-streaming` | Install prefix. **Must be durable** — see below. |
| `--src DIR` | `<prefix>/src` | Where the checkout lives. Re-running updates it in place. |
| `--ref REF` | `master` | Branch, tag, or commit. Use a commit to pin. |
| `--repo URL` | `https://github.com/Hamlib/Hamlib.git` | Override for a fork or mirror. |
| `--jobs N` | detected CPU count | Parallel `make` jobs. |

<details>
<summary>Manual equivalent, if you would rather not run the script</summary>

```bash
prefix="$HOME/.local/hamlib-streaming"

git clone --depth 1 --branch master \
    https://github.com/Hamlib/Hamlib.git "$prefix/src"
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

## Verifying it delivers audio (`naudio_bridge_arm`)

Once the bridge builds, `ctest` gains a `naudio_bridge_arm` test that drives it for real: it starts
the bridge on the hardware-free dummy backend, attaches a client, and checks what actually arrives.
Configure-time success looks like

```
-- naudio: naudio_bridge_arm enabled (na_hamlib_bridge present)
```

and where the bridge was not built the test is simply **not registered** — there is no skipped test
to mistake for coverage.

```bash
ctest --test-dir build -R naudio_bridge_arm --output-on-failure   # ~10 s, two arms
```

CI runs this arm and the fault arm below on `ubuntu-latest` (the `hamlib-bridge` job), building
libhamlib from source with the script above. That job asserts on the **binary** and on named
passing tests rather than on exit codes, because a skipped bridge and an empty test selection both
exit 0. It is the only place the bridge is compiled against glibc and gcc.

It runs two arms, and the second is the one that matters. `-S tone` must deliver **content** and
`-S silence` must deliver **silence** — because those two runs are indistinguishable on every
volume-shaped metric. Measured against the Hamlib dummy: 956160 vs 954240 bytes, 996 vs 994
callbacks, and the bridge's own meter reporting 83% of nominal for *both*. Peak |sample| is the only
thing that separates them, 16383 against 0, so a harness that gates on byte counts or rate passes a
completely silent bridge.

The probe behind it is `naudio_bridge_probe`, built unconditionally against the public C ABI alone —
it needs no libhamlib — so it can be pointed at anything speaking the naudio wire:

```bash
./build/naudio_bridge_probe --help
./build/naudio_bridge_probe --port 4533 --seconds 8 --expect content
```

Its exit status is a tri-state: `0` the expectation held, `1` it did not, `2` the probe itself could
not run — so a broken harness is never read as a clean negative result. `--selftest` proves the
detector separates content from silence using an in-process server, needs neither the bridge nor
libhamlib, and runs on every platform as `naudio_bridge_probe_selftest`. It also proves the probe's
`--tx` path: injected audio must reach the server's TX sink at full amplitude with PTT on, and must
be refused entirely with PTT off.

Absolute rate is reported but never asserted: a correct run against a dummy backend sits near 83% of
nominal because the backend paces itself off `nanosleep`, so any threshold tight enough to catch a
fault also fires on a healthy run. `--min-rate` exists for that comparison and is off by default.

---

## Verifying it survives failure (`naudio_bridge_fault_arm`)

`naudio_bridge_arm` covers the happy path and deliberately makes nothing go wrong. The second arm
covers the two bugs whose *fixes* shipped but whose verification never did, because both were
originally checked with a hand-built shim in a scratchpad that evaporated with it:

| Arm | Fault injected | The bridge must |
|---|---|---|
| `rx-fail` | `rig_stream_read` fails after 10 calls | **exit non-zero** and name the dead worker |
| `control` | *none* — shim loaded, no fault armed | stay up and report `short=0` |
| `tx-short` | every `rig_stream_write` truncated to half | **count** the short writes (`short > 0`) |
| `tx-fail` | `rig_stream_write` fails after 10 calls | **exit non-zero** and name the dead worker |

`rx-fail` and `tx-fail` are the two halves of the same bug, and one arm does not cover both: the two
workers reach `worker_failed()` from separate call sites, so deleting only `tx_thread`'s call leaves
`rx-fail` entirely green. `tx-fail` costs **0.9 s** of the total — it needs no health-line
interval, because process exit and the `tx worker stopped` line are both immediate.

```bash
ctest --test-dir build -R naudio_bridge_fault_arm --output-on-failure   # ~18 s, four arms
```

Neither fault is reachable from outside the process: the Hamlib dummy has no error-injection knob,
and it routes reads through its own `caps->stream_read` hook, so libhamlib's error and `closing`
paths cannot be provoked however the backend is configured. `tests/bridge/failshim.c` is therefore
preloaded into the bridge (`DYLD_INSERT_LIBRARIES` on macOS, `LD_PRELOAD` on ELF) and interposes the
two stream calls. It is armed entirely by environment variables — `NA_FAIL_RX_AFTER`,
`NA_FAIL_TX_AFTER`, `NA_FAIL_CODE`, `NA_FAIL_TX_SHORT` — and with none of them set it is a pure
pass-through, which is exactly what the `control` arm runs.

**The `control` arm is what makes the others mean anything.** A bridge that short-writes during
ordinary operation would satisfy `tx-short` with no fault injected at all. Same shim, same probe,
same TX load, fault switched off — so the fault is isolated as the cause.

**Every arm asserts the shim loaded, and that the calls it depends on were interposed.** Those are
two checks because they are two different failures with one symptom:

| Marker | Printed from | Proves |
|---|---|---|
| `na_failshim: armed …` | the constructor | the library **loaded** |
| `na_failshim: interposed <fn>` | inside `shim_read` / `shim_write` | that call **bound** |

The first is needed because a preload that fails to load produces exactly the output of a healthy
run — no error, no diagnostic — so "no short writes were reported" and "interposition never
happened" are otherwise the same observation, and the second one passes the `control` arm silently.

The first is also **not sufficient**, which is why the second exists. A shim can load, print `armed`
on every run, and still have both interposers as unreachable dead code — exactly what the ELF
visibility trap below did on the first Linux build of the bridge. `interposed <fn>` is emitted from
inside the interposed call, on the pass-through path as well as the failing one, so a shim that
never bound cannot print it however healthy the run looks. Which calls each arm requires is set by
which calls it actually makes, measured rather than assumed:

| Arm | Requires | Why |
|---|---|---|
| `rx-fail` | read | connects no client, so `tx_thread` never writes at all |
| `control` | read + write | `short=0` is satisfied by a shim that never bound — the case it exists for |
| `tx-short` | read + write | `short=0` is reported as issue #6 regressing; the same trap inverted |
| `tx-fail` | read | plus its forced-write line, which is strictly stronger for the write side |

Proved by removing both `DYLD_INTERPOSE` entries, so the shim loads and binds nothing: **all four
arms report a harness fault (exit 2)**, none blames the bridge. Against the harness as it stood
before this check, the same shim reported `rx-fail` and `tx-short` as **live bridge bugs** — issues
#5 and #6 regressing — while `control` **passed clean**. Removing only the write marker leaves
`rx-fail` and `tx-fail` green and fails `control` and `tx-short` alone, which is what shows the two
requirements are independent rather than one check counted twice.

Proved again on ELF, against the mutation that caused the original defect. In an `ubuntu:24.04`
amd64 container, reverting `naudio_failshim`'s `C_VISIBILITY_PRESET` to `hidden` leaves `nm -D`
reporting **no** exported interposers and `nm` two local `t` symbols — and all four arms report a
**harness fault**, with the `armed` marker present in every one of them. The unmutated build in the
same container passes all four, which is itself the evidence that the marker prints under
`LD_PRELOAD`: every arm asserts it, so none of them could have passed otherwise.

> **macOS SIP.** `DYLD_INSERT_LIBRARIES` is stripped the moment a SIP-protected binary is exec'd,
> which includes `/usr/bin/timeout`, `/bin/zsh` and `/usr/bin/perl`. Both driver scripts launch the
> bridge **directly** for this reason. Wrapping it in a timeout disables the interposition with no
> error at all, and the arms then fail as if the bridge were broken.

> **ELF symbol visibility.** `LD_PRELOAD` binds through the *dynamic* symbol table, so the shim's
> two definitions have to be exported. naudio compiles with hidden visibility globally
> (`CMAKE_C_VISIBILITY_PRESET`), which leaves them local — `nm` shows `t` rather than `T`, and
> `nm -D` shows nothing — and the preload then does nothing whatsoever. `naudio_failshim` sets
> `C_VISIBILITY_PRESET default` to opt out. This trap defeated the `armed` check: the shim still
> loads and still prints that line, so the load marker is satisfied while both interposers are dead
> code, and the arms failed as though issues #5 and #6 had regressed rather than as a harness fault.
> Measured on Ubuntu 24.04 / gcc 13.3 on the first Linux build of the bridge; macOS never showed it,
> because `__DATA,__interpose` binds the local definition directly and never consults the dynamic
> symbol table. The `interposed <fn>` marker above is what now catches it — this is the trap it was
> added for, and the arms report a harness fault instead of blaming the bridge.

Reaching the TX path needs a transmitting client, because `tx_thread` calls `rig_stream_write` only
while a TX owner exists — measured, not assumed: armed to fail the *very first* write with no
client connected, the bridge logged zero forced writes in 3 s and exited 0 on `SIGINT`. An idle
client therefore reaches neither `tx-short` nor `tx-fail`. That is what `--tx` is for:

```bash
./build/naudio_bridge_probe --port 4533 --seconds 6 --tx
```

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

### The sample rate is negotiated, not assumed (issue #84)

There is no rate flag, and that is deliberate — the bridge reads the rig's `native_sample_rates`
and picks from it. It keeps **48 kHz whenever the hardware offers it natively**, which is every
case the dummy backend produces, so on the hardware-free path nothing changed. Otherwise it takes
the **closest native rate**, ties to the higher, and tells naudio the same number:

```
na_hamlib_bridge: rate 48000 — native, from rx=[8000,16000,24000,48000,96000]
na_hamlib_bridge: rate 24000 instead of 48000 — 48000 is not native here, so this avoids a
                  resample. rx=[24000,96000] tx=[24000,96000]
```

**Why closest and not highest.** The wire cost of this hop scales linearly with the rate, and a
rig's demodulated audio is bandlimited to a few kHz either way — so taking the highest native rate
would spend the lossy hop's budget carrying content the radio never produced.

**Why it must be native on both directions.** naudio carries one audio format for the whole server,
so the rate chosen here is also the rate the operator's TX audio arrives in. Picking it from the RX
caps alone would open the TX stream at a rate the TX side may not be native at — moving the
resample instead of removing it, and invisibly, since nothing on the TX path reports a conversion.
Where the two directions share no native rate at all, RX wins: it is the mandatory stream and runs
continuously, while TX only carries audio while an operator is keyed.

**What it does not do.** It does not set `require_native = 1`. That flag is all-or-nothing across
*both* axes, and the format axis cannot move — naudio's ABI carries signed 16-bit PCM, so demanding
native format would fail outright against the dummy (which advertises `PCM_F32|OPUS`) and, on
hardware, would only relocate the F32→S16 quantization into the bridge. Negotiating the rate just
removes one stage from what the conversion line has to report: `conv=0x1` (format only) instead of
`conv=0x3` (format + resample).

Against a libhamlib older than PR [#2116](https://github.com/Hamlib/Hamlib/pull/2116) commit
`961093f2` the `native_*` caps fields do not exist, the negotiation compiles out, and the bridge
asks for 48 kHz exactly as it always did.

> **What has been measured, and what has not.** The *policy* is unit-tested on the whole CI matrix
> (`tests/test_stream_rate.cpp`, 10 arms) because it is a pure function over the rate lists. The
> *plumbing* — caps → both streams → `na_server_set_audio_format` → the health metric → a real
> naudio client — was measured against a synthesized non-48 kHz backend, with `conv` losing its
> resample bit and a client receiving clean content at 24 kHz. **No rig has run this.** The dummy
> backend advertises 48 kHz natively, so no hardware-free path exercises a negotiated rate; issue
> [#12](https://github.com/KJ5HST-LABS/naudio/issues/12) is still the open on-air check.

### Over a remote `rigctld` (`-m 2`)

Two commands, and both halves matter:

```bash
rigctld -m 1 -t 5599 -C stream_mode=tone           # streaming rigctld, dummy backend
./build/tools/na_hamlib_bridge -m 2 -r localhost:5599 -p 4533
```

Set the backend's `stream_mode` on **`rigctld`** with `-C`. The bridge's `-S` sets the conf on its
*local* netrigctl rig object, which is not where the dummy backend lives, so `-S` on an `-m 2` run
does not reach the thing generating audio. `rig_stream_net_parse_caps_line: missing required 'type'
field` on bridge startup is normal noise on this path, not a failure.

### What has been verified on this path

Measured with a headless naudio UDP client (`NA_CLIENT_BACKEND_NULL` + `na_client_set_audio_cb`,
transport forced to UDP) attached to the bridge's server, against a streaming `rigctld` on
localhost:

| Check | Result |
|---|---|
| RX reaches a real client | ~932 KB per 12 s window, `gaps=0`, `link_loss=0` |
| Delivery is lossless end to end | Client byte rate within **0.3%** of the bridge's own delivered-byte meter, across every arm |
| Cadence | Largest gap between audio callbacks **12.8 ms**; per-second delivery 81 ± 1 callbacks of 960 B |
| It is audio, not silence | Peak \|sample\| 16383, 99.9% of samples non-zero |
| TX direction | Opens and runs alongside RX; write budget negotiated at 1420 B/call |
| `SIGINT` shutdown | Exit status **0** in 0.56–1.44 s, both worker threads joined, UDP port released — including when a client is still attached |
| Leaks | `leaks(1)` reports 0 both on the live process under load and at teardown |

**A byte count is not proof of audio.** A source in `stream_mode=silence` delivers zeros at exactly
the right rate, with perfect parity and `gaps=0` — it is indistinguishable from a healthy run on
every counter the bridge prints. Any check of this path has to look at the samples themselves;
peak amplitude over a window is enough.

**This path now has an automated arm: `naudio_netrigctl_arm`** (`tests/bridge/netrigctl_arm.sh`,
issue [#88](https://github.com/KJ5HST-LABS/naudio/issues/88) item 8). It stands up `rigctld -m 1`,
runs the bridge over `-m 2` at both `-c 1` and `-c 2`, and fails on the gap-per-read signature
described under "Version floor" below. It is registered wherever `na_hamlib_bridge` is — so the
Linux `hamlib-bridge` CI job now drives `-m 2`, which nothing did before. The paragraph above is
exactly why the arm requires a naudio client to receive *non-silent* audio before it will believe
any gap counter: a silent source satisfies every other check on this page.

Not verified, and not claimed: real hardware, and a link with a non-1500 MTU. FEC recovery under
induced loss is covered below, on the `-m 1` path.

---

## FEC recovery under packet loss (`-R wan`)

`-R wan` selects `udpWan`: XOR forward error correction over blocks of 5 audio packets, plus a
reorder buffer and an adaptive jitter estimator. One parity packet per block recovers **at most one**
lost packet in that block. `-R lan` runs the same transport with FEC off.

This was measured by putting a UDP proxy between the bridge and a client and dropping a chosen
fraction of the server→client `AudioRx` datagrams — never control, heartbeat or parity. Drop
selection is by **audio-packet ordinal, not by sequence arithmetic**: a FEC block is 5 *audio
packets in send order*, while the sequence counter is shared with control and heartbeat packets and
the parity itself consumes one, so `sequence % 5` does not select one packet per block. Each parity
packet names its own block (`startSeq`, `blockSize` in its payload), so what was actually dropped
per block is reconciled against the real boundaries rather than assumed.

Bridge at `-m 1` (Hamlib dummy, `stream_mode=tone`), mono, 960-byte frames — about 100 audio
packets and 20 parity packets per second. Each arm is compared against **its own** no-loss control,
because absolute rate reflects the producer, not correctness (a loopback dummy paces off `nanosleep`
and sits near 80% of nominal while being entirely correct).

| Arm | Loss induced | Delivered vs its own control | Recovered by FEC |
|---|---|---|---|
| `-R wan` | 1 per block of 5 | **99.1%** (924480 / 933120 B) | **193 of 194** |
| `-R wan`, 30 s | 1 per block of 5 | **100.2%** (2427 / 2423 pkts) | **485 of 486** |
| `-R lan` | same pattern, 193 drops | **79.9%** (743040 / 930240 B) | 0 — FEC is off |
| `-R wan` | 2 per block of 5 | 49.7% | **0** — beyond what one parity can repair |

Peak \|sample\| stayed 16383 on every arm, so the recovered packets carry in-range tone audio rather
than XOR garbage that merely restores the byte count.

**The single unrecovered drop in each `-R wan` arm is the last one**, whose block never closed before
the run ended; 193 blocks carried exactly one drop and produced exactly 193 recoveries. A further
4 packets per run are still inside the reorder/FEC pipeline at cutoff — that residual measured
**4 on both a 12-second and a 30-second run** while recoveries scaled 193 → 485, so it is fixed
in-flight depth, not a per-packet leak.

The injector was validated in both directions before any of the above: with dropping disabled it
delivered a byte-identical result to running with no proxy at all (935040 B, 974 packets both ways),
and with every `AudioRx` dropped the client received nothing and the probe's silence check fired.

### A client built on the C ABI must select the profile too

`na_client_set_transport(c, NA_TRANSPORT_UDP)` sets the transport and **nothing else** — it leaves
FEC, reordering, adaptive jitter and control-ARQ off, so a client configured that way receives every
parity packet the bridge sends and discards it. Use **`na_client_set_reliability_profile`** instead;
it selects the transport as part of the profile, so no separate `na_client_set_transport` call is
needed:

```c
na_client_set_reliability_profile(c, NA_RELIABILITY_UDP_WAN);   /* match the bridge's -R wan */
```

Measured on the same bridge, the same injector and the same probe binary, 12 s per arm, each arm
against **its own** no-loss control:

| Client configuration | Delivered under 1-in-5 loss | Recovered |
|---|---|---|
| `na_client_set_transport(UDP)` alone | 648960 / 812160 B = **79.9%** | 0 — no decoder |
| `na_client_set_reliability_profile(UDP_WAN)` | 808320 / 808320 B = **100.0%** | **165 of 169** |

The four unrecovered packets are the fixed in-flight depth of the reorder/FEC pipeline at cutoff, not
a leak — the same residual of 4 that holds across both 12- and 30-second runs while recoveries scale
with duration.

The earlier arm table was taken with a client built directly on `naudio::net::UdpClientConnection`,
because at the time the C ABI could not enable the reliability layer at all. Nothing needs that
detour any more: **`na_client_get_stats`** reports the counters, so a single run can assert how many
packets FEC repaired instead of inferring it from delivered-byte parity against a separate control:

```c
na_client_stats st;
na_client_get_stats(c, &st, sizeof st);
printf("recovered %lld, declined %lld, reordered %lld\n",
       st.packets_recovered_by_fec, st.fec_blocks_unreconciled, st.packets_reordered);
```

Note that `packets_lost` / `packets_out_of_order` / `packet_loss_rate` report **-1, meaning "not
measured"**, on every profile: the sequence-gap tracker runs only when no reorder buffer is engaged,
and every UDP profile configures one. Read `packets_recovered_by_fec` to see loss being repaired; a
-1 there never means "nothing was lost".

**The recovered-audio caveat that used to sit here is fixed.** It reported one out-of-range sample per
12-second run and guessed the cause lay between the connection and the audio callback rather than in
the FEC decoder. Both halves were wrong. It was a **whole corrupted frame** — 480 of 480 samples,
of which only 3 happened to exceed the source's range, because XOR of two same-signal S16 payloads
usually lands back inside it — and it was in `FecDecoder::handleParity`, which read the parity
header's count as a contiguous sequence range. That range is the encoder's block only while the
block's audio packets are consecutive, and the sender's sequence counter is shared with control and
heartbeat traffic. The decoder now declines such a block and counts it as `fec_blocks_unreconciled`,
leaving the packet lost exactly as it would be with FEC off, rather than emitting
`lost ^ displaced` as recovered audio. See [#23](https://github.com/KJ5HST-LABS/naudio/issues/23) and
`docs/protocols.md` R5, which owns the decline contract — note in particular that a decline no longer
implies a lost packet, because the interleaving control message need never reach the decoder
([#55](https://github.com/KJ5HST-LABS/naudio/issues/55)).

Not verified, and not claimed: loss on the `-m 2` netrigctl path, real hardware, and loss patterns
other than the deterministic 1-in-N used here (no bursts, no reordering, no duplication).

---

## Version floor: `-c 2` over netrigctl (`-m 2`) needs libhamlib ≥ `961093f2`

**Stereo over a remote `rigctld` works — but only if *this bridge* is linked against a libhamlib at
or after Hamlib PR [#2116](https://github.com/Hamlib/Hamlib/pull/2116) commit `961093f2`
(2026-08-11).** Below that commit it is silently wrong, and `-c 1` is the workaround. naudio pins no
libhamlib version, so which behaviour you get is a property of the prefix you built against.

The dependency binds on the **bridge's own** libhamlib, not the peer's. That is the surprising half,
so it is the half that was measured rather than reasoned about:

| Bridge's libhamlib | `rigctld` peer | `-c 2` result |
|---|---|---|
| `9f412fe` (pre-fix) | `9f412fe` | **broken** — `gaps` 421 → 841 (~1 per read), 41% of nominal rate |
| `9f412fe` (pre-fix) | `2f076b5` (post-fix) | **`rig_stream_open` fails** — see the version-skew note below |
| `2f076b5` (post-fix) | `9f412fe` | **correct** — 0 gaps, 83% of nominal |
| `2f076b5` (post-fix) | `2f076b5` | **correct** — 0 gaps, 83% of nominal |

Measured 2026-08-13, one machine, same bridge source, `rigctld -m 1 -C stream_mode=tone`, `-x`, two
health ticks each; `-c 1` held 0 gaps on every combination that opened at all. Upgrading the remote
`rigctld` alone does nothing for this — an old `rigctld` still opens its rig **mono**, and a post-fix
bridge nevertheless delivers full, correctly-framed stereo from it (a naudio client read
**429112/430080 non-zero samples** off that run: populated stereo, not mono padded with silence).

**This table is now gated rather than merely recorded.** `naudio_netrigctl_arm` is the automated
form of its top and bottom rows, and it was driven RED against a `9f412fe` prefix before it was
trusted: `-c 2` failed at **836 gaps over 837 RX reads** (41% of nominal) while `-c 1` passed at
**0 gaps over 841** on that same old libhamlib — so the arm detects the channel-specific fault, not
"this libhamlib is too old for anything". Until it existed, an upstream revert would have falsified
this section, and the bridge's compile-time advice with it, in silence.

### What was wrong before `961093f2`

`\stream_open` carried only *type*, *format* and *sample rate* — there was **no channels field on
the wire** (all paths below are in **libhamlib**, not naudio):

| Step | What happened |
|---|---|
| Client asks | `netrigctl_stream_open` sent `\stream_open AUDIO_RX PCM_S16 48000` — the channel count was dropped (`rigs/dummy/netrigctl.c`) |
| Server opens | `rigctld_stream_config_from_args` hardcodes `channels = 1`, so the rig is opened **mono** (`tests/rigctld_stream.c` — still true at `2f076b5`) |
| Caps still say stereo | `\stream_caps` advertises `channels 1..2`, so the open succeeds and looks honoured |
| Truth is on the wire | The server stamps the real `channels` into every packet header … |
| … and is discarded | The client's receive path (`src/stream_net.c`) never compared that field against the stream it opened |

A `-c 2` bridge over `-m 2` therefore received **mono** bytes, handed them to a naudio server
configured for stereo, and naudio re-framed them as stereo — because the C ABI does not resample or
convert (`include/naudio.h`, `na_server_set_audio_format`). The result was mis-framed audio at every
client, not merely quieter or thinner audio.

`961093f2` added `channels=` to the `\stream_open` line (`+\stream_open <type> <format> <rate>
channels=<n> [require_native=1]`). Note the server-side hardcode above is *unchanged* — which is why
the floor is on the client side and why an old peer still works.

### The symptom, if you hit it

A gap counter climbing at roughly one gap per received read while `link_loss` stays `0`:

```
  rx: clients=1 gaps=418 reads=419 link_loss=0 overruns=0 underruns=0
  rx: audio 79160 B/s of 192000 expected (41%)
```

`reads` is the denominator, and it is on the line so the ratio is readable without arithmetic: 418
gaps over 419 reads is mis-framing; 418 over 40000 would be ordinary loss. (Re-measured 2026-08-16
against a `9f412fe` bridge and peer — the same run `naudio_netrigctl_arm` is driven RED by.)

That is not packet loss. The two ends disagree about how many bytes make a frame: the sender
advances the wire timestamp by its own frame size, the receiver expects `payload_len` divided by
*ours*, so every packet looks like a forward jump. The bridge detects exactly that combination and
prints a one-time explanation — naming which of the two cases applies, since it knows at compile
time which libhamlib it holds.

**A client sees nothing wrong.** Measured at `-c 2` over a pre-fix `-m 2`, a naudio UDP client
receives the bridge's bytes at 100% parity, with a steady cadence and a full-scale non-silent signal
— the delivery path is faultless and it is the *shape* of the audio that is wrong. So no client-side
byte, rate, or loss check can detect this; the bridge's gap signature is the only local symptom.

**Do not read the byte-rate line as the alarm.** It is reported because it is useful, but it
reflects how fast the producer runs as much as whether the framing is right — a dummy in
`stream_mode=loopback` with no TX peer paces itself off `nanosleep` and sits near 70% of nominal
while being completely correct at `-c 1`. The gap-per-read signature is the discriminator.

### Version skew is a separate, louder failure

A **pre-fix bridge against a post-fix `rigctld`** does not degrade — it does not start. The old
client cannot parse the new `\stream_caps` reply, so it reads no usable rate list and the open is
rejected against caps it never understood:

```
rig_stream_net_parse_caps_line: missing required 'type' field
validate_config_against_caps: sample rate 48000 not in supported list
na_hamlib_bridge: rig_stream_open(type=0, S16@48k/2ch): Invalid parameter
```

This is **not** channel-related — it reproduces identically at `-c 1`. If you see a rate-rejection
naming a rate the caps dump appears to offer, suspect skew between the bridge's libhamlib and the
`rigctld` it is talking to, and upgrade the bridge.

`-c 2` against a **local** backend (`-m 1`) is unaffected: no `\stream_open` round trip is involved,
and the channel count reaches the backend directly.
