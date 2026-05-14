#include "OloEnginePCH.h"

// =============================================================================
// ComponentRemovedAtRuntimeTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene component-removal × Physics3D body lifecycle. We added an
//   OnComponentAdded hook for Rigidbody3DComponent and a per-entity
//   physics-body cleanup in Scene::DestroyEntity. The remaining gap is
//   `Entity::RemoveComponent<Rigidbody3DComponent>()`: gameplay code that
//   "demotes" an entity from physics-driven to logic-only (e.g. when a
//   character ragdolls and then transitions back to animation, or when a
//   destructible's debris stops being simulated) calls RemoveComponent.
//   Today that just hits `m_Registry.remove<T>` with no Jolt cleanup, so
//   the JoltScene retains a body for an entity whose ECS component is
//   gone — a leak whose first symptom is a JoltScene::Shutdown assert.
//
// Scenario: build a falling body, tick a few frames, RemoveComponent
// the Rigidbody3D, tick more, then let the harness's TearDown run. If the
// engine doesn't release the Jolt body on RemoveComponent, the entt-side
// shutdown trips an assertion on the now-orphaned body record.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Physics3D/JoltScene.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class ComponentRemovedAtRuntimeTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        Rigidbody3DComponent fb;
        fb.m_Type = BodyType3D::Static;
        BoxCollider3DComponent fc;
        fc.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        floor.AddComponent<BoxCollider3DComponent>(fc);
        floor.AddComponent<Rigidbody3DComponent>(fb);

        m_Body = GetScene().CreateEntity("Body");
        m_Body.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
        SphereCollider3DComponent col;
        col.m_Radius = 0.5f;
        m_Body.AddComponent<SphereCollider3DComponent>(col);
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        m_Body.AddComponent<Rigidbody3DComponent>(body);

        EnablePhysics3D();
    }

    Entity m_Body;
};

TEST_F(ComponentRemovedAtRuntimeTest, RemovingRigidbodyReleasesJoltBodyAndStopsTransformUpdates)
{
    // Phase 1: tick so the body is mid-fall.
    RunFrames(/*count=*/15); // 0.25s
    const f32 yMid = m_Body.GetComponent<TransformComponent>().Translation.y;
    ASSERT_LT(yMid, 5.0f - 0.1f) << "body did not fall before component removal";
    ASSERT_TRUE(std::isfinite(yMid));

    // Sanity: the Jolt body is registered against the entity.
    auto* joltScene = GetScene().GetPhysicsScene();
    ASSERT_NE(joltScene, nullptr);
    ASSERT_TRUE(joltScene->GetBody(m_Body))
        << "JoltScene has no body for the entity even though physics is live";

    // Phase 2: remove the rigidbody at runtime.
    m_Body.RemoveComponent<Rigidbody3DComponent>();

    // (a) JoltScene must no longer hold a body for this entity. If it does,
    // we have a leak and Shutdown will assert on the orphaned record.
    EXPECT_FALSE(joltScene->GetBody(m_Body))
        << "JoltScene still holds a body for an entity whose Rigidbody3DComponent "
           "was removed — Entity::RemoveComponent<Rigidbody3DComponent> is missing "
           "its OnComponentRemoved hook, mirror of the OnComponentAdded fix.";

    // (b) The component is actually gone from the ECS side.
    EXPECT_FALSE(m_Body.HasComponent<Rigidbody3DComponent>());

    // Phase 3: tick more — the entity's transform should not be updated by
    // physics anymore (it's now a static logical entity). It also must not
    // crash the engine.
    const glm::vec3 freezePos = m_Body.GetComponent<TransformComponent>().Translation;
    RunFrames(/*count=*/30); // 0.5s
    const glm::vec3 endPos = m_Body.GetComponent<TransformComponent>().Translation;

    EXPECT_TRUE(std::isfinite(endPos.x) && std::isfinite(endPos.y) && std::isfinite(endPos.z))
        << "transform NaN/Inf after RemoveComponent + tick";

    EXPECT_NEAR(endPos.y, freezePos.y, 1e-4f)
        << "transform continued to update after Rigidbody3D was removed — the "
           "stale Jolt body is still being synced into the entity. y went from "
           << freezePos.y << " to " << endPos.y;
}
