// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — TCP protocol handler (impl).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
//
// The receive FSM. A no-data result (ReceiveResult::noData()) means retry; a dead
// result (ReceiveResult::dead() + closed_ = true) on EOF / I/O error / too many
// consecutive errors means the caller tears the connection down.

#include "naudio/net/AudioProtocolHandler.hpp"

#include <chrono>
#include <cstring>
#include <utility>

namespace naudio::net {

namespace {
std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

AudioProtocolHandler::AudioProtocolHandler(Socket socket)
    : socket_(std::move(socket)),
      lastSendTime_(nowMs()),
      lastReceiveTime_(nowMs()),
      recvBuf_(AudioPacket::HEADER_SIZE, 0) {
    // Every TCP send on this connection funnels through sendPacket's sendMutex_, so an
    // unbounded send does not merely stall one frame — it wedges every other sender,
    // including the watchdog and teardown paths whose job is to notice a dead peer
    // (issues #56, #69). Arm the deadline once, here, where the connection is born.
    //
    // ARMED IN THE HANDLER, NOT IN THE TRANSPORTS. Both TcpClientConnection construction
    // sites — TcpClientTransport::connect for a client and TcpServerTransport::acceptClient
    // for a server session — end up here, so this is the single site that covers both, and a
    // third TCP connection site would inherit it rather than have to remember it. #69 armed
    // it in the client transport only, which left every server session with no send deadline
    // at all; that was issue #56's remaining half and this is it.
    //
    // HALF THE NO-RX DEATH WINDOW, derived rather than hand-picked: a frame that cannot be
    // pushed in this long is given up on no later than the point at which the heartbeat
    // watchdog would already have declared the peer dead. A failed send tears the connection
    // down, which is the right outcome — a peer that has not made room for one audio frame in
    // five seconds is not coming back. Socket::setSendTimeout is also the whole-call budget
    // sendAll spends across its retry loop (#70); see that header for what the pair does and
    // does not bound.
    socket_.setSendTimeout(CONNECTION_TIMEOUT_MS / 2);
}

AudioProtocolHandler::~AudioProtocolHandler() { close(); }

// ---- Send ------------------------------------------------------------------

bool AudioProtocolHandler::sendPacket(const AudioPacket& packet) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    return sendPacketLocked(packet);
}

bool AudioProtocolHandler::sendPacketLocked(const AudioPacket& packet) {
    if (closed_.load()) return false;

    std::vector<std::uint8_t> data = packet.serialize();
    if (!socket_.sendAll(data.data(), data.size())) return false;

    lastSendTime_.store(nowMs());
    packetsSent_.fetch_add(1);
    bytesSent_.fetch_add(static_cast<std::int64_t>(data.size()));
    return true;
}

bool AudioProtocolHandler::sendRxAudio(const std::uint8_t* data, std::size_t offset,
                                       std::size_t length) {
    std::vector<std::uint8_t> buf(data + offset, data + offset + length);
    return sendPacket(AudioPacket::createRxAudio(nextSequence(), std::move(buf)));
}

bool AudioProtocolHandler::sendTxAudio(const std::uint8_t* data, std::size_t length) {
    std::vector<std::uint8_t> buf(data, data + length);
    return sendPacket(AudioPacket::createTxAudio(nextSequence(), std::move(buf)));
}

bool AudioProtocolHandler::sendControl(const ControlMessage& message) {
    return sendPacket(AudioPacket::createControl(nextSequence(), message.serialize()));
}

bool AudioProtocolHandler::sendHeartbeat() {
    return sendPacket(AudioPacket::createHeartbeat(nextSequence()));
}

bool AudioProtocolHandler::trySendControl(const ControlMessage& message) {
    std::unique_lock<std::mutex> lock(sendMutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;  // another sender holds it — decline, do not queue
    // The sequence number is allocated only AFTER the lock is won, so a declined call does
    // not burn one and leave a gap in a sequence space nothing ever transmitted.
    return sendPacketLocked(AudioPacket::createControl(nextSequence(), message.serialize()));
}

// ---- Receive FSM ----------------------------------------------------------

AudioProtocolHandler::Fill AudioProtocolHandler::fillRecvBuf() {
    while (recvFilled_ < recvBuf_.size()) {
        RecvResult r = socket_.recv(recvBuf_.data() + recvFilled_,
                                    recvBuf_.size() - recvFilled_);
        if (r.status == IoStatus::Ok) {
            recvFilled_ += r.bytes;
            continue;
        }
        if (r.status == IoStatus::TimedOut) {
            return Fill::Timeout;  // partial progress preserved in recvFilled_
        }
        return Fill::Fatal;  // Closed (EOF) or Error
    }
    return Fill::Filled;
}

void AudioProtocolHandler::resetRecvState() {
    recvPhase_ = PHASE_HEADER;
    recvBuf_.assign(AudioPacket::HEADER_SIZE, 0);
    recvFilled_ = 0;
    recvHeader_.clear();
}

ReceiveResult AudioProtocolHandler::receivePacket(int timeoutMs) {
    if (closed_.load()) {
        return ReceiveResult::dead();
    }

    socket_.setRecvTimeout(timeoutMs);

    if (recvPhase_ == PHASE_HEADER) {
        Fill fr = fillRecvBuf();
        if (fr == Fill::Timeout) return ReceiveResult::noData();  // partial header kept
        if (fr == Fill::Fatal) {
            closed_.store(true);
            return ReceiveResult::dead();
        }

        // Validate magic.
        std::uint16_t magic = static_cast<std::uint16_t>(
            ((recvBuf_[0] & 0xFF) << 8) | (recvBuf_[1] & 0xFF));
        if (magic != AudioPacket::MAGIC) {
            // Invalid magic — scan for the magic bytes to resync the stream.
            consecutiveCrcErrors_++;
            crcErrors_.fetch_add(1);
            if (consecutiveCrcErrors_ >= MAX_CONSECUTIVE_CRC_ERRORS) {
                closed_.store(true);
                return ReceiveResult::dead();
            }

            std::uint8_t magicHi = static_cast<std::uint8_t>((AudioPacket::MAGIC >> 8) & 0xFF);
            std::uint8_t magicLo = static_cast<std::uint8_t>(AudioPacket::MAGIC & 0xFF);
            int keepFrom = -1;

            for (std::size_t i = 1; i + 1 < recvBuf_.size(); i++) {
                if (recvBuf_[i] == magicHi && recvBuf_[i + 1] == magicLo) {
                    keepFrom = static_cast<int>(i);
                    break;
                }
            }
            // Magic may be split at the buffer boundary: keep a trailing
            // magicHi so the next fill can complete the match.
            if (keepFrom < 0 && recvBuf_.back() == magicHi) {
                keepFrom = static_cast<int>(recvBuf_.size() - 1);
            }

            if (keepFrom < 0) {
                recvFilled_ = 0;  // discard garbage header, caller retries
                return ReceiveResult::noData();
            }

            // Shift the kept tail to the front and resume filling the header on
            // the next pass (resumable across timeouts).
            std::size_t kept = recvBuf_.size() - static_cast<std::size_t>(keepFrom);
            std::memmove(recvBuf_.data(), recvBuf_.data() + keepFrom, kept);
            recvFilled_ = kept;
            return ReceiveResult::noData();  // caller retries; next call completes the header
        }

        // Header complete and aligned. Payload length is the last two header bytes.
        int payloadLen = ((recvBuf_[17] & 0xFF) << 8) | (recvBuf_[18] & 0xFF);
        if (static_cast<std::size_t>(payloadLen) > AudioPacket::MAX_PAYLOAD) {
            // Payload too large — skip payload + CRC to stay in sync.
            consecutiveCrcErrors_++;
            crcErrors_.fetch_add(1);
            if (consecutiveCrcErrors_ >= MAX_CONSECUTIVE_CRC_ERRORS) {
                closed_.store(true);
                return ReceiveResult::dead();
            }
            recvPhase_ = PHASE_SKIP;
            recvBuf_.assign(payloadLen + AudioPacket::CRC_SIZE, 0);
            recvFilled_ = 0;
        } else {
            recvHeader_ = recvBuf_;
            recvPhase_ = PHASE_PAYLOAD;
            recvBuf_.assign(payloadLen + AudioPacket::CRC_SIZE, 0);
            recvFilled_ = 0;
        }
    }

    if (recvPhase_ == PHASE_SKIP) {
        Fill fr = fillRecvBuf();
        if (fr == Fill::Timeout) return ReceiveResult::noData();  // skip progress kept
        if (fr == Fill::Fatal) {
            closed_.store(true);
            return ReceiveResult::dead();
        }
        resetRecvState();
        return ReceiveResult::noData();  // oversized packet skipped, caller retries
    }

    if (recvPhase_ == PHASE_PAYLOAD) {
        Fill fr = fillRecvBuf();
        if (fr == Fill::Timeout) return ReceiveResult::noData();  // partial payload kept
        if (fr == Fill::Fatal) {
            closed_.store(true);
            return ReceiveResult::dead();
        }

        // Combine header + payload into the full frame.
        std::vector<std::uint8_t> fullPacket;
        fullPacket.reserve(recvHeader_.size() + recvBuf_.size());
        fullPacket.insert(fullPacket.end(), recvHeader_.begin(), recvHeader_.end());
        fullPacket.insert(fullPacket.end(), recvBuf_.begin(), recvBuf_.end());
        resetRecvState();

        std::optional<AudioPacket> packet = AudioPacket::deserialize(fullPacket);
        if (!packet.has_value()) {
            // CRC validation failed — skip this packet but don't fail the connection.
            consecutiveCrcErrors_++;
            crcErrors_.fetch_add(1);
            if (consecutiveCrcErrors_ >= MAX_CONSECUTIVE_CRC_ERRORS) {
                closed_.store(true);
                return ReceiveResult::dead();
            }
            return ReceiveResult::noData();  // skip this packet, let caller retry
        }

        // Success — reset the consecutive error counter.
        consecutiveCrcErrors_ = 0;
        lastReceiveTime_.store(nowMs());
        packetsReceived_.fetch_add(1);
        bytesReceived_.fetch_add(static_cast<std::int64_t>(fullPacket.size()));
        // The TCP lane constructs no FEC decoder, so nothing here is ever a repair —
        // every packet this handler surfaces was decoded from bytes that actually
        // arrived. Always Live, by construction rather than by policy.
        return ReceiveResult::of(std::move(*packet), Provenance::Live);
    }

    return ReceiveResult::noData();
}

// ---- Heartbeat / timeout / lifecycle ---------------------------------------

bool AudioProtocolHandler::shouldSendHeartbeat() const {
    return nowMs() - lastSendTime_.load() > HEARTBEAT_INTERVAL_MS;
}

bool AudioProtocolHandler::isConnectionTimedOut() const {
    return nowMs() - lastReceiveTime_.load() > CONNECTION_TIMEOUT_MS;
}

std::int64_t AudioProtocolHandler::timeSinceLastReceive() const {
    return nowMs() - lastReceiveTime_.load();
}

void AudioProtocolHandler::close() {
    closed_.store(true);
    // BEFORE close(), not instead of it (issue #56, item 1). A session's writer thread can be
    // parked inside Socket::sendAll against a peer whose receive window has shut, and dropping the
    // descriptor does not reliably return that thread on every platform — it holds its own
    // reference to the open file description. shutdown() changes the socket's state instead, so
    // the parked send returns and the thread unwinds. Without it, teardown depends on a
    // platform behaviour this project only ever verified on macOS.
    socket_.shutdownBoth();
    socket_.close();
}

std::size_t AudioProtocolHandler::discardPendingInput(int budgetMs) {
    if (closed_.load() || !socket_.valid()) return 0;

    // A SHORT per-read timeout, not the budget: with data already buffered the first read returns
    // at once and only the read that finds the buffer empty pays the wait, so the ordinary cost of
    // this call is one 2 ms timeout rather than the whole budget. That matters because the only
    // caller runs on the ACCEPT THREAD — every millisecond spent here is a millisecond no other
    // client can be accepted.
    socket_.setRecvTimeout(2);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    std::uint8_t scratch[2048];
    std::size_t discarded = 0;
    for (;;) {
        RecvResult r = socket_.recv(scratch, sizeof scratch);
        // Anything but Ok means we are done: TimedOut is the buffer being empty (the expected
        // exit), Closed is the peer having already gone, Error is a socket about to be thrown
        // away anyway.
        if (r.status != IoStatus::Ok) break;
        discarded += r.bytes;
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    // The receive timeout is deliberately left as set — this socket is closed immediately after.
    return discarded;
}

}  // namespace naudio::net
