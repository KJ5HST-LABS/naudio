# Changelog

All notable changes to this project are documented here.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Added
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
  (stats, and each device element) or ignored (callbacks). Nothing has
  been tagged and `SOVERSION` is still 0, so no released consumer exists to break.
  Consumers that bind the ABI by hand rather than through the header — the Python, Java and Rust
  example clients — need the new argument at their `na_enumerate` call sites; their `na_device`
  layouts are byte-for-byte unchanged.
  The resulting promise is now written down for packagers as **"Binary compatibility"** in
  `README.md`, condensed at the top of `include/naudio.h`: what the declared size guarantees in each
  version direction, and the append-only growth rule the `NA_*_SIZE_V1` floors encode. An older
  library's zero-filled tail is defined but reads the same as a genuine zero; the version accessors
  added below are what tell the two apart.

### Fixed
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
