#include "OloEnginePCH.h"

// =============================================================================
// Save2DBodyVelocityRoundTripTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Box2D × Rigidbody2DComponent runtime velocity × SaveGameSerializer.
//   Rigidbody2DComponent has explicit `LinearVelocity` / `AngularVelocity`
//   fields commented as "Persisted velocity — snapshot from runtime
//   before save, applied on body creation." The save path is supposed to
//   capture the live Box2D velocity into those fields BEFORE serialising,
//   and the load path is supposed to apply them when re-creating the
//   body. A regression in either direction looks like "saved-mid-air
//   loads as falling-from-rest" — visible to anyone who saves while
//   Mario is jumping.
//
// Scenario: a dynamic 2D body falling under gravity. Tick to give it a
// non-trivial velocity. Capture scene state. Restore into a fresh scene
// + fresh Box2D world. Assert the restored body's resumed simulation
// continues from the saved velocity (bodies-from-rest fall slowly the
// next frame; bodies-with-saved-velocity fall fast).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class Save2DBodyVelocityRoundTripTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Body = GetScene().CreateEntity("Falling2D");
        m_Body.GetComponent<TransformComponent>().Translation = { 0.0f, 10.0f, 0.0f };
        Rigidbody2DComponent body;
        body.Type = Rigidbody2DComponent::BodyType::Dynamic;
        body.FixedRotation = true;
        m_Body.AddComponent<Rigidbody2DComponent>(body);
        BoxCollider2DComponent col;
        col.Size = { 0.4f, 0.4f };
        m_Body.AddComponent<BoxCollider2DComponent>(col);

        EnablePhysics2D();
    }

    Entity m_Body;
};

TEST_F(Save2DBodyVelocityRoundTripTest, SavedRuntimeVelocityIsAppliedOnReload)
{
    // Phase 1: tick so the body builds non-trivial downward velocity.
    TickFor(/*seconds=*/0.5f);
    const f32 yMid = m_Body.GetComponent<TransformComponent>().Translation.y;
    const auto& rb = m_Body.GetComponent<Rigidbody2DComponent>();
    ASSERT_LT(yMid, 10.0f - 0.5f) << "body did not fall before save";

    // Phase 2: capture the scene. CaptureSceneState is responsible for
    // snapshotting the live Box2D velocity into rb.LinearVelocity prior
    // to writing the YAML.
    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    // Phase 3: restore into a fresh scene with its own Box2D world.
    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    // Find the restored body by tag.
    Entity restoredBody = restored->FindEntityByName("Falling2D");
    ASSERT_TRUE(restoredBody);

    // Inspect the persisted velocity. Box2D's free-fall under gravity of
    // 9.81 m/s^2 produces ~4.9 m/s downward velocity at t=0.5s; we tolerate
    // wide bounds because the velocity-iteration solver does sub-steps.
    const auto& restoredRb = restoredBody.GetComponent<Rigidbody2DComponent>();
    EXPECT_LT(restoredRb.LinearVelocity.y, -1.0f)
        << "saved Box2D velocity wasn't captured into Rigidbody2DComponent.LinearVelocity; "
        << "got " << restoredRb.LinearVelocity.y << " (expected substantially negative)";

    // Phase 4: tick the restored scene briefly. The body should keep
    // falling fast, NOT start from rest. Compare against a "from rest"
    // baseline: 0.05s of dt under gravity from v=0 yields ~0.5 m/s
    // velocity, hence ~0.0125m fall. Resuming with v ≈ -4.9 yields
    // ~-0.245m fall. We assert at least 0.1m fall in 0.05s — only the
    // resumed body can clear that bar.
    restored->OnPhysics2DStart();
    const f32 yResumeStart = restoredBody.GetComponent<TransformComponent>().Translation.y;
    {
        const Timestep ts{ 1.0f / 60.0f };
        for (u32 i = 0; i < 3; ++i)
            restored->OnUpdateRuntime(ts); // 0.05s
    }
    const f32 yAfterShortTick = restoredBody.GetComponent<TransformComponent>().Translation.y;
    const f32 fall = yResumeStart - yAfterShortTick;

    EXPECT_GT(fall, 0.1f)
        << "restored body fell only " << fall
        << "m in 0.05s — looks like it started from rest, not from saved velocity. "
           "SaveGameSerializer's velocity round-trip is incomplete.";

    restored->OnPhysics2DStop();
}
