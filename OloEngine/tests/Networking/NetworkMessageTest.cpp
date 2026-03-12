#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Networking/Core/NetworkMessage.h"

namespace OloEngine::Tests
{
    // -------------------------------------------------------------------------
    // NetworkMessageTest
    // -------------------------------------------------------------------------

    TEST(NetworkMessageTest, HeaderSize)
    {
        EXPECT_EQ(sizeof(NetworkMessageHeader), 8u);
    }

    TEST(NetworkMessageTest, DefaultHeaderValues)
    {
        NetworkMessageHeader header;
        EXPECT_EQ(header.Type,  ENetworkMessageType::None);
        EXPECT_EQ(header.Size,  0u);
        EXPECT_EQ(header.Flags, 0u);
    }

    TEST(NetworkMessageTest, ParameterizedConstruction)
    {
        NetworkMessageHeader header(ENetworkMessageType::Ping, 42u, 0x01u);
        EXPECT_EQ(header.Type,  ENetworkMessageType::Ping);
        EXPECT_EQ(header.Size,  42u);
        EXPECT_EQ(header.Flags, 0x01u);
    }

    TEST(NetworkMessageTest, UserMessageRangeStart)
    {
        // ENetworkMessageType::UserMessage must be >= 1000
        EXPECT_GE(static_cast<u16>(ENetworkMessageType::UserMessage), static_cast<u16>(1000));
    }

    TEST(NetworkMessageTest, EntitySnapshotTypeExists)
    {
        NetworkMessageHeader header(ENetworkMessageType::EntitySnapshot, 0u);
        EXPECT_EQ(header.Type, ENetworkMessageType::EntitySnapshot);
    }

} // namespace OloEngine::Tests
