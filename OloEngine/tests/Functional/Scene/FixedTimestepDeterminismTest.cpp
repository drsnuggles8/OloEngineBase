#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional

// =============================================================================
// FixedTimestepDeterminismTest — Functional Test.
//
// Cross-subsystem seam under test:
//   The deterministic fixed-timestep simulation loop + seeded RNG (issue #452).
//   Scene::OnUpdateRuntimeFixed(frameTs, fixedDt) accumulates the variable frame
//   delta and advances the gameplay simulation in fixed `fixedDt` steps (N
//   catch-up steps, clamped), then renders once. This is the foundation the
//   rollback/replay netcode keys off: the same fixed-step sequence must come out
//   regardless of how the wall clock was chopped into frames, and the gameplay
//   RNG (RandomUtils global stream) must replay identically from a seed.
//
//   A regression — feeding the sim the raw variable delta, a drifting
//   accumulator, an un-reset tick counter, or a time-seeded RNG — silently
//   breaks every downstream feature that relies on reproducibility.
//
// Scenarios:
//   1. Frame-rate independence: one falling-ball scene driven at three different
//      pacings (1× / 2× / ½× the fixed step per frame) over the same simulated
//      time must take the same number of fixed steps and end in the same state.
//   2. Accumulator catch-up + remainder carry, the spiral-of-death clamp, and
//      paused / single-step gating all advance the tick counter exactly.
//   3. RandomUtils::SetGlobalSeed makes the gameplay RNG stream replay
//      identically from a seed, and a different seed yields a different stream.
//
// `fixedDt` is chosen as 1/64 s (exactly representable in f32) so the
// accumulator arithmetic is exact and the three pacings take provably-equal
// step counts — the production default is 1/60 (Application::GetFixedTimeStep).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <cmath>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Exactly representable in f32, so fixedDt and its power-of-two multiples
    // accumulate without rounding error — the cross-pacing step counts are then
    // provably equal rather than "equal within float slop".
    constexpr f32 kFixedDt = 1.0f / 64.0f;

    struct RunResult
    {
        glm::vec3 Translation{ 0.0f };
        u64 Tick = 0;
    };

    // Drive a fresh falling-ball scene through the fixed-step entry, feeding
    // `frameTs` per call for `calls` calls. Returns the ball's final position
    // and the number of fixed steps the accumulator actually executed.
    RunResult RunBallScene(f32 frameTs, u32 calls)
    {
        Ref<Scene> scene = Scene::Create();
        scene->SetRenderingEnabled(false);

        auto floor = scene->CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        Rigidbody3DComponent floorBody;
        floorBody.m_Type = BodyType3D::Static;
        BoxCollider3DComponent floorCol;
        floorCol.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        floor.AddComponent<BoxCollider3DComponent>(floorCol);
        floor.AddComponent<Rigidbody3DComponent>(floorBody);

        auto ball = scene->CreateEntity("Ball");
        ball.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
        SphereCollider3DComponent ballCol;
        ballCol.m_Radius = 0.5f;
        ball.AddComponent<SphereCollider3DComponent>(ballCol);
        Rigidbody3DComponent ballBody;
        ballBody.m_Type = BodyType3D::Dynamic;
        ballBody.m_Mass = 1.0f;
        ball.AddComponent<Rigidbody3DComponent>(ballBody);

        scene->OnPhysics3DStart();

        for (u32 i = 0; i < calls; ++i)
        {
            scene->OnUpdateRuntimeFixed(frameTs, kFixedDt);
        }

        RunResult result;
        result.Translation = ball.GetComponent<TransformComponent>().Translation;
        result.Tick = scene->GetSimulationTick();
        scene->OnPhysics3DStop();
        return result;
    }
} // namespace

class FixedTimestepDeterminismTest : public FunctionalTest
{
  protected:
    // The harness scene is unused for the physics runs — they build their own
    // scenes for a clean tick counter — but EnablePhysics3D spins up the task
    // scheduler that JoltScene's job system needs. Mirror DeterministicReplay.
    void BuildScene() override
    {
        EnablePhysics3D();
        GetScene().OnPhysics3DStop();
    }
};

