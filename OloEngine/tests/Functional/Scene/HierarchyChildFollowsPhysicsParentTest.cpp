#include "OloEnginePCH.h"

// =============================================================================
// HierarchyChildOnDestroyedParentTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene hierarchy × entity destruction × physics. Gameplay code spawns
//   parent-child structures (vehicle + turret, character + held items) and
//   destroys them at runtime. The engine has to choose: when the parent is
//   destroyed, are children also destroyed, orphaned, or left dangling? A
//   bug here is "destroying the car leaves the headlight as an invisible
//   ghost entity that still ticks every frame" — invisible to the player,
//   but a memory/perf leak that grows over time.
//
// Scenario: parent + child set up via Entity::SetParent. Parent has physics,
// child does not. Tick once to confirm both alive. Destroy the parent.
// Tick again. Assert one of the well-defined outcomes (child either
// orphaned with parent=null, or child destroyed too) — the *undefined*
// outcome is "child still has a non-null parent UUID pointing at a dead
// entity," which is the bug class.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class HierarchyChildOnDestroyedParentTest : public FunctionalTest
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

        // Parent — physics body, will be destroyed mid-test.
        m_Parent = GetScene().CreateEntity("Parent");
        m_Parent.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
        SphereCollider3DComponent col;
        col.m_Radius = 0.5f;
        m_Parent.AddComponent<SphereCollider3DComponent>(col);
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        m_Parent.AddComponent<Rigidbody3DComponent>(body);

        // Child — no physics, parented to the parent.
        m_Child = GetScene().CreateEntity("Child");
        m_Child.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
        m_Child.SetParent(m_Parent);
        m_ChildUUID = m_Child.GetUUID();

        EnablePhysics3D();
    }

    Entity m_Parent;
    Entity m_Child;
    UUID m_ChildUUID;
};

TEST_F(HierarchyChildOnDestroyedParentTest, ChildDoesNotDanglePointingAtDeadParent)
{
    // Sanity: hierarchy is wired before any destruction.
    {
        ASSERT_TRUE(m_Child.GetParent());
        EXPECT_EQ(m_Child.GetParent().GetUUID(), m_Parent.GetUUID());
    }

    // Tick to confirm the world is alive and the parent moved.
    RunFrames(/*count=*/15); // 0.25s
    ASSERT_LT(m_Parent.GetComponent<TransformComponent>().Translation.y, 5.0f);

    // Destroy the parent mid-tick.
    GetScene().DestroyEntity(m_Parent);
    m_Parent = Entity{};

    // Tick more — gives the engine a frame to process any deferred cleanup.
    RunFrames(/*count=*/15);

    // The child must end up in one of two well-defined states:
    //   (a) destroyed alongside the parent, OR
    //   (b) orphaned: still alive but with no parent (parent UUID == 0)
    auto childOpt = GetScene().TryGetEntityWithUUID(m_ChildUUID);

    if (!childOpt)
    {
        // (a) Recursive destroy — fine. Document via SUCCEED.
        SUCCEED() << "Child was destroyed when parent was destroyed (recursive teardown).";
        return;
    }

    Entity child = *childOpt;
    Entity childParent = child.GetParent();

    // Child is still alive — must be orphaned, not pointing at the dead parent.
    EXPECT_FALSE(childParent)
        << "child still references its destroyed parent — dangling parent UUID is the bug class.";

    // Child's transform should still be readable (no NaN, no UAF).
    const glm::vec3 childPos = child.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(childPos.x) && std::isfinite(childPos.y) && std::isfinite(childPos.z))
        << "orphaned child transform is corrupted after parent destruction";
}
