#include "OloEnginePCH.h"

// =============================================================================
// TwoCharacterControllersCoexistTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene × Physics3D × JoltCharacterController (multi-instance). The
//   CharacterControllerWalksTest pins a single character. This test
//   pins the per-character isolation invariant: two characters started
//   in the same world must each tick their own integrator and end up
//   in different places after independent SetLinearVelocity calls.
//   A regression that aliases controller state across entities (e.g.
//   a static slot in JoltScene::m_CharacterControllers, or a shared
//   CharacterContact dispatch table) collapses both to the same
//   trajectory. The CHARACTER layer's collision-filter setup also
//   needs to either ignore character-vs-character contacts (typical
//   for player vs player) or handle them cleanly; an explosive jitter
//   here would tell us the filter swallowed something.
//
// Scenario: two characters on a shared floor, 5m apart along Z. Each
// gets a perpendicular velocity (+X and -X respectively). After 1s,
// the two characters should be in distinct positions, each having
// moved ~1m in opposite directions along X, with Z roughly preserved
// (no perpendicular drift caused by accidentally sharing state).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/JoltCharacterController.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class TwoCharacterControllersCoexistTest : public FunctionalTest
{
  protected:
    static constexpr f32 kStartZ = 5.0f;

    void BuildScene() override
    {
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        Rigidbody3DComponent floorBody;
        floorBody.m_Type = BodyType3D::Static;
        BoxCollider3DComponent floorCol;
        floorCol.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        floor.AddComponent<BoxCollider3DComponent>(floorCol);
        floor.AddComponent<Rigidbody3DComponent>(floorBody);

        auto makeChar = [this](const char* name, const glm::vec3& pos) -> Entity
        {
            Entity e = GetScene().CreateEntity(name);
            e.GetComponent<TransformComponent>().Translation = pos;
            CapsuleCollider3DComponent capsule;
            capsule.m_Radius = 0.4f;
            capsule.m_HalfHeight = 0.6f;
            e.AddComponent<CapsuleCollider3DComponent>(capsule);
            e.AddComponent<CharacterController3DComponent>();
            return e;
        };

        m_Alpha = makeChar("Alpha", { 0.0f, 1.0f,  kStartZ });
        m_Beta  = makeChar("Beta",  { 0.0f, 1.0f, -kStartZ });

        EnablePhysics3D();
    }

    Entity m_Alpha;
    Entity m_Beta;
};

TEST_F(TwoCharacterControllersCoexistTest, EachControllerIntegratesItsOwnVelocityIndependently)
{
    auto* joltScene = GetScene().GetPhysicsScene();
    ASSERT_NE(joltScene, nullptr);

    auto alphaCtrl = joltScene->GetCharacterController(m_Alpha);
    auto betaCtrl  = joltScene->GetCharacterController(m_Beta);
    ASSERT_TRUE(alphaCtrl)
        << "Alpha got no controller; the iteration over CharacterController3DComponent "
           "entities in OnPhysics3DStart skipped one (likely due to an entt-view bug "
           "when more than one entity has the component).";
    ASSERT_TRUE(betaCtrl)
        << "Beta got no controller — same multi-entity iteration issue.";
    ASSERT_NE(alphaCtrl.Raw(), betaCtrl.Raw())
        << "Both entities ended up sharing the same controller pointer — "
           "JoltScene::m_CharacterControllers is keyed wrong (e.g., overwriting "
           "the same slot for any second character).";

    // Isolate the velocity-application path from gravity.
    alphaCtrl->SetGravityEnabled(false);
    alphaCtrl->SetControlMovementInAir(true);
    betaCtrl->SetGravityEnabled(false);
    betaCtrl->SetControlMovementInAir(true);

    alphaCtrl->SetLinearVelocity({  1.0f, 0.0f, 0.0f }); // +X 1 m/s
    betaCtrl ->SetLinearVelocity({ -1.0f, 0.0f, 0.0f }); // -X 1 m/s

    TickFor(/*seconds=*/1.0f);

    const auto& alphaT = m_Alpha.GetComponent<TransformComponent>().Translation;
    const auto& betaT  = m_Beta .GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(alphaT.x) && std::isfinite(betaT.x));

    EXPECT_GT(alphaT.x, 0.5f)
        << "Alpha didn't move along +X; its controller wasn't ticking (or its "
           "velocity got overwritten by Beta's).";
    EXPECT_LT(betaT.x, -0.5f)
        << "Beta didn't move along -X; same story but symmetric.";

    // Z separation should be preserved — controllers shouldn't bleed into
    // each other's Z component.
    EXPECT_NEAR(alphaT.z,  kStartZ, 0.3f)
        << "Alpha drifted on Z; cross-character state aliasing leaked Beta's "
           "Z toward Alpha.";
    EXPECT_NEAR(betaT.z,  -kStartZ, 0.3f)
        << "Beta drifted on Z — same issue mirrored.";

    // They must not have ended up at the same spot.
    const f32 separation = std::abs(alphaT.x - betaT.x);
    EXPECT_GT(separation, 1.0f)
        << "controllers converged on X (separation " << separation
        << "); they're definitely sharing integrator state.";
}
