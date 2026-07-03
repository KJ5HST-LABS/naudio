#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Copyright (C) 2025-2026 Terrell Deppe
"""Generate language-neutral golden conformance vectors for the net-audio
audio streaming protocol v1.

CRC32 is computed here with Python's zlib (CRC-32/ISO-HDLC) INDEPENDENTLY of the
implementation under test -- so the vectors are a true known-answer test, not a
round-trip-against-self.
"""
import struct, zlib, os, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "vectors.ini"

def h(b: bytes) -> str:
    return b.hex()

def frame(ptype, seq, flags, ts, payload: bytes) -> bytes:
    # 19-byte BE header: magic u16, ver u8, type u8, flags u8, seq i32, ts i64, plen u16
    header = struct.pack('>HBBBiqH', 0xAF01, 1, ptype, flags, seq, ts, len(payload))
    assert len(header) == 19, len(header)
    body = header + payload
    crc = zlib.crc32(body) & 0xFFFFFFFF
    return body + struct.pack('>I', crc)

def ctrl_audio_config(sr, bits, ch, frame_ms, tgt, mn, mx) -> bytes:
    return bytes([0x04]) + struct.pack('>IBBHHHH', sr, bits, ch, frame_ms, tgt, mn, mx)

def fec_parity_payload(start_seq, block_size, payloads):
    maxlen = max(len(p) for p in payloads)
    xor = bytearray(maxlen)
    for p in payloads:
        for j in range(len(p)):
            xor[j] ^= p[j]
    return struct.pack('>iB', start_seq, block_size) + bytes(xor), bytes(xor)

# ---- control-plane encoders (match the ControlMessage wire format byte-for-byte) ----

CLIENT_INFO_MAX = 255  # the whole serialized ClientInfo must fit behind a u8 length-prefix

def serialize_client_info(callsign, name, location) -> bytes:
    """Cap the TOTAL serialized ClientInfo at 255 bytes (not each field at
    255, which could reach 768 and overflow the u8 length-prefix). The 3 length
    bytes are fixed, so the three UTF-8 fields share a 252-byte budget allocated in
    order callsign -> name -> location. This clamp is part of the wire contract, so
    every conforming implementation reproduces these exact bytes. Normal-size
    identities are well under budget and remain byte-unchanged."""
    out = bytearray()
    budget = CLIENT_INFO_MAX - 3
    for field in (callsign.encode('utf-8'), name.encode('utf-8'), location.encode('utf-8')):
        take = min(len(field), budget)
        out += bytes([take]) + field[:take]
        budget -= take
    return bytes(out)

def ctrl_connect_request(version, name, config=None, info=None) -> bytes:
    # 0x01 | ver u8 | nameLen u8 | name | hasConfig u8 | [tgt,min,max u16 BE] | ciLen u8 | ci
    nb = name.encode('utf-8')
    out = bytearray([0x01, version, len(nb)]) + nb
    if config is not None:
        out += bytes([1]) + struct.pack('>HHH', *config)  # bufferTarget, min, max
    else:
        out += bytes([0])
    ci = serialize_client_info(*info) if info is not None else b""
    out += bytes([len(ci)]) + ci
    return bytes(out)

def ctrl_latency(type_byte, ts) -> bytes:
    return bytes([type_byte]) + struct.pack('>q', ts)  # i64 BE timestamp

def ctrl_error(message) -> bytes:
    return bytes([0xFE]) + message.encode('utf-8')  # raw UTF-8, no length prefix

def ctrl_tx(type_byte, client_id) -> bytes:
    return bytes([type_byte]) + client_id.encode('utf-8')  # raw UTF-8 client id

def ctrl_clients_update(client_count, max_clients, tx_owner, client_ids, info_map) -> bytes:
    out = bytearray([0x44, min(client_count, 255), min(max_clients, 255)])
    tb = tx_owner.encode('utf-8')
    out += bytes([len(tb)]) + tb
    out += bytes([len(client_ids)])
    for cid in client_ids:
        idb = cid.encode('utf-8')
        ci = serialize_client_info(*info_map[cid]) if cid in info_map else b""
        out += bytes([len(idb)]) + idb + bytes([len(ci)]) + ci
    return bytes(out)

records = []
def rec(name, **kv):
    records.append((name, kv))

# ---- CRC known-answer ----
rec("crc-kat-check", kind="crc32", inputHex=h(b"123456789"),
    expected="%08x" % (zlib.crc32(b"123456789") & 0xFFFFFFFF))

# ---- byte-exact frame vectors (encode + decode), timestamp pinned to 0 ----
rec("frame-heartbeat-seq5", kind="frame", type=3, sequence=5, flags=0, timestamp=0,
    payloadHex="", frameHex=h(frame(0x03, 5, 0, 0, b"")), expectDecode="accept")

