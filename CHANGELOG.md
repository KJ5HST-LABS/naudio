# Changelog

All notable changes to this project are documented here.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Changed
- **A no-reliability UDP configuration is refused at `na_client_connect` / `na_server_start`**
  (server: UDP or DUAL). `na_client_set_transport(NA_TRANSPORT_UDP)` alone leaves FEC, reordering,
  adaptive jitter and control-ARQ all off — a composition that discards every parity packet, cost
  about two thirds of the audio on a real link, and is indistinguishable from health on loss-free
  loopback, where it is always first "verified". It now fails loudly — on loopback too — with both
  remedies in the error, before any network contact, and the handle recovers once the config is
  fixed. TCP is never gated; any configuration with a reliability component enabled is untouched;
  profile composition (last-writer-wins) is unchanged. **Migration: add one call** —
  `na_client_set_reliability_profile(c, NA_RELIABILITY_UDP_WAN)` (or another `NA_RELIABILITY_UDP_*`
  profile) on clients, `na_server_set_reliability_profile(...)` on servers — or request bare UDP
  explicitly with `NA_RELIABILITY_UDP_BARE`. This is the behavior change that makes the release
  **0.5.0**.

### Added
- **`NA_RELIABILITY_UDP_BARE`** (enum value 6, append-only; `AudioStreamConfig::udpBare()` on the
  C++ side) — UDP with no reliability layer, by explicit request: byte-for-byte the composition
  `set_transport(UDP)` alone used to produce, kept reachable so measurement-control baselines (a
  deliberately unprotected arm) stay reproducible under the new gate. An older library rejects
  value 6 with `NA_ERR_INVALID` per the enum's documented append discipline; gate on
  `na_version_number()`.

- **`na_wav_tap`** — records what a naudio *client* receives, as a 12 kHz mono WAV that the WSJT-X
  decoders read. It is a normal client driving only the public C ABI
  (`na_client_create` / `na_client_set_audio_cb` / `na_client_connect`) on
  `NA_CLIENT_BACKEND_NULL`, so no audio device is opened and the file holds what naudio *delivered*
  rather than what a sound card then did to it. This makes "the audio is intelligible" a decode
  instead of an opinion: measured against a Yaesu on 14.074 MHz, a remote Windows client across a
  WiFi hop produced **21 FT8 decodes**, matching a local client on the adjacent period. It asserts
  the wire format rather than assuming it, reports the client's own counters
  (`packets_recovered_by_fec`, `socket_rx_drops`, `jitter_ms`, `buffer_target_ms`) and can run over
  TCP (`--tcp`) as a discriminating check when UDP comes up short. POSIX and **Windows**;
  see `docs/on-air-verification.md`.

- **`na_audio_source` gains `--reliability lan|wan`.** The example server previously called only
  `na_server_set_transport`, so no FEC parity ever reached the wire and a client asking for
  `NA_RELIABILITY_UDP_WAN` recovered nothing — both ends must select a profile for the reliability
  layer to exist. The default is unchanged (bare transport), so existing behaviour is untouched.

- **Every example client gains `--reliability lan|wan|ft8`.** All five (C, C++, Python, Rust,
  Java) previously called only `na_client_set_transport`, so a copy of any of them ran UDP with
  FEC, reordering, adaptive jitter and control-ARQ all off and discarded every parity packet the
  server sent — the composition that cost two thirds of the audio on the first real network hop
  while remaining invisible on loopback. The flag selects the matching
  `na_client_set_reliability_profile` preset; `--transport` stays for TCP, and the bare default is
  unchanged. Each client's connected/summary lines now report the effective transport (`udp+wan`)
  rather than the `--transport` flag's value, which was wrong whenever a profile was given. The
  example READMEs' UDP guidance now leads with a profile on both ends instead of bare
  `--transport udp`.

- **`docs/on-air-verification.md`** — what a real radio can and cannot verify, and the procedure for
  the part it can, including the FT8 decode check and its mandatory silence control.

### Changed
- **`na_hamlib_bridge`'s periodic RX health line now carries `reads=`, the denominator of the gap
  signature.** It reads
  `rx: clients=1 gaps=418 reads=419 link_loss=0 overruns=0 underruns=0`, where it previously
  omitted the read count. A bare `gaps=418` is not interpretable: 418 gaps over 419 reads is the
  two ends disagreeing about how many bytes make a frame, while 418 over 40000 is ordinary packet
  loss. The bridge already printed that ratio inside its one-time mis-framing warning; putting it
  on every tick lets an operator — or an automated arm — apply the same test without waiting for
  the warning to fire, and without trusting the warning to be the only thing that can see the
  fault. Both counters are cumulative since the stream opened, so the ratio is the one the bridge's
  own alarm tests.

