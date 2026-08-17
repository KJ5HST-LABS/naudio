# On-air verification — what a real radio can and cannot verify

Everything in naudio's test suite is hardware-free by design. That is deliberate and it is also a
hole: no green suite is evidence that audio moves correctly to and from a radio. This document is
the procedure for closing as much of that hole as is closable today, and an honest statement of the
part that is not.

It exists for [issue #12](https://github.com/KJ5HST-LABS/naudio/issues/12).

---

## Read this first: the Hamlib streaming bridge cannot be pointed at a radio yet

`na_hamlib_bridge` sources its audio from Hamlib's audio-streaming subsystem (`rig_stream_open`,
`rig_stream_read`, `rig_stream_write`). **No real radio backend implements that subsystem.**

Measured against `Hamlib 5.0.0~git SHA=0839c03` (upstream master, 2026-08-15), by enumerating every
model the library knows and asking which advertise streaming capabilities:

```
1 of 313 models advertise stream_caps
  model 1  Hamlib  Dummy
```

Confirmed by a second, independent route — instantiating individual models and calling the public
accessor rather than reading the `rig_caps` pointer:

| model | | `rig_stream_caps_count()` |
|---|---|---|
| 1 | Hamlib Dummy | **4** |
| 2 | NET rigctl | 0 — relays whatever its remote offers, so it inherits this same limit |
| 3073 | Icom IC-7300 | **0** |
| 1035 | Yaesu FT-991 | **0** |
| 2037 | Kenwood TS-590SG | **0** |

A backend with no `stream_caps` cannot open a stream, so the bridge exits at startup rather than
running degraded. **Chaining through `rigctld` does not help:** model 2 has no streaming
capabilities of its own, it forwards the remote backend's — and the remote backend is a real rig
with none. The only end of any such chain that can stream is another Dummy.

This is an upstream gap, not a naudio defect. The condition that unblocks it is exactly one thing:
**a radio backend that authors a `stream_caps` table.** To re-check whether that has happened,
enumerate the installed library again:

```c
/* cc scan.c -I$PREFIX/include -L$PREFIX/lib -lhamlib -Wl,-rpath,$PREFIX/lib -o scan */
#include <hamlib/rig.h>
#include <stdio.h>
static int n = 0, s = 0;
static int cb(const struct rig_caps *c, rig_ptr_t d) {
    (void)d; n++;
    if (c->stream_caps) { s++; printf("  model %d %s %s\n", c->rig_model, c->mfg_name, c->model_name); }
    return 1;
}
int main(void) {
    rig_set_debug(RIG_DEBUG_NONE); rig_load_all_backends();
    rig_list_foreach(cb, NULL);
    printf("%d of %d models advertise stream_caps\n", s, n);
    return 0;
}
```

The day that prints anything other than `Dummy`, path B below becomes executable. Until then, the
bridge's real-backend behaviour — format negotiation against real caps, `-k` PTT keying around TX
bursts, a real short-write from a radio that will not take a full buffer — stays unverified, and
this project should say so rather than imply otherwise.

---

## Path A — the device layer, with a radio (executable today)

This is the path that a radio *can* verify right now, and it is the one that retires the
hardware-coverage risk that matters most in practice: naudio's PortAudio capture and playback code,
carrying **real demodulated audio** over a **real network hop**, for hours.

It uses `na_audio_daemon`, not the bridge. Audio comes from the radio's USB codec (or any sound
interface) instead of from Hamlib. CAT and PTT, if you want them checked, are exercised separately
with `rigctl` — PTT is not part of the streaming subsystem, so it works on real rigs today.

### What you need

- The radio connected by its USB-audio interface (the device that shows up as `USB Audio CODEC` or
  similar) — this is `--capture`'s default pattern, so most interfaces need no flag at all.
- A second machine for the client, on the far side of a real network hop. Loopback does not test
  what this is for.
- Optionally a CAT connection and the radio's Hamlib model number, if you want the PTT check.

### 1. Confirm the radio's audio device is visible and is the one you think

```bash
./build/tools/na_audio_daemon --list-devices
```

Take the radio's **capture** id from the listing. Do not skip this and rely on the name pattern:
a `--capture`/`--playback` pattern that matches nothing silently falls back to a hardware-free
fake backend, and **the run can still exit 0** — a misspelled device name reports a pass for a
check that never touched the radio. Use `--capture-id N` once you know the id.

### 2. Prove capture works at all, before any network is involved

```bash
./build/tools/na_audio_daemon --mode capture-probe --capture-id <N> --duration-ms 30000
```

This opens the radio's capture device directly and reports frames, overflow count and RMS. Tune the
radio to a signal, or to open squelch on noise. **RMS must be non-zero** — a silent capture here
means the wrong device, a muted input, or a radio not passing audio, and every later step would
inherit it.

### 3. RX over a real hop, to a remote client

On the radio machine:

```bash
./build/tools/na_audio_daemon --mode hardware --capture-id <N> --transport udp --port 4533
```

On the remote machine:

```bash
./build/examples/na_c_play_to_speakers --host <radio-host> --port 4533 --transport udp --seconds 0
```

Listen. **Intelligibility is a human judgement and there is no substitute for it** — tune to voice,
not to a carrier. Note whether it is intelligible, and note the delay.

### 4. TX and PTT (separate from the audio path, because Hamlib keeps them separate)

PTT does not depend on the streaming subsystem, so it can be checked on a real rig today:

```bash
rigctl -m <model> -r <serial-device> -s <baud> T 1   # key
rigctl -m <model> -r <serial-device> -s <baud> T 0   # unkey
```