rx_payload = bytes([0xDE, 0xAD, 0xBE, 0xEF])
rec("frame-audio-rx-seq1", kind="frame", type=0, sequence=1, flags=0, timestamp=0,
    payloadHex=h(rx_payload), frameHex=h(frame(0x00, 1, 0, 0, rx_payload)),
    expectDecode="accept")

tx_payload = bytes(4)
rec("frame-audio-tx-seq42", kind="frame", type=1, sequence=42, flags=0, timestamp=0,
    payloadHex=h(tx_payload), frameHex=h(frame(0x01, 42, 0, 0, tx_payload)),
    expectDecode="accept")

ac = ctrl_audio_config(48000, 16, 2, 20, 100, 40, 300)
rec("frame-control-audioconfig-seq7", kind="frame", type=2, sequence=7, flags=0, timestamp=0,
    payloadHex=h(ac), frameHex=h(frame(0x02, 7, 0, 0, ac)), expectDecode="accept")

# low-bandwidth flag set, 12k, on an RX frame
rec("frame-audio-rx-lowbw-flag", kind="frame", type=0, sequence=2, flags=2, timestamp=0,
    payloadHex=h(rx_payload), frameHex=h(frame(0x00, 2, 2, 0, rx_payload)),
    expectDecode="accept")

# ---- negative decode vectors ----
rec("frame-neg-bad-magic", kind="frame", expectDecode="reject",
    frameHex=h(b"\x00\x00" + bytes(21)))   # 23 bytes, magic != AF01
good = frame(0x00, 1, 0, 0, rx_payload)
bad_crc = bytearray(good); bad_crc[-1] ^= 0xFF
rec("frame-neg-bad-crc", kind="frame", expectDecode="reject", frameHex=h(bytes(bad_crc)))
rec("frame-neg-too-short", kind="frame", expectDecode="reject", frameHex=h(bytes(10)))

# ---- control message vectors (serialize from fields; some round-trip a scalar) ----
rec("control-audioconfig-default", kind="control", controlType=0x04,
    sampleRate=48000, bitsPerSample=16, channels=2, frameDurationMs=20,
    bufferTargetMs=100, bufferMinMs=40, bufferMaxMs=300,
    expectedPayloadHex=h(ac))

rec("control-nack", kind="control", controlType=0x50, sequence=12345,
    expectedPayloadHex=h(bytes([0x50]) + struct.pack('>i', 12345)))
rec("control-ack", kind="control", controlType=0x51, sequence=67890,
    expectedPayloadHex=h(bytes([0x51]) + struct.pack('>i', 67890)))

msg = "Server is busy".encode()
rec("control-connect-reject-busy", kind="control", controlType=0x03,
    rejectReason=0x01, message="Server is busy",
    expectedPayloadHex=h(bytes([0x03, 0x01, len(msg)]) + msg))

# CONNECT_REQUEST (0x01): minimal (no config, no client info) and with-config.
rec("control-connect-request-minimal", kind="control", controlType=0x01,
    protocolVersion=1, clientName="W1AW-client",
    expectedPayloadHex=h(ctrl_connect_request(1, "W1AW-client")))
rec("control-connect-request-with-config", kind="control", controlType=0x01,
    protocolVersion=1, clientName="W1AW-client", hasConfig=1,
    bufferTargetMs=100, bufferMinMs=40, bufferMaxMs=300,
    expectedPayloadHex=h(ctrl_connect_request(1, "W1AW-client", config=(100, 40, 300))))

# LATENCY_PROBE (0x22) / LATENCY_RESPONSE (0x23): a single i64 BE timestamp.
rec("control-latency-probe", kind="control", controlType=0x22, timestamp=1234567890123,
    expectedPayloadHex=h(ctrl_latency(0x22, 1234567890123)))
rec("control-latency-response", kind="control", controlType=0x23, timestamp=1234567890123,
    expectedPayloadHex=h(ctrl_latency(0x23, 1234567890123)))

# ERROR (0xFE): raw UTF-8 message, no length prefix.
rec("control-error", kind="control", controlType=0xFE, message="internal error",
    expectedPayloadHex=h(ctrl_error("internal error")))

# TX_DENIED (0x41) / TX_PREEMPTED (0x42): raw UTF-8 client id.
rec("control-tx-denied", kind="control", controlType=0x41, clientId="alice-station",
    expectedPayloadHex=h(ctrl_tx(0x41, "alice-station")))
rec("control-tx-preempted", kind="control", controlType=0x42, clientId="bob-station",
    expectedPayloadHex=h(ctrl_tx(0x42, "bob-station")))

