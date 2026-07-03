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
      recvBuf_(AudioPacket::HEADER_SIZE, 0) {}

AudioProtocolHandler::~AudioProtocolHandler() { close(); }

// ---- Send ------------------------------------------------------------------

bool AudioProtocolHandler::sendPacket(const AudioPacket& packet) {
    std::lock_guard<std::mutex> lock(sendMutex_);
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
        return ReceiveResult::of(std::move(*packet));
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
    socket_.close();
}

}  // namespace naudio::net
