#include "OloEnginePCH.h"

// =============================================================================
// RigidbodyDisableGravityFlagTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Rigidbody3DComponent.m_DisableGravity × JoltScene::CreateBody ×
//   Jolt body GravityFactor. The flag is read at body-creation time
//   (inside JoltBody's ctor) and translated into the underlying Jolt
//   body's gravity factor. A regression that ignores the flag silently
//   pulls every "floating platform" / "frozen prop" out of the sky.
//
// Scenario: one dynamic body with `m_DisableGravity = true`, one without
// (control). Tick. Assert the no-gravity body stayed put; the control
// body fell.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class RigidbodyDisableGravityFlagTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Two dynamic bodies in mid-air, same starting height.
        m_NoGravity = GetScene().CreateEntity("Floating");
        m_NoGravity.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
        SphereCollider3DComponent col;
        col.m_Radius = 0.4f;
        m_NoGravity.AddComponent<SphereCollider3DComponent>(col);
        Rigidbody3DComponent floating;
        floating.m_Type = BodyType3D::Dynamic;
        floating.m_Mass = 1.0f;
        floating.m_DisableGravity = true; // the flag under test
        m_NoGravity.AddComponent<Rigidbody3DComponent>(floating);

        m_Control = GetScene().CreateEntity("Falling");
        m_Control.GetComponent<TransformComponent>().Translation = { 2.0f, 5.0f, 0.0f };
        m_Control.AddComponent<SphereCollider3DComponent>(col);
        Rigidbody3DComponent control;
        control.m_Type = BodyType3D::Dynamic;
        control.m_Mass = 1.0f;
        m_Control.AddComponent<Rigidbody3DComponent>(control);

        EnablePhysics3D();
    }

    Entity m_NoGravity;
    Entity m_Control;
};

TEST_F(RigidbodyDisableGravityFlagTest, BodyWithDisableGravityStaysAtRestWhileControlFalls)
{
    const f32 startY = 5.0f;

    TickFor(/*seconds=*/1.0f);

    const f32 yNoGrav = m_NoGravity.GetComponent<TransformComponent>().Translation.y;
    const f32 yControl = m_Control.GetComponent<TransformComponent>().Translation.y;

    EXPECT_TRUE(std::isfinite(yNoGrav));
    EXPECT_TRUE(std::isfinite(yControl));

    // No-gravity body should be approximately where it started. A few cm
    // of drift is acceptable from Jolt's solver but not metres.
    EXPECT_NEAR(yNoGrav, startY, 0.1f)
        << "body with m_DisableGravity = true fell anyway; y went from "
        << startY << " to " << yNoGrav
        << " — the flag is not propagating to Jolt's GravityFactor.";

    // Control body should have fallen ~4.9m in 1s (½g t²).
    EXPECT_LT(yControl, startY - 1.0f)
        << "control body did not fall — gravity is off scene-wide, not just for "
           "the flagged body. Test setup is wrong or gravity broken globally.";
}
