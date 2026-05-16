#include "OloEnginePCH.h"

// =============================================================================
// DeterministicReplayProducesSameStateTest — Functional Test.
//
// Cross-subsystem seam under test:
//   The Functional determinism contract (ADR 0001 + CONTEXT.md): given identical
//   starting conditions and a fixed Timestep, ticking a scene N frames
//   should produce the same end state every time. This is the foundation
//   for replay/rollback features (network prediction, gameplay record-and-
//   playback, deterministic regression diffing). A regression that lets a
//   subsystem read wall-clock time, a non-seeded RNG, or thread-scheduling-
//   dependent state quietly breaks every downstream feature that relies on
//   reproducibility.
//
// Scenario: build a fresh scene, tick 60 frames, snapshot the body's
// transform. Tear the scene down, build the same scene again from
// identical initial conditions, tick the same 60 frames. Snapshots must
// match within tight tolerance.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Spin up a Scene with one falling body, tick `frames`, return the body's
    // final translation. Centralised so both halves of the test exercise the
    // *exact* same code path.
    glm::vec3 RunOnceAndCapture(u32 frames)
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

        const Timestep ts{ 1.0f / 60.0f };
        for (u32 i = 0; i < frames; ++i)
        {
            scene->OnUpdateRuntime(ts);
        }

        const glm::vec3 result = ball.GetComponent<TransformComponent>().Translation;
        scene->OnPhysics3DStop();
        return result;
    }
} // namespace

class DeterministicReplayProducesSameStateTest : public FunctionalTest
{
  protected:
    // The harness's Scene is unused — we build two scenes manually inside
    // the test body to guarantee a clean reset between runs.
    void BuildScene() override
    {
        // Scheduler must be running for JoltJobSystemAdapter on either run.
        EnablePhysics3D();
        // Tear down the harness scene's physics so the per-run scenes own
        // their own JoltScenes from a clean slate.
        GetScene().OnPhysics3DStop();
    }
};

TEST_F(DeterministicReplayProducesSameStateTest, TwoIdenticalRunsProduceMatchingTransforms)
{
    constexpr u32 kFrames = 60; // 1s at 60Hz — past the body's bounce-and-settle.

    const glm::vec3 runA = RunOnceAndCapture(kFrames);
    const glm::vec3 runB = RunOnceAndCapture(kFrames);

    ASSERT_TRUE(std::isfinite(runA.x) && std::isfinite(runA.y) && std::isfinite(runA.z));
    ASSERT_TRUE(std::isfinite(runB.x) && std::isfinite(runB.y) && std::isfinite(runB.z));

    // Liveness: both runs must have actually advanced from the initial state
    // (ball at y=5). Without this guard, a regression where the simulation
    // silently no-ops would still satisfy runA == runB and produce a false
    // pass. The ball drops ~5m under gravity in 1s, so a displacement >0.5m
    // proves real motion happened.
    constexpr f32 kInitialY = 5.0f;
    constexpr f32 kMinAdvance = 0.5f;
    ASSERT_GT(std::fabs(kInitialY - runA.y), kMinAdvance)
        << "runA did not advance from initial state — simulation never ran";
    ASSERT_GT(std::fabs(kInitialY - runB.y), kMinAdvance)
        << "runB did not advance from initial state — simulation never ran";

    // Tight tolerance — Jolt is deterministic given the same inputs and step
    // count. Anything wider than ~1e-4 here means a subsystem is reading
    // non-deterministic state (wall clock, unseeded RNG, thread schedule).
    constexpr f32 kTol = 1e-4f;
    EXPECT_NEAR(runA.x, runB.x, kTol)
        << "deterministic replay diverged on x; runA=" << runA.x << " runB=" << runB.x;
    EXPECT_NEAR(runA.y, runB.y, kTol)
        << "deterministic replay diverged on y; runA=" << runA.y << " runB=" << runB.y;
    EXPECT_NEAR(runA.z, runB.z, kTol)
        << "deterministic replay diverged on z; runA=" << runA.z << " runB=" << runB.z;
}