TEST_F(FixedTimestepDeterminismTest, GameplayIsFrameRateIndependent)
{
    // Same 2.0 s of simulated time, three different frame pacings:
    //   A: one fixed step per frame      (128 frames @ 1×  fixedDt)
    //   B: two fixed steps per frame     ( 64 frames @ 2×  fixedDt)
    //   C: one step every other frame    (256 frames @ ½×  fixedDt)
    // Each feeds JoltScene the identical sequence of 128 Simulate(fixedDt)
    // calls, so the end state must match regardless of frame rate.
    const RunResult a = RunBallScene(kFixedDt, 128);
    const RunResult b = RunBallScene(2.0f * kFixedDt, 64);
    const RunResult c = RunBallScene(0.5f * kFixedDt, 256);

    // Exactly-equal step counts are the core frame-rate-independence claim.
    EXPECT_EQ(a.Tick, 128u);
    EXPECT_EQ(b.Tick, 128u) << "2x-paced run took a different number of fixed steps";
    EXPECT_EQ(c.Tick, 128u) << "half-paced run took a different number of fixed steps";

    // Liveness: the ball actually fell (started at y=5, lands near y=0.5).
    constexpr f32 kInitialY = 5.0f;
    ASSERT_GT(std::fabs(kInitialY - a.Translation.y), 0.5f)
        << "simulation never advanced — ball did not move";

    // Same fixed-step sequence → same physics result (Jolt is deterministic).
    constexpr f32 kTol = 1e-4f;
    EXPECT_NEAR(a.Translation.x, b.Translation.x, kTol);
    EXPECT_NEAR(a.Translation.y, b.Translation.y, kTol);
    EXPECT_NEAR(a.Translation.z, b.Translation.z, kTol);
    EXPECT_NEAR(a.Translation.x, c.Translation.x, kTol);
    EXPECT_NEAR(a.Translation.y, c.Translation.y, kTol);
    EXPECT_NEAR(a.Translation.z, c.Translation.z, kTol);
}

TEST_F(FixedTimestepDeterminismTest, AccumulatorCatchesUpAndCarriesRemainder)
{
    Ref<Scene> scene = Scene::Create();
    scene->SetRenderingEnabled(false);

    // 1.5 fixed steps of wall time per frame (exactly representable).
    const f32 frameTs = 1.5f * kFixedDt;

    scene->OnUpdateRuntimeFixed(frameTs, kFixedDt); // accum 1.5 → 1 step, 0.5 left
    EXPECT_EQ(scene->GetSimulationTick(), 1u);

    scene->OnUpdateRuntimeFixed(frameTs, kFixedDt); // accum 0.5+1.5=2.0 → 2 steps
    EXPECT_EQ(scene->GetSimulationTick(), 3u) << "remainder did not carry into the next frame";

    scene->OnUpdateRuntimeFixed(frameTs, kFixedDt); // accum 1.5 → 1 step
    EXPECT_EQ(scene->GetSimulationTick(), 4u);
}

TEST_F(FixedTimestepDeterminismTest, AccumulatorClampsRunawayFrameToTheStepCap)
{
    Ref<Scene> scene = Scene::Create();
    scene->SetRenderingEnabled(false);

    // A 100-second hitch must not try to run 6400 catch-up steps — the
    // spiral-of-death clamp caps it at s_MaxFixedStepsPerFrame (15) and drops
    // the excess wall-time so the host slows rather than freezes.
    scene->OnUpdateRuntimeFixed(100.0f, kFixedDt);
    EXPECT_EQ(scene->GetSimulationTick(), 15u)
        << "spiral-of-death clamp did not bound the catch-up step count";
}

TEST_F(FixedTimestepDeterminismTest, PausedFixedStepHonoursSingleStepBudget)
{
    Ref<Scene> scene = Scene::Create();
    scene->SetRenderingEnabled(false);

    const f32 frameTs = 4.0f * kFixedDt; // would run 4 steps if not paused
    scene->SetPaused(true);

    scene->OnUpdateRuntimeFixed(frameTs, kFixedDt); // paused, nothing queued
    EXPECT_EQ(scene->GetSimulationTick(), 0u) << "paused scene advanced the simulation";

    scene->Step(2); // queue exactly two single-steps
    scene->OnUpdateRuntimeFixed(frameTs, kFixedDt);
    EXPECT_EQ(scene->GetSimulationTick(), 1u) << "a queued step should advance exactly one tick";
    scene->OnUpdateRuntimeFixed(frameTs, kFixedDt);
    EXPECT_EQ(scene->GetSimulationTick(), 2u);

    scene->OnUpdateRuntimeFixed(frameTs, kFixedDt); // budget consumed, re-frozen
    EXPECT_EQ(scene->GetSimulationTick(), 2u) << "scene advanced after the step budget was spent";
}

TEST_F(FixedTimestepDeterminismTest, SeededRngReplaysIdenticallyFromASeed)
{
    constexpr u64 kSeed = 0xC0FFEEULL;
    constexpr int kDraws = 32;

    auto drawSequence = [](u64 seed)
    {
        RandomUtils::SetGlobalSeed(seed);
        std::vector<u32> values;
        values.reserve(kDraws);
        for (int i = 0; i < kDraws; ++i)
        {
            values.push_back(RandomUtils::UInt32(0u, 1'000'000u));
        }
        return values;
    };

    const std::vector<u32> first = drawSequence(kSeed);
    const std::vector<u32> second = drawSequence(kSeed);
    EXPECT_EQ(first, second) << "same seed produced a different RNG stream — not deterministic";

    const std::vector<u32> other = drawSequence(0xBADF00DULL);
    EXPECT_NE(first, other) << "a different seed produced the same stream — seeding has no effect";
}