Confirm the radio actually keys and unkeys. What this does **not** verify is the thing #12 asks
about — the interaction between a PTT event and TX audio arriving slightly before or after it,
which lives in `na_hamlib_bridge -k` and is blocked with path B.

### 5. The soak, and the drift verdict

Run for hours, capturing the health output with a timestamp on every line:

```bash
./build/tools/na_audio_daemon --mode hardware --capture-id <N> --duration-ms 0 2>&1 \
  | while IFS= read -r l; do printf '%s %s\n' "$(date +%s)" "$l"; done > soak.log
```

Then take the verdict:

```bash
tests/bridge/health_drift.sh soak.log
```

See the next section for what it decides and why.

---

## The drift analyser

`tests/bridge/health_drift.sh` answers #12's third acceptance item — *"a multi-hour run shows no
unbounded drift in `overruns` / `underruns`"* — which is the one item that cannot be settled by
looking at the log.

**Why a trend and not a total.** Every counter on the health line is cumulative. A rig that hiccups
four times while the audio device settles and then runs clean for six hours, and a rig whose sample
clock is genuinely off, can reach similar totals by opposite routes. Only the shape separates them,
so the test is:

> **sustained drift == the counter is still climbing in the final quarter of the run**

A startup burst flattens and passes. A persistent rate mismatch does not, because the mismatch is
what the counter counts. There is no tuned constant — the discriminator is zero-versus-nonzero tail
growth, not a threshold to re-tune per radio.

| exit | meaning |
|---|---|
| 0 | no sustained drift — every asserted counter flat in the final quarter |
| 1 | sustained drift — at least one still climbing; the report names which |
| 2 | harness fault — too few ticks for a tail quartile, or an unreadable log |

**Exit 2 is deliberate and matters.** A soak that died after ninety seconds must not report "no
drift found": that is an absence which was never a measurement. Fewer than `--min-ticks` (default 8)
health ticks is a fault, not a pass.

`overruns`, `underruns`, `rig_overruns` and `rig_underruns` are asserted. `gaps`, `link_loss`,
`reads` and the TX ring's `short`/`lost_queue`/`lost_requeue` are printed beside the verdict and
asserted on by nothing — each either has a sharper detector elsewhere or is an input to the on-air
test rather than a fault in it.

**Timestamp the capture.** With epoch-prefixed lines the elapsed time is read from the log and
labelled `(measured)`; without them it is inferred from the nominal 5 s cadence and labelled as
such — and an inferred clock cannot see a stalled producer, because it keeps counting. The verdict
itself is computed from sample order, so it survives a missing clock; only the per-hour figures need
one.

The analyser is gated by `naudio_health_drift` (`--selftest`), which runs on every POSIX build and
drives flat, sustained, timestamped and truncated logs through the real code path. That gate exists
because a detector that spends its life reporting "no drift" is indistinguishable from a broken one
unless something shows it can fire.

---

## Path B — the bridge, when a backend exists

Nothing here is executable until the enumeration at the top of this document reports a real radio.
Recorded now so the procedure is not reconstructed from scratch on the day.

Two topologies, and they are not equivalent:

**Direct** — the bridge opens the radio itself:

```bash
./build/tools/na_hamlib_bridge -m <model> -r <serial-device> -p 4533 -k
```

`-r` sets Hamlib's generic `rig_pathname` token, so this works for a serial rig and not only for
netrigctl. **But the bridge sets only `rig_pathname` and `stream_mode`** — there is no way to pass
`serial_speed`, `civaddr`, `ptt_type` or `ptt_pathname`. A radio that needs any of those cannot be
configured through the bridge, which is a real limitation to fix before this path is used in anger.

**Via `rigctld`** — all radio configuration lives in `rigctld`, which has flags for it:

```bash
rigctld -m <model> -r <serial-device> -s <baud> -t 4532
./build/tools/na_hamlib_bridge -m 2 -r 127.0.0.1:4532 -p 4533 -k
```

Use `127.0.0.1`, not `localhost` — on Linux `localhost` resolves to `::1` first, the UDP data plane
fails there while the TCP control plane survives, and the result looks like a subscribe timeout
rather than an address-family problem. This path also carries a version floor for `-c 2`; see
[`hamlib-streaming-bridge.md`](hamlib-streaming-bridge.md) § "Version floor".

The bridge already reports the negotiated format at startup, which is #12's fourth acceptance item
nearly for free — capture these lines verbatim:

```
na_hamlib_bridge: rate 48000 — native, from rx=[8000,16000,24000,48000,96000]
na_hamlib_bridge: stream type=0 S16@48000 is CONVERTED by libhamlib: sample-format (conv=0x1)
```

The first names the rate it chose and the set it chose from; the second says whether libhamlib is
converting rather than passing bytes through, and which stage. Against a real radio that is the
answer to *"what is the rig's actual format, and what does the bridge do if it is not S16@48k"*.

---

## Acceptance mapping for #12

| # | acceptance item | status |
|---|---|---|
| 1 | RX audio from a real rig reaches a remote client, intelligibly | **Path A** covers this for the device layer. Not coverable through the bridge until a backend exists |
| 2 | TX from a remote client is transmitted, `-k` keying the rig | **Blocked.** PTT alone is checkable with `rigctl`; the keying/TX-audio interaction is bridge-only |
| 3 | A multi-hour run shows no unbounded drift | **Instrumented and gated** — `health_drift.sh`, usable on either path |
| 4 | The rig's actual stream format is written down | **Blocked** for a real rig — the bridge prints it, but cannot open one |
