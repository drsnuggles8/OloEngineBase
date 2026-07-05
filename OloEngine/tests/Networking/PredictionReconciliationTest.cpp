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

TEST(PredictionReconciliationTest, ReconcileSmoothsResimulatedStateTowardPreReconcileSnapshot)
{
    // Regression test for N1: TransformSmooth/Rigidbody3DSmooth must ease the
    // entity's *current* (resimulated) value toward the pre-reconcile server
    // snapshot by `rate` — i.e. glm::mix(current, pre, rate) — not the other
    // way around. With a low rate (default 0.1) the corrected value should
    // stay close to the resimulated prediction and only nudge slightly toward
    // the server snapshot; a swapped mix() would instead snap it mostly to
    // the server value, which SetSmoothingRate's "lower = smoother but slower
    // correction" docs contradict.
    ClientPrediction prediction;
    Scene scene;
    Entity e = CreateTestEntity(scene, 42);

    // Pre-reconcile (server-confirmed) state: origin.
    e.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 0.0f);

    prediction.SetSmoothingRate(0.1f);
    prediction.SetHardSnapThreshold(5.0f); // stay well under the hard-snap distance

    prediction.SetInputApplyCallback(
        [](Scene& s, u64 uuid, const u8* /*data*/, u32 /*size*/)
        {
            auto entityOpt = s.TryGetEntityWithUUID(UUID(uuid));
            ASSERT_TRUE(entityOpt.has_value());
            // Resimulated (predicted) state after applying the unconfirmed input.
            entityOpt->GetComponent<TransformComponent>().Translation = glm::vec3(1.0f, 0.0f, 0.0f);
        });

    prediction.RecordInput(1, 42, { 0x01 });
    prediction.RecordInput(2, 42, { 0x02 });

    // Confirm tick 1 — tick 2 is unconfirmed and gets re-simulated + smoothed.
    prediction.Reconcile(scene, 1);

    f32 const x = e.GetComponent<TransformComponent>().Translation.x;
    // Expected: mix(current=1.0, pre=0.0, rate=0.1) == 0.9
    EXPECT_NEAR(x, 0.9f, 1e-4f);
    // Must stay closer to the resimulated value (1.0) than to the server
    // snapshot (0.0) — a swapped mix() would put it at 0.1 instead.
    EXPECT_GT(x, 0.5f);
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
