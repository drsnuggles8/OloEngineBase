#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Core/NetworkMessage.h"
#include "OloEngine/Serialization/Archive.h"

using namespace OloEngine;

TEST(NetworkMessageTest, HeaderSerializationRoundtrip)
{
    NetworkMessageHeader original;
    original.Type = ENetworkMessageType::EntitySnapshot;
    original.Size = 1024;
    original.Flags = 3;

    // Write
    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer.ArIsNetArchive = true;
    writer << original.Type;
    writer << original.Size;
    writer << original.Flags;
    writer << original.Version;

    // Read
    FMemoryReader reader(buffer);
    reader.ArIsNetArchive = true;
    NetworkMessageHeader loaded;
    reader << loaded.Type;
    reader << loaded.Size;
    reader << loaded.Flags;
    reader << loaded.Version;

    EXPECT_EQ(loaded.Type, original.Type);
    EXPECT_EQ(loaded.Size, original.Size);
    EXPECT_EQ(loaded.Flags, original.Flags);
    EXPECT_EQ(loaded.Version, original.Version);
    EXPECT_EQ(loaded.Version, NetworkMessageHeader::kCurrentVersion);
    EXPECT_FALSE(reader.IsError());
}

TEST(NetworkMessageTest, PrimitivePayloadRoundtrip)
{
    u32 origU32 = 42;
    f32 origF32 = 3.14f;
    std::string origStr = "Hello Network";

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer.ArIsNetArchive = true;
    writer << origU32;
    writer << origF32;
    writer << origStr;

    u32 loadedU32 = 0;
    f32 loadedF32 = 0.0f;
    std::string loadedStr;

    FMemoryReader reader(buffer);
    reader.ArIsNetArchive = true;
    reader << loadedU32;
    reader << loadedF32;
    reader << loadedStr;

    EXPECT_EQ(loadedU32, origU32);
    EXPECT_FLOAT_EQ(loadedF32, origF32);
    EXPECT_EQ(loadedStr, origStr);
    EXPECT_FALSE(reader.IsError());
}

TEST(NetworkMessageTest, EmptyPayload)
{
    NetworkMessageHeader original;
    original.Type = ENetworkMessageType::Ping;
    original.Size = 0;
    original.Flags = 0;

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original.Type;
    writer << original.Size;
    writer << original.Flags;
    writer << original.Version;

    FMemoryReader reader(buffer);
    NetworkMessageHeader loaded;
    reader << loaded.Type;
    reader << loaded.Size;
    reader << loaded.Flags;
    reader << loaded.Version;

    EXPECT_EQ(loaded.Type, ENetworkMessageType::Ping);
    EXPECT_EQ(loaded.Size, 0u);
    EXPECT_EQ(loaded.Flags, 0u);
    EXPECT_EQ(loaded.Version, NetworkMessageHeader::kCurrentVersion);
    EXPECT_FALSE(reader.IsError());
}

TEST(NetworkMessageTest, MaxSizePayload)
{
    constexpr u32 maxSize = 0xFFFFFFFF;

    NetworkMessageHeader original;
    original.Type = ENetworkMessageType::UserMessage;
    original.Size = maxSize;
    original.Flags = 0xFF;

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer << original.Type;
    writer << original.Size;
    writer << original.Flags;
    writer << original.Version;

    FMemoryReader reader(buffer);
    NetworkMessageHeader loaded;
    reader << loaded.Type;
    reader << loaded.Size;
    reader << loaded.Flags;
    reader << loaded.Version;

    EXPECT_EQ(loaded.Type, ENetworkMessageType::UserMessage);
    EXPECT_EQ(loaded.Size, maxSize);
    EXPECT_EQ(loaded.Flags, 0xFF);
    EXPECT_EQ(loaded.Version, NetworkMessageHeader::kCurrentVersion);
    EXPECT_FALSE(reader.IsError());
}
