#include "OloEnginePCH.h"

// =============================================================================
// InitialLinearVelocityIsAppliedTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Rigidbody3DComponent.m_InitialLinearVelocity × JoltScene::CreateBody.
//   The "initial velocity" field is read once at body construction time
//   and applied as the body's starting linear velocity. Projectiles,
//   particles-as-bodies, and physics-driven debris all rely on it.
//   Without it everything spawns from rest, regardless of what the
//   designer/code asked for.
//
// Scenario: a body created with `m_InitialLinearVelocity = (5, 0, 0)`
// and gravity disabled. After one second of unobstructed simulation,
// the body should have travelled ~5m along +x. Disable gravity to
// isolate this signal from free-fall.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class InitialLinearVelocityIsAppliedTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Body = GetScene().CreateEntity("Projectile");
        m_Body.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
        SphereCollider3DComponent col;
        col.m_Radius = 0.3f;
        m_Body.AddComponent<SphereCollider3DComponent>(col);
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        body.m_DisableGravity = true; // isolate the velocity signal
        body.m_LinearDrag = 0.0f;
        body.m_InitialLinearVelocity = { 5.0f, 0.0f, 0.0f }; // 5 m/s along +x
        m_Body.AddComponent<Rigidbody3DComponent>(body);

        EnablePhysics3D();
    }

    Entity m_Body;
};

TEST_F(InitialLinearVelocityIsAppliedTest, BodyTravelsFiveMetersInOneSecondAtFiveMetersPerSecond)
{
    const f32 startX = m_Body.GetComponent<TransformComponent>().Translation.x;
    ASSERT_NEAR(startX, 0.0f, 1e-4f);

    TickFor(/*seconds=*/1.0f);

    const auto& t = m_Body.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z));

    // With v=5 m/s for 1s, expect x ≈ 5. Jolt's integrator should be exact
    // to within solver tolerance on a body with no forces acting on it
    // (gravity disabled, drag = 0).
    EXPECT_NEAR(t.x, 5.0f, 0.2f)
        << "body did not travel 5m in 1s; x=" << t.x
        << " — m_InitialLinearVelocity was not applied at body construction "
           "(or LinearDrag was inadvertently non-zero).";

    // Y/Z stayed at start position (no gravity, no lateral forces).
    EXPECT_NEAR(t.y, 5.0f, 0.05f) << "body drifted vertically; gravity wasn't really disabled";
    EXPECT_NEAR(t.z, 0.0f, 0.05f) << "body drifted on Z";
}
