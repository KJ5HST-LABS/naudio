// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — UDP ClientConnection (the reliability hub).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "naudio/AudioPacket.hpp"
#include "naudio/ControlMessage.hpp"
#include "naudio/ControlReliability.hpp"
#include "naudio/FecDecoder.hpp"
#include "naudio/FecEncoder.hpp"
#include "naudio/JitterEstimator.hpp"
#include "naudio/PacketReorderBuffer.hpp"
#include "naudio/net/ClientAddress.hpp"
#include "naudio/net/Socket.hpp"
#include "naudio/net/Transport.hpp"

namespace naudio::net {

// A blocking FIFO of AudioPackets — the ordered queue. The reorder buffer / FEC
// decoder
// emit ordered packets here (producer side, under the connection's pipe lock);
// receivePacket() pops them (consumer side). poll(timeoutMs) blocks up to the
// deadline; poll() is the non-blocking drain. shutdown() wakes any blocked
// consumer so close() never strands a receive thread.
class BlockingPacketQueue {
public:
    // Backlog cap. A stalled consumer or a packet flood would otherwise let
    // the queue grow without bound (OOM). ~20 s of audio at 100 packets/s — far
    // above any healthy backlog, so it only ever trips under genuine stall/flood.
    static constexpr std::size_t kDefaultMaxSize = 2048;

    explicit BlockingPacketQueue(std::size_t maxSize = kDefaultMaxSize)
        : maxSize_(maxSize == 0 ? kDefaultMaxSize : maxSize) {}

    void offer(AudioPacket packet) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= maxSize_) {
                // Drop the OLDEST — for real-time audio the freshest data is the
                // most useful — and record it (the library has no logger; the
                // drop counter is the surfaced signal). C2.
                queue_.pop_front();
                ++drops_;
            }
            queue_.push_back(std::move(packet));
        }
        cv_.notify_one();
    }

    // Non-blocking pop: a packet if one is ready, else nullopt.
    std::optional<AudioPacket> poll() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        AudioPacket p = std::move(queue_.front());
        queue_.pop_front();
        return p;
    }

    // Blocking pop with a deadline. timeoutMs == 0 blocks indefinitely (until a
    // packet arrives or shutdown()). Returns nullopt on timeout/shutdown.
    std::optional<AudioPacket> poll(int timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto ready = [this] { return !queue_.empty() || shutdown_; };
        if (timeoutMs <= 0) {
            cv_.wait(lock, ready);
        } else {
            if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready)) {
                return std::nullopt;  // deadline elapsed
            }
        }
        if (queue_.empty()) return std::nullopt;  // shutdown
        AudioPacket p = std::move(queue_.front());
        queue_.pop_front();
        return p;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    // Number of packets dropped because the queue was at capacity.
    std::int64_t droppedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return drops_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<AudioPacket> queue_;
    bool shutdown_ = false;
    std::size_t maxSize_ = kDefaultMaxSize;
    std::int64_t drops_ = 0;
};

// Per-connection reliability configuration. Defaults mirror UdpClientConnection's
// no-FEC, no-jitter, no-control-reliability defaults with the default reorder
// window (DEFAULT_REORDER_WINDOW / MAX_HOLD_MS). The
// server transport fills this from its setters; the client transport from its
// ctor; DUAL from the AudioStreamConfig.
struct UdpReliabilityConfig {
    int reorderWindowSize = 8;   // 0 disables reordering (passthrough).
    int reorderMaxHoldMs = 30;
    bool fecEnabled = false;
    int fecBlockSize = 5;
    bool adaptiveJitterEnabled = false;
    int jitterMinMs = 0;
    int jitterMaxMs = 0;
    double jitterMultiplier = 3.0;
    bool controlReliabilityEnabled = false;
    int controlRetransmitMaxAttempts = 3;
};

