#include "OloEnginePCH.h"

// =============================================================================
// PhysicsRestartIsCleanTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene × Physics3D lifecycle. The editor stops and restarts the runtime
//   constantly — every Play / Stop cycle calls OnPhysics3DStop +
//   OnPhysics3DStart on the same Scene. Bugs here look like "the second
//   Play session has bodies that don't move" or "Jolt asserts on shutdown
//   because some body was left dangling," and they're invisible to the
//   per-method physics tests because those test one cycle in isolation.
//
// Scenario: build a falling body, run physics for 0.5s, stop physics
// (OnPhysics3DStop), restart physics (OnPhysics3DStart), tick again, and
// assert the body still simulates correctly in the second cycle. Two-cycle
// is the minimum to expose the "second-time" bug class.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class PhysicsRestartIsCleanTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        auto& fb = floor.AddComponent<Rigidbody3DComponent>();
        fb.m_Type = BodyType3D::Static;
        auto& fc = floor.AddComponent<BoxCollider3DComponent>();
        fc.m_HalfExtents = { 50.0f, 0.5f, 50.0f };

        m_Body = GetScene().CreateEntity("Body");
        m_Body.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
        auto& body = m_Body.AddComponent<Rigidbody3DComponent>();
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        auto& col = m_Body.AddComponent<SphereCollider3DComponent>();
        col.m_Radius = 0.5f;

        EnablePhysics3D();
    }

    [[nodiscard]] f32 BodyY() const
    {
        return m_Body.GetComponent<TransformComponent>().Translation.y;
    }

    Entity m_Body;
};

TEST_F(PhysicsRestartIsCleanTest, SecondPhysicsCycleSimulatesIdenticallyToFirst)
{
    // ---------- Cycle 1 ----------
    const f32 startY = BodyY();
    ASSERT_NEAR(startY, 5.0f, 1e-4f) << "body did not start at y=5";

    TickFor(/*seconds=*/0.5f);
    const f32 cycle1EndY = BodyY();
    ASSERT_LT(cycle1EndY, startY - 0.5f)
        << "body did not fall during cycle 1 — physics was never bound";
    ASSERT_TRUE(std::isfinite(cycle1EndY));

    // Stop physics. After OnPhysics3DStop, JoltScene's body table is empty
    // and Initialize→Shutdown→Initialize must be idempotent.
    GetScene().OnPhysics3DStop();
    // Reset the body's transform to exactly the same starting state as
    // cycle 1 so we can compare end-states like-for-like.
    m_Body.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
    // Clear the runtime body token so the next OnPhysics3DStart re-binds
    // (it would re-bind anyway because OnPhysics3DStop nulls it, but make
    // it explicit so the contract is visible at the test site).
    m_Body.GetComponent<Rigidbody3DComponent>().m_RuntimeBodyToken = 0;

    // ---------- Cycle 2 ----------
    GetScene().OnPhysics3DStart();

    ASSERT_NEAR(BodyY(), 5.0f, 1e-4f) << "body did not start cycle 2 at y=5";

    TickFor(/*seconds=*/0.5f);
    const f32 cycle2EndY = BodyY();
    ASSERT_TRUE(std::isfinite(cycle2EndY))
        << "cycle 2 produced NaN — stale Jolt state from cycle 1";

    EXPECT_LT(cycle2EndY, 5.0f - 0.5f)
        << "body did not fall during cycle 2 — restart left the body unbound; y=" << cycle2EndY;

    // The two cycles started from the same state and ran for the same
    // simulated duration with the same dt. Determinism contract (ADR 0001
    // / CONTEXT.md) says they should converge to within solver noise.
    EXPECT_NEAR(cycle2EndY, cycle1EndY, 0.05f)
        << "cycle 2 final y=" << cycle2EndY
        << " diverged from cycle 1 final y=" << cycle1EndY
        << " — restart is not deterministic vs. fresh start";
}
