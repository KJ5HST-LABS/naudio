// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — wire codec primitives.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace naudio {

// Big-endian read/write cursors over byte buffers — used by the AudioPacket +
// ControlMessage codecs and reusable by the reliability algorithms.
//
// ByteReader's getters assume the
// caller has already checked remaining(): every ControlMessage parse path guards
// its reads with a remaining() check. W4 hardening: a read past the end never
// advances the cursor past the end and trips a sticky truncated() flag — instead
// of silently zero-filling AND over-advancing (`pos_ += n`), which would let
// malformed frames through instead of rejecting them. Well-formed messages never
// trip it; a parser that wants to reject
// malformed input can check truncated() at its boundary.

// A position-tracking big-endian reader over a borrowed byte slice.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t len) : data_(data), len_(len), pos_(0) {}
    explicit ByteReader(const std::vector<std::uint8_t>& data)
        : data_(data.data()), len_(data.size()), pos_(0) {}

    // Bytes left to read.
    std::size_t remaining() const { return pos_ < len_ ? len_ - pos_ : 0; }
    // The current read position.
    std::size_t position() const { return pos_; }
    // Sets the read position (used to skip fields and to enforce framing).
    void setPosition(std::size_t pos) { pos_ = pos; }

    // True if any read attempted to go past the end (sticky). Well-formed messages
    // never set it; a parser can reject malformed input by checking this.
    bool truncated() const { return truncated_; }

    // Reads one byte and advances. Past the end: returns 0, trips truncated(), and
    // does NOT advance past the end.
    std::uint8_t getU8() {
        if (pos_ < len_) return data_[pos_++];
        truncated_ = true;
        return 0;
    }

    // Reads a big-endian unsigned 16-bit value and advances (the
    // `getShort() & 0xFFFF` idiom).
    std::uint16_t getU16() {
        std::uint16_t hi = getU8();
        std::uint16_t lo = getU8();
        return static_cast<std::uint16_t>((hi << 8) | lo);
    }

    // Reads a big-endian signed 32-bit value and advances.
    std::int32_t getI32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | getU8();
        return static_cast<std::int32_t>(v);
    }

    // Reads a big-endian signed 64-bit value and advances.
    std::int64_t getI64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | getU8();
        return static_cast<std::int64_t>(v);
    }

    // Reads `n` bytes (copied) and advances. Clamps to the remaining length, never
    // advances past the end, and trips truncated() on a short read.
    std::vector<std::uint8_t> readBytes(std::size_t n) {
        std::size_t avail = remaining();
        std::size_t take = n < avail ? n : avail;
        std::vector<std::uint8_t> out(data_ + pos_, data_ + pos_ + take);
        pos_ += take;
        if (take < n) truncated_ = true;
        return out;
    }

    // Reads `n` bytes as a UTF-8 string (lossy — bytes copied verbatim). Clamps to
    // the remaining
    // length, never advances past the end, and trips truncated() on a short read.
    std::string readString(std::size_t n) {
        std::size_t avail = remaining();
        std::size_t take = n < avail ? n : avail;
        std::string out(reinterpret_cast<const char*>(data_ + pos_), take);
        pos_ += take;
        if (take < n) truncated_ = true;
        return out;
    }

private:
    const std::uint8_t* data_;
    std::size_t len_;
    std::size_t pos_;
    bool truncated_ = false;
};

// A big-endian writer that appends to an owned byte buffer (the serialize side).
class ByteWriter {
public:
    explicit ByteWriter(std::size_t reserve = 0) { buf_.reserve(reserve); }

    void putU8(std::uint8_t v) { buf_.push_back(v); }
    void putU16(std::uint16_t v) {
        buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
    void putI32(std::int32_t v) {
        std::uint32_t u = static_cast<std::uint32_t>(v);
        for (int i = 3; i >= 0; --i) buf_.push_back(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFF));
    }
    void putU32(std::uint32_t u) {
        for (int i = 3; i >= 0; --i) buf_.push_back(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFF));
    }
    void putI64(std::int64_t v) {
        std::uint64_t u = static_cast<std::uint64_t>(v);
        for (int i = 7; i >= 0; --i) buf_.push_back(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFF));
    }
    void putBytes(const std::uint8_t* data, std::size_t n) { buf_.insert(buf_.end(), data, data + n); }
    void putBytes(const std::vector<std::uint8_t>& data) { buf_.insert(buf_.end(), data.begin(), data.end()); }

    // The accumulated bytes.
    const std::vector<std::uint8_t>& bytes() const { return buf_; }
    std::vector<std::uint8_t> take() { return std::move(buf_); }
    std::size_t size() const { return buf_.size(); }

private:
    std::vector<std::uint8_t> buf_;
};

}  // namespace naudio