// UDP implementation of ClientConnection.
//
// Unlike TCP (one socket per client, a stream framing FSM), a UDP connection
// shares the server's single socket and is identified by remote address; one
// datagram is exactly one frame, so there is no resumable framing FSM. What this
// class adds over a bare datagram pipe is the RELIABILITY HUB: it assembles the
// reliability subsystems (reorder buffer -> FEC decoder -> ordered queue on receive;
// FEC encoder + control ARQ on send) into a live data path. This is where the
// reliability algorithms run in anger.
//
// Two receive modes:
//   server-fed   (ownsSocket == false): the server demux thread calls
//                enqueueReceived(); the application thread calls receivePacket(),
//                which drains the ordered queue. The connection only SENDS on the
//                shared socket (it never reads it).
//   client-owned (ownsSocket == true):  receivePacket() reads the owned socket
//                directly (receiveFromSocket), runs the pipeline, drains the queue.
//
// Threading:
// one `pipe` mutex guards all five recovery subsystems. It is NEVER held across a
// socket send — every receive-path action that must send (control ACK, NACK
// retransmit, retransmit sweep) collects packets under the lock and sends them
// after releasing it (collect-under-lock / send-after-unlock).
// The emitter lambdas capture `this`, so the connection is non-movable and is
// always held via shared_ptr. Stats are atomic for lock-free const getters.
class UdpClientConnection : public ClientConnection {
public:
    static constexpr int UDP_HEARTBEAT_INTERVAL_MS = 3000;   // faster than TCP for NAT keepalive
    static constexpr int UDP_CONNECTION_TIMEOUT_MS = 8000;   // faster failover than TCP
    static constexpr int MAX_CONSECUTIVE_CRC_ERRORS = 20;    // UDP tolerates more than TCP
    // Datagram buffer: header + max payload + CRC + margin.
    static constexpr std::size_t MAX_DATAGRAM_SIZE =
        AudioPacket::HEADER_SIZE + AudioPacket::MAX_PAYLOAD + AudioPacket::CRC_SIZE + 64;

    // Server-fed: borrows the server's shared socket (must outlive this
    // connection); ownsSocket == false. `sharedSocket` is used only for sends.
    UdpClientConnection(Socket* sharedSocket, std::string remoteHost,
                        std::uint16_t remotePort, ClientAddress address,
                        const UdpReliabilityConfig& cfg);

    // Client-owned: takes ownership of a dedicated socket; ownsSocket == true.
    UdpClientConnection(Socket ownedSocket, std::string remoteHost,
                        std::uint16_t remotePort, ClientAddress address,
                        const UdpReliabilityConfig& cfg);

    ~UdpClientConnection() override;

    UdpClientConnection(const UdpClientConnection&) = delete;
    UdpClientConnection& operator=(const UdpClientConnection&) = delete;

    const ClientAddress& clientAddress() const override { return address_; }

    // --- Send (false on I/O failure / closed) ---
    bool sendControl(const ControlMessage& message) override;
    bool sendRxAudio(const std::uint8_t* data, std::size_t offset, std::size_t length) override;
    bool sendTxAudio(const std::uint8_t* data, std::size_t length) override;
    bool sendHeartbeat() override;
    bool sendPacket(const AudioPacket& packet) override;

    // --- Receive ---
    ReceiveResult receivePacket(int timeoutMs) override;

    // Feeds a packet from the server demux (server-fed mode). rawBytes is the
    // datagram length (for byte stats). (UdpServerTransport calls this.)
    void enqueueReceived(const AudioPacket& packet, int rawBytes);
    // Records a CRC error observed by the demux (undeserializable datagram).
    void recordCrcError();

    // --- Heartbeat / timeout ---
    bool shouldSendHeartbeat() override;
    bool isConnectionTimedOut() const override {
        return nowMs() - lastReceiveTime_.load() > UDP_CONNECTION_TIMEOUT_MS;
    }
    std::int64_t timeSinceLastReceive() const override {
        return nowMs() - lastReceiveTime_.load();
    }

    // --- Statistics ---
    std::int64_t packetsSent() const override { return packetsSent_.load(); }
    std::int64_t packetsReceived() const override { return packetsReceived_.load(); }
    std::int64_t bytesSent() const override { return bytesSent_.load(); }
    std::int64_t bytesReceived() const override { return bytesReceived_.load(); }
    int crcErrors() const override { return crcErrors_.load(); }

    std::int64_t packetsLost() const override { return packetsLost_.load(); }
    std::int64_t packetsOutOfOrder() const override { return packetsOutOfOrder_.load(); }
    double packetLossRate() const override;
    std::int64_t packetsReordered() const override;
    std::int64_t packetsRecoveredByFec() const override;
    double jitterMs() const override;
    int adaptiveBufferTargetMs() const override;
    std::int64_t controlRetransmits() const override;

    // --- Diagnostics (concrete; not on the virtual ClientConnection interface) ---
    // Packets dropped from the ordered queue under backpressure and the
    // current queue depth — exposed so a flood test can assert the bound holds.
    std::int64_t orderedQueueDrops() const { return orderedQueue_.droppedCount(); }
    std::size_t orderedQueueSize() const { return orderedQueue_.size(); }

