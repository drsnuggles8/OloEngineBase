#include "OloEnginePCH.h"

// =============================================================================
// PhysicsLayerFilteringTest — Functional Test.
//
// Cross-subsystem seam under test:
//   PhysicsLayerManager × Rigidbody3DComponent × Physics3D simulation. The
//   layer manager is process-static config; the body component carries a
//   layer ID; the broad-phase consults the collision matrix at every
//   contact check. A regression in layer registration, bit-mask building,
//   or the broad-phase filter shows up as either "things that should pass
//   through each other collide" (ghost bullets blocked by friendly
//   characters) or the inverse (player walks through walls). Neither is
//   visible to a per-subsystem unit test that only round-trips the layer
//   table.
//
// Scenario: configure two custom layers Alpha and Beta that do *not*
// collide with each other, plus a "Ground" layer that both collide with.
// Two dynamic spheres on Alpha and Beta start at y=5 with the Beta sphere
// directly below the Alpha sphere — close enough that without filtering
// they'd collide before reaching the floor. Tick. Assert both reach the
// floor (i.e. the upper Alpha sphere passed *through* the lower Beta).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Physics3D/PhysicsLayer.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class PhysicsLayerFilteringTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // PhysicsLayerManager is a process-static singleton. Clean it up
        // first so previous tests can't leak layers into ours.
        PhysicsLayerManager::ClearLayers();

        m_GroundLayer = PhysicsLayerManager::AddLayer("Functional_Ground", /*setCollisions=*/false);
        m_AlphaLayer = PhysicsLayerManager::AddLayer("Functional_Alpha", /*setCollisions=*/false);
        m_BetaLayer = PhysicsLayerManager::AddLayer("Functional_Beta", /*setCollisions=*/false);

        // Collision matrix: Alpha and Beta both collide with Ground, but
        // explicitly *not* with each other.
        PhysicsLayerManager::SetLayerCollision(m_AlphaLayer, m_GroundLayer, true);
        PhysicsLayerManager::SetLayerCollision(m_BetaLayer, m_GroundLayer, true);
        PhysicsLayerManager::SetLayerCollision(m_AlphaLayer, m_BetaLayer, false);

        // Floor on Ground layer.
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        auto& fb = floor.AddComponent<Rigidbody3DComponent>();
        fb.m_Type = BodyType3D::Static;
        fb.m_LayerID = m_GroundLayer;
        auto& fc = floor.AddComponent<BoxCollider3DComponent>();
        fc.m_HalfExtents = { 50.0f, 0.5f, 50.0f };

        // Beta sphere — *below* the Alpha sphere. If filtering works, Alpha
        // falls through Beta. If filtering is broken, Alpha lands on Beta
        // and stays at y ≈ 1.5.
        m_BetaSphere = GetScene().CreateEntity("BetaSphere");
        m_BetaSphere.GetComponent<TransformComponent>().Translation = { 0.0f, 1.5f, 0.0f };
        auto& betaBody = m_BetaSphere.AddComponent<Rigidbody3DComponent>();
        betaBody.m_Type = BodyType3D::Dynamic;
        betaBody.m_Mass = 1.0f;
        betaBody.m_LayerID = m_BetaLayer;
        auto& betaCol = m_BetaSphere.AddComponent<SphereCollider3DComponent>();
        betaCol.m_Radius = 0.5f;

        // Alpha sphere — above Beta, dropping straight down.
        m_AlphaSphere = GetScene().CreateEntity("AlphaSphere");
        m_AlphaSphere.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
        auto& alphaBody = m_AlphaSphere.AddComponent<Rigidbody3DComponent>();
        alphaBody.m_Type = BodyType3D::Dynamic;
        alphaBody.m_Mass = 1.0f;
        alphaBody.m_LayerID = m_AlphaLayer;
        auto& alphaCol = m_AlphaSphere.AddComponent<SphereCollider3DComponent>();
        alphaCol.m_Radius = 0.5f;

        EnablePhysics3D();
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        // Restore the layer manager so we don't pollute later tests.
        PhysicsLayerManager::ClearLayers();
    }

    Entity m_AlphaSphere;
    Entity m_BetaSphere;
    u32 m_GroundLayer = INVALID_LAYER_ID;
    u32 m_AlphaLayer = INVALID_LAYER_ID;
    u32 m_BetaLayer = INVALID_LAYER_ID;
};

TEST_F(PhysicsLayerFilteringTest, AlphaPassesThroughBetaAndBothLandOnGround)
{
    // Sanity: layers were configured the way we asked.
    ASSERT_TRUE(PhysicsLayerManager::ShouldCollide(m_AlphaLayer, m_GroundLayer));
    ASSERT_TRUE(PhysicsLayerManager::ShouldCollide(m_BetaLayer, m_GroundLayer));
    ASSERT_FALSE(PhysicsLayerManager::ShouldCollide(m_AlphaLayer, m_BetaLayer))
        << "collision matrix didn't actually disable Alpha↔Beta collision";

    TickFor(/*seconds=*/3.0f);

    const f32 alphaY = m_AlphaSphere.GetComponent<TransformComponent>().Translation.y;
    const f32 betaY = m_BetaSphere.GetComponent<TransformComponent>().Translation.y;

    ASSERT_TRUE(std::isfinite(alphaY) && std::isfinite(betaY))
        << "transform contains NaN/Inf";

    // Both bodies should be resting at floor height (sphere radius 0.5,
    // floor top y=0). Allow generous slop because they may have stacked
    // momentarily before separating.
    EXPECT_NEAR(alphaY, 0.5f, 0.15f)
        << "Alpha did not reach the floor; y=" << alphaY
        << ". If Alpha is sitting on Beta (y≈1.5+0.5≈2.0) the layer filter is broken.";
    EXPECT_NEAR(betaY, 0.5f, 0.15f)
        << "Beta did not reach the floor; y=" << betaY;

    // Stronger explicit guard: Alpha must not have stacked on Beta.
    EXPECT_LT(alphaY, 1.5f)
        << "Alpha stuck above floor — Alpha-Beta collision filter did not apply";
}