# CLIENTS_UPDATE (0x44): the ClientInfo-overflow case. The single entry's ClientInfo is 273 bytes
# pre-clamp (250 callsign + 10 name + 10 location); the total-<=255 clamp keeps the
# callsign (250), truncates name to 2 ("BB"), and drops location -> a valid u8
# infoLen of 255. Pre-fix this would write infoLen = 273 & 0xFF = 17 and mis-frame.
_cu_info = {"w1aw": ("A" * 250, "B" * 10, "C" * 10)}
rec("control-clients-update-clamp", kind="control", controlType=0x44,
    clientCount=1, maxClients=4, txOwner="w1aw", clientId="w1aw",
    infoCallsign="A" * 250, infoName="B" * 10, infoLocation="C" * 10,
    expectedCallsign="A" * 250, expectedName="BB", expectedLocation="",
    expectedPayloadHex=h(ctrl_clients_update(1, 4, "w1aw", ["w1aw"], _cu_info)))

# ---- FEC encode vectors ----
p3 = [bytes([0x10,0x20,0x30]), bytes([0x01,0x02,0x03]), bytes([0xFF,0x00,0x0F])]
parity3, xor3 = fec_parity_payload(0, 3, p3)
rec("fec-encode-3", kind="fec_encode", blockSize=3, startSequence=0,
    payloadsHex=",".join(h(p) for p in p3),
    expectedXorHex=h(xor3), expectedParityPayloadHex=h(parity3))

pv = [bytes([0x01,0x02]), bytes([0x03,0x04,0x05,0x06])]
parityv, xorv = fec_parity_payload(0, 2, pv)
rec("fec-encode-varlen", kind="fec_encode", blockSize=2, startSequence=0,
    payloadsHex=",".join(h(p) for p in pv),
    expectedXorHex=h(xorv), expectedParityPayloadHex=h(parityv))

# ---- FEC recovery vector (1 missing) ----
d0 = bytes([0x11,0x22,0x33]); d1 = bytes([0x44,0x55,0x66]); d2 = bytes([0x77,0x88,0x99])
parityR, xorR = fec_parity_payload(0, 3, [d0, d1, d2])
rec("fec-recover-1missing", kind="fec_recover", blockSize=3, startSequence=0,
    presentHex="0:%s,2:%s" % (h(d0), h(d2)),     # seq:payload
    parityPayloadHex=h(parityR), missingSequence=1, expectedRecoveredHex=h(d1))

# ---- reorder vectors ----
rec("reorder-inorder", kind="reorder", windowSize=8, maxHoldMs=0,
    inputSeqs="0,1,2", expectedEmitted="0,1,2",
    expectedReordered=0, expectedDroppedLate=0, expectedGaps=0)
rec("reorder-ooo", kind="reorder", windowSize=8, maxHoldMs=0,
    inputSeqs="0,2,1", expectedEmitted="0,1,2",
    expectedReordered=1, expectedDroppedLate=0, expectedGaps=0)
rec("reorder-window-flush", kind="reorder", windowSize=3, maxHoldMs=0,
    inputSeqs="0,2,3,4", expectedEmitted="0,GAP,2,3,4",
    expectedReordered=3, expectedDroppedLate=0, expectedGaps=1)

# ---- jitter (structural; reproducible via public API in both languages) ----
rec("jitter-empty", kind="jitter", minMs=20, maxMs=200, multiplier="3.0",
    packets=0, expectedTargetMs=-1, expectedPacketCount=0)
rec("jitter-first-packet", kind="jitter", minMs=20, maxMs=200, multiplier="3.0",
    packets=1, expectedTargetMs=20, expectedPacketCount=1)

# ---- emit INI ----
os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)
with open(OUT, "w") as f:
    f.write("# net-audio audio streaming protocol v1 -- golden conformance vectors\n")
    f.write("# GENERATED by tools/gen_vectors.py. CRC32 = python zlib (computed independently).\n")
    f.write("# See conformance/README.md for the format and the per-kind field schema.\n")
    f.write("# All *Hex fields are lowercase hex; empty = empty byte string.\n\n")
    for name, kv in records:
        f.write("[%s]\n" % name)
        for k, v in kv.items():
            f.write("%s = %s\n" % (k, v))
        f.write("\n")

print("wrote %d vectors to %s" % (len(records), OUT))
# sanity echo of the heartbeat frame for the report
print("heartbeat-seq5 frameHex =", h(frame(0x03, 5, 0, 0, b"")))
print("audioconfig payload     =", h(ac))
print("fec-encode-3 xor        =", h(xor3))
