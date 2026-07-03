# net-audio Audio Streaming Protocol — Specification v1

**Status:** Stable / frozen wire contract (`0xAF01`, version byte = 1).
**Spec version:** 1.0 (2026-06-27).
**Scope:** the *audio* half of a radio-streaming toolkit. I/Q streaming is explicitly out of scope (see §1.2).
**Provenance:** this document is the normative, field-by-field definition of the `0xAF01` audio wire. Every normative value here is implemented and pinned by the language-neutral golden-vector conformance suite (see §12), so the wire is byte-deterministic and independently checkable. Where the toolkit *plan* describes capabilities that are **not** in the v1 wire, they are isolated in §13 (Proposed extensions) and are explicitly **non-normative**.

---

## 1. Introduction

### 1.1 Purpose

net-audio is a low-latency, multi-client streaming protocol for **demodulated receiver audio** and **transmit audio** between a station host (which has the radio) and one or more clients (decoders, listeners, waterfalls, remote operators). It adds, on top of raw PCM transport, the reliability and multi-tenancy machinery that bare PCM-over-UDP lacks: adaptive jitter buffering, XOR forward error correction, packet reordering, control-message ARQ, and priority-based transmit arbitration.

This specification is **language-neutral**. It defines the bytes on the wire and the algorithms that produce and consume them, so that conformant implementations in any language interoperate. It is the single source of truth; the conformance suite (§12) is its executable companion.

### 1.2 The audio / I-Q boundary (normative scope)