    // UDP datagrams handed to the socket whose serialized size exceeded
    // AudioPacket::UDP_MAX_PAYLOAD and therefore IP-fragment on a standard
    // ~1500-byte-MTU path. Non-zero means a caller is sizing UDP audio frames
    // above the MTU — every fragment must arrive or the whole datagram (and any
    // FEC benefit) is lost. Advisory only; the datagram is still sent.
    std::int64_t oversizedDatagrams() const { return oversizedDatagrams_.load(); }

    std::string remoteAddress() const override {
        return remoteHost_ + ":" + std::to_string(remotePort_);
    }
    bool isClosed() const override { return closed_.load(); }
    void close() override;

private:
    // Builds the reorder -> FEC -> ordered-queue chain and the send-side encoder /
    // control reliability from cfg (the constructor's shared body).
    void initPipeline(const UdpReliabilityConfig& cfg);

    // Records an audio packet in the FEC encoder and sends the parity packet if a
    // block completed. Collect-under-lock / send-after-unlock.
    void maybeSendFecParity(const AudioPacket& audioPacket);

    // Outcome of a control-reliability pass: whether the packet was consumed (an
    // ACK/NACK that must not surface to the application) and any packets to send
    // AFTER the pipe lock is released.
    struct ControlOutcome {
        bool consumed = false;
        std::vector<AudioPacket> outgoing;
    };
    // Runs ACK/NACK/critical-ack logic. MUST be called with pipe_ held; it never
    // sends — the caller sends `outgoing` after unlocking.
    ControlOutcome handleControlReliabilityLocked(const AudioPacket& packet);

    // Collects timed-out control packets to retransmit (under pipe_) and sends
    // them after releasing the lock. Piggybacked on the heartbeat check.
    void checkControlRetransmits();

    // Client-side socket read path (ownsSocket only).
    ReceiveResult receiveFromSocket(int timeoutMs);

    // True if a datagram's numeric source matches the configured server endpoint
    // (remoteHost_/remotePort_). Spoofed/stray datagrams are dropped distinctly,
    // never counted as CRC errors. Only used in the client-owned path.
    bool isExpectedSender(const std::string& host, std::uint16_t port) const;

    // Sequence-gap detection (single receive thread per connection; atomics make
    // the const getters safe).
    void trackSequence(std::int32_t sequenceNumber);

    std::int32_t nextSequence() { return sequenceCounter_.fetch_add(1); }

    static std::int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // The active socket (owned or borrowed); sends/recvs go through it.
    Socket* socket_;
    std::optional<Socket> ownedSocket_;  // present iff ownsSocket_
    bool ownsSocket_;

    std::string remoteHost_;
    std::uint16_t remotePort_;
    ClientAddress address_;
    // Numeric IPv4 of remoteHost_, resolved once in initPipeline; "" if it could
    // not be resolved (host validation is then skipped — fail-open so a working
    // connection is never broken by a resolver hiccup). C3.
    std::string expectedSenderHost_;

    std::atomic<std::int32_t> sequenceCounter_{0};
    std::atomic<bool> closed_{false};

    BlockingPacketQueue orderedQueue_;

    // The reliability subsystems, all guarded by pipe_. A
    // disengaged optional means the feature is off.
    mutable std::mutex pipe_;
    std::optional<PacketReorderBuffer> reorderBuffer_;
    std::optional<FecEncoder> fecEncoder_;
    std::optional<FecDecoder> fecDecoder_;
    std::optional<JitterEstimator> jitterEstimator_;
    std::optional<ControlReliability> controlReliability_;

    // Timing (wall-clock milliseconds).
    std::atomic<std::int64_t> lastSendTime_{0};
    std::atomic<std::int64_t> lastReceiveTime_{0};

    // Statistics.
    std::atomic<std::int64_t> packetsSent_{0};
    std::atomic<std::int64_t> packetsReceived_{0};
    std::atomic<std::int64_t> bytesSent_{0};
    std::atomic<std::int64_t> bytesReceived_{0};
    std::atomic<int> crcErrors_{0};
    std::atomic<int> consecutiveCrcErrors_{0};
    std::atomic<std::int64_t> oversizedDatagrams_{0};  // datagrams > UDP_MAX_PAYLOAD (will IP-fragment)

    // Sequence-gap detection state (single writer: the receive thread).
    std::atomic<std::int64_t> highestSequenceSeen_{-1};
    std::atomic<std::int64_t> packetsLost_{0};
    std::atomic<std::int64_t> packetsOutOfOrder_{0};
};

}  // namespace naudio::net
