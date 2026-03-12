#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Networking/Replication/ComponentReplicator.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

#include <glm/glm.hpp>

namespace OloEngine::Tests
{
    // -------------------------------------------------------------------------
    // ComponentReplicatorTest
    // -------------------------------------------------------------------------

    TEST(ComponentReplicatorTest, TransformRoundtrip)
    {
        TransformComponent src;
        src.Translation = { 1.0f, 2.0f, 3.0f };
        src.Rotation    = { 0.1f, 0.2f, 0.3f };
        src.Scale       = { 2.0f, 2.0f, 2.0f };

        // --- Serialize ---
        std::vector<u8> buffer;
        {
            FMemoryWriter writer(buffer);
            ComponentReplicator::Serialize(writer, src);
        }

        EXPECT_FALSE(buffer.empty());

        // --- Deserialize ---
        TransformComponent dst;
        {
            FMemoryReader reader(buffer);
            ComponentReplicator::Serialize(reader, dst);
        }

        EXPECT_FLOAT_EQ(dst.Translation.x, src.Translation.x);
        EXPECT_FLOAT_EQ(dst.Translation.y, src.Translation.y);
        EXPECT_FLOAT_EQ(dst.Translation.z, src.Translation.z);
        EXPECT_FLOAT_EQ(dst.Rotation.x, src.Rotation.x);
        EXPECT_FLOAT_EQ(dst.Rotation.y, src.Rotation.y);
        EXPECT_FLOAT_EQ(dst.Rotation.z, src.Rotation.z);
        EXPECT_FLOAT_EQ(dst.Scale.x, src.Scale.x);
        EXPECT_FLOAT_EQ(dst.Scale.y, src.Scale.y);
        EXPECT_FLOAT_EQ(dst.Scale.z, src.Scale.z);
    }

    TEST(ComponentReplicatorTest, ArIsNetArchiveFlagIsSet)
    {
        TransformComponent tc;
        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        ComponentReplicator::Serialize(writer, tc);
        EXPECT_TRUE(writer.ArIsNetArchive);
    }

    TEST(ComponentReplicatorTest, Rigidbody2DRoundtrip)
    {
        Rigidbody2DComponent src;
        src.Type          = Rigidbody2DComponent::BodyType::Dynamic;
        src.FixedRotation = true;

        std::vector<u8> buffer;
        {
            FMemoryWriter writer(buffer);
            ComponentReplicator::Serialize(writer, src);
        }

        Rigidbody2DComponent dst;
        {
            FMemoryReader reader(buffer);
            ComponentReplicator::Serialize(reader, dst);
        }

        EXPECT_EQ(dst.Type, src.Type);
        EXPECT_EQ(dst.FixedRotation, src.FixedRotation);
    }

    TEST(ComponentReplicatorTest, Rigidbody3DRoundtrip)
    {
        Rigidbody3DComponent src;
        src.m_Type          = Rigidbody3DComponent::BodyType3D::Dynamic;
        src.m_Mass          = 5.0f;
        src.m_LinearDrag    = 0.1f;
        src.m_AngularDrag   = 0.05f;
        src.m_DisableGravity = false;

        std::vector<u8> buffer;
        {
            FMemoryWriter writer(buffer);
            ComponentReplicator::Serialize(writer, src);
        }

        Rigidbody3DComponent dst;
        {
            FMemoryReader reader(buffer);
            ComponentReplicator::Serialize(reader, dst);
        }

        EXPECT_EQ(dst.m_Type, src.m_Type);
        EXPECT_FLOAT_EQ(dst.m_Mass, src.m_Mass);
        EXPECT_FLOAT_EQ(dst.m_LinearDrag, src.m_LinearDrag);
        EXPECT_FLOAT_EQ(dst.m_AngularDrag, src.m_AngularDrag);
        EXPECT_EQ(dst.m_DisableGravity, src.m_DisableGravity);
    }

} // namespace OloEngine::Tests
