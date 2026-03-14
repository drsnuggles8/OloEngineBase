#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Core/NetworkMessage.h"

using namespace OloEngine;

TEST(NetworkMessageDispatcherTest, RegisterAndDispatch)
{
    NetworkMessageDispatcher dispatcher;

    u32 receivedSender = 0;
    u32 receivedSize = 0;
    bool called = false;

    dispatcher.RegisterHandler(ENetworkMessageType::Ping,
                               [&](u32 sender, const u8* /*data*/, u32 size)
                               {
                                   called = true;
                                   receivedSender = sender;
                                   receivedSize = size;
                               });

    u8 payload[] = { 1, 2, 3, 4 };
    dispatcher.Dispatch(42, ENetworkMessageType::Ping, payload, 4);

    EXPECT_TRUE(called);
    EXPECT_EQ(receivedSender, 42u);
    EXPECT_EQ(receivedSize, 4u);
}

TEST(NetworkMessageDispatcherTest, HasHandler)
{
    NetworkMessageDispatcher dispatcher;

    EXPECT_FALSE(dispatcher.HasHandler(ENetworkMessageType::Ping));

    dispatcher.RegisterHandler(ENetworkMessageType::Ping, [](u32, const u8*, u32) {});

    EXPECT_TRUE(dispatcher.HasHandler(ENetworkMessageType::Ping));
    EXPECT_FALSE(dispatcher.HasHandler(ENetworkMessageType::Pong));
}

TEST(NetworkMessageDispatcherTest, UnregisteredTypeDoesNotCrash)
{
    NetworkMessageDispatcher dispatcher;

    // Should not crash — just logs a warning
    u8 payload[] = { 0 };
    dispatcher.Dispatch(0, ENetworkMessageType::RPC, payload, 1);
}

TEST(NetworkMessageDispatcherTest, MultipleHandlersForDifferentTypes)
{
    NetworkMessageDispatcher dispatcher;

    int pingCount = 0;
    int pongCount = 0;

    dispatcher.RegisterHandler(ENetworkMessageType::Ping, [&](u32, const u8*, u32)
                               { ++pingCount; });
    dispatcher.RegisterHandler(ENetworkMessageType::Pong, [&](u32, const u8*, u32)
                               { ++pongCount; });

    dispatcher.Dispatch(1, ENetworkMessageType::Ping, nullptr, 0);
    dispatcher.Dispatch(2, ENetworkMessageType::Ping, nullptr, 0);
    dispatcher.Dispatch(3, ENetworkMessageType::Pong, nullptr, 0);

    EXPECT_EQ(pingCount, 2);
    EXPECT_EQ(pongCount, 1);
}

TEST(NetworkMessageDispatcherTest, HandlerReplacesExisting)
{
    NetworkMessageDispatcher dispatcher;

    int firstCount = 0;
    int secondCount = 0;

    dispatcher.RegisterHandler(ENetworkMessageType::Ping, [&](u32, const u8*, u32)
                               { ++firstCount; });
    dispatcher.RegisterHandler(ENetworkMessageType::Ping, [&](u32, const u8*, u32)
                               { ++secondCount; });

    dispatcher.Dispatch(0, ENetworkMessageType::Ping, nullptr, 0);

    EXPECT_EQ(firstCount, 0);
    EXPECT_EQ(secondCount, 1);
}

TEST(NetworkStatsTest, RecordSendUpdatesCounters)
{
    NetworkStats stats;

    stats.RecordSend(100);
    stats.RecordSend(200);

    EXPECT_EQ(stats.TotalBytesSent, 300u);
    EXPECT_EQ(stats.TotalMessagesSent, 2u);
    EXPECT_EQ(stats.TotalBytesReceived, 0u);
    EXPECT_EQ(stats.TotalMessagesReceived, 0u);
}

TEST(NetworkStatsTest, RecordReceiveUpdatesCounters)
{
    NetworkStats stats;

    stats.RecordReceive(50);
    stats.RecordReceive(75);
    stats.RecordReceive(25);

    EXPECT_EQ(stats.TotalBytesReceived, 150u);
    EXPECT_EQ(stats.TotalMessagesReceived, 3u);
}

TEST(NetworkStatsTest, UpdateRatesComputesCorrectly)
{
    NetworkStats stats;

    stats.RecordSend(1000);
    stats.RecordReceive(500);

    // Accumulate 1 second
    stats.UpdateRates(1.0f);

    EXPECT_FLOAT_EQ(stats.BytesSentPerSec, 1000.0f);
    EXPECT_FLOAT_EQ(stats.BytesReceivedPerSec, 500.0f);
    EXPECT_FLOAT_EQ(stats.MessagesSentPerSec, 1.0f);
    EXPECT_FLOAT_EQ(stats.MessagesReceivedPerSec, 1.0f);
}

TEST(NetworkStatsTest, UpdateRatesNotTriggeredBeforeOneSecond)
{
    NetworkStats stats;

    stats.RecordSend(1000);
    stats.UpdateRates(0.5f);

    // Should not have updated yet (< 1 second accumulated)
    EXPECT_FLOAT_EQ(stats.BytesSentPerSec, 0.0f);
    EXPECT_FLOAT_EQ(stats.MessagesSentPerSec, 0.0f);
}

TEST(NetworkStatsTest, ResetClearsEverything)
{
    NetworkStats stats;

    stats.RecordSend(100);
    stats.RecordReceive(200);
    stats.UpdateRates(1.0f);
    stats.Reset();

    EXPECT_EQ(stats.TotalBytesSent, 0u);
    EXPECT_EQ(stats.TotalBytesReceived, 0u);
    EXPECT_EQ(stats.TotalMessagesSent, 0u);
    EXPECT_EQ(stats.TotalMessagesReceived, 0u);
    EXPECT_FLOAT_EQ(stats.BytesSentPerSec, 0.0f);
    EXPECT_FLOAT_EQ(stats.BytesReceivedPerSec, 0.0f);
}
