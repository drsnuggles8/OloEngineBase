#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Prediction/ClientPrediction.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

using namespace OloEngine;

// Helper: create a minimal scene with one entity that has IDComponent + TransformComponent
static Entity CreateTestEntity(Scene& scene, u64 uuid)
{
    Entity e = scene.CreateEntityWithUUID(UUID(uuid), "TestEntity");
    return e;
}

TEST(PredictionReconciliationTest, RecordInputStoresInBuffer)
{
    ClientPrediction prediction;

    std::vector<u8> data = { 0x01, 0x02 };
    prediction.RecordInput(1, 42, data);
    prediction.RecordInput(2, 42, { 0x03 });

    auto const& buffer = prediction.GetInputBuffer();
    EXPECT_NE(buffer.GetByTick(1), nullptr);
    EXPECT_NE(buffer.GetByTick(2), nullptr);
    EXPECT_EQ(prediction.GetCurrentTick(), 2u);
}

TEST(PredictionReconciliationTest, ReconcileDiscardsConfirmedInputs)
{
    ClientPrediction prediction;
    Scene scene;

    prediction.RecordInput(1, 42, { 0x01 });
    prediction.RecordInput(2, 42, { 0x02 });
    prediction.RecordInput(3, 42, { 0x03 });

    // Server confirms up to tick 2
    prediction.Reconcile(scene, 2);

    EXPECT_EQ(prediction.GetLastConfirmedTick(), 2u);

    auto const& buffer = prediction.GetInputBuffer();
    EXPECT_EQ(buffer.GetByTick(1), nullptr);
    EXPECT_EQ(buffer.GetByTick(2), nullptr);
    // Tick 3 should still be present (unconfirmed)
    EXPECT_NE(buffer.GetByTick(3), nullptr);
}

TEST(PredictionReconciliationTest, ReconcileResimuatesUnconfirmedInputs)
{
    ClientPrediction prediction;
    Scene scene;

    u32 applyCount = 0;
    prediction.SetInputApplyCallback(
        [&applyCount](Scene& /*s*/, u64 /*uuid*/, const u8* /*data*/, u32 /*size*/)
        { ++applyCount; });

    prediction.RecordInput(1, 42, { 0x01 });
    prediction.RecordInput(2, 42, { 0x02 });
    prediction.RecordInput(3, 42, { 0x03 });

    // Server confirms tick 1 — ticks 2 and 3 must be re-simulated
    prediction.Reconcile(scene, 1);

    EXPECT_EQ(applyCount, 2u);
}

TEST(PredictionReconciliationTest, ReconcileIgnoresAlreadyConfirmedTick)
{
    ClientPrediction prediction;
    Scene scene;

    u32 applyCount = 0;
    prediction.SetInputApplyCallback(
        [&applyCount](Scene& /*s*/, u64 /*uuid*/, const u8* /*data*/, u32 /*size*/)
        { ++applyCount; });

    prediction.RecordInput(1, 42, { 0x01 });
    prediction.Reconcile(scene, 1);
    applyCount = 0;

    // Reconciling with the same (or older) tick should be a no-op
    prediction.Reconcile(scene, 1);
    EXPECT_EQ(applyCount, 0u);
}

TEST(PredictionReconciliationTest, SmoothingRateGetSet)
{
    ClientPrediction prediction;

    EXPECT_FLOAT_EQ(prediction.GetSmoothingRate(), 0.1f);
    prediction.SetSmoothingRate(0.5f);
    EXPECT_FLOAT_EQ(prediction.GetSmoothingRate(), 0.5f);
}

TEST(PredictionReconciliationTest, ReconcilePassesEntityUUIDToCallback)
{
    ClientPrediction prediction;
    Scene scene;

    std::vector<u64> receivedUUIDs;
    prediction.SetInputApplyCallback(
        [&receivedUUIDs](Scene& /*s*/, u64 uuid, const u8* /*data*/, u32 /*size*/)
        { receivedUUIDs.push_back(uuid); });

    prediction.RecordInput(1, 100, { 0x01 });
    prediction.RecordInput(2, 200, { 0x02 });
    prediction.RecordInput(3, 100, { 0x03 });

    // Confirm tick 1 — ticks 2,3 re-simulated
    prediction.Reconcile(scene, 1);

    ASSERT_EQ(receivedUUIDs.size(), 2u);
    EXPECT_EQ(receivedUUIDs[0], 200u);
    EXPECT_EQ(receivedUUIDs[1], 100u);
}