This spec covers **audio only** — PCM samples of demodulated receiver output or transmit input. It does **not** define an I/Q (baseband) streaming format. I/Q is a separate concern owned upstream (Hamlib's `rig_stream_*` work, issue #1940). A station that streams both audio and I/Q runs two independent transports; this spec governs only the audio one. See §10 for the capability-parity relationship with Hamlib's audio side.

### 1.3 Terminology

- **Server** — the endpoint attached to the radio. Broadcasts RX audio to many clients; mixes/arbitrates TX audio from clients (1→many out, many→1 in).
- **Client** — any consumer/producer of the stream (decoder, listener, waterfall, remote op).
- **Frame** — one complete on-the-wire packet: header + payload + CRC.
- **Packet** — synonym for frame at the wire layer; also the in-memory object.
- **Control message** — a structured message carried *inside* the payload of a `CONTROL` frame (§6).
- MUST / SHOULD / MAY per RFC 2119.

### 1.4 Conventions

- **All multi-byte integer fields on the wire are big-endian (network byte order)** unless a field is explicitly noted otherwise. (The one big-endian exception is the *audio sample payload*, which is little-endian PCM — see §5.1. The framing header and all control-message scalar fields are big-endian.)
- Integers are two's-complement signed unless marked unsigned (`u8`/`u16`/`u32`).
- Hex literals are written `0xAF01`. Byte sequences are written space-separated, e.g. `AF 01`.
- "nanos" = nanoseconds; "ms" = milliseconds.

---

## 2. Transport layer

### 2.1 Transport types

A server is bound in one of three transport modes:

| Mode | Semantics |
|------|-----------|
| **TCP** | Reliable, ordered byte stream. Default. No reliability layer needed (TCP provides it). |
| **UDP** | Datagram, lower latency, no head-of-line blocking. Enables the reliability layer (§8). |
| **DUAL** | TCP **and** UDP bound to the **same port number**, accepting both kinds of client simultaneously. |

A **single client uses a single transport for all of its frames** — control, RX audio, TX audio, heartbeat, and (UDP only) FEC parity all flow over that one connection. DUAL does **not** split control-over-TCP / audio-over-UDP for one client; rather, it lets *different* clients pick different transports against one listening port.

### 2.2 DUAL binding

TCP and UDP ports are independent at the OS level, so a DUAL server binds a TCP listener and a UDP socket to the same numeric port. Binding order is TCP first (to resolve an ephemeral port-0 request to a concrete port), then UDP to that resolved port; if the UDP bind fails the TCP listener MUST be rolled back/closed. Accept loops poll TCP then UDP. Per-transport client IDs SHOULD be namespaced (`tcp-…`, `udp-…`).

### 2.3 UDP client registration

On UDP, a client is registered **only** upon receipt of a valid, deserializable `CONNECT_REQUEST` control frame (§6.2) from its source address. Unsolicited or malformed datagrams MUST NOT register a client. (This closes a trivial spoof/garbage-registration vector.)

### 2.4 Framing

- **TCP:** frames are length-delimited by the header's 2-byte payload-length field. A reader consumes the fixed 19-byte header, then `payloadLength` payload bytes, then 4 CRC bytes. A robust reader maintains a resync state machine: on a bad magic it scans forward byte-by-byte for the next `0xAF01`; on an oversized payload-length it skips; on `MAX_CONSECUTIVE_CRC_ERRORS` (= 5) consecutive validation failures it MAY abort the connection. A successful decode resets the consecutive-error counter.
- **UDP:** one frame per datagram. Payloads SHOULD be kept ≤ `UDP_MAX_PAYLOAD` (= 1400 bytes) to avoid IP fragmentation (advisory, not enforced by the codec).

### 2.5 Ports

- **Default service port: `4533`** (TCP, UDP, or both for DUAL).
- There is **no discovery mechanism in this spec** (no multicast beacon, no broadcast probe). Service discovery, if any, is provided out-of-band by the host application. (A UDP discovery port is *proposed* in §13.4.)

---

## 3. Packet (frame) format — `0xAF01`

Every frame is: **19-byte fixed header + payload (0…N bytes) + 4-byte CRC32**. Total frame size = `19 + payloadLength + 4`.

### 3.1 Header layout (19 bytes, big-endian)

| Offset | Size | Type | Field | Notes |
|-------:|-----:|------|-------|-------|
| 0 | 2 | u16 | **Magic** | Constant `0xAF01`. |
| 2 | 1 | u8 | **Version** | Constant `1` for this spec. |
| 3 | 1 | u8 | **Type** | Packet type, §3.2. |
| 4 | 1 | u8 | **Flags** | Bitfield, §3.3. |
| 5 | 4 | i32 | **Sequence** | Monotonic counter, §3.4. |
| 9 | 8 | i64 | **Timestamp** | Monotonic nanos, §3.5. |
| 17 | 2 | u16 | **Payload length** | Length of the payload in bytes. Read as unsigned. |

Immediately following the header:

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 19 | `payloadLength` | **Payload** | Type-specific, §5. |
| 19 + `payloadLength` | 4 | **CRC32** | §3.6. |

`MAX_PAYLOAD` = **16384** bytes. A frame whose payload-length field exceeds this MUST be rejected.

### 3.2 Packet types

| Name | Value | Direction | Meaning |
|------|------:|-----------|---------|
| `AUDIO_RX` | `0x00` | server → client | Demodulated receiver audio. |
| `AUDIO_TX` | `0x01` | client → server | Transmit audio (subject to arbitration, §7). |
| `CONTROL` | `0x02` | both | Carries a `ControlMessage` (§6). |
| `HEARTBEAT` | `0x03` | both | Keep-alive (§6.5). |
| `FEC_PARITY` | `0x04` | server → client (UDP) | XOR parity packet for a FEC block (§5.2, §8.2). |

An unknown type value MUST cause the frame to be rejected (decode returns "no packet").

> **Implementer note / historical caveat.** Some older prose documentation (e.g. an early README table) listed the type values off-by-one (`AUDIO_RX=0x01`, …) and omitted `FEC_PARITY`. **The values in this table are authoritative** (`AUDIO_RX=0x00 … FEC_PARITY=0x04`).

### 3.3 Flags

A bitfield (currently 2 bits defined):

| Bit | Name | Meaning |
|----:|------|---------|
| `0x01` | `COMPRESSED` | Payload is compressed (reserved; transports today carry uncompressed PCM). |
| `0x02` | `LOW_BANDWIDTH` | Stream is using a reduced (e.g. 12 kHz) sample rate. |

Undefined bits MUST be sent as 0 and SHOULD be ignored on receipt.

### 3.4 Sequence number

- A single **monotonically increasing 32-bit counter**, shared across *all* packet types from a given sender (RX, TX, control, heartbeat, parity all draw from one counter that starts at 0).
- It is a **packet counter**, not a sample index or a millisecond clock.
- **Wraparound:** plain two's-complement overflow (`0x7FFFFFFF` → `0x80000000`). The reliability layer treats the sequence as **unsigned** for ordering/windowing comparisons (§8.3); a frame's raw 32-bit value is carried verbatim on the wire.

### 3.5 Timestamp

- An **8-byte signed monotonic timestamp in nanoseconds**, sampled by the sender when the packet object is created.
- It is meaningful **only as a delta** for jitter estimation (§8.1); it is **not** wall-clock/epoch time and MUST NOT be interpreted as such, and MUST NOT be compared for equality across implementations.
- The clock origin is implementation-defined (e.g. a monotonic process clock, or Unix-epoch nanoseconds); any conforming origin round-trips the field byte-identically. **Conformance vectors pin the timestamp to a fixed value** so frames are byte-deterministic (§12.3).

### 3.6 CRC32

- Algorithm: **CRC-32/ISO-HDLC** — the standard CRC-32 used by zlib/gzip/PNG (reflected polynomial `0xEDB88320`, init `0xFFFFFFFF`, final XOR `0xFFFFFFFF`, input and output reflected). Known-answer: `CRC32("123456789") = 0xCBF43926`.
- **Coverage:** the 19 header bytes **plus** all `payloadLength` payload bytes. The CRC field itself is **not** covered.
- **Position:** the final 4 bytes of the frame, at offset `19 + payloadLength`.
- **Endianness:** big-endian (the 32-bit CRC value is written most-significant-byte first).
- A frame whose recomputed CRC does not equal the carried CRC MUST be rejected.

### 3.7 Decode validation order (normative)

A decoder MUST reject a frame (yield "no packet", not throw) on any of the following, checked in order:

1. **Too short:** total length `< 19 + 4` (= 23).
2. **Bad magic:** bytes [0,2) ≠ `0xAF01`.
3. **Unknown type:** type byte not in §3.2.
4. **Bad length:** `payloadLength > MAX_PAYLOAD`, or total length `< 19 + payloadLength + 4`.
5. **CRC mismatch:** recomputed CRC ≠ carried CRC.

The **version byte is read but not validated** at the frame layer in v1 (version negotiation/rejection is a *control-layer* concern — `CONNECT_REJECT` reason `VERSION_MISMATCH`, §6.2). A future revision MAY add frame-layer version gating; v1 decoders accept any version byte that otherwise passes 1–5. This is documented behavior, not an accident — note it when hardening.

---

## 4. Encode procedure (normative)

To serialize a frame:

1. Allocate `19 + payloadLength + 4` bytes, big-endian.
2. Write magic `0xAF01`, version `1`, type, flags, sequence (i32), timestamp (i64), payloadLength (u16).
3. Write the payload bytes.
4. Compute CRC-32 over bytes `[0, 19 + payloadLength)` and append it as a big-endian u32.

---

## 5. Payload formats

### 5.1 Audio payload (`AUDIO_RX`, `AUDIO_TX`)

At the framing layer the audio payload is an **opaque byte array**; the frame header carries no per-packet format descriptor. The format is established once per session by the handshake (§6.3) and is, in v1:

- **Encoding:** linear PCM, **signed, little-endian** (`PCM_SIGNED`, LE).
- **Sample size:** 16 bits/sample (the negotiated default; the AUDIO_CONFIG field is a byte and can carry other depths, but v1 implementations use 16).
- **Channels:** interleaved. v1 supports **mono (1)** and **stereo (2)**. Mono is conveyed by the channel count; a mono source MAY be duplicated to stereo by the producer.
- **Frame duration:** a session property (default 20 ms; 10 ms for UDP presets). For 48 kHz/16-bit/stereo a 20 ms audio frame is 3840 bytes (960 samples × 2 ch × 2 bytes).

> v1 transports audio **byte-for-byte unchanged** from server to every client (the "native broadcast" path, §6.4). Per-subscription resampling / channel remap / format conversion is **not** part of v1; it is proposed in §13.1. Sample-rate/channel/format *conversion building blocks* live in companion DSP code, not in this wire spec.

### 5.2 FEC parity payload (`FEC_PARITY`)

A parity frame's payload is:

| Offset | Size | Type | Field |
|-------:|-----:|------|-------|
| 0 | 4 | i32 (BE) | `startSequence` — sequence of the first audio packet in the protected block. |
| 4 | 1 | u8 | `blockSize` (N) — number of audio packets the parity protects. |
| 5 | `maxPayloadLen` | bytes | `xorData` — byte-wise XOR of the N audio payloads. |

`xorData[j] = payload₀[j] XOR payload₁[j] XOR … XOR payload_{N-1}[j]`, where payloads shorter than `maxPayloadLen` (the longest payload in the block) are zero-padded on the right for the XOR. See §8.2 for encode/recover semantics.

### 5.3 Control payload (`CONTROL`) — `ControlMessage`

A `CONTROL` frame's payload is a `ControlMessage`:

| Offset | Size | Field |
|-------:|-----:|-------|
| 0 | 1 | **Control type** (u8, §6.1) |
| 1 | M | **Type-specific data** (§6) |

Total control-message length = `1 + M`. An empty payload (length < 1) or an unknown control-type byte MUST be rejected. All scalar fields inside control-message data are **big-endian**.

---

## 6. Control plane

### 6.1 Control message types

| Name | Value | Name | Value |
|------|------:|------|------:|
| `CONNECT_REQUEST` | `0x01` | `LATENCY_RESPONSE` | `0x23` |
| `CONNECT_ACCEPT` | `0x02` | `STATS_UPDATE` | `0x30` |
| `CONNECT_REJECT` | `0x03` | `TX_GRANTED` | `0x40` |
| `AUDIO_CONFIG` | `0x04` | `TX_DENIED` | `0x41` |
| `STREAM_START` | `0x10` | `TX_PREEMPTED` | `0x42` |
| `STREAM_STOP` | `0x11` | `TX_RELEASED` | `0x43` |
| `STREAM_PAUSE` | `0x12` | `CLIENTS_UPDATE` | `0x44` |
| `STREAM_RESUME` | `0x13` | `NACK` | `0x50` |
| `HEARTBEAT` | `0x20` | `CONTROL_ACK` | `0x51` |
| `HEARTBEAT_ACK` | `0x21` | `ERROR` | `0xFE` |
| `LATENCY_PROBE` | `0x22` | `DISCONNECT` | `0xFF` |

> `STREAM_START/STOP/PAUSE/RESUME` and `STATS_UPDATE` are defined in the vocabulary but are **not dispatched** by v1 endpoints (reserved). Implementations MUST tolerate (ignore) them if received.

`CONNECT_REJECT` reason codes: `BUSY 0x01`, `VERSION_MISMATCH 0x02`, `FORMAT_NOT_SUPPORTED 0x03`, `AUTH_FAILED 0x04`, `REJECTED 0xFF`.

### 6.2 Handshake messages

**`CONNECT_REQUEST` (0x01) data:**

```
version            u8
nameLen            u8
name               name[nameLen]           (UTF-8)
configFlag         u8                       (0 = no buffer prefs; nonzero = present)
  if configFlag != 0:
    bufferTargetMs u16
    bufferMinMs    u16
    bufferMaxMs    u16
    clientInfoLen  u8
    clientInfo     clientInfo[clientInfoLen]
```

`clientInfo` sub-encoding: `callsignLen u8, callsign…, nameLen u8, name…, locationLen u8, location…` (each ≤ 255 bytes, UTF-8).

> **v1 negotiation scope:** the request carries **only buffer-timing preferences** — not sample rate, channels, or bit depth. A client inherits the server's native audio format; it cannot request a different one in v1. Format negotiation is proposed in §13.1.

**`CONNECT_ACCEPT` (0x02):** empty data.

**`CONNECT_REJECT` (0x03) data:** `reason u8, msgLen u8, msg…` (UTF-8).

**`AUDIO_CONFIG` (0x04) data (14 bytes, "new" form):**

```
sampleRate       u32
bitsPerSample    u8
channels         u8
frameDurationMs  u16
bufferTargetMs   u16
bufferMinMs      u16
bufferMaxMs      u16
```

A backward-compatible **8-byte legacy form** omits the three trailing buffer shorts (sampleRate, bits, channels, frameDurationMs only); decoders MUST accept both lengths.

### 6.3 Connect sequence

```
Client                         Server
  | --- CONNECT_REQUEST ------> |   (UDP: also registers the client, §2.3)
  | <-- AUDIO_CONFIG ---------- |   (server's native format + accepted buffer prefs)
  | <-- CONNECT_ACCEPT -------- |   (or CONNECT_REJECT with a reason)
  |                             |   server registers client with broadcaster + mixer
  | <-- CLIENTS_UPDATE -------- |   (roster broadcast to all)
  | <== AUDIO_RX stream ======> |
```

On accept, the client applies **only the format fields** of AUDIO_CONFIG into its existing config (an in-place merge that preserves its own transport/FEC/reorder/jitter settings — it MUST NOT wholesale-replace its config from AUDIO_CONFIG).

### 6.4 RX broadcast / TX receive

- **RX broadcast:** the server sends the identical RX audio bytes (same buffer, offset, length) to every connected client (1→many). v1 performs no per-client transformation.
- **TX receive:** each client's `AUDIO_TX` payloads are forwarded into the mixer (§7).

### 6.5 Heartbeats

- **TCP:** heartbeat interval 5 s, connection timeout 10 s.
- **UDP:** heartbeat interval 3 s, connection timeout 8 s.
- A receiver of a `HEARTBEAT` control replies with `HEARTBEAT_ACK`. (A `HEARTBEAT` *frame* — packet type `0x03` — is the transport-level keep-alive; the `HEARTBEAT`/`HEARTBEAT_ACK` *control messages* are the application-level liveness exchange.)

### 6.6 Roster (`CLIENTS_UPDATE`, 0x44)

Broadcast on every connect, disconnect, and TX-owner change. Data:

```
clientCount   u8
maxClients    u8
txOwnerLen    u8
txOwner       txOwner[txOwnerLen]          (UTF-8 client id; empty = no TX owner)
numClients    u8
  repeated numClients times:
    idLen     u8, id…                       (UTF-8)
    infoLen   u8, clientInfo…               (callsign/name/location, as §6.2)
```

### 6.7 Latency probe / disconnect

- `LATENCY_PROBE` (0x22) / `LATENCY_RESPONSE` (0x23) data: `timestamp i64` (echoed back to measure RTT).
- `DISCONNECT` (0xFF): empty data; graceful teardown.
- `ERROR` (0xFE): raw UTF-8 message bytes, **no length prefix** (consumes the remainder of the data).

---

## 7. TX arbitration

Many clients may send `AUDIO_TX`; exactly one may own the transmit channel at a time. Arbitration is **mixer-side and implicit**:

- A client **requests TX implicitly** by sending `AUDIO_TX` frames — there is no explicit "request TX" control message in v1.
- **Priority levels:** `LOW(0)`, `NORMAL(1)`, `HIGH(2)`, `EXCLUSIVE(3)`. Preemption is strict-greater: a submitter preempts the current owner only if its level is numerically greater.
- **Claim rules:** first submit claims a free channel; the same owner keeps it while active; a higher-priority submitter preempts; otherwise the submit is rejected.
- The server emits `TX_GRANTED` / `TX_DENIED` / `TX_PREEMPTED` / `TX_RELEASED` accordingly (`TX_DENIED` is sent once per denial episode, not per frame, to avoid spam).
- **Idle-release:** the owner's channel is released after `txIdleTimeoutMs` (default **500 ms**) without TX activity. Release is driven both by the playback loop and an independent periodic check (so a server with no local playback device still releases).

> **v1 limitation (documented):** session priority is fixed at `NORMAL`; there is **no control message for a client to set or raise its priority** over the wire. In practice v1 arbitration is therefore first-come-holds with idle-release. Client-settable priority is a proposed extension (§13.3).

---

## 8. Reliability profiles

The reliability layer applies on **UDP** (TCP gets ordering/reliability from the transport itself and reports zero for these stats). Each component is independently enable-able and parameterized per §9.

### 8.1 Adaptive jitter estimator (RFC 3550-style)

Maintains a smoothed inter-arrival jitter and derives an adaptive playout-buffer target.

**State:** `jitterMs` (init 0.0), `adaptiveTargetMs` (init = `minMs`), previous arrival/send nanos, `packetCount`, `lastRampDownPacket`. The first packet only establishes the baseline (no update).

**Per-packet update** (using send timestamp from the frame, arrival from the local clock; both in nanos):

```
arrivalDelta   = arrivalNanos - prevArrivalNanos
sendDelta      = sendNanos    - prevSendNanos
jitterSampleMs = |arrivalDelta - sendDelta| / 1_000_000
jitterMs      += (jitterSampleMs - jitterMs) / 16          # EMA, gain = 1/16
```

**Target derivation:**

```
desired = ceil(jitterMs * multiplier + minMs)              # multiplier default 3.0
desired = clamp(desired, minMs, maxMs)
```

Note `minMs` is **added inside** the formula (a floor baked into the value) *and* used as the clamp lower bound; with zero jitter the target equals `minMs`.

**Asymmetric ramp:**
- **Ramp up — immediate:** if `desired > adaptiveTargetMs`, set target = `desired`.
- **Ramp down — gradual:** decrease by at most **1 ms per `RAMP_DOWN_INTERVAL` (= 100) packets**, never below `desired`.

`getAdaptiveBufferTargetMs()` returns `-1` until at least one packet has been processed.

### 8.2 XOR forward error correction

**Encoder.** Groups N audio packets per parity packet (N = `blockSize`, default **5**, valid range **2…10**). When N payloads have accumulated, emit a `FEC_PARITY` frame whose payload is `[startSeq i32][blockSize u8][xorData]` (§5.2). `xorData` length = the longest payload in the block; shorter payloads are zero-padded on the right for the XOR.

**Decoder.** Audio packets are emitted immediately on arrival *and* retained for recovery. A parity frame's `startSeq`/`blockSize` define the block `[startSeq, startSeq+N)`:
- **0 missing:** parity is redundant; discard.
- **1 missing:** recover it — `lost = xorData XOR (all present payloads in the block)` (truncating each present payload to the parity length). Emit the recovered packet as `AUDIO_RX` with `sequence = missingSeq`.
- **2+ missing:** unrecoverable; emit a gap marker (silence) for each still-missing slot.

A block that does not complete within the **block timeout (default 60 ms)** is flushed (gaps for the missing). Audio packets arriving *before* their parity are held in a pending block (cleared after **2× timeout**).

### 8.3 Packet reorder buffer

Releases packets in ascending (unsigned) sequence order, holding out-of-order arrivals briefly.

- **Parameters:** `windowSize` (≥ 1) and `maxHoldMs` (≥ 0).
- **In-order** (`seq == nextExpected`): emit immediately (zero added latency), advance, then drain any now-contiguous buffered packets.
- **Future** (`seq > nextExpected`): buffer with arrival time; if the buffer reaches `windowSize`, **force-flush**.
- **Late/duplicate** (`seq < nextExpected`): discard (counted).
- **Timeout:** if the oldest buffered packet's age ≥ `maxHoldMs`, force-flush.
- **Force-flush** emits buffered packets in order, emitting a **gap marker (null) for each missing sequence** between `nextExpected` and the highest buffered sequence, then sets `nextExpected = highest + 1`.

Sequence comparison is **unsigned 32-bit** (so the field wraps cleanly).

### 8.4 Control ARQ (`ControlReliability`)

Reliable delivery for *critical* control messages over UDP.

- **Tracking ring:** the last **16** critical control packets (keyed by frame sequence; oldest evicted on overflow).
- **Critical types** (tracked / ACK-required): `CONNECT_ACCEPT, CONNECT_REJECT, AUDIO_CONFIG, STREAM_START, STREAM_STOP, STREAM_PAUSE, STREAM_RESUME, TX_GRANTED, TX_DENIED, TX_PREEMPTED, TX_RELEASED, CLIENTS_UPDATE, DISCONNECT`.
- **Non-critical** (never ARQ'd, to avoid ACK-of-ACK loops): `HEARTBEAT, HEARTBEAT_ACK, LATENCY_PROBE, LATENCY_RESPONSE, STATS_UPDATE, ERROR, CONNECT_REQUEST, NACK, CONTROL_ACK`.
- **ACK:** receiver replies `CONTROL_ACK(seq)`; sender clears the pending entry.
- **NACK:** `NACK(seq)` triggers immediate retransmission of the stored packet.
- **Timeout retransmit:** default timeout **500 ms**; max **3 attempts** total (the initial send counts as attempt 1 → up to **2 retransmits**), then drop.
- `NACK` and `CONTROL_ACK` data are each a single `sequence u32`.

### 8.5 Audio ring buffer (playout)

A locked circular byte buffer between the network and the audio device.

- **Capacity:** default = `bufferMaxMs × 2` worth of bytes.
- **Write (producer):** never blocks; on overflow it **drops the oldest** bytes to make room (counted as an overrun).
- **Read (consumer):** blocks up to a timeout; returns a **partial** read rather than waiting for the full request; a timeout with no data returns 0 (counted as an underrun) — the buffer does **not** self-zero-fill, the caller does (§8.6).

### 8.6 Loss / gap policy (normative behavior of v1)

- The reorder buffer and FEC decoder produce **gap markers (nulls)** for missing sequences internally.
- In v1, lost audio is **realized as silence at playout**: when the ring buffer underruns, the playback path writes a frame of zeros. Raw-PCM consumers that tap the stream receive the exact received payloads with **no silence inserted** for losses (lost frames are simply absent).
- Loss is **surfaced quantitatively** via counters: `packetsLost`, `gapsEmitted`, `fecBlocksFailed`, ring `underrunCount`/`overrunCount`.

> **Honest status for the toolkit goal.** The toolkit plan calls for high-rate/SDR streams to *surface exact gaps rather than silence-fill*. v1 does **not** implement an explicit "gap marker delivered to the application instead of silence" path — gaps become silence at playout and counters elsewhere. Treat per-application gap-marker delivery as a **proposed v1.1 behavior** (§13.2), not an existing guarantee.

---

## 9. Configuration presets

naudio ships the following named presets. (These are the real factory names; earlier plan prose used informal slugs — the names below are authoritative.) All use 48 kHz / 16-bit / stereo unless noted.

| Preset | Transport | Frame ms | Buffer min/target/max ms | Reorder win/hold | FEC | Adaptive jitter | Control ARQ | Notes |
|--------|-----------|---------:|--------------------------|------------------|-----|-----------------|-------------|-------|
| *(default ctor)* | TCP | 20 | 40 / 100 / 300 | off (0) | off | off | off | balanced baseline |
| `lowBandwidth` | TCP | 20 | 40 / 100 / 300 | off | off | off | off | **12 kHz** sample rate |
| `ft8Optimized` | TCP | 20 | 20 / 40 / 100 | off | off | off | off | low-latency digital (TCP) |
| `voiceOptimized` | TCP | 20 | 60 / 120 / 300 | off | off | off | off | SSB voice stability |
| `udpLan` | UDP | 10 | 20 / 40 / 150 | 8 / 20 | off | off | on | low-loss LAN |
| `udpWan` | UDP | 10 | 60 / 120 / 400 | 8 / 40 | **on (N=5)** | **on** | on | lossy WAN — full reliability |
| `udpFt8` | UDP | 10 | 15 / 30 / 80 | 8 / 15 | off | off | on | minimal-latency digital (UDP) |
| `udpIq` | UDP | 10 | 30 / 60 / 200 | 8 / 30 | off | off | on | **192 kHz** SDR rate (~768 KB/s) |
| `dualDefault` | DUAL | 20 | 20 / 40 / 100 | 8 / 20 | off | off | on | TCP+UDP, FT8 buffers |

Shared defaults: port `4533`, max clients `4`, TX idle timeout `500 ms`, jitter multiplier `3.0`, FEC block size `5`, control ARQ max attempts `3`.

---

## 10. Capability-parity audit vs Hamlib audio (#1940)

This section compares net-audio v1 with Hamlib's audio streaming (issue/PR #1940). The two are **complementary, not competing**: #1940 is a C API (`rig_stream_*`) integrated into Hamlib backends, while net-audio is a wire protocol plus a reliability / virtual-audio layer that can sit **on top of** that API. The audit below marks where each carries a capability the other does not — the intent is layering (reuse #1940 for rig-integrated transport; add net-audio's jitter buffer, FEC / reorder / ARQ, and virtual-audio bridge where an application needs them), not replacement. Hamlib retains ownership of I/Q throughout.

**Source/caveat.** This audit is based on the **published #1940 design** (the `rig_stream_*` C API and 32-byte big-endian UDP datagram described in the project plan and the upstream design thread), **not** a read of merged Hamlib source — at the time of writing the network-audio streaming code is not present in the local Hamlib checkout. Treat the Hamlib column as "published design intent"; re-verify against merged code before any formal interop claim.

| Capability | net-audio v1 | Hamlib #1940 (audio, published design) | Parity status |
|------------|--------------|----------------------------------------|---------------|
| Transport | TCP / UDP / DUAL | UDP (32-byte header datagram) | net-audio ⊇ (adds TCP + DUAL) |
| Framing integrity | CRC32 per frame | 32-bit seq only; no frame checksum (relies on UDP) | net-audio has explicit CRC32 |
| Sequence / ordering | 32-bit seq + reorder buffer | per-packet sequence | net-audio adds active reorder |
| Jitter buffering | adaptive RFC-3550 estimator | client concern | net-audio ⊇ (spec'd + shared) |
| Forward error correction | XOR 1/N | not in base PCM design | **net-audio-only** |
| Control ARQ (ACK/NACK) | yes (critical control) | not in base PCM design | **net-audio-only** |
| Multi-client RX fan-out | yes (unicast replication) | yes (one RX stream to several listeners; multicast groups) | **both** (net-audio unicast-replicates; #1940 adds multicast) |
| TX path | full-duplex (RX + TX) | full-duplex (`AUDIO_TX` / `IQ_TX`, timed TX / burst-PTT) | **both** |
| Multi-client TX arbitration | priority mixer + idle-release | not in base design | **net-audio adds arbitration** |
| Virtual-audio bridge | yes (companion) | explicitly "separate library" per upstream | **net-audio-only** (the upstream invitation) |
| Sample formats | PCM_SIGNED 16-bit LE (v1) | CS8/CS16/CF32/CU8 family (their I/Q set) | **GAP — Hamlib richer on formats** |
| Float / 8-bit / unsigned PCM | not in v1 | present in their type set | **GAP → close in §13.1** |
| RF-center / time-anchor metadata | timestamp only (no RF center) | VITA-49-style UTC anchor / center freq | **GAP → close in §13.5** |
| Per-subscription format negotiation | buffer timing only | n/a | proposed §13.1 |

**Net:** net-audio and #1940 are complementary. net-audio contributes the reliability stack (XOR FEC / reorder / control ARQ), an adaptive jitter buffer, per-client TX arbitration, and the cross-platform virtual-audio bridge — the "separate library" pieces upstream invited — **layered on top of** #1940's `rig_stream_*` transport. #1940 leads on **sample-format breadth** and **stream metadata** (RF center / precise time anchor) and owns I/Q. net-audio's `0xAF01` wire is a **distinct** FEC/ARQ transport, not a replacement for #1940's 32-byte UDP datagram: an application can use #1940 for rig-integrated audio and add net-audio's layer where it needs reliability or virtual-audio plumbing. Where net-audio would close its own gaps, the work is additive — format breadth (§13.1) and stream-fact metadata (§13.5) — and neither changes the frozen `0xAF01` framing.

---

## 11. Versioning & compatibility

- The `0xAF01` magic + version byte = 1 frame is a **frozen contract**. Field offsets, sizes, the type enum (0x00–0x04), and CRC semantics MUST NOT change under version 1.
- **Extensions** that preserve the frame layout (new control message types, new AUDIO_CONFIG fields appended after the v1 fields, new flags bits, new presets) are **minor** and backward compatible: unknown control types/flags are ignored; AUDIO_CONFIG already supports length-based forward/backward compatibility (§6.2).
- **Breaking** changes (header layout, type renumbering, CRC change, audio sample-format renegotiation that changes the audio-payload contract) require **bumping the version byte to 2** and a frame-layer version gate (§3.7). Such changes MUST be specified before the first such frame is emitted.
- The jitter, FEC, reorder, and ARQ algorithms and their constants (EMA 1/16, FEC N∈[2,10] default 5, parity header 5 bytes, ARQ ring 16 / timeout 500 ms / 3 attempts) are part of the contract for interoperating reliability and are versioned with the spec.

---

## 12. Conformance

The executable companion to this spec is the **golden-vector conformance suite** in [`conformance/`](../../conformance/). It contains language-neutral, hand-derived known-answer vectors (with CRCs computed independently of the implementation under test) covering the frame codec, CRC, FEC, jitter, reorder, control messages, and presets. A conformance test loads these vectors and validates encode and decode against them.

- **C/C++ reference:** `tests/conformance/ConformanceTest.cpp` (built and run as part of the standard `ctest` suite, test `Conformance.GoldenVectors`).

Passing against the language-neutral vectors is the proof that an implementation matches this spec — the gate every reference client (C, C++, Python, and any other binding) must also pass.

### 12.3 Determinism rule for byte-exact vectors

Because the frame timestamp is sampled at packet-creation time (§3.5), a byte-exact frame vector MUST pin the timestamp to a fixed value (the vectors use `timestamp = 0`). Conformance MUST NOT assert timestamp equality across implementations for live (non-pinned) frames.

---

## 13. Proposed extensions (NON-NORMATIVE — not in v1)

These are the toolkit-plan capabilities that are **not** in the v1 wire. They are recorded here so the spec is honest about the gap and so future versions have a starting point. Nothing in this section is implemented or conformance-tested.

### 13.1 Per-subscription format negotiation + format breadth
Let each client request its own sample rate, channel layout (stereo / mono / VFO-A=L / VFO-B=R / downmix), and sample format (int16 / float32 / int8 / uint8) at subscribe time, with the server transcoding per subscription **or** the client converting locally — both first-class; default = native full resolution (the existing zero-cost identical-broadcast fast path). Requires: extending `CONNECT_REQUEST`/`AUDIO_CONFIG` to carry the requested format, a `sampleFormat` enum, and the conversion building blocks. Closes the §10 format-breadth gap.

### 13.2 Application-level gap markers
Deliver explicit gap markers to high-rate/SDR consumers instead of silence-filling, so decoders see exact discontinuities. Requires a delivery path that forwards the reorder/FEC null markers to the application rather than collapsing them to silence at playout (§8.6).

### 13.3 Client-settable TX priority
A control message to let a client declare/raise its `TxPriority` over the wire (today fixed at `NORMAL`, §7).

### 13.4 Discovery
An optional UDP discovery/beacon (an application-defined discovery port is one such mechanism today, outside this spec).

### 13.5 Stream-fact metadata (`StreamDescription`)
Carry stream facts — sample rate, channel layout, sample format, source kind, optional RF-center frequency, and a precise time anchor (adopting a VITA-49-style UTC convention where it aids interop) — **explicitly excluding** station topology (receiver count, phase-coherence, transverter offsets), which is Hamlib's or the host app's. Closes the §10 metadata gap.

---

## Appendix A — Constant reference

| Constant | Value |
|----------|------:|
| Magic | `0xAF01` |
| Version | `1` |
| Header size | 19 bytes |
| CRC size | 4 bytes (CRC-32/ISO-HDLC, big-endian) |
| MAX_PAYLOAD | 16384 |
| UDP_MAX_PAYLOAD (advisory) | 1400 |
| Default port | 4533 |
| Packet types | RX 0x00, TX 0x01, CONTROL 0x02, HEARTBEAT 0x03, FEC_PARITY 0x04 |
| Flags | COMPRESSED 0x01, LOW_BANDWIDTH 0x02 |
| Jitter EMA gain | 1/16 |
| Jitter multiplier (default) | 3.0 |
| Jitter ramp-down interval | 100 packets (1 ms/step) |
| FEC block size (default / range) | 5 / 2…10 |
| FEC parity header | 5 bytes (`startSeq` i32 + `blockSize` u8) |
| FEC block timeout | 60 ms (pending block 2×) |
| Reorder window (UDP presets) | 8 |
| Control ARQ ring | 16 |
| Control ARQ timeout / max attempts | 500 ms / 3 |
| Heartbeat (TCP) interval / timeout | 5 s / 10 s |
| Heartbeat (UDP) interval / timeout | 3 s / 8 s |
| TX idle timeout (default) | 500 ms |
| Max consecutive CRC errors (TCP resync) | 5 |
| Default audio format | 48000 Hz, 16-bit, 2 ch, PCM signed LE, 20 ms frame |

---

*End of net-audio Audio Streaming Protocol Specification v1.0.*
