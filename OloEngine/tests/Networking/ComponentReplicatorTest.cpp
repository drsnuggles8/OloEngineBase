#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Replication/ComponentReplicator.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

#include <array>
#include <cmath>
#include <limits>

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
    EXPECT_NEAR(loaded.GetRotationEuler().x, 0.5f, 1e-4f);
    EXPECT_NEAR(loaded.GetRotationEuler().y, 0.6f, 1e-4f);
    EXPECT_NEAR(loaded.GetRotationEuler().z, 0.7f, 1e-4f);
    EXPECT_FLOAT_EQ(loaded.Scale.x, 2.0f);
    EXPECT_FLOAT_EQ(loaded.Scale.y, 3.0f);
    EXPECT_FLOAT_EQ(loaded.Scale.z, 4.0f);
}

// ── Untrusted wire-float hardening ───────────────────────────────────
// Snapshot bytes come from the network and are untrusted. A NaN/±inf float must
// be replaced with a safe fallback on deserialize, never reach the scene.

// Serialize a raw transform payload (9 floats, in wire order: translation, euler,
// scale) so we can inject arbitrary bit patterns the way a malicious peer would.
// (u8 / f32 are global typedefs from Core/Base.h, not members of namespace OloEngine.)
static std::vector<u8> MakeRawTransformBytes(std::array<f32, 9> values)
{
    using namespace OloEngine;

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer.ArIsNetArchive = true;
    for (f32& v : values)
    {
        writer << v;
    }
    return buffer;
}

TEST(ComponentReplicatorTest, TransformRejectsNonFiniteWireFloats)
{
    using namespace OloEngine;

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();

    // Every field poisoned: translation, rotation euler, and scale.
    auto buffer = MakeRawTransformBytes({ nan, inf, -inf, nan, inf, -inf, nan, inf, -inf });

    TransformComponent loaded;
    FMemoryReader reader(buffer);
    reader.ArIsNetArchive = true;
    ComponentReplicator::Serialize(reader, loaded);

    EXPECT_FALSE(reader.IsError());

    // Nothing non-finite survives into the component.
    EXPECT_TRUE(std::isfinite(loaded.Translation.x));
    EXPECT_TRUE(std::isfinite(loaded.Translation.y));
    EXPECT_TRUE(std::isfinite(loaded.Translation.z));
    EXPECT_TRUE(std::isfinite(loaded.Scale.x));
    EXPECT_TRUE(std::isfinite(loaded.Scale.y));
    EXPECT_TRUE(std::isfinite(loaded.Scale.z));
    EXPECT_TRUE(std::isfinite(loaded.GetRotationEuler().x));
    EXPECT_TRUE(std::isfinite(loaded.GetRotationEuler().y));
    EXPECT_TRUE(std::isfinite(loaded.GetRotationEuler().z));

    // Fallbacks: translation/rotation → 0, scale → 1.
    EXPECT_FLOAT_EQ(loaded.Translation.x, 0.0f);
    EXPECT_FLOAT_EQ(loaded.Translation.y, 0.0f);
    EXPECT_FLOAT_EQ(loaded.Translation.z, 0.0f);
    EXPECT_FLOAT_EQ(loaded.Scale.x, 1.0f);
    EXPECT_FLOAT_EQ(loaded.Scale.y, 1.0f);
    EXPECT_FLOAT_EQ(loaded.Scale.z, 1.0f);
}

TEST(ComponentReplicatorTest, TransformKeepsFiniteFieldsWhenSanitizing)
{
    using namespace OloEngine;

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();

    // Only Translation.y is poisoned; the rest must round-trip untouched.
    auto buffer = MakeRawTransformBytes({ 1.0f, nan, 3.0f, 0.0f, 0.0f, 0.0f, 2.0f, 4.0f, 8.0f });

    TransformComponent loaded;
    FMemoryReader reader(buffer);
    reader.ArIsNetArchive = true;
    ComponentReplicator::Serialize(reader, loaded);

    EXPECT_FLOAT_EQ(loaded.Translation.x, 1.0f);
    EXPECT_FLOAT_EQ(loaded.Translation.y, 0.0f); // sanitized
    EXPECT_FLOAT_EQ(loaded.Translation.z, 3.0f);
    EXPECT_FLOAT_EQ(loaded.Scale.x, 2.0f);
    EXPECT_FLOAT_EQ(loaded.Scale.y, 4.0f);
    EXPECT_FLOAT_EQ(loaded.Scale.z, 8.0f);
}

TEST(ComponentReplicatorTest, Rigidbody3DRejectsNonFiniteWireFloats)
{
    using namespace OloEngine;

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();

    // Wire layout: i32 bodyType, f32 mass, vec3 linear velocity, vec3 angular velocity.
    std::vector<u8> buffer;
    {
        FMemoryWriter writer(buffer);
        writer.ArIsNetArchive = true;
        i32 bodyType = 0;
        f32 mass = nan;
        std::array<f32, 6> vel = { inf, -inf, nan, inf, -inf, nan };
        writer << bodyType;
        writer << mass;
        for (f32& v : vel)
        {
            writer << v;
        }
    }

    Rigidbody3DComponent loaded;
    FMemoryReader reader(buffer);
    reader.ArIsNetArchive = true;
    ComponentReplicator::Serialize(reader, loaded);

    EXPECT_FALSE(reader.IsError());

    EXPECT_TRUE(std::isfinite(loaded.m_Mass));
    EXPECT_GE(loaded.m_Mass, 0.0f);
    EXPECT_TRUE(std::isfinite(loaded.m_InitialLinearVelocity.x));
    EXPECT_TRUE(std::isfinite(loaded.m_InitialLinearVelocity.y));
    EXPECT_TRUE(std::isfinite(loaded.m_InitialLinearVelocity.z));
    EXPECT_TRUE(std::isfinite(loaded.m_InitialAngularVelocity.x));
    EXPECT_TRUE(std::isfinite(loaded.m_InitialAngularVelocity.y));
    EXPECT_TRUE(std::isfinite(loaded.m_InitialAngularVelocity.z));
}

TEST(ComponentReplicatorTest, Rigidbody3DRejectsNegativeMass)
{
    using namespace OloEngine;

    std::vector<u8> buffer;
    {
        FMemoryWriter writer(buffer);
        writer.ArIsNetArchive = true;
        i32 bodyType = 0;
        f32 mass = -5.0f;
        std::array<f32, 6> vel = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        writer << bodyType;
        writer << mass;
        for (f32& v : vel)
        {
            writer << v;
        }
    }

    Rigidbody3DComponent loaded;
    FMemoryReader reader(buffer);
    reader.ArIsNetArchive = true;
    ComponentReplicator::Serialize(reader, loaded);

    EXPECT_GE(loaded.m_Mass, 0.0f);
}
