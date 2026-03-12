#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"

namespace OloEngine::Tests
{
    // -------------------------------------------------------------------------
    // EntitySnapshotTest
    // -------------------------------------------------------------------------

    TEST(EntitySnapshotTest, EmptySceneProducesEmptySnapshot)
    {
        Scene scene;
        auto buffer = EntitySnapshot::Capture(scene);
        EXPECT_TRUE(buffer.empty());
    }

    TEST(EntitySnapshotTest, OnlyReplicatedEntities)
    {
        Scene scene;

        // Create a non-replicated entity
        Entity notReplicated = scene.CreateEntity("NonReplicated");
        notReplicated.AddComponent<NetworkIdentityComponent>().IsReplicated = false;
        notReplicated.AddComponent<TransformComponent>().Translation = { 1.0f, 2.0f, 3.0f };

        auto buffer = EntitySnapshot::Capture(scene);
        EXPECT_TRUE(buffer.empty()); // No replicated entities
    }

    TEST(EntitySnapshotTest, CaptureAndApplyRoundtrip)
    {
        Scene scene;

        // Create a replicated entity
        Entity entity = scene.CreateEntity("Replicated");
        auto& nic = entity.AddComponent<NetworkIdentityComponent>();
        nic.IsReplicated  = true;
        nic.OwnerClientID = 0;

        auto& tc = entity.GetComponent<TransformComponent>(); // CreateEntity adds this
        tc.Translation = { 10.0f, 20.0f, 30.0f };
        tc.Rotation    = { 0.1f, 0.2f, 0.3f };
        tc.Scale       = { 2.0f, 2.0f, 2.0f };

        // Capture
        auto buffer = EntitySnapshot::Capture(scene);
        EXPECT_FALSE(buffer.empty());

        // Modify the transform
        tc.Translation = { 0.0f, 0.0f, 0.0f };

        // Apply — should restore original values
        EntitySnapshot::Apply(scene, buffer);

        EXPECT_FLOAT_EQ(tc.Translation.x, 10.0f);
        EXPECT_FLOAT_EQ(tc.Translation.y, 20.0f);
        EXPECT_FLOAT_EQ(tc.Translation.z, 30.0f);
        EXPECT_FLOAT_EQ(tc.Rotation.x, 0.1f);
        EXPECT_FLOAT_EQ(tc.Rotation.y, 0.2f);
        EXPECT_FLOAT_EQ(tc.Rotation.z, 0.3f);
        EXPECT_FLOAT_EQ(tc.Scale.x, 2.0f);
        EXPECT_FLOAT_EQ(tc.Scale.y, 2.0f);
        EXPECT_FLOAT_EQ(tc.Scale.z, 2.0f);
    }

    TEST(EntitySnapshotTest, ApplyEmptyBufferIsHarmless)
    {
        Scene scene;
        scene.CreateEntity("Test");
        EXPECT_NO_THROW(EntitySnapshot::Apply(scene, {}));
    }

} // namespace OloEngine::Tests
