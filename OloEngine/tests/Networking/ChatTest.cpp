#include <gtest/gtest.h>

#include "OloEngine/Networking/Chat/ChatChannel.h"
#include "OloEngine/Networking/Chat/ChatManager.h"

using namespace OloEngine;

// ============================================================================
// ChatChannel Tests
// ============================================================================

TEST(ChatChannel, JoinLeave)
{
    ChatChannel channel(1, EChatChannelType::Zone, "General");

    channel.Join(10);
    channel.Join(20);
    EXPECT_TRUE(channel.HasSubscriber(10));
    EXPECT_TRUE(channel.HasSubscriber(20));
    EXPECT_EQ(channel.GetSubscriberCount(), 2u);

    channel.Leave(10);
    EXPECT_FALSE(channel.HasSubscriber(10));
    EXPECT_EQ(channel.GetSubscriberCount(), 1u);
}

TEST(ChatChannel, Properties)
{
    ChatChannel channel(42, EChatChannelType::Guild, "MyGuild");
    EXPECT_EQ(channel.GetID(), 42u);
    EXPECT_EQ(channel.GetType(), EChatChannelType::Guild);
    EXPECT_EQ(channel.GetName(), "MyGuild");
}

// ============================================================================
// ChatManager Tests
// ============================================================================

TEST(ChatManager, CreateDestroyChannel)
{
    ChatManager mgr;

    u32 ch1 = mgr.CreateChannel(EChatChannelType::Zone, "Zone1");
    u32 ch2 = mgr.CreateChannel(EChatChannelType::Global, "World");
    EXPECT_EQ(mgr.GetChannelCount(), 2u);

    mgr.DestroyChannel(ch1);
    EXPECT_EQ(mgr.GetChannelCount(), 1u);
    EXPECT_EQ(mgr.GetChannel(ch1), nullptr);
    EXPECT_NE(mgr.GetChannel(ch2), nullptr);
}

TEST(ChatManager, ZoneChatDelivery)
{
    ChatManager mgr;
    u32 chID = mgr.CreateChannel(EChatChannelType::Zone, "Zone1");

    mgr.JoinChannel(chID, 1);
    mgr.JoinChannel(chID, 2);
    mgr.JoinChannel(chID, 3);

    ChatMessage msg;
    msg.SenderClientID = 1;
    msg.SenderName = "Player1";
    msg.Type = EChatChannelType::Zone;
    msg.ChannelID = chID;
    msg.Content = "Hello zone!";

    auto recipients = mgr.RouteMessage(msg);
    EXPECT_EQ(recipients.size(), 3u); // All subscribers including sender
    EXPECT_EQ(mgr.GetTotalMessagesRouted(), 1u);
}

TEST(ChatManager, MessageFilterBlocking)
{
    ChatManager mgr;
    u32 chID = mgr.CreateChannel(EChatChannelType::Zone, "Zone1");
    mgr.JoinChannel(chID, 1);

    // Set filter that blocks messages containing "blocked"
    mgr.SetMessageFilter([](const ChatMessage& msg)
                         { return msg.Content.find("blocked") == std::string::npos; });

    ChatMessage goodMsg;
    goodMsg.ChannelID = chID;
    goodMsg.Content = "Hello!";
    EXPECT_EQ(mgr.RouteMessage(goodMsg).size(), 1u);

    ChatMessage badMsg;
    badMsg.ChannelID = chID;
    badMsg.Content = "This is blocked content";
    EXPECT_TRUE(mgr.RouteMessage(badMsg).empty());
}

TEST(ChatManager, RemoveClientFromAllChannels)
{
    ChatManager mgr;
    u32 ch1 = mgr.CreateChannel(EChatChannelType::Zone, "Zone1");
    u32 ch2 = mgr.CreateChannel(EChatChannelType::Global, "World");

    mgr.JoinChannel(ch1, 1);
    mgr.JoinChannel(ch2, 1);

    mgr.RemoveClientFromAllChannels(1);

    EXPECT_FALSE(mgr.GetChannel(ch1)->HasSubscriber(1));
    EXPECT_FALSE(mgr.GetChannel(ch2)->HasSubscriber(1));
}

TEST(ChatManager, GetChannelsByType)
{
    ChatManager mgr;
    mgr.CreateChannel(EChatChannelType::Zone, "Zone1");
    mgr.CreateChannel(EChatChannelType::Zone, "Zone2");
    mgr.CreateChannel(EChatChannelType::Global, "World");

    auto zoneChannels = mgr.GetChannelsByType(EChatChannelType::Zone);
    EXPECT_EQ(zoneChannels.size(), 2u);

    auto globalChannels = mgr.GetChannelsByType(EChatChannelType::Global);
    EXPECT_EQ(globalChannels.size(), 1u);
}
