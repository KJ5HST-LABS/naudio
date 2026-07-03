// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — TCP protocol handler (the framing FSM).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "naudio/AudioPacket.hpp"
#include "naudio/ControlMessage.hpp"
#include "naudio/net/Socket.hpp"
#include "naudio/net/Transport.hpp"  // ReceiveResult

namespace naudio::net {

// Protocol-level send/receive over one TCP socket: framing, CRC handling,
// heartbeat timing, and a resumable receive FSM. Owns the socket (it is moved
// in) and closes it on close().
//
// The receive FSM is the TCP wire-resilience:
// a recv timeout mid-frame preserves the partial bytes so the next call resumes
// byte-exact instead of desyncing; a bad magic triggers a byte-wise resync
// (including a magic split across the buffer boundary); an over-long payload is
// skipped to stay in sync; MAX_CONSECUTIVE_CRC_ERRORS in a row tears the
// connection down.
//
// Threading: sendPacket is mutex-guarded; the receive
// FSM is single-threaded (one receive thread per connection); stats are atomic;
// closed is atomic. Non-copyable/non-movable (holds a mutex + Socket) — construct
// in place / own via the connection.
class AudioProtocolHandler {
public:
    static constexpr int HEARTBEAT_INTERVAL_MS = 5000;   // TCP heartbeat cadence
    static constexpr int CONNECTION_TIMEOUT_MS = 10000;  // no-RX death window
    static constexpr int MAX_CONSECUTIVE_CRC_ERRORS = 5;

    explicit AudioProtocolHandler(Socket socket);
    ~AudioProtocolHandler();

    AudioProtocolHandler(const AudioProtocolHandler&) = delete;
    AudioProtocolHandler& operator=(const AudioProtocolHandler&) = delete;

    // --- Send (false on I/O failure / closed) ---
    bool sendPacket(const AudioPacket& packet);
    bool sendRxAudio(const std::uint8_t* data, std::size_t offset, std::size_t length);
    bool sendRxAudio(const std::vector<std::uint8_t>& data) {
        return sendRxAudio(data.data(), 0, data.size());
    }
    bool sendTxAudio(const std::uint8_t* data, std::size_t length);
    bool sendControl(const ControlMessage& message);
    bool sendHeartbeat();

    // --- Receive (the FSM) ---
    // timeoutMs == 0 blocks. See ReceiveResult: a frame, no-data (retry), or a
    // dead connection.
    ReceiveResult receivePacket(int timeoutMs);

    // --- Heartbeat / timeout ---
    bool shouldSendHeartbeat() const;
    bool isConnectionTimedOut() const;
    std::int64_t timeSinceLastReceive() const;

    // --- Statistics ---
    std::int32_t currentSequence() const { return sequenceCounter_.load(); }
    std::int64_t packetsSent() const { return packetsSent_.load(); }
    std::int64_t packetsReceived() const { return packetsReceived_.load(); }
    std::int64_t bytesSent() const { return bytesSent_.load(); }
    std::int64_t bytesReceived() const { return bytesReceived_.load(); }
    int crcErrors() const { return crcErrors_.load(); }

    std::string remoteAddress() const { return socket_.remoteAddress(); }
    bool isClosed() const { return closed_.load(); }
    void close();

private:
    // Receive FSM phases.
    enum Phase { PHASE_HEADER = 0, PHASE_PAYLOAD = 1, PHASE_SKIP = 2 };

    // Outcome of a buffer fill: full, deadline-hit (progress kept), or fatal.
    enum class Fill { Filled, Timeout, Fatal };

    Fill fillRecvBuf();
    void resetRecvState();
    std::int32_t nextSequence() { return sequenceCounter_.fetch_add(1); }

    Socket socket_;
    std::mutex sendMutex_;  // serializes sendPacket

    std::atomic<std::int32_t> sequenceCounter_{0};
    std::atomic<bool> closed_{false};
    std::atomic<std::int64_t> lastSendTime_;
    std::atomic<std::int64_t> lastReceiveTime_;

    std::atomic<std::int64_t> packetsSent_{0};
    std::atomic<std::int64_t> packetsReceived_{0};
    std::atomic<std::int64_t> bytesSent_{0};
    std::atomic<std::int64_t> bytesReceived_{0};
    std::atomic<int> crcErrors_{0};

    // Receive-thread-only FSM state (not shared — no synchronization needed).
    int consecutiveCrcErrors_ = 0;
    int recvPhase_ = PHASE_HEADER;
    std::vector<std::uint8_t> recvBuf_;
    std::size_t recvFilled_ = 0;
    std::vector<std::uint8_t> recvHeader_;  // completed header while reading payload
};

}  // namespace naudio::net
