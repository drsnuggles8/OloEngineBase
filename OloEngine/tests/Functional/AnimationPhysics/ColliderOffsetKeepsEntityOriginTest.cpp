#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional

// =============================================================================
// ColliderOffsetKeepsEntityOriginTest — Functional Test.
//
// Cross-subsystem seam under test:
//   BoxCollider3DComponent.m_Offset × JoltShapes (an offset collider becomes a
//   translated shape, whose centre of mass is the offset) × JoltScene's
//   transform sync.
//
// An entity's Translation is its ORIGIN, which is also where the physics body
// is created. The sync used to write the body's CENTRE OF MASS back instead.
// The two are the same point until a collider has an offset; then the entity
// jumped by the offset on the first step, and everything drawn under it with it.
//
// Scenario: weightless dynamic boxes whose collider sits half a metre below the
// entity. One rests; one spins about Z. The resting one must stay where it was
// authored. The spinning one turns about its centre of mass, so its origin has
// to travel round it: after half a turn it is a metre lower. A third spins on
// an entity scaled by two: the offset scales with the entity, like the extents
// it offsets, so its origin ends two metres lower.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class ColliderOffsetKeepsEntityOriginTest : public FunctionalTest
{
  protected:
    static constexpr f32 kStartY = 5.0f;
    static constexpr f32 kOffsetY = -0.5f;

    Entity Spawn(const char* name, f32 x, const glm::vec3& angularVelocity, f32 scale = 1.0f)
    {
        Entity entity = GetScene().CreateEntity(name);
        entity.GetComponent<TransformComponent>().Translation = { x, kStartY, 0.0f };
        entity.GetComponent<TransformComponent>().Scale = glm::vec3(scale);
        BoxCollider3DComponent box;
        box.m_HalfExtents = { 0.5f, 0.25f, 0.5f };
        box.m_Offset = { 0.0f, kOffsetY, 0.0f };
        entity.AddComponent<BoxCollider3DComponent>(box);
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 10.0f;
        body.m_DisableGravity = true;
        body.m_LinearDrag = 0.0f;
        body.m_AngularDrag = 0.0f;
        body.m_InitialAngularVelocity = angularVelocity;
        entity.AddComponent<Rigidbody3DComponent>(body);
        return entity;
    }

    void BuildScene() override
    {
        m_Resting = Spawn("Resting", 0.0f, glm::vec3(0.0f));
        m_Spinning = Spawn("Spinning", 4.0f, { 0.0f, 0.0f, glm::pi<f32>() });
        m_SpinningScaled = Spawn("SpinningScaled", 12.0f, { 0.0f, 0.0f, glm::pi<f32>() }, 2.0f);
        EnablePhysics3D();
    }

    Entity m_Resting;
    Entity m_Spinning;
    Entity m_SpinningScaled;
};

TEST_F(ColliderOffsetKeepsEntityOriginTest, TheEntityStaysAtItsOriginAndTurnsAboutItsCentreOfMass)
{
    TickFor(/*seconds=*/1.0f);

    const glm::vec3 resting = m_Resting.GetComponent<TransformComponent>().Translation;
    ASSERT_TRUE(std::isfinite(resting.x) && std::isfinite(resting.y) && std::isfinite(resting.z));
    EXPECT_NEAR(resting.x, 0.0f, 0.01f);
    EXPECT_NEAR(resting.y, kStartY, 0.01f)
        << "a body at rest moved by its collider's offset: the sync wrote the centre of mass ("
        << kStartY + kOffsetY << ") over the entity's origin";
    EXPECT_NEAR(resting.z, 0.0f, 0.01f);

    // Half a turn about Z in one second. The centre of mass, half a metre below the
    // origin, stays put; the origin ends half a metre below IT.
    const glm::vec3 spinning = m_Spinning.GetComponent<TransformComponent>().Translation;
    ASSERT_TRUE(std::isfinite(spinning.x) && std::isfinite(spinning.y) && std::isfinite(spinning.z));
    EXPECT_NEAR(spinning.x, 4.0f, 0.08f);
    EXPECT_NEAR(spinning.y, kStartY + 2.0f * kOffsetY, 0.08f)
        << "the origin did not travel round the centre of mass as the body turned";

    // The same on an entity twice the size: the collider is twice as far below it.
    const glm::vec3 scaled = m_SpinningScaled.GetComponent<TransformComponent>().Translation;
    ASSERT_TRUE(std::isfinite(scaled.x) && std::isfinite(scaled.y) && std::isfinite(scaled.z));
    EXPECT_NEAR(scaled.x, 12.0f, 0.16f);
    EXPECT_NEAR(scaled.y, kStartY + 2.0f * 2.0f * kOffsetY, 0.16f)
        << "the offset did not scale with the entity: the navmesh, DiscoverySystem and the gizmo put it "
           "through the entity's matrix, and the simulated collider is somewhere else";
}
