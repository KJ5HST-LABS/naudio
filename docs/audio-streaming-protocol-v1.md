# net-audio Audio Streaming Protocol — Specification v1

**Status:** Stable / frozen wire contract (`0xAF01`, version byte = 1).
**Spec version:** 1.4 (2026-09-01) — **discovery is adopted from §13.4** as the new normative §6.8, via the §11 minor-extension path: two new control types (`DISCOVER` `0x60`, `DISCOVER_REPLY` `0x61`) carried in ordinary `CONTROL` frames. The framing, the type enum (0x00–0x04), the CRC semantics and every pre-existing golden vector are **untouched** — a pre-1.4 endpoint ignores both new types, which is exactly what §11 requires of it. The exchange is **client-initiated and side-effect-free**: a server answers a probe without creating a connection, consuming a client slot or starting a stream, and it **never** transmits unsolicited. See §6.8.
**Spec version history:** 1.3 (2026-08-31) — §6.2.1 only, and **no wire change whatsoever**: the rate and the channel layout of a format request are now granted **independently** of one another, where 1.2 granted the request exactly or not at all. The 15-byte `AUDIO_CONFIG` already carried the granted rate and the granted layout in separate fields, so a partial grant was always *expressible* — 1.2 simply never produced one, and an unservable layout therefore discarded a servable rate. Every byte layout, type number, CRC semantic and golden vector is untouched. See §6.2.1. · 1.2 (2026-08-22) — per-subscription RX format negotiation, adopted from §13.1 in reduced scope (rate + channel layout only; RX only): appended `CONNECT_REQUEST` fields, an extended `AUDIO_CONFIG` form, and the grant rules — all via the §11 minor-extension path. The 1.2 vectors in `conformance/vectors/vectors-v1_2.ini` are loaded by the conformance suite (`Conformance.GoldenVectorsV12`) alongside the reference implementation. · 1.1 (2026-08-15) — §8.4 only: `CONNECT_REQUEST` became a critical (ARQ'd) control type.
**Scope:** The *audio* half of a radio-streaming toolkit.
**Provenance:** This document is the normative, field-by-field definition of the `0xAF01` audio wire. Every normative value here is implemented and pinned by the language-neutral golden-vector conformance suite (see §12), so the wire is byte-deterministic and independently checkable. Where the toolkit *plan* describes capabilities that are **not** in the v1 wire, they are isolated in §13 (Proposed extensions) and are explicitly **non-normative**.

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

Independent port spaces make the paired bind sound in general, but they do not make it certain: a host may reserve blocks of UDP ports that the TCP allocator knows nothing about, and a bind into one is refused on a *permission* error (Windows `WSAEACCES`) rather than an address-in-use error. Whether that happens is purely a function of which port the OS chose, so it presents as an intermittent failure unrelated to the caller. Accordingly, **when the caller requested port 0**, a server SHOULD roll back and retry the pair on a fresh OS-assigned port rather than failing — a caller that named no port was promised no *particular* port, so retrying preserves the contract it was given. The retry MUST be bounded. It MUST NOT extend to a caller that **named** a port: that caller asked for a specific number, and it is also the only path on which a genuine privilege refusal is reachable, which a retry would mask. Note that ephemeral ports are typically issued sequentially, so a bounded retry walks *into* a contiguous reserved block one port at a time — the bound must exceed the block widths the target hosts reserve, or the retry will exhaust inside one. None of this is observable on the wire.

### 2.3 UDP client registration

On UDP, a client is registered **only** upon receipt of a valid, deserializable `CONNECT_REQUEST` control frame (§6.2) from its source address. Unsolicited or malformed datagrams MUST NOT register a client. (This closes a trivial spoof/garbage-registration vector.)

### 2.4 Framing

- **TCP:** frames are length-delimited by the header's 2-byte payload-length field. A reader consumes the fixed 19-byte header, then `payloadLength` payload bytes, then 4 CRC bytes. A robust reader maintains a resync state machine: on a bad magic it scans forward byte-by-byte for the next `0xAF01`; on an oversized payload-length it skips; on `MAX_CONSECUTIVE_CRC_ERRORS` (= 5) consecutive validation failures it MAY abort the connection. A successful decode resets the consecutive-error counter.
- **UDP:** one frame per datagram. Payloads SHOULD be kept ≤ `UDP_MAX_PAYLOAD` (= 1400 bytes) to avoid IP fragmentation (advisory, not enforced by the codec). The obligation falls on the **sender**, not on the application feeding it: a sender whose audio frame exceeds the budget SHOULD split it across several complete, independently-sequenced audio frames rather than emit one oversized datagram, since each is a whole frame and a receiver reassembles nothing. Splitting at a whole sample frame is required — a boundary inside a sample decodes every later sample one channel out of phase. This is not app-layer fragmentation and adds nothing to the wire format. *(Non-normative: the reference implementation does this for every preset, sample rate and channel count; see issue #86.)*

### 2.5 Ports

- **Default service port: `4533`** (TCP, UDP, or both for DUAL).
- **Discovery** uses this same service port: a client broadcasts a `DISCOVER` to it and each server answers by unicast (§6.8, since spec 1.4). Discovery therefore resolves an *address*, not an address+port — a prober must already know which port to probe, and `4533` is the default it should try first. A server on a non-default port is found only by a probe aimed at that port. There is deliberately **no separate discovery port** and **no beacon**: a server never transmits unsolicited.

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
| `FEC_PARITY` | `0x04` | both (UDP) | XOR parity packet for a FEC block (§5.2, §8.2). Emitted from **both** audio send paths, so a client protects its `AUDIO_TX` lane exactly as a server protects `AUDIO_RX`. (This row read "server → client" until 2026-08-06; the implementation has always sent parity in both directions, and the row was corrected rather than the behaviour — see issue #55.) |

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

> v1 transports audio **byte-for-byte unchanged** from server to every client (the "native broadcast" path, §6.4), and that path remains the default. **Since spec 1.2**, a connection that requested and was granted a reduced RX format (§6.2.1) receives audio decimated / channel-reduced to its granted format instead; every other connection stays byte-for-byte native. Sample-*format* conversion (beyond int16) remains proposed in §13.1, and the conversion building blocks live in companion DSP code, not in this wire spec.

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
| `CONNECT_REQUEST` | `0x01` | `STATS_UPDATE` | `0x30` |
| `CONNECT_ACCEPT` | `0x02` | `TX_GRANTED` | `0x40` |
| `CONNECT_REJECT` | `0x03` | `TX_DENIED` | `0x41` |
| `AUDIO_CONFIG` | `0x04` | `TX_PREEMPTED` | `0x42` |
| `STREAM_START` | `0x10` | `TX_RELEASED` | `0x43` |
| `STREAM_STOP` | `0x11` | `CLIENTS_UPDATE` | `0x44` |
| `STREAM_PAUSE` | `0x12` | `NACK` | `0x50` |
| `STREAM_RESUME` | `0x13` | `CONTROL_ACK` | `0x51` |
| `HEARTBEAT` | `0x20` | `DISCOVER` | `0x60` |
| `HEARTBEAT_ACK` | `0x21` | `DISCOVER_REPLY` | `0x61` |
| `LATENCY_PROBE` | `0x22` | `ERROR` | `0xFE` |
| `LATENCY_RESPONSE` | `0x23` | `DISCONNECT` | `0xFF` |

> `DISCOVER` / `DISCOVER_REPLY` are **since spec 1.4** (§6.8). A pre-1.4 endpoint does not know them and ignores them, per §11.

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

> **v1 negotiation scope:** the request carries **only buffer-timing preferences** — not sample rate, channels, or bit depth. A client inherits the server's native audio format. **Since spec 1.2** a client MAY additionally request a lower RX rate / reduced channel layout via the appended fields of §6.2.1; the remaining format breadth (sample formats beyond int16) stays proposed in §13.1.

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

### 6.2.1 Per-subscription RX format request (since spec 1.2; independent grants since 1.3)

A client MAY append a **format request** to its `CONNECT_REQUEST`, asking for a lower sample
rate and/or a reduced channel layout for the audio the server sends *to it*. Scope is
deliberately narrow: **RX direction only** (`AUDIO_TX` payloads remain in the server's native
format regardless of any grant), **rate + channel layout only** (`bitsPerSample` stays 16;
§13.1's sample-format breadth remains proposed), and **reduction only** (decimation and channel
selection/downmix — never upsampling, channel synthesis, or fractional-ratio resampling).

**Appended `CONNECT_REQUEST` fields** — immediately after the v1 fields (i.e. after
`clientInfo[clientInfoLen]`):

```
requestedRate    u32     (0 = no rate preference: keep the native rate)
requestedLayout  u8      (channel layout, table below)
```

| `requestedLayout` | Name | Payload channels | Meaning |
|------:|------|-----:|---------|
| 0 | `NATIVE` | native | No layout change. |
| 1 | `MONO_DOWNMIX` | 1 | (L+R)/2 of the native stereo. |
| 2 | `LEFT_ONLY` | 1 | Native channel 0 (e.g. VFO-A). |
| 3 | `RIGHT_ONLY` | 1 | Native channel 1 (e.g. VFO-B). |

A request of `requestedRate = 0, requestedLayout = 0` is valid and requests the native format
unchanged; its use is as a **1.2 probe** — the extended `AUDIO_CONFIG` reply (below) reveals
whether the server understands format requests at all.

**Extended `AUDIO_CONFIG` (15-byte form)** — the 14-byte form of §6.2 plus one appended byte:

```
grantedLayout    u8      (same table)
```

The server sends the extended form **only** on a connection whose `CONNECT_REQUEST` carried a
format request; on every other connection it sends the v1 form unchanged, byte-for-byte. The
existing `sampleRate` / `channels` / `frameDurationMs` fields carry the **granted** values —
`AUDIO_CONFIG` remains the sole authority for what that connection's `AUDIO_RX` payloads carry,
extended form or not, and the client-side merge rule of §6.3 is unchanged.

**Grant rules (normative).** The request carries two independent dimensions, and **since 1.3
the server answers each on its own merits**: it grants the rate if it can serve the rate, grants
the layout if it can serve the layout, and declines either **alone** by answering native for
*that dimension only*. It never substitutes a value the client did not ask for.

- It MUST NOT grant a `requestedRate` unless `requestedRate` is a positive integer divisor of
  the native rate **and** `requestedRate × frameDurationMs` is divisible by 1000 (the granted
  `samplesPerFrame` stays integral). A `requestedRate` of 0 asks for nothing and is therefore
  never declined.
- It MUST NOT grant layouts 1–3 unless the native channel count is 2. (`MONO_DOWNMIX`, at
  native mono, is declined like the rest.) A layout byte this revision does not define is
  likewise declined. **Declining the layout MUST NOT withdraw a granted rate.**
- Where the native `bitsPerSample` is not 16, the server MUST decline **both** dimensions: the
  reduction units are int16-only, and either dimension would have to convert.
- It MAY decline either dimension for any reason by answering native for it. It MUST NOT reject
  the connection merely because a format request is unservable (`CONNECT_REJECT
  FORMAT_NOT_SUPPORTED` remains for configurations that are incompatible outright, as in v1).
- A granted connection's `AUDIO_RX` frames are produced from that subscription's own converted
  payload stream: its sequence numbers, FEC parity (§5.2), and frame sizes are computed over
  **what that connection is sent**, exactly as the per-connection framing of §2.4 already
  implies. The native broadcast path (§6.4) is unaffected for every non-requesting client.

> **Why 1.3 changed this.** Under 1.2's all-or-nothing rule a client that asked for a rate *and*
> a layout, and could only be given the rate, received **neither**. That made an optimisation
> request unsafe to send speculatively — asking for more could return less — and it bit hardest
> in the most sensible configuration: against a **mono-native** server, the obvious way to halve
> bandwidth at the source, layouts 1–3 are unservable, so `(12000, MONO_DOWNMIX)` was answered
> native 48 kHz. A real client (WSJT-X) was measured refusing to start on exactly that, while
> `(12000, NATIVE)` against the same server was granted. Independent grants remove the coupling
> without moving a single byte on the wire.

**Detection semantics (client side).** The reply's *form* says who answered; its *fields* say
what was granted:

- 14-byte (or 8-byte legacy) `AUDIO_CONFIG` → the server predates 1.2 (or the request was not
  parsed); the format fields carry the native format. The client proceeds exactly as v1.
- 15-byte form → the server understood the request. Read the **fields**, not the form, for what
  was granted; since 1.3 there are three outcomes, and a client MUST cope with all of them:
  fully granted (both dimensions equal the request), **partially granted** (one dimension equals
  the request, the other states native), or fully declined (native rate with
  `grantedLayout = 0`). A client detects a declined *layout* as `grantedLayout = 0` after
  requesting a non-zero one, and a declined *rate* as `sampleRate` equal to the native rate
  after requesting a lower one.

**`AUDIO_CONFIG` is the sole authority (normative, restating §6.2).** Whatever the grant outcome,
the connection's `AUDIO_RX` payloads carry exactly the `sampleRate` / `channels` /
`bitsPerSample` this message states. A client MUST size and interpret its buffers from those
fields and MUST NOT infer the stream format from `grantedLayout`, from what it requested, or from
the server's native format. This was already the rule in 1.2; 1.3 is the revision where ignoring
it becomes observable, because a partial grant is the first reply whose format fields match
neither the request nor the native format.

**Decoder tolerance (normative, restating §11 for these two messages).** A `CONNECT_REQUEST`
decoder MUST ignore trailing bytes it does not recognize; a trailing run of **fewer than 5
bytes** after `clientInfo` is not a format request and MUST be ignored. When 5 or more bytes
follow `clientInfo`, the **first 5** are the format request and any remainder is a later minor
extension, ignored by a 1.2 decoder — the same prefix-parse rule, one revision on. An
`AUDIO_CONFIG` decoder MUST accept any data length ≥ 8, reading the fields it recognizes and
ignoring the remainder. (The v1 reference parsers already behave this way; 1.2 makes it a
requirement, which is what lets these fields append compatibly.)

**Compatibility matrix:**

| Client | Server | On the wire | Result |
|---|---|---|---|
| v1 | v1 | unchanged | Native format, byte-identical to v1. |
| 1.2, no request | any | unchanged | Byte-identical to v1. |
| 1.2, request | v1 | appended request bytes ignored by the server | 14-byte `AUDIO_CONFIG`, native format; client detects "not understood" and proceeds native. |
| v1 | 1.2 | no request → v1 form reply | Byte-identical to v1. |
| 1.2, request | 1.2 | appended fields both ways | All-or-nothing grant; 15-byte `AUDIO_CONFIG` either way. |
| 1.2, request | **1.3** | unchanged bytes | May receive a **partial** grant, which 1.2's own text does not name. Harmless: `AUDIO_CONFIG` is the sole authority (above) and states the stream truthfully, so a conformant 1.2 client sizes its buffers correctly and decodes correctly. The only client that can be surprised is one that inferred the format from `grantedLayout` rather than from the fields — which 1.2 already forbade. |
| 1.3, request | 1.2 | unchanged bytes | All-or-nothing, as 1.2 has always answered. A 1.3 client sees a full grant or a full decline and needs no special case: the three outcomes it already handles simply never include the partial one. |
| 1.3, request | 1.3 | unchanged bytes | Independent per-dimension grants per the rules above. |

### 6.3 Connect sequence

```
Client                         Server
  | --- CONNECT_REQUEST ------> |   (UDP: also registers the client, §2.3)
  | <-- AUDIO_CONFIG ---------- |   (native — or granted (§6.2.1) — format + accepted buffer prefs)
  | <-- CONNECT_ACCEPT -------- |   (or CONNECT_REJECT with a reason)
  |                             |   server registers client with broadcaster + mixer
  | <-- CLIENTS_UPDATE -------- |   (roster broadcast to all)
  | <== AUDIO_RX stream ======> |
```

On accept, the client applies **only the format fields** of AUDIO_CONFIG into its existing config (an in-place merge that preserves its own transport/FEC/reorder/jitter settings — it MUST NOT wholesale-replace its config from AUDIO_CONFIG).

### 6.4 RX broadcast / TX receive

- **RX broadcast:** the server sends the identical RX audio bytes (same buffer, offset, length) to every connected client (1→many) — the zero-cost native path, and the behavior of every v1 connection. **Since spec 1.2**, a connection granted a reduced format (§6.2.1) is instead served from its own converted payload stream; all non-requesting connections keep the identical-bytes path.
- **TX receive:** each client's `AUDIO_TX` payloads are forwarded into the mixer (§7). `AUDIO_TX` is **always** in the server's native format — a §6.2.1 grant applies to RX only.

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

### 6.8 Discovery (`DISCOVER` 0x60 / `DISCOVER_REPLY` 0x61) — since spec 1.4

Adopted from §13.4. The exchange lets a client find servers on a local segment without being told their addresses, and it is **side-effect-free by construction**: answering a probe MUST NOT create a connection, consume a client slot, start a stream, or change any server state a client can observe.

**Client-initiated only — a server MUST NOT transmit unsolicited.** There is no beacon and no periodic announcement; a server nobody is probing sends nothing. A discovery round therefore costs N+1 datagrams, on demand: one broadcast (or multicast) `DISCOVER` from the client, and one **unicast** `DISCOVER_REPLY` from each server that hears it, sent back to the prober rather than to the segment.

**`DISCOVER` (0x60) data:**

```
token   u32     opaque; echoed verbatim in the reply
```

The token correlates replies with the probe round that caused them. A client SHOULD choose it unpredictably, and MUST ignore a reply whose token it did not send.

**`DISCOVER_REPLY` (0x61) data (16 bytes + name):**

```
token           u32     echoed from the DISCOVER
port            u16     the port this server serves audio on
transports      u8      bitmask: 0x01 TCP, 0x02 UDP (a DUAL server sets both)
sampleRate      u32     native RX sample rate
bitsPerSample   u8
channels        u8
clientCount     u8      clients connected now (saturating at 255)
maxClients      u8      capacity (saturating at 255)
nameLen         u8
name            name[nameLen]    (UTF-8 operator-supplied label; MAY be empty)
```

The server's **address is not a field**. It is the source address of the reply datagram — the only value the prober can act on, and the only one that stays correct when the server is bound to one of several interfaces.

**What the reply deliberately does not carry.** The roster and client identities (§6.6), callsigns, `clientInfo`, station location, TX-owner state, statistics, and any credential or token beyond the echoed one. A `DISCOVER_REPLY` is answerable to an unauthenticated stranger, so it carries what a chooser needs to pick a server and nothing that would reward harvesting it. An implementation MUST NOT extend the reply with client-identifying fields.

**Server behavior (normative):**

1. A server MAY answer `DISCOVER`; whether it does is a local policy knob. (The reference implementation answers **by default**, and can be turned off — see the disclosure note below.)
2. A server that answers MUST reply by **unicast to the source address of the probe**, and MUST NOT broadcast the reply.
3. A server MUST answer **regardless of occupancy**. A server at `maxClients` still replies, reporting `clientCount == maxClients`; occupancy is a fact the chooser needs, not a reason for silence.
4. A server MUST rate-limit replies per source address (see Security considerations).
5. A server MUST NOT treat a `DISCOVER` as registration. It creates no connection and no pending entry, so a probe leaves no trace a subsequent `CONNECT_REQUEST` from the same address could inherit.
6. Where the transport has no datagram path to an unknown sender — a **TCP-only** server — discovery is **not available**. That is a property of the transport, not an omission: there is no way to broadcast a connection attempt. A DUAL server is discoverable over its UDP half and reports both bits in `transports`.

**Sequence numbers on a connectionless reply.** §3.4 defines the sequence as one monotonic
counter per *sender*. A `DISCOVER_REPLY` belongs to no connection, so it cannot draw from one; a
responder SHOULD keep its own counter for these frames. A prober MUST NOT use the sequence to
correlate a reply with its probe — that is the token's job — and MUST NOT expect the sequences of
replies from different servers, or of successive replies from one server, to relate to each other
in any way. The field is present because every `0xAF01` frame has one, not because it carries
meaning here.

**Interaction with the anti-spoof rule (§2.3).** A UDP server registers a client only on a valid `CONNECT_REQUEST` from an unknown sender, and drops every other datagram from one. Spec 1.4 adds `DISCOVER` as the **second and only other** exempt case, and its exemption is strictly narrower: a `CONNECT_REQUEST` from an unknown sender *creates a connection*, whereas a `DISCOVER` from an unknown sender is *answered and forgotten*. A datagram from an unknown sender that is neither MUST still be dropped, and §2.3's rule is otherwise unchanged.

**Security considerations.**

- **Amplification.** A `DISCOVER` frame is 28 bytes on the wire and a `DISCOVER_REPLY` is 40 bytes plus the name, so the exchange amplifies by roughly 1.4× — small, but not 1×, and the source address of a UDP datagram is unverified. A server MUST rate-limit replies per source address, and that limiter's own state MUST be bounded, so that probing from many forged source addresses cannot exhaust it. The reference implementation allows one reply per source per second over a bounded table of recent sources, and drops the oldest entry rather than growing.
- **Disclosure.** Answering discovery tells an unauthenticated stranger that a server exists, what format it serves, and how full it is. That is the point of the feature, and it is why (1) is a knob. An operator who does not wish to be found turns it off; a server on an untrusted segment SHOULD additionally be bound to a specific interface rather than the wildcard address.
- **No accumulated state.** Because a reply creates nothing, a discovery flood costs a server only the replies its limiter permits. It cannot fill the bounded pending-connection registry, which is the resource a `CONNECT_REQUEST` flood consumes.

---

## 7. TX arbitration

Many clients may send `AUDIO_TX`; exactly one may own the transmit channel at a time. Arbitration is **mixer-side and implicit**:

- A client **requests TX implicitly** by sending `AUDIO_TX` frames — there is no explicit "request TX" control message in v1.
- **Priority levels:** `LOW(0)`, `NORMAL(1)`, `HIGH(2)`, `EXCLUSIVE(3)`. Preemption is strict-greater: a submitter preempts the current owner only if its level is numerically greater.
- **Claim rules:** first submit claims a free channel; the same owner keeps it while active; a higher-priority submitter preempts; otherwise the submit is rejected.
- The server emits `TX_GRANTED` / `TX_DENIED` / `TX_PREEMPTED` / `TX_RELEASED` accordingly (`TX_DENIED` is sent once per denial episode, not per frame, to avoid spam).
- **Idle-release:** the owner's channel is released after `txIdleTimeoutMs` (default **500 ms**) without TX activity. Release is driven both by the playback loop and an independent periodic check (so a server with no local playback device still releases).

> **Implementation note — locally reconstructed frames (non-normative; the wire is unchanged).**
> The claim and idle-release rules above are stated in terms of frames a server *receives*. When
> the reliability layer is carrying `AUDIO_TX` under FEC (§8), some frames are not received at all
> but **rebuilt locally from parity** — the peer sent the frame once, it was lost, and the receiver
> reconstructed it. This is invisible on the wire and to the sender, so it changes no framing, no
> field, and no conformance vector; it is purely a receiver-side distinction.
>
> Because arbitration here is *implicit* — submitting audio **is** the claim — a reconstructed
> frame would otherwise arbitrate on the peer's behalf without the peer having sent anything new,
> and could re-claim a channel after its own idle-release. A server **MAY** therefore treat only
> directly-received frames as claim / preempt / deny / lease-refresh events, while still mixing a
> reconstructed frame's audio when the peer already owns the channel. naudio does exactly that
> (issue #65); the owning statement of its rules is the `submitTxAudio` contract in
> `include/naudio/net/AudioMixer.hpp`. A peer cannot distinguish a server that does this from one
> that does not, except that the second may key a transmitter on audio nobody currently intends to
> send — which is why naudio's bridge, whose default profile is the only FEC-enabled preset, does
> not.

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

**Decoder.** Audio packets are retained for recovery and delivered immediately on arrival — *except* while a repair may still be pending. A decoder that observes a gap in the sequence numbers it is handed holds the audio behind that gap until the block resolves, then delivers it in sequence order. Like the recovery rule below this is a *local delivery* rule, not a wire rule: it changes when a receiver hands frames to its application, never what is serialized. Delivering unconditionally on arrival instead makes a recovered packet reach the application after **every** block member that follows it in sequence order — parity is computed over the block, so it necessarily arrives after them — which is a reordering no downstream buffer can undo, since every stage able to re-sequence sits ahead of the decoder (issue #66). With no loss nothing is held and the added latency is zero. A parity frame's `startSeq`/`blockSize` define the block `[startSeq, startSeq+N)`:
- **0 missing:** parity is redundant; discard.
- **1 missing:** recover it — `lost = xorData XOR (all present payloads in the block)` (truncating each present payload to the parity length). Emit the recovered packet **with the block's own audio type** (`AUDIO_RX` for an RX block, `AUDIO_TX` for a TX block — read off a present member, not assumed) and `sequence = missingSeq`. This is a *local delivery* rule, not a wire rule: a recovered packet is handed to the application and is never serialized or sent. Stamping it `AUDIO_RX` unconditionally makes a receiver route a repaired TX frame to its default branch and drop it silently while counting the repair (issue #55).
- **Declined:** if the range cannot be shown to be the encoder's block, do not recover — see `docs/protocols.md` R5. The range is not the block whenever a non-audio packet occupies a slot inside it, whenever a member of the block is found at or past `startSeq + blockSize`, or whenever a slot's stored copy was released before the parity arrived. The decoder MUST NOT key this decision on the parity frame's own sequence number: §3.4 constrains that field only to be monotonic and shared, so conforming senders may number it differently.
- **2+ missing:** unrecoverable; emit a gap marker (silence) for each still-missing slot.

A block that does not complete within the **block timeout (default 60 ms)** is flushed (gaps for the missing) — which also releases any audio held for ordering, so a receiver whose parity stream stops sees a bounded delay rather than a stall. Audio packets arriving *before* their parity are held in a pending block (cleared after **2× timeout**). The ordering hold is bounded independently of that repair cache, by a packet count, so it still terminates when nothing ticks the decoder at all.

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
- **Critical types** (tracked / ACK-required): `CONNECT_REQUEST, CONNECT_ACCEPT, CONNECT_REJECT, AUDIO_CONFIG, STREAM_START, STREAM_STOP, STREAM_PAUSE, STREAM_RESUME, TX_GRANTED, TX_DENIED, TX_PREEMPTED, TX_RELEASED, CLIENTS_UPDATE, DISCONNECT`.
- **Non-critical** (never ARQ'd, to avoid ACK-of-ACK loops): `HEARTBEAT, HEARTBEAT_ACK, LATENCY_PROBE, LATENCY_RESPONSE, STATS_UPDATE, ERROR, NACK, CONTROL_ACK`.

> **Spec 1.1 — `CONNECT_REQUEST` moved from non-critical to critical.** In spec 1.0 it was the only
> message in the connection-establishment exchange that was **not** retransmitted: `CONNECT_ACCEPT`,
> `CONNECT_REJECT` and `AUDIO_CONFIG` all were. The asymmetry was not deliberate and it was
> one-sided in the worst direction — a lost *server* reply recovered in one 500 ms interval, while a
> lost *client* request could not be recovered at all and cost the caller the whole connect timeout
> followed by a failed connect. A client opening a session over a lossy link is exactly the moment
> the ARQ layer exists for.
>
> **This is a §11 "minor, backward compatible" extension, not a version-byte change.** No frame
> layout, type number, field offset or CRC semantic moves; the golden vectors are unaffected. The
> two mixed-version behaviours are bounded and benign, and neither can strand a connection:
>
> - **New sender, old receiver.** The old receiver does not treat `CONNECT_REQUEST` as critical, so
>   it never returns a `CONTROL_ACK`. The new sender retransmits until its 3-attempt budget is
>   spent, then drops the pending entry and carries on — at most **two extra datagrams**, and the
>   duplicates reach an *already-established* session (the receiver demultiplexes by sender
>   endpoint), where an unexpected `CONNECT_REQUEST` is ignored like any other unhandled control.
> - **Old sender, new receiver.** The new receiver ACKs a request the old sender is not tracking.
>   An unsolicited `CONTROL_ACK` names a sequence with no pending entry and is discarded, which is
>   the same path a duplicate ACK has always taken.
>
> ACK-of-ACK is still avoided: `CONTROL_ACK` and `NACK` remain non-critical, so nothing here
> acknowledges an acknowledgement.
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
| Per-subscription format negotiation | rate + channel layout since spec 1.2 (§6.2.1); buffer timing in v1 | n/a | sample-format half still proposed §13.1 |

**Net:** net-audio and #1940 are complementary. net-audio contributes the reliability stack (XOR FEC / reorder / control ARQ), an adaptive jitter buffer, per-client TX arbitration, and the cross-platform virtual-audio bridge — the "separate library" pieces upstream invited — **layered on top of** #1940's `rig_stream_*` transport. #1940 leads on **sample-format breadth** and **stream metadata** (RF center / precise time anchor) and owns I/Q. net-audio's `0xAF01` wire is a **distinct** FEC/ARQ transport, not a replacement for #1940's 32-byte UDP datagram: an application can use #1940 for rig-integrated audio and add net-audio's layer where it needs reliability or virtual-audio plumbing. Where net-audio would close its own gaps, the work is additive — format breadth (§13.1) and stream-fact metadata (§13.5) — and neither changes the frozen `0xAF01` framing.

---

## 11. Versioning & compatibility

- The `0xAF01` magic + version byte = 1 frame is a **frozen contract**. Field offsets, sizes, the type enum (0x00–0x04), and CRC semantics MUST NOT change under version 1.
- **Extensions** that preserve the frame layout (new control message types, new AUDIO_CONFIG **or CONNECT_REQUEST** fields appended after the v1 fields, new flags bits, new presets) are **minor** and backward compatible: unknown control types/flags are ignored, and control-message decoders MUST ignore trailing bytes they do not recognize (normative since 1.2 — §6.2.1; the v1 reference parsers already parse prefix-only). AUDIO_CONFIG already supports length-based forward/backward compatibility (§6.2). Spec 1.2's per-subscription format request (§6.2.1) and spec 1.4's discovery exchange (§6.8) are instances of exactly this path: both add control types and fields only, and neither touches the frame layout, the packet-type enum or the CRC.
- **Breaking** changes (header layout, type renumbering, CRC change, audio sample-format renegotiation that changes the audio-payload contract) require **bumping the version byte to 2** and a frame-layer version gate (§3.7). Such changes MUST be specified before the first such frame is emitted.
- The jitter, FEC, reorder, and ARQ algorithms and their constants (EMA 1/16, FEC N∈[2,10] default 5, parity header 5 bytes, ARQ ring 16 / timeout 500 ms / 3 attempts) are part of the contract for interoperating reliability and are versioned with the spec.

---

## 12. Conformance

The executable companion to this spec is the **golden-vector conformance suite** in [`conformance/`](../conformance/). It contains language-neutral, hand-derived known-answer vectors (with CRCs computed independently of the implementation under test) covering the frame codec, CRC, FEC, jitter, reorder, control messages, and presets. A conformance test loads these vectors and validates encode and decode against them.

- **C/C++ reference:** `tests/conformance/ConformanceTest.cpp` (built and run as part of the standard `ctest` suite, test `Conformance.GoldenVectors`).

Passing against the language-neutral vectors is the proof that an implementation matches this spec — the gate every reference client (C, C++, Python, and any other binding) must also pass.

**Spec 1.2 vectors.** The §6.2.1 wire forms are pinned by a second generated file,
`conformance/vectors/vectors-v1_2.ini` (same generator, same independence rules): byte-exact
encodings of the appended `CONNECT_REQUEST` fields and the 15-byte `AUDIO_CONFIG`, plus
**tolerance vectors** asserting the v1 view of the extended payloads (what a pre-1.2 decoder
must extract, and that a short trailing run is not a format request). The file is loaded by
`Conformance.GoldenVectorsV12` under the same fail-closed, 0-skipped contract as
`vectors.ini`; `vectors.ini` itself is unchanged by 1.2 — the no-drift check is part of
regeneration.

**Spec 1.4 vectors.** The §6.8 discovery forms are pinned by a third generated file,
`conformance/vectors/vectors-v1_4.ini` (same generator, same independence rules), loaded by
`Conformance.GoldenVectorsV14` under the same fail-closed, 0-skipped gate: one `DISCOVER` probe
and three `DISCOVER_REPLY` encodings — named, unnamed, and a server **at capacity**, which §6.8
requires to answer rather than go silent. Neither `vectors.ini` nor `vectors-v1_2.ini` is changed
by 1.4, and regeneration verifies that: a spec revision that only ADDS control types must leave
every earlier file byte-identical.

### 12.3 Determinism rule for byte-exact vectors

Because the frame timestamp is sampled at packet-creation time (§3.5), a byte-exact frame vector MUST pin the timestamp to a fixed value (the vectors use `timestamp = 0`). Conformance MUST NOT assert timestamp equality across implementations for live (non-pinned) frames.

---

## 13. Proposed extensions (NON-NORMATIVE — not in v1)

These are the toolkit-plan capabilities that are **not** in the v1 wire. They are recorded here so the spec is honest about the gap and so future versions have a starting point. Nothing that is still *proposed* here is implemented or conformance-tested. Two entries have since been adopted in whole or in part and say so in their own headings — §13.1 (the rate/layout half, spec 1.2 §6.2.1) and §13.4 (in full, spec 1.4 §6.8); an adopted item keeps its heading here so that a reader arriving from an older citation is sent forward rather than left believing it is still a proposal.

### 13.1 Format breadth (the remainder — rate/layout negotiation was adopted in spec 1.2)
Spec 1.2 adopted the rate + channel-layout half of this proposal as §6.2.1 (server-side
conversion, reduction only, RX only). What **remains proposed** here: sample formats beyond
int16 (float32 / int8 / uint8, via a `sampleFormat` enum), upsampling or fractional-ratio
rates, client-side conversion as a first-class alternative to server-side, and a TX-direction
format request. Closes the rest of the §10 format-breadth gap.

### 13.2 Application-level gap markers
Deliver explicit gap markers to high-rate/SDR consumers instead of silence-filling, so decoders see exact discontinuities. Requires a delivery path that forwards the reorder/FEC null markers to the application rather than collapsing them to silence at playout (§8.6).

### 13.3 Client-settable TX priority
A control message to let a client declare/raise its `TxPriority` over the wire (today fixed at `NORMAL`, §7).

### 13.4 Discovery — **ADOPTED in spec 1.4; see §6.8**
No longer proposed. Discovery was specified as §6.8 on 2026-09-01 (issue #100) in a deliberately narrow form: a **client-initiated, side-effect-free** `DISCOVER` / `DISCOVER_REPLY` exchange on the existing service port, with **no beacon** — the word this section used to use, and the wrong architecture. Server-initiated announcement was considered and **rejected on the record**: it would put periodic broadcast traffic on the segment forever, paid for by every host on it, to answer a question that is only asked at the moment a client is choosing a server. What remains outside the spec is discovery of the *port* (a probe resolves an address, §2.5) and any cross-subnet or multicast-routed mechanism.

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
| Channel layouts (spec 1.2, §6.2.1) | NATIVE 0, MONO_DOWNMIX 1, LEFT_ONLY 2, RIGHT_ONLY 3 |
| Format-request tail (spec 1.2, §6.2.1) | `requestedRate` u32 BE + `requestedLayout` u8 (5 bytes, appended to CONNECT_REQUEST) |
| Extended AUDIO_CONFIG (spec 1.2, §6.2.1) | 15 bytes: the 14-byte form + `grantedLayout` u8 |
| Discovery control types (spec 1.4, §6.8) | `DISCOVER` 0x60, `DISCOVER_REPLY` 0x61 |
| DISCOVER data (spec 1.4, §6.8) | `token` u32 BE (4 bytes) |
| DISCOVER_REPLY data (spec 1.4, §6.8) | 16 bytes + name: `token` u32, `port` u16, `transports` u8, `sampleRate` u32, `bitsPerSample` u8, `channels` u8, `clientCount` u8, `maxClients` u8, `nameLen` u8, `name` |
| Discovery transport bits (spec 1.4, §6.8) | TCP 0x01, UDP 0x02 |
| Discovery reply rate limit (reference impl) | 1 reply per source address per 1000 ms, bounded table |

---

*End of the net-audio Audio Streaming Protocol Specification (wire version 1; spec revision history in the title block).*
