#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Prediction/LagCompensator.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Networking/Replication/SnapshotBuffer.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

using namespace OloEngine;

class LagCompensatorTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Scene = CreateScope<Scene>();
        m_Compensator = LagCompensator();
        m_Buffer = SnapshotBuffer();
    }

    Scope<Scene> m_Scene;
    LagCompensator m_Compensator;
    SnapshotBuffer m_Buffer;
};

TEST_F(LagCompensatorTest, RewindAndRestoreWorks)
{
    // Create an entity and capture snapshot at tick 1
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "Player");
    e.AddComponent<NetworkIdentityComponent>();
    auto& tc = e.GetComponent<TransformComponent>();
    tc.Translation = { 1.0f, 0.0f, 0.0f };

    m_Buffer.Push(1, EntitySnapshot::Capture(*m_Scene));

    // Move entity to new position at tick 5
    tc.Translation = { 5.0f, 0.0f, 0.0f };
    m_Buffer.Push(5, EntitySnapshot::Capture(*m_Scene));

    glm::vec3 rewindPos{ 0.0f };
    bool callbackCalled = false;

    bool result = m_Compensator.PerformLagCompensatedCheck(
        *m_Scene, m_Buffer, { .TargetTick = 1, .CurrentTick = 5, .TickRateHz = 20 },
        [&](Scene& scene)
        {
            callbackCalled = true;
            Entity rewound = scene.GetEntityByUUID(UUID(100));
            rewindPos = rewound.GetComponent<TransformComponent>().Translation;
        });

    EXPECT_TRUE(result);
    EXPECT_TRUE(callbackCalled);
    // During callback, entity should be at tick 1 position
    EXPECT_FLOAT_EQ(rewindPos.x, 1.0f);

    // After rewind, entity should be restored to current position
    EXPECT_FLOAT_EQ(tc.Translation.x, 5.0f);
}

TEST_F(LagCompensatorTest, RejectsFutureTick)
{
    bool result = m_Compensator.PerformLagCompensatedCheck(
        *m_Scene, m_Buffer, { .TargetTick = 10, .CurrentTick = 5, .TickRateHz = 20 },
        [](Scene&) {});

    EXPECT_FALSE(result);
}

TEST_F(LagCompensatorTest, RejectsExcessiveRewind)
{
    // At 20Hz, 200ms = 4 ticks max rewind. Try 10 ticks = 500ms
    Entity e = m_Scene->CreateEntityWithUUID(UUID(100), "Player");
    m_Buffer.Push(1, EntitySnapshot::Capture(*m_Scene));

    bool result = m_Compensator.PerformLagCompensatedCheck(
        *m_Scene, m_Buffer, { .TargetTick = 1, .CurrentTick = 11, .TickRateHz = 20 },
        [](Scene&) {});

    EXPECT_FALSE(result); // 10 ticks at 20Hz = 500ms > 200ms max
}

TEST_F(LagCompensatorTest, MaxRewindGetSet)
{
    EXPECT_FLOAT_EQ(m_Compensator.GetMaxRewindMs(), 200.0f);
    m_Compensator.SetMaxRewindMs(300.0f);
    EXPECT_FLOAT_EQ(m_Compensator.GetMaxRewindMs(), 300.0f);
}

TEST_F(LagCompensatorTest, RejectsWhenNoHistoryAvailable)
{
    // Empty buffer
    bool result = m_Compensator.PerformLagCompensatedCheck(
        *m_Scene, m_Buffer, { .TargetTick = 1, .CurrentTick = 3, .TickRateHz = 20 },
        [](Scene&) {});

    EXPECT_FALSE(result);
}
