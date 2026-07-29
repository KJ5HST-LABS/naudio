# Changelog

All notable changes to this project are documented here.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Fixed
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
  `packets_recovered_by_fec`, `fec_blocks_unreconciled`, `packets_reordered`, `queue_drops` and the
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
