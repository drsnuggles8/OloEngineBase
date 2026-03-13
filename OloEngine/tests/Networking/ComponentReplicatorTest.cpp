#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Replication/ComponentReplicator.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

TEST(ComponentReplicatorTest, TransformRoundtrip)
{
    using namespace OloEngine;

    TransformComponent original;
    original.Translation = { 1.0f, 2.0f, 3.0f };
    original.Rotation = { 0.1f, 0.2f, 0.3f };
    original.Scale = { 4.0f, 5.0f, 6.0f };

    // Serialize
    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer.ArIsNetArchive = true;
    ComponentReplicator::Serialize(writer, original);

    // Deserialize
    TransformComponent loaded;
    FMemoryReader reader(buffer);
    reader.ArIsNetArchive = true;
    ComponentReplicator::Serialize(reader, loaded);

    EXPECT_FLOAT_EQ(loaded.Translation.x, 1.0f);
    EXPECT_FLOAT_EQ(loaded.Translation.y, 2.0f);
    EXPECT_FLOAT_EQ(loaded.Translation.z, 3.0f);
    EXPECT_FLOAT_EQ(loaded.Rotation.x, 0.1f);
    EXPECT_FLOAT_EQ(loaded.Rotation.y, 0.2f);
    EXPECT_FLOAT_EQ(loaded.Rotation.z, 0.3f);
    EXPECT_FLOAT_EQ(loaded.Scale.x, 4.0f);
    EXPECT_FLOAT_EQ(loaded.Scale.y, 5.0f);
    EXPECT_FLOAT_EQ(loaded.Scale.z, 6.0f);
    EXPECT_FALSE(reader.IsError());
}

TEST(ComponentReplicatorTest, ArIsNetArchiveFlag)
{
    using namespace OloEngine;

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer.ArIsNetArchive = true;

    EXPECT_TRUE(writer.ArIsNetArchive);

    FMemoryReader reader(buffer);
    reader.ArIsNetArchive = true;

    EXPECT_TRUE(reader.ArIsNetArchive);
}
