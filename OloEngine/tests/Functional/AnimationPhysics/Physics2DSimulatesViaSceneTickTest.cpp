#include "OloEnginePCH.h"

// =============================================================================
// Physics2DSimulatesViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × Box2D × TransformComponent sync. We previously caught the
//   "OnUpdateRuntime calls b2World_Step on a null world" bug; this is the
//   positive-case complement: with `OnPhysics2DStart` actually called, a
//   2D body should fall under gravity and its TransformComponent should
//   pick up Box2D's per-frame position. A regression that breaks the
//   Scene→Box2D body sync (separate code path from the 3D one) silently
//   freezes every 2D actor in projects.
//
// Scenario: a static 2D floor + a dynamic 2D box dropped from y=5. Tick
// for ~3s. Assert the box's TransformComponent.y settled near the floor.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class Physics2DSimulatesViaSceneTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Static floor — large box across X, just below origin.
        auto floor = GetScene().CreateEntity("Floor2D");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -1.0f, 0.0f };
        Rigidbody2DComponent floorBody;
        floorBody.Type = Rigidbody2DComponent::BodyType::Static;
        floor.AddComponent<Rigidbody2DComponent>(floorBody);
        BoxCollider2DComponent floorCol;
        floorCol.Size = { 25.0f, 0.5f };
        floor.AddComponent<BoxCollider2DComponent>(floorCol);

        // Dynamic box dropped from y=5.
        m_Box = GetScene().CreateEntity("Box2D");
        m_Box.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
        Rigidbody2DComponent boxBody;
        boxBody.Type = Rigidbody2DComponent::BodyType::Dynamic;
        boxBody.FixedRotation = true; // simpler assertions
        m_Box.AddComponent<Rigidbody2DComponent>(boxBody);
        BoxCollider2DComponent boxCol;
        boxCol.Size = { 0.5f, 0.5f };
        m_Box.AddComponent<BoxCollider2DComponent>(boxCol);

        EnablePhysics2D();
    }

    Entity m_Box;
};

TEST_F(Physics2DSimulatesViaSceneTickTest, BoxFallsAndLandsOnStaticFloor)
{
    const f32 startY = m_Box.GetComponent<TransformComponent>().Translation.y;
    ASSERT_NEAR(startY, 5.0f, 1e-3f);

    // 3s is plenty for a 4-unit drop under Box2D's default gravity.
    TickFor(/*seconds=*/3.0f);

    const auto& t = m_Box.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(t.x) && std::isfinite(t.y))
        << "2D body produced NaN/Inf";

    // Floor top is at y = -1 + 0.5 = -0.5. Box half-extent = 0.5. Resting
    // y should be -0.5 + 0.5 = 0.0. Allow Box2D's iterative-solver slop.
    EXPECT_LT(t.y, 1.0f)
        << "2D body did not fall — Scene→Box2D sync wiring is broken; y=" << t.y;
    EXPECT_GT(t.y, -0.6f)
        << "2D body fell through the static floor; y=" << t.y;

    // FixedRotation kept it upright; with no horizontal velocity it shouldn't
    // have drifted.
    EXPECT_NEAR(t.x, 0.0f, 0.1f)
        << "2D body drifted laterally without input; x=" << t.x;
}
