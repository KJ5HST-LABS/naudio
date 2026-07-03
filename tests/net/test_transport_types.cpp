// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — transport value types.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Value types: ClientAddress (equality/hash by id only) + ReceivedPacket.

#include "naudio/net/ClientAddress.hpp"

#include <gtest/gtest.h>

#include <unordered_map>
#include <unordered_set>

using namespace naudio;       // AudioPacket, PacketType
using namespace naudio::net;  // ClientAddress, ReceivedPacket

TEST(ClientAddress, EqualityIsByIdOnlyIgnoringEndpoint) {
    ClientAddress a("tcp-1");
    ClientAddress b("tcp-1", "10.0.0.5", 4533);  // same id, different/added endpoint
    ClientAddress c("tcp-2", "10.0.0.5", 4533);  // different id, same endpoint

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(b, c);
}

TEST(ClientAddress, EndpointAccessors) {
    ClientAddress tcp("tcp-1");
    EXPECT_FALSE(tcp.hasEndpoint());

    ClientAddress udp("udp-1", "127.0.0.1", 5000);
    ASSERT_TRUE(udp.hasEndpoint());
    EXPECT_EQ(udp.host(), "127.0.0.1");
    EXPECT_EQ(udp.port(), 5000);
}

TEST(ClientAddress, HashIsByIdOnly) {
    std::hash<ClientAddress> h;
    EXPECT_EQ(h(ClientAddress("x")), h(ClientAddress("x", "1.2.3.4", 9)));
    EXPECT_NE(h(ClientAddress("x")), h(ClientAddress("y")));

    // Usable as a set/map key, deduplicating by id.
    std::unordered_set<ClientAddress> set;
    set.insert(ClientAddress("a"));
    set.insert(ClientAddress("a", "9.9.9.9", 1));  // same id -> no new entry
    set.insert(ClientAddress("b"));
    EXPECT_EQ(set.size(), 2u);
}

TEST(ClientAddress, ToString) {
    EXPECT_EQ(ClientAddress("tcp-3").toString(), "ClientAddress[tcp-3]");
    EXPECT_EQ(ClientAddress("udp-3", "127.0.0.1", 4533).toString(),
              "ClientAddress[udp-3 @ 127.0.0.1:4533]");
}

TEST(ReceivedPacket, CarriesPacketAndSender) {
    AudioPacket pkt = AudioPacket::createHeartbeat(7);
    ReceivedPacket rp(std::move(pkt), ClientAddress("udp-9", "127.0.0.1", 4533));

    EXPECT_EQ(rp.packet().packetType(), PacketType::Heartbeat);
    EXPECT_EQ(rp.packet().sequence(), 7);
    EXPECT_EQ(rp.sender().id(), "udp-9");
    EXPECT_EQ(rp.sender().port(), 4533);
}
