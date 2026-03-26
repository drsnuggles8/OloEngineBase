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
    original.SetRotationEuler({ 0.1f, 0.2f, 0.3f });
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
    EXPECT_FLOAT_EQ(loaded.GetRotationEuler().x, 0.1f);
    EXPECT_FLOAT_EQ(loaded.GetRotationEuler().y, 0.2f);
    EXPECT_FLOAT_EQ(loaded.GetRotationEuler().z, 0.3f);
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

// ── Registration System Tests ────────────────────────────────────────

class ComponentRegistryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        OloEngine::ComponentReplicator::ClearRegistry();
    }
    void TearDown() override
    {
        OloEngine::ComponentReplicator::ClearRegistry();
    }
};

TEST_F(ComponentRegistryTest, RegisterDefaultsPopulatesThreeComponents)
{
    using namespace OloEngine;

    EXPECT_TRUE(ComponentReplicator::GetRegistry().empty());

    ComponentReplicator::RegisterDefaults();

    EXPECT_EQ(ComponentReplicator::GetRegistry().size(), 3u);
    EXPECT_TRUE(ComponentReplicator::IsRegistered("TransformComponent"));
    EXPECT_TRUE(ComponentReplicator::IsRegistered("Rigidbody2DComponent"));
    EXPECT_TRUE(ComponentReplicator::IsRegistered("Rigidbody3DComponent"));
}

TEST_F(ComponentRegistryTest, IsRegisteredReturnsFalseForUnknown)
{
    using namespace OloEngine;
    EXPECT_FALSE(ComponentReplicator::IsRegistered("FakeComponent"));
}

TEST_F(ComponentRegistryTest, RegisterCustomComponent)
{
    using namespace OloEngine;

    bool called = false;
    ComponentReplicator::Register("CustomComponent",
                                  [&](FArchive& /*ar*/, void* /*comp*/)
                                  { called = true; });

    EXPECT_TRUE(ComponentReplicator::IsRegistered("CustomComponent"));

    auto const* fn = ComponentReplicator::GetSerializer("CustomComponent");
    ASSERT_NE(fn, nullptr);

    // Invoke the serializer to verify it works
    std::vector<u8> buf;
    FMemoryWriter writer(buf);
    (*fn)(writer, nullptr);
    EXPECT_TRUE(called);
}

TEST_F(ComponentRegistryTest, GetSerializerReturnsNullForUnknown)
{
    using namespace OloEngine;
    EXPECT_EQ(ComponentReplicator::GetSerializer("Nope"), nullptr);
}

TEST_F(ComponentRegistryTest, ClearRegistryRemovesAll)
{
    using namespace OloEngine;

    ComponentReplicator::RegisterDefaults();
    EXPECT_EQ(ComponentReplicator::GetRegistry().size(), 3u);

    ComponentReplicator::ClearRegistry();
    EXPECT_TRUE(ComponentReplicator::GetRegistry().empty());
}

TEST_F(ComponentRegistryTest, RegisteredTransformSerializerWorks)
{
    using namespace OloEngine;

    ComponentReplicator::RegisterDefaults();

    auto const* fn = ComponentReplicator::GetSerializer("TransformComponent");
    ASSERT_NE(fn, nullptr);

    TransformComponent original;
    original.Translation = { 7.0f, 8.0f, 9.0f };
    original.SetRotationEuler({ 0.5f, 0.6f, 0.7f });
    original.Scale = { 2.0f, 3.0f, 4.0f };

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer.ArIsNetArchive = true;
    (*fn)(writer, &original);

    TransformComponent loaded;
    FMemoryReader reader(buffer);
    reader.ArIsNetArchive = true;
    (*fn)(reader, &loaded);

    EXPECT_FLOAT_EQ(loaded.Translation.x, 7.0f);
    EXPECT_FLOAT_EQ(loaded.Translation.y, 8.0f);
    EXPECT_FLOAT_EQ(loaded.Translation.z, 9.0f);
    EXPECT_FLOAT_EQ(loaded.Scale.x, 2.0f);
    EXPECT_FLOAT_EQ(loaded.Scale.y, 3.0f);
    EXPECT_FLOAT_EQ(loaded.Scale.z, 4.0f);
}
