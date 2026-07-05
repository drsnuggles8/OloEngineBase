// OLO_TEST_LAYER: Functional
#include "OloEnginePCH.h"

// =============================================================================
// WorldTransformPropagationTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene::PropagateWorldTransforms() × Entity::SetParent (issue #499). Before
//   this pass existed, every consumer read TransformComponent::GetTransform()
//   (local space) directly — a parented entity's world position never composed
//   its parent's transform. This test locks in the opposite: after a tick,
//   Entity::GetWorldTransform() must equal the flattened parent-chain product,
//   for deep chains, after re-parenting, and for orphaned/root entities.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/constants.hpp>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Bitwise/near-equality helper — the propagation pass composes matrices via
    // glm's floating-point ops, so an exact bit-compare would be too strict.
    ::testing::AssertionResult Vec3Near(const glm::vec3& a, const glm::vec3& b, f32 eps = 1e-4f)
    {
        if (glm::all(glm::epsilonEqual(a, b, eps)))
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure()
               << "expected (" << b.x << ", " << b.y << ", " << b.z << ") got (" << a.x << ", " << a.y << ", " << a.z << ")";
    }

    glm::vec3 WorldTranslation(Entity entity)
    {
        return glm::vec3(entity.GetWorldTransform()[3]);
    }
} // namespace

class WorldTransformPropagationTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Root = GetScene().CreateEntity("Root");
        m_Child = GetScene().CreateEntity("Child");
        m_Grandchild = GetScene().CreateEntity("Grandchild");
        m_GreatGrandchild = GetScene().CreateEntity("GreatGrandchild");
        m_OtherRoot = GetScene().CreateEntity("OtherRoot");
    }

    Entity m_Root, m_Child, m_Grandchild, m_GreatGrandchild, m_OtherRoot;
};

TEST_F(WorldTransformPropagationTest, RootEntityWorldMatchesLocal)
{
    m_Root.GetComponent<TransformComponent>().Translation = { 5.0f, 1.0f, -2.0f };

    RunFrames(1);

    EXPECT_TRUE(Vec3Near(WorldTranslation(m_Root), { 5.0f, 1.0f, -2.0f }));
}

TEST_F(WorldTransformPropagationTest, DeepChainComposesTranslationAlongParentChain)
{
    // Each link offsets by (1, 0, 0) in its own local space, no rotation — so a
    // 3-deep chain should place the great-grandchild at local-x == 4 in world space.
    m_Root.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f };
    m_Child.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f };
    m_Grandchild.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f };
    m_GreatGrandchild.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f };

    m_Child.SetParent(m_Root);
    m_Grandchild.SetParent(m_Child);
    m_GreatGrandchild.SetParent(m_Grandchild);

    RunFrames(1);

    EXPECT_TRUE(Vec3Near(WorldTranslation(m_Root), { 1.0f, 0.0f, 0.0f }));
    EXPECT_TRUE(Vec3Near(WorldTranslation(m_Child), { 2.0f, 0.0f, 0.0f }));
    EXPECT_TRUE(Vec3Near(WorldTranslation(m_Grandchild), { 3.0f, 0.0f, 0.0f }));
    EXPECT_TRUE(Vec3Near(WorldTranslation(m_GreatGrandchild), { 4.0f, 0.0f, 0.0f }));
}

TEST_F(WorldTransformPropagationTest, ParentRotationIsAppliedToChildOffset)
{
    // Parent rotated 90 degrees about Y maps its local +X axis to world -Z, so a
    // child offset by local (1, 0, 0) should land at world (~0, 0, ~-1), not (1, 0, 0).
    m_Root.GetComponent<TransformComponent>().SetRotationEuler({ 0.0f, glm::half_pi<float>(), 0.0f });
    m_Child.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f };
    m_Child.SetParent(m_Root);

    RunFrames(1);

    EXPECT_TRUE(Vec3Near(WorldTranslation(m_Child), { 0.0f, 0.0f, -1.0f }, 1e-3f));
}

TEST_F(WorldTransformPropagationTest, ReparentingUpdatesWorldTransformNextTick)
{
    m_Root.GetComponent<TransformComponent>().Translation = { 10.0f, 0.0f, 0.0f };
    m_OtherRoot.GetComponent<TransformComponent>().Translation = { -10.0f, 0.0f, 0.0f };
    m_Child.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f };

    m_Child.SetParent(m_Root);
    RunFrames(1);
    EXPECT_TRUE(Vec3Near(WorldTranslation(m_Child), { 11.0f, 0.0f, 0.0f }));

    m_Child.SetParent(m_OtherRoot);
    RunFrames(1);
    EXPECT_TRUE(Vec3Near(WorldTranslation(m_Child), { -9.0f, 0.0f, 0.0f }));
}

TEST_F(WorldTransformPropagationTest, UnparentingFallsBackToLocalTransform)
{
    m_Root.GetComponent<TransformComponent>().Translation = { 10.0f, 0.0f, 0.0f };
    m_Child.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f };
    m_Child.SetParent(m_Root);
    RunFrames(1);
    EXPECT_TRUE(Vec3Near(WorldTranslation(m_Child), { 11.0f, 0.0f, 0.0f }));

    m_Child.SetParent(Entity{});
    RunFrames(1);
    EXPECT_TRUE(Vec3Near(WorldTranslation(m_Child), { 1.0f, 0.0f, 0.0f }));
}

TEST_F(WorldTransformPropagationTest, DestroyedParentLeavesChildWorldTransformDefinedAsLocal)
{
    // DestroyEntity doesn't clean up RelationshipComponent (a pre-existing gap
    // guarded separately by HierarchyChildFollowsPhysicsParentTest); the
    // propagation pass must treat a dangling m_ParentHandle as "no parent"
    // rather than crashing or propagating stale/NaN data.
    m_Root.GetComponent<TransformComponent>().Translation = { 10.0f, 0.0f, 0.0f };
    m_Child.GetComponent<TransformComponent>().Translation = { 1.0f, 0.0f, 0.0f };
    m_Child.SetParent(m_Root);
    RunFrames(1);
    EXPECT_TRUE(Vec3Near(WorldTranslation(m_Child), { 11.0f, 0.0f, 0.0f }));

    GetScene().DestroyEntity(m_Root);
    RunFrames(1);

    const glm::mat4 childWorld = m_Child.GetWorldTransform();
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            EXPECT_TRUE(std::isfinite(childWorld[col][row])) << "world transform contains a non-finite element after parent destruction";

    EXPECT_TRUE(Vec3Near(WorldTranslation(m_Child), { 1.0f, 0.0f, 0.0f }));
}