- **`tools/build-streaming-hamlib.sh` now builds Hamlib from upstream `Hamlib/Hamlib` @ `master`,
  not from PR #2116's fork branch.** The streaming subsystem
  ([PR #2116](https://github.com/Hamlib/Hamlib/pull/2116)) **merged upstream on 2026-08-15**, so
  `rig_stream_*` now lives in Hamlib proper. The old defaults pointed at
  `mikaelnousiainen/Hamlib` @ `streaming-subsystem-pr` — a merged PR's branch, which is
  deletion-eligible and was naudio's only source for the dependency. Repointing keeps the
  deliberate tracking property (naudio follows where the streaming API goes, and a red
  `hamlib-bridge` CI job is the early notice that it moved) while removing that single point of
  failure. Pass `--repo`/`--ref` to override; the fork remains valid if you need its history.

  **This does not mean a packaged libhamlib will work.** Merged into `master` is not shipped in a
  release — `apt`/Homebrew still deliver 4.x with no `rig_stream_*`, so this script stays required
  until a Hamlib release carries the API.

  Verified against upstream `master` (`0839c03`) rather than argued: the script builds and installs
  it, `check_struct_has_member(struct rig_stream_caps, channels)` reports **Success** (the exact
  probe that discriminates the post-`b538567b` channel-list shape), `na_hamlib_bridge` is produced
  as a **binary**, and all three bridge arms — `naudio_bridge_probe_selftest`, `naudio_bridge_arm`,
  `naudio_bridge_fault_arm` — pass against it.

- **The script and `docs/hamlib-streaming-bridge.md` now warn that refreshing a prefix in place
  requires deleting the build directory.** CMake caches `check_*` results and does not re-run them
  when the header underneath changes, so an existing tree keeps compiling the code path chosen for
  the *previous* libhamlib and fails with `no member named 'channels_min' in
  'struct rig_stream_caps'` — which reads like a naudio bug and is not. Now that the default ref is
  a moving branch, this is the normal consequence of re-running the script rather than an
  exceptional one. Documented from a measured occurrence, not a hypothesis.

## [0.4.0] — 2026-08-16

**The first tagged release, and the point at which the binary-compatibility promise in
`README.md` § "Binary compatibility" starts binding.** Everything below shipped untagged before
this date; the tag is what makes it something a consumer can pin and a packager can rely on.
`SOVERSION` is `0` and tracks the major version, so every `0.x` release shares one soname — the
declared-size (`struct_size`) mechanism, not a soname bump, is what keeps an appended field from
breaking an already-compiled consumer.

### Added
- **`na_client_stats.fec_pending_discarded` (`@since 0.4.0`) — audio packets dropped from a pending
  FEC block without ever having been offered to a parity: repair capacity that expired.** It is the
  missing half of the answer to *"why is `packets_recovered_by_fec` zero while every other counter
  looks healthy?"* — the question that took two sessions to resolve on this project before the
  counter existed at all. A non-zero reading says the decoder was holding block members it never got
  to weigh, so the opportunity was lost upstream of any decision it made. A **sibling** of
  `fec_blocks_unreconciled`, not a residue of `packets_recovered_by_fec`: that counts blocks the
  decoder *declined*, this counts material it never got to decline on.

  **It is not audio loss**, and the name invites exactly that misreading. The decoder emits every
  audio packet to the application **on arrival** and only then caches a copy for FEC, so the pending
  block is a repair cache and this counts copies leaving it — the audio itself was already
  delivered, in order. Read as a loss meter it double-counts against `sequence_gaps`. It also does
  not imply a decline: only the retention-cap mechanism also moves `fec_blocks_unreconciled`, while
  the idle timeout moves this alone. `-1` where no FEC decoder exists (every profile but
  `NA_RELIABILITY_UDP_WAN`, and TCP).

  Observed moving through the public C ABI **before** this contract was written: a relay dropping
  every parity leaves no block able to complete, giving **409 discards against 0** on the identical
  lossless path, with 455 audio packets still delivered. The paired arms are in
  `tests/c_client_stats.c` — asserted as a comparison rather than as `> 0`, because the counter can
  move on a clean run too when the trailing partial block idles out. Version accordingly moves
  0.3.0 → 0.4.0, and `NA_CLIENT_STATS_SIZE_V4` joins V1/V2/V3 rather than V3 being redefined.

- **`ClientConnection::fecPendingPacketsDiscarded()` and `ClientStats::fecPendingDiscarded`** — the
  C++ path behind that field. `FecDecoder` has counted the event since the retention-policy work;
  nothing carried it up to a consumer.

### Changed
- **`CONNECT_REQUEST` is now an ARQ-protected control message, so a client's handshake survives a
  lost datagram** (spec 1.1, §8.4). It was the only message in the connection-establishment
  exchange that was *not* retransmitted — `CONNECT_ACCEPT`, `CONNECT_REJECT` and `AUDIO_CONFIG` all
  were — and the asymmetry ran the wrong way: a lost **server** reply recovered in one 500 ms
  retransmit interval, while a lost **client** request could not be recovered at all and cost the
  caller the entire 10 s connect timeout followed by a failed `connect()`. Opening a session over a
  lossy link is exactly what the ARQ layer exists for. Measured end to end with one request dropped:
  connect now succeeds in **506–508 ms** (one retransmit interval), against a 10.5 s failure before.

  **Marking the type critical was necessary but not sufficient**, and the second half is the part
  worth knowing about: `recordSent` only makes an entry *pending*, and the retransmit sweep's only
  pump was the heartbeat loop — which does not start until after the handshake has already
  returned. The handshake also spent its whole wait parked in a single blocking receive, so nothing
  could have run a sweep even if one had been scheduled. `AudioStreamClient::performHandshake` now
  waits in slices and runs the sweep between them, via a new `ClientConnection::pumpControlRetransmits`
  (defaulted to a no-op — TCP is a reliable ordered stream and has no control-ARQ). Without that,
  the enum change would have been inert in precisely the way `DISCONNECT` still is.

  **Wire compatibility: this is a §11 minor, backward-compatible extension, not a version-byte
  change.** No frame layout, type number, field offset or CRC semantic moves, and the golden vectors
  are untouched. Mixed-version behaviour is bounded and cannot strand a connection: a new sender
  against an old receiver gets no `CONTROL_ACK`, spends its 3-attempt budget, and carries on — at
  most two extra datagrams, delivered to an already-established session where an unexpected
  `CONNECT_REQUEST` hits the control switch's `default:` and is ignored; an old sender against a new
  receiver gets an ACK it is not tracking, which `onAckReceived` discards with no side effect. Both
  paths were read and verified rather than assumed. `CONTROL_ACK` and `NACK` remain non-critical, so
  nothing acknowledges an acknowledgement.

### Fixed
- **Control-message retransmission is no longer disrupted by a system clock step.**
  `ControlReliability` was the one reliability component timed by the wall clock, while
  `FecDecoder`, `PacketReorderBuffer` and `JitterEstimator` all use the monotonic clock. An NTP
  step **backward** of T suppressed every pending critical-control retransmit for T; a step
  **forward** timed them all out at once, burning retransmit attempts toward the silent
  exhaustion-drop. The stamps are only ever compared against each other and never reported or put
  on the wire, so the monotonic clock is strictly correct here. Behaviour is unchanged on a machine
  whose clock does not step.

- **A NACK-driven control retransmit now spends an attempt and restarts the retransmit timer.**
  `ControlReliability::onNackReceived` incremented only the retransmit counter, so a NACK-driven
  resend was outside the `maxAttempts` budget the class documents and left `lastSendTime` at the
  original send — meaning the timeout sweep could resend the same packet again moments later. A
  packet already at `maxAttempts` is now dropped and `nullopt` returned, the same disposal
  `checkRetransmitsAt` gives an exhausted entry. naudio never sends a NACK, but it does handle a
  received one, so the budget was a peer's to spend without limit.

- **The reorder buffer's hold timeout is measured from the oldest arrival, not the smallest
  sequence.** `PacketReorderBuffer::checkTimeoutAt` read `arrivalTimes_.begin()`, and that map is
  keyed by *sequence* — so `begin()` is the lowest sequence number, whose arrival time says nothing
  about which packet has waited longest. Under decreasing-sequence arrivals every newly buffered
  packet became the new `begin()` and restarted the hold of everything already buffered, so
  `maxHoldMs` was not the maximum time a packet could be held: the real bound was
  `(reorderBufferSize - 1) x reorderMaxHoldMs`, which is 280 ms on the `udpWan` preset against a
  documented 40 ms. The size-triggered flush at `reorderBufferSize` was what ended the chain, not
  the timeout. Head-of-line latency only; no packet was lost or misordered.

- **Re-recording an already-pending control sequence no longer evicts an unrelated one.**
  `ControlReliability::recordSentAt` freed a slot before checking whether the sequence was already
  pending. A re-record replaces in place and adds no entry, so at capacity the eviction destroyed
  the oldest pending control — the one with the fewest retransmits left — to make room for a put
  that never happened, and left the buffer one entry short of capacity. Latent for naudio's own
  callers, none of which re-records a sequence; reachable by any consumer of the installed
  `naudio/ControlReliability.hpp`.

### Added
- **`NA_RELIABILITY_UDP_IQ` and `NA_RELIABILITY_DUAL` (`@since 0.3.0`) — the two
  `AudioStreamConfig` presets a C consumer could not reach.** `na_reliability_profile` reached three
  of the eight C++ preset factories (`udpLan`, `udpWan`, `udpFt8`) plus a default-constructed reset;
  it now reaches five. The two added were capabilities rather than conveniences: SDR
  **IQ streaming** (UDP, 10 ms framing, 60/30/200 ms buffers) was **not configurable from C at all**
  — not even reconstructible, since the preset's buffer targets and framing have no individual C
  setter — and **dual transport** was reachable only as a bare `na_server_set_transport` call that
  left the buffer and reorder settings the preset pairs with it unapplied. Both are wired into the
  client and server setters alike. Values are append-only and 0–3 are unchanged, so this is a
  source- and binary-compatible addition; an older library rejects a newer value with
  `NA_ERR_INVALID` rather than misapplying it.

  Two contract details are documented on the enum and on the setters, because neither is guessable:
  **`NA_RELIABILITY_UDP_IQ` does not apply its own defining 192 kHz.** Sample rate belongs to
  `na_server_set_audio_format`, and the server's profile setter preserves the audio-format fields so
  the two setters compose in either order — so IQ streaming is the one profile that needs both calls
  (`na_server_set_audio_format(s, 192000, 16, ch)` alongside the profile). Dropping `sampleRate`
  from that preserve list would have bought convenience in one profile at the cost of
  order-independence in all of them. **`NA_RELIABILITY_DUAL` is a server-side capability:** a client
  picks exactly one transport, so the value aliases to TCP on the client exactly as
  `NA_TRANSPORT_DUAL` does, leaving its reorder settings inert there.

  Coverage, with its limits stated rather than implied: `tests/c_server_smoke.c` gains ordering arms
  E and F — E is the suite's only *(connect, connect)* expectation, since serving both transports on
  one port is a claim no other profile can satisfy — and `tests/c_client_profile.c` gains section
  (7), which pins UDP_IQ's preset by stats fingerprint (reorder engaged, adaptive jitter off) and
  the client-side DUAL alias. What is **not** covered: IQ's 192 kHz is pinned nowhere, because the
  negotiated sample rate has no observable through the public C ABI — issue #36 declined adding an
  accessor, and both candidate detectors were re-measured and are dead (total byte volume, and the
  RX callback's chunk size, which is MTU-derived at 1376 bytes on UDP whatever the rate).

- **`Socket::lastSendStop()` (C++ API) — why the last `sendAll()` on this thread stopped, rather
  than only whether it succeeded.** `sendAll` returns a bare `bool`, and `false` covers three
  materially different events: the whole-call send budget was spent (issue #70's guarantee), a
  `::send` failed or timed out with no room freed, or the socket was already closed. A caller that
  needs to distinguish "the budget ended a call that was making progress" from "the peer went
  quiet" previously could not, and neither could the test suite — which is what motivated this.
  **On Windows that distinction is still unavailable, and the accessor says so rather than
  guessing:** the budget is consulted only after a partial write, and Winsock reports no byte
  count on a timed-out blocking send, so a wedged call returns `SendFailed` having made progress
  it could not report. The call remains bounded by one deadline — issue #70's guarantee holds on
  every platform — but `Budget` is effectively POSIX-only, which the header now states.
  Static and thread-local rather than a member, so threads sharing one `Socket` (the writer bridge
  does) each get their own answer and `sizeof(Socket)` is unchanged. Purely additive: `sendAll`'s
  signature and behaviour are untouched, and the one in-tree call site is unaffected.

- **`na_client_stats.socket_rx_drops` (`@since 0.3.0`) — the first counter for audio lost
  *locally*, in the kernel's socket receive buffer, before naudio ever sees it.** This was the
  one loss mode no field reported: a full receive buffer tail-drops, discarding the *newest*
  arrivals, so a consumer that cannot keep up reads an unbroken prefix and simply stops early.
  Nothing it read has a hole in it, so `sequence_gaps` correctly reports nothing (measured: 127
  of 400 packets read with `sequence_gaps == 0`), and `queue_drops` cannot move on a client at
  all. Sourced from Linux `SO_RXQ_OVFL`; **`-1` on macOS and Windows**, which expose no
  per-socket equivalent — the struct's established encoding for "not measuring". A portable
  derived estimate was rejected: it would conflate local loss with wire loss and freeze a name
  claiming more than the mechanism observes. It **lags by a whole receive buffer** — the kernel
  stamps its discard total on each datagram as it *queues* it, so a 0 means "nothing reported
  yet", never "nothing was lost". Version accordingly moves 0.2.0 → 0.3.0, and
  `NA_CLIENT_STATS_SIZE_V3` joins V1/V2 rather than V2 being redefined.
- `Socket::enableReceiveDropCounter()` / `Socket::receiveDrops()`, and
  `ClientConnection::socketReceiveDrops()` — the C++ path behind that field.
- `ControlReliability::onNackReceivedAt(seq, nowMs)` — the explicit-time form of
  `onNackReceived`, matching the existing `recordSentAt` / `checkRetransmitsAt` pair.

### Fixed
- **Two distinct devices that share a name are no longer merged into one.** Device identity was
  `name + hostApi`, so two identical rigs on one host API — e.g. a pair of "USB Audio CODEC"
  interfaces, which is the shape naudio's own default capture pattern looks for — collapsed into a
  single record. The second device's backend ids were dropped and it appeared nowhere in the
  enumeration, so it could not be selected even by explicit device id.

  The merge exists for one real backend shape: an ALSA-style **split** record, where the same
  physical device is reported once capture-only and once playback-only. Those two records'
  directions are disjoint by construction, so the merge is now restricted to
  **complementary-direction** records; two records that both claim the same direction stay separate
  devices with their own ids. A genuine split pair can never overlap, which is what makes the
  overlap a safe discriminator.

- **`na_audio_daemon` rejects a non-numeric `--capture-id` / `--playback-id` instead of opening
  device 0.** The numeric options went through `std::atoi`, which returns 0 for a non-numeric string
  and gives the caller no way to tell that from a genuine `0` — and 0 is a valid PortAudio device
  index. So `--capture-id usb` opened device 0 and the RMS gate could pass on whatever that device
  happened to hear, reporting a green on-air check against the wrong radio; `--capture-id 3x` was
  silently accepted as 3. All four numeric options (`--capture-id`, `--playback-id`, `--port`,
  `--duration-ms`) are now parsed strictly — the whole token must be consumed — and range-checked,
  with a distinct message for each failure.

  Its `--help` text now also warns that an explicitly requested `--playback` pattern matching
  nothing falls back to the hardware-free drain and can still exit 0, and the run itself names the
  pattern that missed. That path is unchanged behaviour; it was simply invisible, and a misspelled
  sink name produced a pass for a check that never fed the digital-mode app.

- **Unloading a virtual sink no longer destroys other sinks whose names merely contain its own.**
  `unloadSinkPipeline` selected modules with a bare `grep <name>` and piped the result straight
  into `xargs pactl unload-module`, so tearing down a sink named `naudio` also unloaded `naudio_2`
  and `naudio-backup` — operator sinks naudio never created. `checkSinkCommand` had the matching
  read-side fault: it reported a sink as existing whenever its name appeared as a substring of any
  other sink's name, or of an unrelated line's driver or sample-spec field.

  The `sink_name=<name>` token is now matched exactly, delimited by whitespace or the line edges,
  and the existence check compares the whole name field (`cut -f2 | grep -Fx`). `grep -w` is not
  sufficient here: sink names may contain `-`, which is not a word constituent, so `sink_name=my-sink`
  would still have matched `sink_name=my-sink-2`. SAFE_SHELL_ARG guarantees the name cannot inject
  shell syntax; it never guaranteed the name was unique.

- **The guide's printed Linux commands apply the same SAFE_SHELL_ARG gate as the executed path.**
  `linuxConfigurationCommands()` and the Linux sample-rate suggestions interpolated the configured
  sink name and description into `pactl` command text with no validation, while
  `PlatformConfigurator` validated both before running anything. The executed path was never
  exposed, but the printed one was inviting an operator to paste an unvalidated command. Both now
  refuse, with a note naming the offending value, so the documented and executed paths agree on
  what is rejected.

- **A device whose exact-format probe failed is no longer reported ready.** `verifyConfiguration`
  decided `success = formatSupported || issues.empty()`, so a failed probe could still pass on the
  second clause. When the probe fails the "detected" rate and channel count fall back to the
  device's reported defaults, and both mismatch messages are only emitted when detected differs
  from required — so a device that happens to default to exactly the required rate *and* channel
  count produced no issues at all, and the empty list was read as success. The verification printed
  "Ready: YES" for a device `StreamOpener` then refused to open, which is the worst possible answer
  for setting up a station.

  The probe is now a requirement rather than one of two ways to pass: `formatSupported && no
  issues`. The two `&& !formatSupported` clauses on the mismatch branches were dead — the detected
  values are copied from the required ones whenever the format is supported, so those branches
  cannot be reached in that case — and re-adding one kills no test, which is how the deadness was
  confirmed rather than argued. They are gone, with the invariant written down in their place.

- **`na_audio_daemon` opens each direction on that direction's own backend id.** The daemon passed
  the merged record's `backendId` — the first-seen record's id — to both the server's capture
  device and the client's playback device, bypassing `backendIdFor()`. On a split capture-only +
  playback-only pair the per-direction ids are exactly where the two directions differ, so the
  daemon opened the wrong record and failed on a correctly configured system. `StreamOpener` and
  the C ABI already resolved per direction; the daemon — the tool used for real-hardware
  verification — was the outlier. Its progress lines now also print the id actually opened rather
  than the merged one.

- **An FEC-recovered packet is delivered in its own sequence position instead of after the rest of
  its block.** A repair can only be built when the parity arrives, and parity is computed over the
  block, so it necessarily arrives after every member that follows the loss. The decoder delivered
  each packet the instant it arrived, so the repair was always handed over last: a block
  `100..104` losing `102` was delivered `100, 101, 103, 104, 102`, on both the RX and TX lanes.
  Measured across every loss position, the repair landed last in all of them — displaced by four
  frame positions when the block's *first* slot was lost, and in order only when the loss was the
  block's own last slot.

  **No buffer setting masked this.** The reorder buffer sits ahead of the decoder, the "jitter"
  stage is an estimator that only timestamps arrivals, and everything after the decoder is a FIFO
  queue feeding a ring appended in call order — so every stage able to re-sequence sat upstream of
  the one stage that could disorder. Depth bought delay, not order.

  The decoder now **holds the audio behind an observed sequence gap** until the block resolves
  (its parity, the block timeout, or a packet cap, whichever comes first) and then delivers it in
  sequence order. The hold is conditional, which is the whole point: with no loss nothing is held
  and the added latency is exactly zero. A general ordering stage after the decoder would instead
  have paid block latency on every packet whether or not anything was lost.

  **Latency after a loss is bounded** by the block remainder plus its parity — at `udpWan`, the
  only preset that enables FEC, at most ~40–50 ms against that profile's 120 ms buffer target.
  Provenance is unchanged: the repair still arrives marked recovered, with the same bytes and the
  same audio type. No wire change and no C ABI change — the spec already classed recovery delivery
  as a local delivery rule rather than a wire rule.

### Changed
- **`na_hamlib_bridge` negotiates its sample rate from the rig's `native_sample_rates` instead of
  demanding 48 kHz.** The bridge asked libhamlib for 48 kHz and told naudio 48 kHz on every rig, and
  never consulted `native_sample_rates` at all — it was read in exactly one place, the capability
  dump on the *failed*-open path. On a rig whose native rate is not 48 kHz that bought a
  libsamplerate resample the bridge had the information to avoid, inside the one hop this tool
  exists to keep short.

  It now keeps **48 kHz whenever the hardware offers it natively** — every case the dummy backend
  produces, so the hardware-free path is byte-for-byte unchanged — and otherwise takes the
  **closest native rate**, ties to the higher. Closest rather than highest is a bandwidth
  judgement: the wire cost scales with the rate while a rig's demodulated audio is bandlimited
  either way, so the highest native rate would spend the lossy hop's budget on content the radio
  never produced.

  **The rate must be native on both directions.** naudio carries one server-wide audio format, so
  the rate chosen is also the rate the operator's TX audio arrives in; choosing from the RX caps
  alone would open TX at a rate it may not be native at, moving the resample rather than removing
  it — and silently, since nothing on the TX path reports a conversion. Where the two directions
  share no native rate, RX wins: it is mandatory and continuous, TX only carries audio while an
  operator is keyed.

  **Operator-visible output changed.** The startup line gains `rate=`, a new line names the chosen
  rate and the list it came from, and the conversion line reports the real rate — it previously
  printed a hardcoded `S16@48k`, which was true only while the rate was hardcoded too. The
  `% of nominal` RX health figure now derives its denominator from the negotiated rate; leaving it
  at 48 kHz would have made a correct 24 kHz stream read as a 43% shortfall.

  **The format axis is unchanged and `require_native` stays 0.** naudio's ABI carries signed 16-bit
  PCM, so demanding a native format would fail against the dummy and would only relocate the
  F32→S16 quantization into the bridge. Negotiating the rate removes one stage from what the
  conversion line reports: `conv=0x1` instead of `conv=0x3`.

  Compiled out against a libhamlib older than Hamlib PR #2116 commit `961093f2`, which has no
  `native_*` caps fields — that prefix keeps the 48 kHz it always used. **No C ABI change and no
  wire change.**

### Added
- **`AudioStreamServer::removeStreamListener()` (C++ API) — hand a listener back and know when it is
  safe to destroy.** `addStreamListener` had no counterpart, so the only way to satisfy its
  borrowed-must-outlive-the-server rule was to declare the listener first and never destroy it
  early. Its sibling `AudioStreamClient` already had a `removeStreamListener`, which makes the
  server the asymmetric one — and that existing client function **only erased from the roster**,
  which reads as a detach and was not one.

  **The wait is the feature.** Every `notify*` snapshots the listener roster and captures the
  *pointers* by value, so a callback queued before an erase still holds the pointer. Erasing alone
  returns while that task is pending, so a caller that erased and then destroyed raced it. Both
  classes now erase and then fence the dispatcher, so the documented post-condition holds: once
  `removeStreamListener()` returns, no callback for that listener is running or will ever fire
  again. Called from *inside* a callback it detaches without waiting — a thread cannot wait for
  itself, and the only task that could hold the pointer is the one on the caller's own stack.

  **`stop()` deliberately still does not drain** (issue #89). The obvious repair — drain the
  dispatcher inside `stop()` — was implemented and measured before being rejected: `start()` after
  `stop()` is a supported, documented transition (`na_server_stats.running`), and
  `CallbackDispatcher` is not restartable, so the drain left a restarted server running correctly
  and **permanently mute with `start()` still returning success**. It also would not have held on
  the re-entrant path `naudio.h` explicitly permits (calling `na_server_stop` from inside a
  callback), where the dispatcher's own same-thread guard skips the join. The strong
  "no callback in flight once it returns" guarantee stays on `na_server_destroy` /
  `~AudioStreamServer`, where it is already documented and costs nothing.

  **No C ABI change and no wire change.** The C ABI reaches its callbacks through a single
  library-owned adapter destroyed by `na_server_destroy`, which already drains and joins, so it
  never had this hole.

- **`NAUDIO_SANITIZE` build option — sanitizer instrumentation for the whole tree.** Accepts
  `address`, `undefined`, `thread`, or a comma list (`address,undefined`); empty is the default and
  adds no flag to a normal build. `address` and `thread` are rejected at configure time because the
  two runtimes cannot share a process. Two CI jobs on `ubuntu-latest` now run the hardware-free
  ctest suite under ASan+UBSan(+LSan) and under TSan.

  **Two flag choices are measurements, not conventions.** `-fno-sanitize-recover=all` rides with
  `undefined` because UBSan is *recoverable* by default — it prints `runtime error: …` and still
  exits 0, so without the flag a CI job would report PASS forever while printing findings nobody
  reads. And `-fno-sanitize=enum` is the one suppression, carrying its justification in place: the
  three sites it fires on are the C ABI's own defensive validation of untrusted C input
  (`na_strerror`, `na_client_set_reliability_profile`, `na_server_set_reliability_profile`), whose
  `default:` branches are measured to work on today's toolchains.

  Leak detection is Linux-only — `detect_leaks` is unsupported on Darwin — so a local macOS ASan
  run is not leak coverage.

  **No C ABI change and no wire change.** Build-system and test-suite only.

- **`Socket::setSendBufferSize` / `Socket::setRecvBufferSize` (C++ API) — socket buffer sizing that
  can *shrink*, and that reports what the kernel actually did.** `setSendBufferAtLeast` /
  `setRecvBufferAtLeast` only ever raise, and their never-shrink clause is load-bearing for UDP
  (macOS refuses a `sendto()` larger than `SO_SNDBUF`), so a caller wanting a smaller buffer must
  not be able to reach it by passing a smaller number to those. These are separate calls.

  **They return the effective size rather than a bool**, because no kernel is obliged to honour the
  request and "did it work" is only answerable by reading it back. Measured on macOS/arm64: a
  request of 65536 *after* `connect` is honoured exactly, while the same request *before* `connect`
  is clamped up to a floor (8192 → 65328 `SO_SNDBUF`, 8192 → 326640 `SO_RCVBUF`). Linux commonly
  reports back double what was asked.

  **A readback is not proof the change reached the wire, and on Winsock a receive buffer does not
  bound the connection at all.** Measured on `windows-latest`: a 64 KiB receive buffer — set on the
  listening socket *before* the handshake **and** on the accepted socket after it — still absorbed
  an 8 MB send in ~30 ms with the peer reading nothing, while `getsockopt` reported 65536 back
  throughout. Windows enables dynamic send buffering by default and auto-tunes `SO_SNDBUF`, so a
  nonzero request there is advisory. `bytes == 0` is therefore legal and is the one setting that
  disables buffering for that direction outright, forcing `::send` to wait on the peer; POSIX
  kernels clamp 0 up to their own minimum instead, so it is not portable. Callers that depend on
  the size must assert on the return value and, where it matters, on behaviour as well.

  **No C ABI change and no wire change.** This is a C++ header addition only.

- **`AudioStreamClient::setTransportFactory` (C++ API) — the client-side mirror of
  `AudioStreamServer::setTransportFactory`.** Supplies the `ClientTransport` that `connect()` will
  use, replacing the one `config.transportType` would have selected. `ClientTransport` was already a
  pure-abstract interface with two implementations; only the injection point was missing.

  One contract difference from the server's, and it is the one that matters: the server consults its
  factory **once per run** (`start()`), whereas the client consults its own **once per attempt** — so
  a reconnect calls it again, and a factory that starts returning null mid-run is reached by
  `reconnectInternal()` rather than by `connect()`. Both sites now fail that attempt with an error
  instead of dereferencing the result; on a reconnect the backoff loop continues normally.

  **No C ABI change and no wire change.** This is a C++ header addition only.

- **`na_server_get_stats` + `na_server_stats` (`@since 0.2.0`) — the server-side counters, where the
  two fields that carry no information on a client actually move.** `na_client_stats` documents
  `control_retransmits` and `queue_drops` as always 0 on a client: both are written by paths only a
  server-side connection reaches. Until now there was nowhere in the C ABI to read them from —
  `na_server_get_stats` did not exist — so two fields shipped in the public ABI with no surface that
  could observe them. The new call reports the five aggregates the transports already computed
  (`packets_`/`bytes_` + `crc_errors`) plus those two and a `clients_connected` roster size, and the
  size travels as an explicit `struct_size` **parameter**, matching `na_client_get_stats`, because
  this is a struct the library writes. `NA_SERVER_STATS_SIZE_V1` is the floor.

  **It is a gauge over the live roster, not a lifetime total** — the one contract difference from
  `na_client_stats`, and the one most likely to be got wrong. Every field is summed across the
  clients connected *at the moment of the call*, so a client that disconnects takes its counters out
  of the sum and these fields **can decrease**. Measured rather than reasoned about: two clients
  being fanned 60 frames each read `packets_sent` 127, and 64 after one disconnected. A consumer
  computing a delta across a roster change reads negative throughput; the header says so, and both
  the C++ and the pure-C suites assert the decrease.

  `control_retransmits` is now **observed non-zero on a real server session** for the first time —
  6 resends from 3 distinct unacked critical control messages, bounded above by the retransmit
  attempt limit and below by the client's own count of duplicate control datagrams, a quantity the
  server never computes. Previous coverage drove the counter at the class level via a synthetic
  NACK, which proved the mechanism and not that a server reaches it.

  `queue_drops`, by contrast, is **reachable in principle and unreached in practice**, and issue
  #29's stated reason for expecting it to move — that a demux thread fills the queue while the
  application thread drains it — turns out not to be sufficient. 20,000 packets pushed as fast as
  the socket would accept them left the counter at 0, because the server's receive path has no
  blocking step in it by design. The evidence is that 0 standing against an enqueued volume many
  times the 2048-packet capacity — not that the packets all arrived, which varies by platform and
  says nothing about the drain in any case, since `packets_received` is incremented at enqueue.
  The counter is correctly wired: a consumer artificially stalled 1 ms per packet produced 57,115
  drops from 60,001 received, with `packets_received` unchanged from a healthy run. So it is a safety
  net that fires if a blocking step is ever introduced on the receive path, not a meter on normal
  operation — and the 0 is committed as an executable assertion so the limitation cannot quietly
  become folklore.

- **`na_client_stats.sequence_gaps` (`@since 0.2.0`) — the post-reorder loss measure, and the first
  field appended under the struct-size promise.** Until now no counter in this struct could report
  loss on a profile a consumer can actually select: `packets_lost` and its two siblings are `-1` on
  every built-in UDP profile, because the sequence-gap tracker runs only where no reorder buffer is
  engaged and all of them engage one, while `queue_drops` cannot move on a client at all. The new
  field is the exact **complement** of those three — it carries a reading precisely where they read
  `-1`, and reads `-1` itself precisely where they carry one — so between them a reading always
  exists. The measurement is `PacketReorderBuffer`'s existing gap count, which the conformance
  vectors already pinned but which reached no consumer.
  Two things it deliberately does **not** claim, both stated on the field:
  - **Not final loss.** It is counted before the FEC decoder sees the stream (the pipeline is
    reorder → FEC → queue), so a slot counted here may still be refilled by parity; the unrecovered
    remainder is `sequence_gaps - packets_recovered_by_fec`. It also counts every packet type
    sharing the sequence space — audio, parity and control alike.
  - **Not local loss.** Issue #29 proposed this counter as `socket_buffer_drops`, expecting it to
    surface a too-slow consumer's kernel-dropped datagrams. It cannot, and measurement is what
    settled it: an overflowing socket buffer **tail-drops**, so a stalled consumer reads an unbroken
    prefix of the stream and stops early, leaving no hole for any gap-based counter to find. Driven
    to 8 s, a client stalled to roughly a quarter of the offered rate read 127 of 400 forwarded
    packets with `sequence_gaps` at 0. Local loss remains unreported by this struct; the test suite
    asserts that 0 deliberately so the limitation stays executable.

  This is also the first exercise of the size mechanism added below: the field is written **only**
  when the caller's declared `struct_size` reaches it, so a consumer compiled against the v1 header
  can call a 0.2.0 library and keep its shorter struct intact. `NA_CLIENT_STATS_SIZE_V2` is that
  threshold; `NA_CLIENT_STATS_SIZE_V1` remains the floor and is unchanged. The library version moves
  **0.1.0 → 0.2.0** accordingly (header macros and `project(VERSION)` together, as the configure-time
  gate requires), which changes the SONAME to `libnaudio.0.2.0` — a relink, no source change: no v1
  field moved and no existing call site needs an edit.

- **`na_version_number()` and `na_version_string()` — the run-time half of the binary-compatibility
  promise, so a zero-filled struct tail can be read as fill rather than as a measurement.** The
  struct-size work above made an older library's tail *defined*; it could not make it *informative*,
  because the zero it writes is indistinguishable from a measured zero — and `na_client_stats`
  encodes "the library is not measuring this" as `-1` on purpose, so the two conventions collide
  exactly there. The accessors are compiled into the shared library, so they report the binary a
  process actually **loaded**, while the new `NAUDIO_VERSION_MAJOR` / `_MINOR` / `_PATCH` macros
  (and the derived `NAUDIO_VERSION_NUMBER` / `NAUDIO_VERSION_STRING`) report what the caller
  **compiled** against. Comparing them separates fill from measurement, given that every field
  appended after the first tag names the version it arrived in — a rule now stated in the growth
  policy. Comparisons go through `NA_VERSION_ENCODE(major, minor, patch)`, whose ordering is total
  for components within `NA_VERSION_MAX_COMPONENT`. Both accessors are infallible — no allocation,
  no backend, callable on any thread before `na_context_create()` — and neither touches
  `na_last_error()`, which the header documents as a property of fallible calls only. The header's
  version macros are hand-written, because the public header is deliberately generated by nothing;
  a configure-time check in `CMakeLists.txt` makes them unbuildable if they drift from
  `project(VERSION)`, since a version that answers *wrongly* is worse than one that does not answer.

### Changed
- **`na_hamlib_bridge` no longer tells operators that stereo over netrigctl is impossible** (issue
  #21). It is possible, and has been since Hamlib PR #2116 commit `961093f2` (2026-08-11) put
  `channels=` on the `\stream_open` wire. Until now the bridge printed *"`\stream_open` carries no
  channels field … Use `-c 1` on that path"* at the moment an operator was debugging, and
  `docs/hamlib-streaming-bridge.md` carried it as a standing **"Known limit"** — both false against
  a current libhamlib, and both printed with no way to tell which case you were in.

  **The advice is now selected at compile time** from `NAUDIO_HAMLIB_HAS_STREAM_CONV` — the existing
  `rig_stream_get_conversions` probe, which detects the same commit — so a pre-fix build still names
  the real limit and a post-fix build says the netrigctl channels limit is *not* the explanation.
  The gap-signature diagnosis itself is unchanged; only the recommended action is. The build's
  `pre-961093f2` STATUS line names the `-c 2` consequence too, since configure time is when the
  choice of prefix is still in hand.

  **The floor binds on the bridge's own libhamlib, not the peer's** — measured, not inferred, and it
  is the half that was previously undocumented. `-c 2` over `-m 2`, one machine, same bridge source,
  `rigctld -m 1 -C stream_mode=tone`: a pre-fix bridge (`9f412fe`) against a pre-fix `rigctld` gives
  `gaps` 421 → 841, ~1 per RX read, 41% of nominal; a post-fix bridge (`2f076b5`) gives **0 gaps at
  83% against a pre-fix `rigctld` as well as a post-fix one** — even though `rigctld` still hardcodes
  `channels = 1` and opens its rig mono. A naudio client read **429112/430080 non-zero samples** off
  that mixed run, so it is populated stereo, not mono padded with silence. Upgrading the remote
  `rigctld` alone fixes nothing here.

  Documented alongside it, because it is the failure an operator will actually mis-diagnose: a
  **pre-fix bridge against a post-fix `rigctld` does not start at all** — the old client cannot parse
  the new `\stream_caps` reply and rejects its own request with *"sample rate 48000 not in supported
  list"*, a message naming a rate the caps dump appears to offer. Not channel-related; identical at
  `-c 1`.

- **`na_server_inject_audio` and `na_server_client_count` no longer both say "connected"** (issue
  #48). They describe two different sets, and the header gave a consumer no way to tell: a client
  joins the **roster** at accept, before its handshake, and becomes a **broadcast target** only once
  that handshake completes. `na_server_client_count` reports the former. An inject in the window
  between them is **discarded** — no queue, no retry — and still returns `NA_OK`, so the obvious
  inject-as-soon-as-a-client-appears loop drops audio at every join, non-deterministically, with no
  counter that reports it.

  **No new symbol.** `on_stream_started` already fires after the session is registered as a
  broadcast target, so the barrier a consumer needs is on the ABI today; the header now names it,
  with a worked example, and states which set each call reports. Adding an accessor for the ready
  count would widen a published surface to say what the callback already says. Documentation only —
  no behavior changed here.

  `tests/bridge/na_bridge_probe.c` was making exactly this mistake — its gate polled
  `na_server_client_count` under a comment claiming the guarantee that call does not give — and now
  gates on `on_stream_started`. Measured: the window is too narrow to lose a frame on this loopback
  (60 of 60, five runs), but forced open with a 300 ms delay before target registration the old gate
  delivered 36–37 frames of 60 **while the selftest still reported OK**, because it asserts
  `calls > 0`; the new gate delivers 60 of 60 under the same delay.

- **BREAKING (C ABI): the client identity and device setters now refuse to run after connect has
  been attempted, and `na_client_set_capture_device` refuses the NULL backend outright.**
  `na_client_set_identity`, `na_client_set_playback_device` and `na_client_set_capture_device` were
  unguarded plain-member writes, while the reconnect worker re-runs the handshake and reads exactly
  those members — a concurrent `std::string` assignment against that read is undefined behaviour,
  and the header explicitly permits the trigger by stating any client method may be called from
  inside an event callback (`on_reconnecting` among them). They now return `NA_ERR_INVALID` once
  `na_client_connect` has been attempted, which is the gate `na_client_set_callbacks` and
  `na_client_set_audio_cb` already carried for this same reason. Set them before connecting.

  Separately, `na_client_set_capture_device` now returns **`NA_ERR_UNSUPPORTED`** on a
  NULL-backend client instead of `NA_OK`. That backend cannot capture, so the call could never
  succeed; the failure was merely *deferred* to `na_client_connect`, where it surfaced only after a
  live server handshake and left the handle spent. `na_server_set_capture_device` has always
  refused it at the setter — the two siblings now agree. TX from a NULL-backend client goes through
  `na_client_set_tx_inject`.

  **No wire change.** No callers exist outside the test suite for the capture setter.

- **The failure-path lifecycle contract is now specified, and the return code — not the errbuf
  text — is what tells a caller whether to retry.** `NA_ERR_BACKEND` means the *attempt* failed and
  the handle is still usable; `NA_ERR_INVALID` means the *handle* is spent and no retry on it can
  succeed. A client becomes terminal at the first `na_client_disconnect`, or at a connect failure
  past the start of the handshake; a transport-level failure such as a refused socket does not. A
  **failed** `na_server_start` no longer consumes the server handle — only a successful start is
  one-shot, which is what the header already said.

- **BREAKING (C ABI): all four caller-allocated structs now carry their own size, so an
  appended field stops being an out-of-bounds access against an already-compiled consumer.** None
  of them recorded how large the caller believed them to be, so adding any field to a future
  release would have made the library read or write past the end of a struct allocated by a
  consumer compiled against the older header — silently, with no symbol rename and no link error.
  The mechanism follows the struct's **direction**, because one size does not fit both:
  - `na_client_callbacks` and `na_server_callbacks` are **read** by the library and gain an in-band
    `size_t struct_size` as their first member, set by the caller (Win32's `cbSize` idiom). Callers
    already `memset` these, so it is one line beside the existing one. `na_client_set_callbacks` /
    `na_server_set_callbacks` return `NA_ERR_INVALID` below `NA_CLIENT_CALLBACKS_SIZE_V1` /
    `NA_SERVER_CALLBACKS_SIZE_V1`, which includes the `0` an unset field carries.
  - `na_client_stats` is **written** by the library, so its size travels as an explicit parameter:
    `na_client_get_stats(client, out, sizeof *out)`, `NA_ERR_INVALID` below
    `NA_CLIENT_STATS_SIZE_V1`. An in-band member would have required callers to initialize an
    out-parameter, inverting this struct's long-standing "never needs pre-zeroing" contract.
  - `na_device` is **written by the library as an ARRAY**, so its size likewise travels as a
    parameter: `na_enumerate(ctx, out, max, sizeof out[0])`, `NA_ERR_INVALID` below
    `NA_DEVICE_SIZE_V1`. This is the case where the old signature was not merely awkward but
    unsound — `max` is an element *count*, and the library strode the array by its **own**
    `sizeof(na_device)`. Appending a field would therefore have written every element after the
    first past its slot in a consumer's array and mis-parsed all of them. The library now strides
    by the caller's element size, and `na_device`'s layout itself is unchanged.
  In every direction the library honours the caller's declared size as a hard bound: a shorter
  caller keeps its un-allocated tail untouched, and a longer one has its extra bytes zero-filled
  (stats, and each device element) or ignored (callbacks). Nothing had
  been tagged when this break was taken and `SOVERSION` was still 0, so no released consumer
  existed to break.
  Consumers that bind the ABI by hand rather than through the header — the Python, Java and Rust
  example clients — need the new argument at their `na_enumerate` call sites; their `na_device`
  layouts are byte-for-byte unchanged.
  The resulting promise is now written down for packagers as **"Binary compatibility"** in
  `README.md`, condensed at the top of `include/naudio.h`: what the declared size guarantees in each
  version direction, and the append-only growth rule the `NA_*_SIZE_V1` floors encode. An older
  library's zero-filled tail is defined but reads the same as a genuine zero; the version accessors
  added below are what tell the two apart.

### Fixed
- **UDP audio frames are now sized from the MTU advisory instead of from the frame cadence, so no
  preset emits a datagram that IP-fragments** (issue #86). `AudioPacket::UDP_MAX_PAYLOAD` (1400)
  has always documented that a larger datagram fragments on a standard ~1500-byte path, and that
  losing any one fragment discards the whole datagram — but the rule was addressed to *callers*
  while naudio's own presets all broke it. One call to send audio became exactly one datagram,
  whose size was however much audio `frameDurationMs` happened to name. Measured, with no oversized
  inject involved and at each preset's own default settings:

  | preset | audio frame | datagram before | datagram after | packets/frame |
  |---|---|---|---|---|
  | default / TCP shape (20 ms) | 3840 B | 3863 B | 1399 B | 1 → 3 |
  | `udpLan` / `udpWan` / `udpFt8` (10 ms) | 1920 B | 1943 B | 1399 B | 1 → 2 |
  | `udpIq` (192 kHz, 10 ms) | 7680 B | 7703 B | 1399 B | 1 → 6 |
  | `lowBandwidth` | 960 B | 983 B | 983 B | 1 → 1 (unchanged) |

  **`udpWan` is the only preset that enables FEC, and it was a preset whose own frames defeated it:**
  a fragmented 1943-byte datagram loses everything if any fragment is dropped, including the parity
  meant to cover that loss. None of this was visible in testing because loopback has no MTU.

  `UdpClientConnection` now splits outgoing audio at
  `AudioPacket::udpMaxAudioPayload(sampleFrameBytes)` — the advisory budget rounded **down to a
  whole sample frame**, since a chunk boundary inside a sample decodes every later sample one
  channel out of phase. **This is not app-layer fragmentation and not a wire change:** each chunk
  is a complete, independently-sequenced 0xAF01 audio frame, and the receiver reassembles nothing,
  because it appends payloads to a byte-stream ring in sequence order exactly as before. It is the
  same property the RX fan-out already relied on for issue #20's oversized inject, applied one
  layer down so that **both** audio producers are covered — the server's RX fan-out and the
  client's TX capture loop reach this one point, and fixing either alone would have left the other
  fully exposed. FEC parity needs no separate handling: it is the XOR of its block's payloads, so
  it shrinks with them.

  **The cost is more packets per second, in exchange for datagrams that survive a real path**: 100
  → 200 pkt/s on the voice presets, 100 → 600 on `udpIq`. `lowBandwidth` was already compliant and
  is byte-identical. Frame cadence, latency and `frameDurationMs`'s meaning are unchanged; the
  reorder window (8 packets) and FEC block (5 packets) now span proportionally less time, which
  widens the FEC decoder's derived hold bound rather than narrowing it.

  **`UdpClientConnection::oversizedDatagrams()` finally means something.** It previously counted
  ordinary audio at every UDP preset and was read by nothing anywhere — not by a test, not by the C
  ABI, not by the bridge. It now stays 0 on every preset (asserted), and a non-zero value reports
  only what nothing upstream can shrink: an outsized control message, or a format whose single
  sample frame does not fit the advisory at all — a case where alignment is deliberately preferred
  over MTU safety, because fragmentation costs loss resilience while misalignment corrupts.

  **Added to the public C++ headers:** `AudioPacket::UDP_MAX_AUDIO_PAYLOAD`,
  `AudioPacket::udpMaxAudioPayload()`, `AudioStreamConfig::sampleFrameBytes()`,
  `AudioStreamConfig::udpMaxAudioPayload()`, and `UdpReliabilityConfig::maxAudioPayloadBytes`.
  **No C ABI change and no wire change**; `docs/audio-streaming-protocol-v1.md` §2.4 gains a
  non-normative note on which side of the connection owes the advisory.

- **`Socket::close()` no longer frees a descriptor while a syscall is still running on it**
  (issue #90). Every transport's shutdown path closed a socket while another thread was parked in
  `recv()` / `accept()` on that same descriptor — `AudioStreamServer::stop()` →
  `TcpServerTransport::close()` → … → `Socket::close()`, doing exactly what its comment said
  ("close the transport to unblock the accept thread"). POSIX leaves that undefined, and the
  practical hazard is not a lost wakeup but **descriptor reuse**: once `::close` returns the integer
  is free, so any thread opening any file or socket can be handed the same number while the first
  thread is still inside its syscall, which then refers to a different object. On a server that
  accepts connections during teardown the window is real. It was the single most-reported race in
  the first instrumented run — 17 arms.

  **Fixed inside `Socket` rather than by reordering each transport's teardown.** The descriptor's
  lifetime is the socket's own invariant, so a rule spread across every transport would have to be
  re-derived by the next one. `close()` now takes the handle (no new syscall can enter), `shutdown`s
  it to wake anyone already inside, waits for the in-flight count to reach zero, and only then
  frees it. Every method that hands the descriptor to a syscall — including the option setters,
  where a stray `setsockopt` would silently reconfigure whatever socket inherited the number — is
  covered. No transport's shutdown ordering changed, so the two-pass `stop()` from #57 is untouched.

  **The wait is bounded and cannot trade the race for a hang**; if the deadline is ever reached the
  descriptor is freed as before, and `Socket::drainTimeouts()` counts it so that residual is
  visible instead of silent.

  **`Socket::acceptTcp` now polls in slices** (summed back to the caller's unchanged deadline) so a
  concurrent `close()` is seen promptly. `shutdown` does not wake a `select` on a listener, so
  without this every server teardown paid out the accept loop's whole poll: measured 138.58 s →
  165.85 s on the full suite, +28.0 s in flat ~1.00 s steps across 29 tests, brought back to
  140.00 s by the slicing. Linux gains too — `close()` never woke a blocked `select` there at all.

  **The TSan suppression this issue existed to retire is gone**, so the sanitizer job now reddens
  on every race in the tree with nothing excused.

- **`na_hamlib_bridge` builds again against a current Hamlib PR #2116 branch** (issue #81). Upstream
  commit `b538567b` replaced `rig_stream_caps`' `channels_min`/`channels_max` **range** with an exact
  0-terminated `channels[]` **list** — openable channel counts need not be contiguous, so a backend
  offering 1 and 4 but neither 2 nor 3 could not be described by a range, and the range form quietly
  implied counts that were not openable. The bridge still read the old field names, so it failed to
  compile against the branch it tracks; anyone building the bridge from current upstream got four
  compile errors.

  **naudio keeps tracking the PR branch head rather than pinning a commit.** The streaming API is
  still in review, and early notice that it moved is worth more than a build that cannot be
  surprised. What absorbs the churn is that nothing assumes a shape: `tools/CMakeLists.txt` now
  capability-checks *which* revision is installed and compiles the matching code, so both the list
  and the range form build. The channel-list check is a `check_struct_has_member` rather than a
  `check_symbol_exists`, because a struct-field change exports identical symbols and would otherwise
  surface only as a compile error. Verified against three real prefixes — pre-`961093f2`,
  `961093f2`, and `b538567b` — all three configure, build, and produce a working binary.

  **The caps diagnostic prints a list where the API provides one.** On a failed `rig_stream_open`
  the bridge dumps the backend's capabilities; `channels=1..4` becomes `channels=1,4` against a
  current libhamlib, which is the openable set rather than a range spanning counts that are not.

  No C ABI change and no wire change. The bridge is an optional tool, off by default.

- **Server teardown no longer destroys the worker-barrier condition variable while a session worker
  is still signalling it** (issue #28). `AudioStreamServer::threadFinished()` released
  `threadsMutex_` *before* calling `threadsCv_.notify_all()`, which let the whole teardown complete
  in the gap: the worker decrements to zero and unlocks, `stop()`'s barrier wakes and returns,
  `~AudioStreamServer` runs to completion and destroys `threadsCv_`, and the worker — still between
  the unlock and the notify — calls `pthread_cond_broadcast` on freed memory. The notify now happens
  under the lock, so the waiter cannot return from `wait()` until after it completes.

  Found by the TSan CI job added in this release, **on Linux only** — two full TSan runs on
  macOS/libc++ never reproduced it. `AudioStreamClient::threadFinished()` had always notified under
  its lock; the two siblings disagreed and only the client was right.

  No C ABI change and no wire change.

- **`na_server_start` on a TCP transport no longer reports success on Windows for a port another
  server is already listening on** (issue #85). The TCP listener was bound with `SO_REUSEADDR` on
  every platform. On POSIX that is the restart-after-`TIME_WAIT` accommodation and it never permits
  two live listeners — but Winsock's option of the same name permits binding a port another socket
  is **actively listening on**, which is the behaviour `SO_EXCLUSIVEADDRUSE` exists to prevent. So
  on Windows a second server started cleanly on a port already in use, `na_server_start` returned
  `NA_OK` for a port it did not have, neither process was told, and connections reached only one of
  them.

  The listener now asks for *port ownership* rather than for a named flag, and each platform sets
  whichever option delivers it: `SO_REUSEADDR` on POSIX, `SO_EXCLUSIVEADDRUSE` on Windows. The
  Windows option is also strictly stronger than setting nothing, since the bare default still lets
  another process take the port by setting `SO_REUSEADDR` itself.

  This is the same defect as #83 on the other transport and platform, but not the same fix: #83
  could drop the flag outright because UDP has no `TIME_WAIT` for it to accommodate, whereas TCP
  does. **A port is still immediately rebindable by a restarting server while its own previous
  connections are in `TIME_WAIT`, on Windows under the new option as well as on POSIX** — asserted
  as an explicit control on every CI job, because a fix that traded the double bind for a server
  that cannot restart promptly would pass a "second bind is refused" test just as happily. That
  control replaces one that staged no `TIME_WAIT` at all and so held whether the flag was set or
  not.

  **This is a behaviour change on Windows**, and the only callers it can affect are ones that were
  previously getting silent success: a start that used to return `NA_OK` on an occupied TCP port now
  fails with a bind error. POSIX behaviour is unchanged. No C ABI change and no wire change —
  `na_server_start`'s signature and error convention are untouched; it now reports failure where it
  previously reported success.

- **`na_hamlib_bridge` lost a client's TX channel without saying anything** (issue #17). The bridge
  would carry an operator's transmit audio for about a second and then discard everything after it,
  with a healthy-looking console, an open port and no error on either side.

  The underlying fault was found and fixed earlier in the library: a legal maximum-payload RX packet
  (16407 bytes) exceeded macOS's default `SO_SNDBUF` of 9216, `sendto()` refused it with EMSGSIZE,
  and the writer loop responded by closing the session — which unregisters it from the mixer and so
  releases any TX channel it held. The socket buffers were raised and the writer loop was given a
  diagnostic naming the direction and size of the refused send.

  **None of that reached the bridge.** `na_hamlib_bridge` registered no `na_server_callbacks` table,
  and the C ABI forwards `onError` only when `cbs.on_error` is set, so every server-side fault was
  constructed, posted to the dispatch thread, and dropped for want of a listener. The bridge now
  registers `on_error`, `on_client_connected` and `on_client_disconnected` before `na_server_start`,
  and separately logs each TX-ownership episode with its duration and the bytes that actually
  reached the radio. A lost channel now reads:

  ```
  na_hamlib_bridge: tx owner acquired
  na_hamlib_bridge: tx owner released after 4.45s, 304320 B to the rig
  na_hamlib_bridge: server error [audio-1]: Connection timeout
  na_hamlib_bridge: client audio-1 disconnected
  ```

  This is diagnostic output only — no C ABI change, no wire change, and no change to which audio the
  bridge delivers.

- **The installed package was unusable on MSVC and on any FetchContent-built install** (issue #61).
  `find_package(naudio)` failed two different ways, both on platforms whose CI build was green —
  and green precisely because those jobs built the library and never installed or consumed it.

  The single root cause: `naudio_pa` linked PortAudio `PUBLIC`, so the *build-time target name* was
  baked into the installed `naudioTargets.cmake` — `PkgConfig::PORTAUDIO` from a system build, the
  bare `portaudio_static` from a bundled one — and `naudioConfig.cmake` had to recreate that exact
  name. It could recreate only one of the two, and only through pkg-config. So a consumer with no
  pkg-config (MSVC) got *"naudio could not be found because dependency PkgConfig could not be
  found"*, and a consumer of a bundled install got *"imported targets are referenced, but are
  missing: portaudio_static"*.

  PortAudio is now linked in the **build interface only**, so both resolution paths export an
  identical targets file naming no PortAudio target at all, and `naudioConfig.cmake` resolves
  PortAudio on the *consumer's* side — pkg-config first, since `portaudio-2.0.pc` carries the full
  private closure, then a plain `find_library`/`find_path` for hosts that have none.

  **Not finding PortAudio is no longer fatal to the package.** It is needed by exactly one target,
  `naudio::naudio_pa`; the shared C ABI `naudio::naudio` has its device backend linked in already,
  so a C / Hamlib consumer — the primary audience — can now use the package on a host with no
  PortAudio development files and no pkg-config. That case reports itself at configure time and
  sets `naudio_PORTAUDIO_FOUND` to `FALSE`, rather than surfacing later as an undefined-`Pa_*` wall.

- **A bundled build installed PortAudio's own files into naudio's prefix** (issue #61). PortAudio's
  install rules ran as part of ours, depositing `portaudio.h`, `pa_mac_core.h`, `libportaudio.a`,
  `libportaudio.{so,dylib}`, `lib/cmake/portaudio/`, `lib/pkgconfig/portaudio-2.0.pc` and
  `share/doc/portaudio/` alongside naudio's. `PA_DISABLE_INSTALL` is the analogue of the
  `INSTALL_GTEST OFF` already set for GoogleTest, which PortAudio had no counterpart for.

- **`naudio.pc` hard-errored wherever `portaudio-2.0.pc` was absent** (issue #61). pkg-config
  resolves `Requires.private` on **every** query, not only `--static`, so `Requires.private:
  portaudio-2.0` made plain `pkg-config --cflags naudio` *and* `--libs naudio` exit 1 on such a host
  (measured, pkgconf 2.5.1). On a bundled build the module does not exist anywhere and the claim was
  simply false; on a system build it broke consumers needing none of it, since the file describes the
  **shared** `libnaudio`, whose PortAudio dependency the dynamic linker already carries. The static
  closure is now raw flags in `Libs.private`, which triggers no module lookup, so the `.pc` degrades
  instead of erroring.

- **On Linux, tearing down a session leaked its writer thread whenever the peer had stopped
  reading** (issue #56). Closing a socket drops a *descriptor*; it does not by itself return a
  thread already blocked in the kernel on that socket, because a blocked `send()` holds its own
  reference to the open file description. naudio had no `::shutdown()` call anywhere, so teardown
  depended on a behaviour that only some platforms provide. Measured against a peer whose receive
  window had shut: on macOS a bare `close()` woke the parked sender immediately, while on Linux it
  was **still parked five seconds later** — and `shutdown()` before `close()` woke it in **11 ms**.
  Every connection close now half-closes both directions first. The platform that papered over this
  is the one naudio was developed on; the one that did not is the one Hamlib mostly ships to.

- **A client that stopped reading could wedge the server thread that handles its own traffic**
  (issue #56). The session's receive loop answered a latency probe and a TX denial with a direct
  send, and every send holds one mutex across the whole socket write — so while the writer thread
  was blocked against a stalled peer, the receive thread blocked behind it. The thread that
  processes that client's TX audio, its heartbeats and its DISCONNECT stopped, precisely because
  the client had stopped reading. Both are now queued for the writer thread, which is what the
  writer bridge exists for. A depth cap on that queue is part of the same fix rather than an extra:
  control messages do not count toward the byte-based backlog cap, so with the receive thread no
  longer blocking, a peer that spams latency probes while refusing to drain would have queued one
  response per probe forever — the wedge had been doing that bounding.

  **Consequence for a backlogged client:** a latency probe answered from the queue reports a larger
  round-trip than one answered inline. That is the honest number — a client whose queue is deep
  genuinely is that far behind — and it is preferable to stalling that client's TX audio to make
  one measurement look better.

- **On Windows, a rejected client received no reason at all — it saw a reset connection instead of
  "Maximum clients (N) reached"** (issue #87). The server decides a reject at *accept*, before
  reading a byte, so a client that behaved normally — connect, send `CONNECT_REQUEST`, then wait for
  ACCEPT/REJECT, which is exactly what `na_client_connect` does — had left that request unread in
  the server's receive buffer. Closing a TCP socket on top of unread received data makes the stack
  send an RST rather than a FIN (RFC 1122 §4.2.2.13), and the RST makes the peer discard its own
  receive buffer, the already-delivered CONNECT_REJECT with it. The reject reason is the only
  diagnostic a turned-away operator gets, and `Busy` vs `Rejected` is what distinguishes "try again
  later" from "this server will never take you". The reject path now drains that pending input
  before closing, so the close emits a FIN.

  **Not reproducible off Windows**, which is worth stating so nobody reads a green macOS run as
  coverage: the same sequence was driven 200 times on macOS loopback (five read-delays from 0 to
  300 ms crossed with 0 and 20 unread filler packets) and the reject arrived 200/200. The failing
  evidence is CI, where it is deterministic from the first attempt.

  **Delivery is documented as BEST-EFFORT, not guaranteed** — the code previously read as though it
  were. The drain fixes the case a real client hits; a peer that keeps sending past the drain budget,
  or whose data arrives after the close, can still reset. A client should treat a bare disconnect
  during connect as "rejected, reason unknown".

- **A client the server rejected stayed in the transport's connection map for the life of the
  server, inflating every `na_server_get_stats` counter** (issue #64). `rejectClient` sent the
  CONNECT_REJECT and closed the socket, but closing is not leaving: only the transport's
  `disconnectClient` removes the entry, and every `na_server_stats` aggregate is a **sum over the
  entries currently in that map**. A rejected connection therefore kept contributing its own reject
  message forever, so a server doing nothing but refusing clients — one sitting at `max_clients`
  while a client retries, or one with no capture device — drifted upward on counters documented as
  a *gauge over the live roster*, never a lifetime total. Measured across 8 rejects with an empty
  roster: `packets_sent` 8 and `bytes_sent` 440 where both must read 0, identical on TCP and UDP.
  All three reject paths share one `rejectClient`, which now evicts through the transport, exactly
  as an admitted session already did when it closed.

- **A peer that failed the handshake produced an `on_client_connected` with no matching
  `on_client_disconnected`** (issue #64). The connect event fires when the session enters the
  roster, which is at accept — before a word has been read — and the handshake-failure paths then
  closed the session and returned without emitting anything. Any peer that connected and said the
  wrong thing (a port scanner, a version-mismatched client, a half-open probe) left a connect event
  that nothing ever closed, so a listener that pairs the two accumulated one phantom client per
  probe. The server's own roster was never wrong — `na_server_client_count` was correct throughout —
  which is why only an event-pairing listener could see it.

  **The contract is now stated in `naudio.h`:** `on_client_connected` / `on_client_disconnected`
  bracket roster membership and are always balanced, including for a session that never completes a
  handshake; `on_stream_started` / `on_stream_stopped` nest inside that pair and bracket the window
  in which the client is actually carrying audio. **Behavior change for consumers:** a failed
  handshake now emits a disconnect event that it previously did not. Nothing about a *successful*
  client's event order changed.

- **`na_server_inject_audio` silently discarded everything past 16384 bytes and still reported
  success** (issue #20). `AudioPacket::serialize()` clamps its payload to `MAX_PAYLOAD` — correctly,
  because the wire's length field is a `u16` and the decoder rejects anything longer — but nothing
  above it ever split an oversized buffer. A single inject therefore became one clamped packet and
  the remainder was dropped with no error, no counter and no log, while every layer from the codec
  up to the C ABI returned success. Measured before the fix, over a real loopback client:
  `na_server_inject_audio(srv, buf, 70000)` returned `NA_OK` and delivered 16384 bytes, losing
  53616; a 16385-byte call lost exactly one byte. The ceiling was also undocumented, so a consumer
  had no way to discover it — and `na_hamlib_bridge` injects exactly 16384, sitting precisely on it.

  The RX fan-out now frames an oversized buffer into wire-sized packets, so every injected byte is
  delivered. This is **not** app-layer fragmentation and **not** a wire-format change: each chunk is
  a complete, independently-sequenced `0xAF01` audio packet, indistinguishable from the frames the
  capture path already emits, and the receiver reassembles nothing. Chunks are aligned to a whole
  sample frame so a boundary can never fall mid-sample.

  **Behavior change for consumers:** one `na_server_inject_audio` call may now surface at the client
  as more than one RX audio callback. The byte stream is preserved exactly and in order; the frame
  boundaries are naudio's to choose and were never a promised part of the ABI. An inject of 16384
  bytes or fewer is byte-identical to before, so callers already sized under the old ceiling —
  including the bridge — see no change at all. The size contract is now stated on the function in
  `naudio.h`.

- **A server client that stopped reading but kept sending was never evicted when no audio was
  flowing** (issue #56). The session's heartbeat loop discarded the result of its own send, and it
  was the only mechanism that could have noticed. The connection timeout could not: it is keyed on
  the time of the last *receive*, so a peer that keeps transmitting refreshes that clock however
  long its receive window has been shut. The outbound backlog cap could not either: it bounds
  queued *volume*, and nothing queues while the server has no audio to fan out. With both silent,
  a session failing every send stayed on the roster indefinitely, holding two threads and one of
  the four default client slots — so four such peers locked out every subsequent client.

  A failed heartbeat now tears the session down and reports why. This matches the writer thread in
  the same class, which already treated a failed send as fatal; the heartbeat loop was the one send
  path that ignored its verdict. Consumers see `on_error` followed by `on_client_disconnected`, and
  the client slot is released. Measured before the fix: seven consecutive failed heartbeats across
  eight timeout evaluations with the client still counted as connected.

  **No C ABI change and no wire change** — a client that was previously retained forever is now
  disconnected, through the existing callbacks.

- **Two lifecycle races reachable from the public C ABI** (issue #57). `na_server_stop` could
  return while a client admitted *during* its teardown was still starting: `handleNewClient`
  sampled the running flag once at entry, so `stop()` passed its thread barrier precisely because
  the session's thread had not been counted yet, cleared the roster, and then blocked joining the
  accept thread — which *guarantees* the straggler's detached run loop is spawned before `stop()`
  returns. Nothing joined it, so the destructor tore the server's mutexes down underneath a live
  thread; `na_server_destroy`'s documented stop-then-delete contract was the reachable path. The
  same window let the session reopen the shared capture device after teardown had closed it.
  Separately, `na_client_disconnect` racing a completing `na_client_connect` spawned the playback
  worker on an already-nulled stream, which dereferences before testing anything — process death
  on a detached thread, with `on_error` never firing.

- **`na_client_connect` on a spent handle no longer completes a full server handshake before
  failing** (issue #58). The closed check ran *inside* the connect path, after the handshake and
  audio-line opening, so a retry opened a socket, handshook, and was accepted into the server's
  roster before aborting. The cost landed on the server: with `max_clients=1` the phantom session
  held the only slot for the full 10 s connection timeout, rejecting legitimate clients as busy,
  while `na_client_is_connected` reported 0 throughout. That retry also leaked its socket and
  freshly opened audio lines, because the teardown early-returned on the already-set flag.

- **A failed `na_server_start` no longer bricks the server handle** (issue #58). The one-shot flag
  fired on *attempt* with no reset on any failure path, so a bind collision left a handle that
  could neither be started nor reconfigured — the retry returned `NA_ERR_INVALID` with the errbuf
  untouched. Retrying a momentarily busy port is the Hamlib bridge's normal startup case.

- **`na_client_stats.connected` no longer reads 1 during an incomplete handshake** (issue #58). It
  keyed on the connection object, which is installed before the handshake runs, so for the whole
  up-to-10 s window of a connect that would fail it claimed a live connection — and labelled every
  counter beside it as a reading from one — while `na_client_is_connected` returned 0 at the same
  instant.

- **`na_server_start` on a UDP transport no longer reports success for a port another process is
  already serving** (issue #83). The UDP server socket was bound with `SO_REUSEADDR`. On TCP that
  flag is the standard restart-after-`TIME_WAIT` accommodation and never permits two live listeners;
  UDP has no `TIME_WAIT`, so it bought the server nothing — and what it cost was platform-dependent.
  Measured with controls: two UDP sockets that *both* set it bind the same `addr:port` successfully
  on **Linux** and are refused on **macOS**. So on Linux a second server started cleanly on a port
  already in use, `na_server_start` returned `NA_OK` for a port it did not have, neither process was
  told, and datagrams reached only one of them.

  **This is a behaviour change on Linux**, and the only callers it can affect are ones that were
  previously getting silent success: a start that used to return `NA_OK` on an occupied UDP port now
  fails with a bind error. A port is still immediately rebindable once its server closes — verified
  as an explicit control, since UDP has no `TIME_WAIT` for the flag to have been accommodating.
  macOS and Windows behaviour is unchanged; they already refused the second bind.

  No C ABI change and no wire change — `na_server_start`'s signature and error convention are
  untouched; it now reports failure where it previously reported success.

- **`na_client_disconnect` / `AudioStreamClient::disconnect()` no longer wait out a heartbeat
  interval after the connection is lost** (issue #76). `closed_` is the predicate every internal
  interruptible sleep waits on, so setting it is only half the terminal transition: the three sites
  that claim it on a connection loss — reconnect exhaustion, the "connection unstable" cutoff, and
  the auto-reconnect-off path — set it without notifying the condition variable. A worker already
  parked in that sleep was therefore never woken; it slept out the remainder of **its own** interval
  while the join barrier blocked behind it. The longest such interval is the heartbeat check's
  3000 ms, so a consumer calling `na_client_disconnect` after a peer went away paid up to ~3 s of
  teardown, with no way to opt out — a cost coupled by accident to a constant chosen for heartbeat
  cadence rather than for teardown latency.

  Measured on all three paths: **2787 ms** after reconnect exhaustion, **2942 ms** on the
  unstable-connection cutoff, **2937 ms** with auto-reconnect disabled — the last being the path an
  embedder that turns reconnection off pays on every loss. All three now return promptly. The claim
  and the wake are a single operation internally, so the two cannot be separated again.

  **No C ABI change, no wire change, and no change to which events fire or in what order** — this is
  teardown latency only. Note the cost was intermittent, not constant: the stall requires the parked
  worker to already be asleep when the transition lands, which is why it read as ordinary variance.

- **`na_client_disconnect` / `AudioStreamClient::disconnect()` no longer wait on a wedged TCP writer
  before sending their courtesy DISCONNECT** (issue #77). Every TCP send on a connection funnels
  through one `sendMutex_` held across the whole `Socket::sendAll`, so a peer that had stopped
  reading parked the audio writer *holding that lock* — and `disconnect()`'s courtesy control queued
  behind it, before it could attempt a send at all. `closeResources()`, which is what breaks the
  wedge, runs **after** the salvo. Measured through the test seam: `disconnect()` had not returned at
  4 s and no DISCONNECT was ever attempted.

  The salvo now uses a **non-blocking** send (`ClientConnection::trySendControl`, `std::try_to_lock`)
  that declines when another sender already holds the lock, on the reasoning that a held lock means
  the socket is backed up and the courtesy frame could not have arrived promptly anyway. The salvo is
  **not** skipped otherwise: on a free lock both copies still go out with their yields, which is what
  keeps a clean TCP disconnect from reporting a receive error to `on_error`. UDP has no such lock and
  forwards unchanged — same two copies, same timing.

  **What this does and does not bound.** It removes the wait *for the lock*, which was the term that
  could be paid before any send was attempted. Once the lock is won, `sendAll` still spends up to its
  whole-call budget (`CONNECTION_TIMEOUT_MS / 2`); that budget is deliberate and unchanged, so a
  disconnect against a wedged peer is prompt rather than instantaneous.

  **C++ API change, no C ABI change and no wire change.** `ClientConnection` gains a pure-virtual
  `trySendControl`; any out-of-tree implementer of that interface must add it. The C ABI, the
  `0xAF01` frame format, and the DISCONNECT control itself are untouched.

- **A DUAL server bound to port 0 no longer fails when the OS reserves the port its own allocator
  just handed out** (issue #73). `DualServerTransport::bind` binds TCP, reads back the assigned port,
  then binds UDP to that same number — TCP and UDP port spaces are independent, which is what lets one
  port number serve both. Independent is not the same as certain: a host may reserve blocks of UDP
  ports the TCP allocator knows nothing about, and a bind into one is refused with a *permission*
  error (Windows `WSAEACCES`/10013) rather than address-in-use. Whether it happens depends only on
  which port the OS chose, so it surfaced as an intermittent — five arms failed together on a Windows
  runner against a commit that changed `CHANGELOG.md` and nothing else.

  A caller that requested **port 0** now has the pair rolled back and retried on a fresh OS-assigned
  port, bounded at 32 attempts. That preserves the contract such a caller was given: it asked for any
  port, and it still gets any port. A caller that **named** a port is unchanged — one attempt, that
  port or nothing. Keeping the retry off that path is deliberate: a caller that named a number should
  get that number rather than a silent substitute, and it is also the only path on which a genuine
  privilege refusal is reachable, which a retry would otherwise mask. On exhaustion the underlying
  refusal is reported rather than a summary of it, so the error stays diagnosable.

  The bound is 32 rather than "a few" because ephemeral ports are issued sequentially (measured:
  +1 per bind across 24 cycles, with a just-released port never re-issued), so each retry steps one
  port further into a reserved range — a small budget would exhaust *inside* a typical 16-port block
  and fix nothing.

  **No C ABI change and no wire change**; `docs/audio-streaming-protocol-v1.md` §2.2 records the
  retry as a bounded `SHOULD` for other implementations, since nothing about it is observable on the
  wire.

- **The client's connection-timeout watchdog is no longer evaluated downstream of a blocking send**
  (issue #71). `AudioStreamClient`'s heartbeat loop is the only thing that notices a peer which has
  stopped sending, and its `isConnectionTimedOut()` check sat *between* two blocking sends — the
  heartbeat before it and the latency probe after. Every TCP send funnels through one mutex held
  across the whole write, so a peer whose receive window has closed parks any sender for a send
  budget (`CONNECTION_TIMEOUT_MS / 2`) plus one in-flight write, and parks every other sender behind
  it. The watchdog was therefore not evaluated for as long as the peer stayed wedged: precisely the
  condition it exists to detect, and its detection latency could stretch by two budgets plus a full
  check interval.

  The check now runs **before** any send in the iteration, and **again** after the heartbeat send.
  At most one parked send can now separate two consecutive evaluations, instead of both of them
  either side of a check interval. The second call also stops the latency probe — a pure diagnostic
  whose answer is worthless on a dead connection — from spending a further budget on a peer already
  declared gone.

  **This bounds the delay; it does not remove it.** A send that has already parked still runs to its
  budget, because nothing can interrupt it from inside the loop.

  **No C ABI change and no wire change** — an `on_error` reporting `"Connection timeout"` simply
  arrives sooner against a stalled peer.

- **`disconnect()` no longer pays a second send deadline on a peer already known to be gone**
  (issue #71). The courtesy DISCONNECT control is sent twice with a short yield, because on UDP a
  Winsock loopback can discard the in-flight datagram when the socket closes microseconds after
  `sendto`. The second copy was sent unconditionally — so against a TCP peer that had stopped
  reading, the first send spent a whole send budget establishing that the socket was broken and the
  second then spent another one, roughly doubling the worst case (~10 s at the default
  `CONNECTION_TIMEOUT_MS / 2` per budget). The second copy is now sent only if the first succeeded.

  **UDP behaviour is unchanged**: there the first `sendto` succeeds, so both copies still go out with
  their yields, which is the whole point of the double-send. The salvo is also still sent on TCP
  rather than skipped in favour of FIN — it is what closes the server's session *before* the FIN
  arrives, and without it every clean TCP disconnect would report a receive error to the server's
  listeners and across the C ABI to `on_error`.

  **No C ABI change and no wire change** — a `na_client_disconnect` against a stalled peer simply
  returns sooner.

- **A TCP peer that stops reading can no longer stall a send indefinitely, in either direction.**
  Issues #56 and #70. Every send on a TCP connection funnels through one mutex held across a
  blocking write, so a peer whose receive window closes does not merely delay its own audio — it
  parks the writer and, behind it, the heartbeat watchdog and teardown paths whose whole job is to
  notice that the peer is dead. Two gaps are closed together.

  **Server sessions now carry a send deadline at all.** It was previously armed only on the client,
  so a server session's writer could block with nothing to release it, holding a `maxClients` slot
  and wedging the receive thread's inline control replies for every other client on the connection.
  It is now armed once where the connection is born, which covers both directions.

  **A send deadline is now a bound on the whole write, not on each attempt.** `SO_SNDTIMEO` applies
  per system call, so a write that moves some bytes and then stalls restarts the clock: a peer that
  drip-feeds a little room on a timer stretched the call in proportion to how long it cared to keep
  that up. Measured against a peer draining 32 KB every 40 ms under a 200 ms deadline: 3263 ms, 16.3
  times the deadline, versus 201 ms once the whole-call budget is spent across the retries.

  **What it does not promise, stated because the pair is easy to over-read.** The bound is the
  configured deadline plus at most one write already in flight — the operating system's timer covers
  a *wait for room*, not a call, so a write that keeps making progress is not interrupted (measured:
  8 MB moved past 4 seconds inside a single call under a 1-second deadline). For audio that residual
  is bounded by one maximum-payload frame. A timed-out send remains fatal to the connection, as it
  already was: the frame on the wire is truncated, so it is not retryable.

  No C ABI change and no wire change. `Socket::setSendTimeout` and `Socket::sendAll` are installed
  C++ headers whose documented contract changed.

- **Losing an audio device mid-stream no longer kills the process — it is reported through
  `on_error`.** Issue #59. `PlaybackStream::write` and `CaptureStream::read` throw
  `DeviceUnavailable` on any mid-stream PortAudio error: a USB codec unplugged, a device the OS
  reclaimed, a sample-rate change forced by another application. All four device-IO worker loops let
  that exception escape, and none of those threads has a handler — the client's two run detached,
  the server's two on plain `std::thread`s — so the throw was `std::terminate`. The process vanished
  and `on_error` never fired, which is the worst available combination: no diagnosis, no chance to
  reconnect, and nothing in the log. On the server it took every connected client down with it, and
  on `na_hamlib_bridge` that is the server dying mid-transmission with the transmitter's PTT keyed.

  All four loops now catch the exception, report it, and return. Returning closes the dead stream by
  RAII and ends only that loop: the **connection and the server stay up**, so a client whose playback
  device disappears keeps its session rather than dropping off the air, and the server's clients hear
  silence instead of a peer that vanished.

  **C-visible consequence:** `on_error` fires where the process previously died, with a message of
  the form `"Playback device lost: <reason>"` or `"Capture device lost: <reason>"`. On the client
  the `client_id` is `"local"`, matching the other client-local faults. On the server it is the
  **empty string** — the shared capture or playback device is the *source*, so its loss affects every
  client equally rather than any one of them, and `""` is the same id the accept-error path already
  uses (`AudioStreamServer.cpp:740`). **No C ABI change** — no new `na_*` symbol, no struct field, no
  wire change — so a C consumer recompiles against nothing new and simply stops losing the process.
  The C++ surface gains two purely additive hooks in installed headers,
  `AudioBroadcaster::setCaptureErrorListener` and `AudioMixer::MixerListener::onPlaybackDeviceError`,
  which the server wires for you; a direct C++ embedder may set them itself.
  Issue #65. `AudioMixer::submitTxAudio` **is** the channel-claim mechanism — there is no separate
  "I want to transmit" call, and `releaseTx` has no production caller — so the 500 ms idle release
  was the entire safety story. A frame the FEC layer rebuilt looked exactly like a live one, which
  meant a repair arriving after the release could silently re-claim the channel for a client that
  had stopped transmitting. On the bridge this is not abstract: `na_hamlib_bridge` keys a real
  transmitter's PTT off `na_server_tx_owner()`, and its default `-R wan` profile is the only
  FEC-enabled preset, so the reconstructed frame could key a radio.

  Provenance now travels with the packet — `FecDecoder` → the ordered queue → `ReceiveResult` →
  the server → the mixer — and the mixer decides the **arbitration verdict** and the **audio
  write** separately. A recovered frame is written only in the one state where it has a consumer
  (the owner's own stream) and never claims, preempts, or denies. A new outcome distinguishes
  *"a repair we declined to act on"* from *"a client asked and was refused"*, so a repair cannot
  spend the client's single per-episode `TX_DENIED`.

  **C-visible consequences**, all of them the disappearance of spurious events: `on_tx_granted`,
  `on_clients_update` and `on_tx_denied` no longer fire for a reconstructed frame. A client that
  legitimately holds the channel is unaffected — its repaired audio is still mixed. One deliberate
  behaviour change beyond that: the idle lease now runs from the last **live** frame, so a talker
  whose tail is carried only by repairs releases up to one FEC block early. That is the direction
  that cannot *extend* a transmission, and it was chosen for that reason.

- **FEC no longer fabricates a duplicate audio frame when an interleaving control packet never
  reaches the decoder, and a repaired transmit frame is no longer silently discarded.** Two defects
  in the same recovery path (issue #55).

  *The fabrication.* A block's parity is read as the range `[startSeq, startSeq + blockSize)`, which
  is the encoder's block only while the block's audio is contiguous. The existing guard for that
  (issue #23) inspected only packets **present** in the range, on the premise that an interleaving
  control message fills its slot and so is visible. Control reliability falsifies the premise: a
  `CONTROL_ACK` draws a sequence from the shared counter and is consumed before the reorder/FEC
  pipeline, so its slot is simply absent and reads exactly like a lost audio packet. With one such
  slot the XOR remainder is the block member the stranger displaced — a frame the application
  **already received** — so the decoder delivered a byte-exact duplicate of it and counted a repair.
  Measured with **no packet loss at all**: six frames delivered for five sent. The decoder now
  declines when it holds evidence of the displaced *member* (an audio packet at or past the range
  end, in the repair cache or already evicted from it) rather than looking for the stranger. It
  deliberately does **not** consult the parity frame's own sequence number, which §3.4 leaves free
  for a conforming sender to choose; keying on it would silently disable FEC against a legal peer.

  *The discarded repair.* Recovered packets were stamped `AUDIO_RX` unconditionally, though the
  encoder is driven from both audio send paths and a client protects its `AUDIO_TX` lane the same
  way. A repaired transmit frame therefore arrived at the server typed as receive audio, hit the
  `default` branch of the session's receive switch and was dropped — while the repair counter had
  already incremented. Recovery now carries the block's own audio type, read off a present member;
  a range whose members disagree, or whose declared block size is outside the encoder's validated
  range, is declined rather than guessed. The recovered packet is delivered locally and never
  serialized, so this is a delivery fix, not a wire change.

  The two shipped together deliberately: fixing the type alone would have promoted the fabricated
  frames from "silently dropped" to "submitted to the transmit mixer". No ABI change, no wire change.

- **FEC no longer "recovers" a slot the decoder itself discarded, which delivered a duplicate audio
  frame at zero packet loss.** The decoder emits each audio packet to the application on arrival and
  keeps only a copy for repair. When retention released that copy — either the idle timeout or the
  packet cap — a later parity had no way to tell the resulting hole from a packet the peer never
  sent: with exactly one such hole it ran the XOR, whose remainder is precisely the released frame,
  and delivered a **byte-exact duplicate of audio the application already had** while counting it as
  a repair. Measured on `NA_RELIABILITY_UDP_WAN` with **no packet loss at all**: six frames
  delivered for five sent.

  This was reachable on any client whose peer paused mid-block for longer than the derived
  pending-block bound (490 ms on `UDP_WAN`), which for a PTT-gated transmit lane is ordinary rather
  than exceptional. The decoder now records the sequences retention destroys and declines recovery
  for any parity range containing one, counting it in the existing "unreconciled" total alongside
  the interleaved-control case it already tracked. A genuinely missing packet is still recovered —
  only a slot the decoder itself released is refused, which costs that block its repair and never
  emits a frame the sender did not send. No ABI change, no wire change.

- **A server-side UDP connection no longer strands a reordered packet, or holds a repair cache
  forever, while traffic is paused** — the reorder buffer and the FEC decoder both hold packets
  against a deadline, and neither owns a thread, so each expires only when a caller ticks it. A
  client-owned connection ticked both from the limb that observes "no datagram arrived within the
  deadline". The server-fed connection — the one
  `na_server_set_reliability_profile(NA_RELIABILITY_UDP_WAN)` builds — had no such limb: its only
  tick was on packet *arrival*, which cannot fire during a pause, and it never ticked the FEC
  decoder at all.

  Two consequences, both measured on identical config against the client-owned sibling. A packet
  held behind a sequence gap was **never delivered** while traffic was paused: after 600 ms of
  silence with a live consumer polling, the server-fed connection had delivered `[0]` where the
  client delivered `[0,2]`. That is lost **audio**, not merely a lost repair opportunity. And the
  decoder's derived idle bound never ran: after 1470 ms — three times `UDP_WAN`'s derived
  490 ms — the server released **0** of 12 held packets against the client's **12**, so the
  repair cache was bounded only by its packet cap, and a timeout discard could not be
  distinguished from a cap eviction.

  Both hold-timeouts are now ticked from the server-fed no-data limb as well, making the two
  receive modes symmetric; each measurement above now reports the client's value on both. The
  ordering is load-bearing and documented: the ordered queue is polled *first* and the pipeline
  mutex taken only afterwards, because holding it across the poll's deadline would stall the demux
  thread for the whole timeout. No ABI change, no wire change. Pinned by two arms that assert the
  two modes *agree* rather than asserting a number per mode, since the defect was a divergence.

- **FEC no longer discards a block whose own packets are still arriving** — on
  `NA_RELIABILITY_UDP_WAN`, the only profile with FEC enabled, an arrival stall could destroy a
  whole block's repair. The decoder held each block's audio for a fixed 120 ms measured from the
  block's *creation*, so a block still filling normally could be dropped mid-flight and the parity
  then found every slot missing. Measured against a 5-packet block at the profile's 10 ms cadence
  with one packet lost, a single **91 ms** stall was enough — unremarkable on the WAN link this
  profile exists for.

  The bound is now measured from the **last packet added** to the block, so it expresses a maximum
  arrival *gap* and no longer tightens as blocks lengthen, and its size is derived from the stream
  shape the profile declares — block period + reorder hold + the jitter buffer's own maximum —
  rather than from a constant. `UDP_WAN` derives 490 ms against the previous 120, moving the
  smallest fatal stall from 91 ms to 471 ms; the end-to-end recovery arm's cliff moves from 48 ms
  to ~250 ms of injection cadence. The tolerance is a gap between packets that *arrive*, so a run
  of N consecutive losses consumes N+1 frame intervals of it.

  Retention is additionally capped by packet count, so a peer that never sends parity cannot grow
  the buffer without bound. Discarded packets are now counted rather than vanishing silently —
  previously nothing was emitted and no counter moved, which is why this cost two sessions to
  diagnose. The count is not yet exposed through the C ABI. No ABI change, no wire change; audio
  delivery is unaffected, since packets are emitted on arrival and the block retains only copies
  for repair.

- **`na_client_stats` no longer claims two counters can move on a client when they cannot** — the
  struct's contract listed `control_retransmits` as live on `NA_RELIABILITY_UDP_LAN` / `_FT8`, and
  described `queue_drops` as reporting a consumer too slow to drain the queue. Neither is reachable
  through `na_client_*` on any profile. Only a *critical* control type is tracked for retransmission
  and the sole critical message a client sends is `DISCONNECT`, dispatched after the thread that
  pumps the retransmit sweep has exited; and a client fills and drains its ordered queue from one
  thread — the receive path empties the queue before reading the socket — so the queue cannot reach
  its 2048-packet cap, and a slow consumer instead loses audio in the kernel's socket buffer, which
  no counter reports. Both fields still exist, still read 0, and are now documented as carrying no
  information on a client (they remain live on the server side of the same class). No ABI change:
  the struct layout, field order and sizes are unchanged. `crc_errors`, by contrast, is genuinely
  live on a client and is now covered by a test that corrupts datagrams in flight rather than
  dropping them — a dropped datagram never arrives, so no amount of loss testing could move it.
- **FEC no longer presents a mis-reconciled block as recovered audio** — the XOR parity header
  carries a *count* of audio packets, which the decoder read as the contiguous sequence range
  `[startSeq, startSeq + blockSize)`. That is the encoder's block only while the block's audio
  packets are consecutive, and the connection's sequence counter is shared with control and
  heartbeat traffic: one control message sent between two audio packets of a block displaced the
  block's last packet out of the range. A single loss in such a block then emitted the lost frame
  XORed with the displaced one — a whole frame of wrong samples delivered as recovered audio, and a
  quiet one, since without loss the block still looked complete and XOR of two same-signal S16
  payloads almost always stays inside the source's amplitude range. Such a block is now **declined**
  (counted by `FecDecoder::fecBlocksUnreconciled`) and its lost packet stays lost, exactly as with
  FEC disabled. Affects UDP profiles with FEC enabled; documented as limitation **R5** in
  `docs/protocols.md`.

### Added
- **Client-side reliability counters on the C ABI** — `na_client_get_stats()` fills one
  `na_client_stats` with the current connection's transport and reliability totals, including
  `packets_recovered_by_fec`, `fec_blocks_unreconciled`, `packets_reordered`, `crc_errors` and the
  jitter estimate. These were private to `AudioStreamClient`, reachable through neither an accessor
  nor a listener, so a consumer could *enable* loss recovery but not observe it: showing that FEC
  repaired anything meant inferring it from delivered-byte parity against a separate no-loss control
  run, which cannot distinguish "FEC recovered 29 packets" from "nothing was dropped this time".
  `AudioStreamClient::stats()` exposes the same snapshot to C++ consumers.
  `packets_lost` / `packets_out_of_order` / `packet_loss_rate` report **-1 for "not measured"**
  rather than 0: the sequence-gap tracker runs only when no reorder buffer is engaged and every UDP
  profile configures one, so a 0 there would read as "nothing was lost".
- **Client-side reliability profiles on the C ABI** — `na_client_set_reliability_profile()` applies a
  `na_reliability_profile` (transport + framing + FEC / reorder / adaptive jitter / control-ARQ) in
  one call, before connect. This is the only way to enable a client's loss-recovery layer:
  `na_client_set_transport()` selects the transport and nothing else, so a client configured with it
  alone ran UDP with the whole reliability layer off and discarded every FEC parity packet the server
  sent. Selecting a UDP profile makes a separate `na_client_set_transport()` call unnecessary; both
  setters write the transport and the last one wins.
- **Client-side TX audio injection on the C ABI** — `na_client_inject_tx_audio()` feeds a client's
  TX path directly, and `na_client_set_tx_inject()` enables it before connect. Together they make
  the `na_client_*` surface symmetric with `na_server_inject_audio()` on the RX side, so a headless
  client with no capture device (the `NA_CLIENT_BACKEND_NULL` backend, which cannot capture) can
  originate TX audio. Injected audio obeys the same PTT gate as captured audio.
- **Server-side audio format and UDP reliability profiles on the C ABI** —
  `na_server_set_audio_format()` and `na_server_set_reliability_profile()` let a C consumer configure
  what the server puts on the wire, which until now was reachable only from C++.
  `na_server_set_transport()` could already select UDP, but the FEC / reorder / adaptive-jitter /
  control-ARQ settings that make UDP worth selecting lived solely in the C++ `AudioStreamConfig`
  presets, and the `na_server_*` surface had no format setter at all, so the advertised format was
  pinned to its 48000 / 16 / 2 default. A C or Hamlib consumer could therefore stand up a UDP server
  with naudio's entire resilience stack switched off, and could not serve mono. Both are pre-start
  configuration like the rest of the `na_server_set_*` family, returning `NA_ERR_INVALID` on a NULL
  server or after `na_server_start()`.
  - `na_server_set_audio_format(server, sample_rate, bits_per_sample, channels)` sets the format the
    server advertises and broadcasts. **The v1 wire carries signed 16-bit PCM**, so `bits_per_sample`
    must be 16 and `channels` 1 or 2, with `sample_rate` > 0; any other combination is rejected with
    `NA_ERR_INVALID` rather than silently converted. naudio does not resample or change channel
    count, so the bytes fed to `na_server_inject_audio()` — and those delivered to
    `na_server_tx_audio_cb` — must already match this layout.
  - `na_server_set_reliability_profile(server, profile)` applies one `na_reliability_profile` —
    transport, framing and the whole reliability bundle — in a single call. **Selecting a UDP
    profile makes a separate `na_server_set_transport()` call unnecessary**, because the profile
    carries the transport with it. It leaves the audio format and max-clients untouched, so it and
    `na_server_set_audio_format()` **compose order-independently** — neither undoes the other
    whichever is called first, and the same holds for `na_server_set_max_clients()`.
    `na_server_set_transport()` is the one exception: it and the profile setter both write the
    transport, and the last one called wins.
  The `na_reliability_profile` enum arrives with these setters and is shared with the client setter
  above: `NA_RELIABILITY_DEFAULT` (the plain defaults — TCP, with FEC, reorder, jitter and
  control-ARQ all off), `NA_RELIABILITY_UDP_LAN`, `NA_RELIABILITY_UDP_WAN` (the resilient
  remote-operating profile: XOR FEC + adaptive jitter + reorder + control-ARQ) and
  `NA_RELIABILITY_UDP_FT8`. Both ends must select the same one — a server on
  `NA_RELIABILITY_UDP_WAN` sends parity packets that a client left on any other profile receives and
  discards, with no loss recovery and no error.
- **Initial release of the naudio C/C++ audio-streaming toolkit.**
  - **net-audio wire protocol, spec v1** — the frozen `0xAF01` frame contract: versioned framing
    with per-frame CRC32, a 32-bit sequence + reorder buffer, adaptive (RFC-3550-style) jitter
    buffering, XOR forward error correction, and control-message ARQ. Pinned by a language-neutral
    golden-vector conformance suite.
  - **Multi-tenant streaming server and client** over TCP, UDP, or DUAL transport, with per-client
    TX arbitration (priority mixer + idle-release).
  - **Stable C ABI** (`include/naudio.h`) over a C++ implementation — the `na_client_*` and
    `na_server_*` surfaces plus device I/O, exported with hidden visibility + `SOVERSION`. A C++
    API (`naudio::`) is also available for consumers that link the static archives.
  - **Cross-platform device layer** — enumeration / selection and format probing over PortAudio,
    plus a virtual-audio setup guide for BlackHole (macOS), VB-CABLE (Windows), and a PulseAudio
    null-sink (Linux), 48 kHz first-class.
  - **CMake packaging** — `find_package(naudio)` and pkg-config resolve both the C ABI and the C++
    API; PortAudio and GoogleTest resolve from the system or via FetchContent.
  - **Example clients** — a "play to speakers" client in C, C++, Python, Java, and Rust, each
    driving only the public C ABI, plus a demo streaming source.
  - **Tooling** — `na_audio_daemon`, a hardware-smoke driver that exercises the real PortAudio
    capture / playback path the hardware-free test suite cannot reach.
