// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — ControlReliability (ARQ) unit tests.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// No conformance vector gates the ARQ layer, so
// these tests ARE the contract: critical-type gating, ACK/NACK, timeout-based
// retransmit (driven via the injected checkRetransmitsAt clock, no sleeps),
// oldest-first eviction at BUFFER_SIZE, and CONTROL_ACK generation.
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "naudio/AudioPacket.hpp"
#include "naudio/ControlMessage.hpp"
#include "naudio/ControlReliability.hpp"

using naudio::AudioPacket;
using naudio::ControlMessage;
using naudio::ControlReliability;
using naudio::ControlType;

namespace {

AudioPacket controlPacket(std::int32_t seq, const ControlMessage& msg) {
    return AudioPacket::createControl(seq, msg.serialize());
}

ControlReliability reliability() { return ControlReliability(3, 500); }

TEST(ControlReliability, ConstructorRejectsInvalidArgs) {
    EXPECT_THROW(ControlReliability(0, 500), std::invalid_argument);
    EXPECT_THROW(ControlReliability(3, 0), std::invalid_argument);
}

TEST(ControlReliability, CriticalTypesAreTracked) {
    EXPECT_TRUE(ControlReliability::isCriticalType(ControlType::ConnectAccept));
    EXPECT_TRUE(ControlReliability::isCriticalType(ControlType::TxGranted));
    EXPECT_TRUE(ControlReliability::isCriticalType(ControlType::StreamStart));
    EXPECT_TRUE(ControlReliability::isCriticalType(ControlType::ClientsUpdate));
    EXPECT_TRUE(ControlReliability::isCriticalType(ControlType::Disconnect));
}

TEST(ControlReliability, NonCriticalTypesAreNotTracked) {
    EXPECT_FALSE(ControlReliability::isCriticalType(ControlType::Heartbeat));
    EXPECT_FALSE(ControlReliability::isCriticalType(ControlType::HeartbeatAck));
    EXPECT_FALSE(ControlReliability::isCriticalType(ControlType::LatencyProbe));
    EXPECT_FALSE(ControlReliability::isCriticalType(ControlType::StatsUpdate));
    EXPECT_FALSE(ControlReliability::isCriticalType(ControlType::Nack));
    EXPECT_FALSE(ControlReliability::isCriticalType(ControlType::ControlAck));
    EXPECT_FALSE(ControlReliability::isCriticalType(ControlType::ConnectRequest));
}

TEST(ControlReliability, RecordSentTracksCriticalControls) {
    ControlReliability r = reliability();
    r.recordSentAt(controlPacket(1, ControlMessage::txGranted()), 1000);
    EXPECT_EQ(1u, r.pendingCount());
}

TEST(ControlReliability, RecordSentIgnoresNonCriticalControls) {
    ControlReliability r = reliability();
    r.recordSentAt(controlPacket(1, ControlMessage::heartbeat()), 1000);
    EXPECT_EQ(0u, r.pendingCount());
}

TEST(ControlReliability, RecordSentIgnoresNonControlPackets) {
    ControlReliability r = reliability();
    r.recordSentAt(AudioPacket::createRxAudio(1, std::vector<std::uint8_t>(100, 0)), 1000);
    EXPECT_EQ(0u, r.pendingCount());
}

TEST(ControlReliability, AckRemovesPendingPacket) {
    ControlReliability r = reliability();
    r.recordSentAt(controlPacket(42, ControlMessage::connectAccept()), 1000);
    EXPECT_EQ(1u, r.pendingCount());
    EXPECT_TRUE(r.onAckReceived(42));
    EXPECT_EQ(0u, r.pendingCount());
}

TEST(ControlReliability, AckForUnknownSeqReturnsFalse) {
    ControlReliability r = reliability();
    EXPECT_FALSE(r.onAckReceived(999));
}

TEST(ControlReliability, NackReturnsStoredPacket) {
    ControlReliability r = reliability();
    r.recordSentAt(controlPacket(10, ControlMessage::txGranted()), 1000);
    auto retransmit = r.onNackReceived(10);
    ASSERT_TRUE(retransmit.has_value());
    EXPECT_EQ(10, retransmit->sequence());
    EXPECT_EQ(1, r.controlRetransmits());
}

TEST(ControlReliability, NackForEvictedPacketReturnsNullopt) {
    ControlReliability r = reliability();
    EXPECT_FALSE(r.onNackReceived(999).has_value());
    EXPECT_EQ(0, r.controlRetransmits());
}

TEST(ControlReliability, CheckRetransmitsReturnsTimedOutPackets) {
    ControlReliability r = reliability();
    r.recordSentAt(controlPacket(5, ControlMessage::streamStart()), 1000);
    EXPECT_TRUE(r.checkRetransmitsAt(1400).empty());  // before timeout
    auto retransmits = r.checkRetransmitsAt(1500);     // at timeout
    ASSERT_EQ(1u, retransmits.size());
    EXPECT_EQ(5, retransmits[0].sequence());
    EXPECT_EQ(1, r.controlRetransmits());
}

TEST(ControlReliability, ExhaustedRetriesRemovesPacket) {
    ControlReliability r(2, 100);  // max 2 attempts
    r.recordSentAt(controlPacket(1, ControlMessage::txDenied()), 0);
    auto first = r.checkRetransmitsAt(100);  // attempt 2 of 2 -> retransmit
    EXPECT_EQ(1u, first.size());
    EXPECT_EQ(1u, r.pendingCount());
    auto second = r.checkRetransmitsAt(200);  // exhausted -> remove
    EXPECT_TRUE(second.empty());
    EXPECT_EQ(0u, r.pendingCount());
}

TEST(ControlReliability, BufferEvictsOldestWhenFull) {
    ControlReliability r = reliability();
    for (std::int32_t i = 0; i < static_cast<std::int32_t>(ControlReliability::BUFFER_SIZE); ++i) {
        r.recordSentAt(controlPacket(i, ControlMessage::txGranted()), 1000);
    }
    EXPECT_EQ(ControlReliability::BUFFER_SIZE, r.pendingCount());
    r.recordSentAt(controlPacket(100, ControlMessage::txGranted()), 1000);  // evicts seq 0
    EXPECT_EQ(ControlReliability::BUFFER_SIZE, r.pendingCount());
    EXPECT_FALSE(r.onNackReceived(0).has_value());   // seq 0 evicted
    EXPECT_TRUE(r.onNackReceived(100).has_value());   // seq 100 present
}

TEST(ControlReliability, GenerateAckForCriticalControl) {
    ControlReliability r = reliability();
    auto ack = r.generateAck(controlPacket(7, ControlMessage::txGranted()));
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ControlType::ControlAck, ack->messageType());
    EXPECT_EQ(7, ack->parseControlAckSequence());
}

TEST(ControlReliability, GenerateAckReturnsNulloptForNonCritical) {
    ControlReliability r = reliability();
    EXPECT_FALSE(r.generateAck(controlPacket(1, ControlMessage::heartbeat())).has_value());
}

TEST(ControlReliability, GenerateAckReturnsNulloptForNonControl) {
    ControlReliability r = reliability();
    EXPECT_FALSE(
        r.generateAck(AudioPacket::createRxAudio(1, std::vector<std::uint8_t>(100, 0))).has_value());
}

TEST(ControlReliability, ResetClearsAllState) {
    ControlReliability r = reliability();
    r.recordSentAt(controlPacket(1, ControlMessage::txGranted()), 1000);
    r.onNackReceived(1);
    EXPECT_EQ(1u, r.pendingCount());
    EXPECT_EQ(1, r.controlRetransmits());
    r.reset();
    EXPECT_EQ(0u, r.pendingCount());
    EXPECT_EQ(0, r.controlRetransmits());
}

}  // namespace
