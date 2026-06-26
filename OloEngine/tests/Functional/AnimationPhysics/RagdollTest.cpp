#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// RagdollTest — Functional Test.
//
// Cross-subsystem seam under test:
//   RagdollComponent (authored ECS data) × Animation skeleton/bone-entity
//   mapping (BoneEntityUtils) × JoltScene ragdoll expansion × Physics3D
//   simulation, all driven through the real Scene::OnPhysics3DStart /
//   OnUpdateRuntime path.
//
// FOUNDATION slice (issue #308 item 5): at physics start JoltScene::CreateRagdolls
// expands every enabled RagdollComponent into a chain of per-bone Rigidbody3D +
// sphere-collider + SwingTwist-joint components on the skeleton's bone entities
// (reusing the existing constraint infrastructure rather than Jolt's dedicated
// Ragdoll class). A bone already carrying a Rigidbody3DComponent is kept, so an
// authored Static root bone anchors the ragdoll. Each parent->child link gets a
// SwingTwist joint pivoting about the parent bone's origin, with
// CollideConnected = false. At physics stop the generated components are removed
// again, restoring the authored scene.
//
// These tests assert (1) the generated structure, (2) the physically-meaningful
// swing-cone limit a working ragdoll holds (a broken/free joint fails by a wide
// margin), (3) clean teardown, and (4) the save-game round-trip — using
// positional tolerances, never float `==` (see CLAUDE.md / docs/testing.md).
//
// Functional-test contract: see docs/testing.md §7, ADR 0001/0002/0003.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"

#include <glm/glm.hpp>
#include <cmath>
#include <string>

using namespace OloEngine;
using namespace OloEngine::Functional;

class RagdollTest : public FunctionalTest
{
  protected:
    // Each test builds its own scene in the body, then calls EnablePhysics3D()
    // (which snapshots whatever exists at call time), so BuildScene stays empty.
    void BuildScene() override
    { /* intentionally empty — each test authors its own scene before EnablePhysics3D() */
    }

    // A two-bone skeleton: a root and one child limb (parentIndices {-1, 0}).
    static Ref<Skeleton> MakeTwoBoneSkeleton(const std::string& rootName, const std::string& limbName)
    {
        Ref<Skeleton> skeleton = Ref<Skeleton>::Create();
        skeleton->m_BoneNames = { rootName, limbName };
        skeleton->m_ParentIndices = { -1, 0 };
        return skeleton;
    }

    static glm::vec3 Pos(Entity e)
    {
        return e.GetComponent<TransformComponent>().Translation;
    }

    // Build the standard "static root + dynamic limb" ragdoll scene used by the
    // physics tests. The root bone is authored Static (so it anchors the ragdoll);
    // the limb bone has no body, so the ragdoll generates a dynamic one and a
    // SwingTwist joint linking limb -> root. Returns {ragdollRoot, rootBone, limbBone}.
    struct RagdollScene
    {
        Entity m_RagdollRoot;
        Entity m_RootBone;
        Entity m_LimbBone;
    };

    RagdollScene BuildStaticRootRagdoll(f32 swingLimitDeg, f32 twistLimitDeg)
    {
        Ref<Skeleton> skeleton = MakeTwoBoneSkeleton("Root", "Limb");

        Entity ragdollRoot = GetScene().CreateEntity("RagdollRoot");
        ragdollRoot.AddComponent<SkeletonComponent>(skeleton);
        auto& ragdoll = ragdollRoot.AddComponent<RagdollComponent>();
        ragdoll.m_SkeletonEntity = 0; // resolve from this entity's SkeletonComponent
        ragdoll.m_BoneMass = 1.0f;
        ragdoll.m_BoneRadius = 0.2f;
        ragdoll.m_SwingLimitDeg = swingLimitDeg;
        ragdoll.m_TwistLimitDeg = twistLimitDeg;

        // Root bone: a pre-authored Static anchor at the pivot.
        Entity rootBone = GetScene().CreateEntity("Root");
        rootBone.SetParent(ragdollRoot);
        rootBone.GetComponent<TransformComponent>().Translation = { 0.0f, 3.0f, 0.0f };
        auto& rootRb = rootBone.AddComponent<Rigidbody3DComponent>();
        rootRb.m_Type = BodyType3D::Static;
        rootBone.AddComponent<SphereCollider3DComponent>().m_Radius = 0.2f;

        // Limb bone: 1 m to the +X side of the root, horizontal, NO body yet — the
        // ragdoll generates a dynamic one and the SwingTwist joint to the root.
        Entity limbBone = GetScene().CreateEntity("Limb");
        limbBone.SetParent(ragdollRoot);
        limbBone.GetComponent<TransformComponent>().Translation = { 1.0f, 3.0f, 0.0f };

        return { ragdollRoot, rootBone, limbBone };
    }
};

// -----------------------------------------------------------------------------
// Structure — the ragdoll pass generates a dynamic body + collider + SwingTwist
// joint for the limb bone, keeps the pre-authored Static root, and registers one
// ragdoll / one constraint.
// -----------------------------------------------------------------------------
TEST_F(RagdollTest, GeneratesBodiesAndJointsForBonesMissingThem)
{
    RagdollScene s = BuildStaticRootRagdoll(/*swing*/ 30.0f, /*twist*/ 10.0f);

    // Before play: the limb carries no physics components.
    EXPECT_FALSE(s.m_LimbBone.HasComponent<Rigidbody3DComponent>());
    EXPECT_FALSE(s.m_LimbBone.HasComponent<PhysicsJoint3DComponent>());

    EnablePhysics3D();

    ASSERT_NE(GetScene().GetPhysicsScene(), nullptr);
    EXPECT_EQ(GetScene().GetPhysicsScene()->GetRagdollCount(), 1u) << "ragdoll was not built";
    EXPECT_EQ(GetScene().GetPhysicsScene()->GetConstraintCount(), 1u) << "the parent->child SwingTwist joint was not created";

    // The limb gained a generated dynamic body, a sphere collider, and a
    // SwingTwist joint connected to the root bone.
    ASSERT_TRUE(s.m_LimbBone.HasComponent<Rigidbody3DComponent>()) << "ragdoll did not generate a body for the limb";
    EXPECT_EQ(s.m_LimbBone.GetComponent<Rigidbody3DComponent>().m_Type, BodyType3D::Dynamic);
    EXPECT_TRUE(s.m_LimbBone.HasComponent<SphereCollider3DComponent>());
    ASSERT_TRUE(s.m_LimbBone.HasComponent<PhysicsJoint3DComponent>()) << "ragdoll did not generate the SwingTwist joint";
    const auto& joint = s.m_LimbBone.GetComponent<PhysicsJoint3DComponent>();
    EXPECT_EQ(joint.m_Type, JointType3D::SwingTwist);
    EXPECT_EQ(joint.m_ConnectedEntity, s.m_RootBone.GetUUID());
    EXPECT_FALSE(joint.m_CollideConnected) << "ragdoll links must not collide with each other";

    // The pre-authored Static root was kept as-is (NOT overwritten with a body).
    ASSERT_TRUE(s.m_RootBone.HasComponent<Rigidbody3DComponent>());
    EXPECT_EQ(s.m_RootBone.GetComponent<Rigidbody3DComponent>().m_Type, BodyType3D::Static);
    EXPECT_FALSE(s.m_RootBone.HasComponent<PhysicsJoint3DComponent>()) << "the root bone has no parent and must not get a joint";
}

// -----------------------------------------------------------------------------
// Swing-cone limit — the limb starts horizontal (level with the static root,
// 1 m to the side) and droops under gravity, but the SwingTwist cone clips the
// droop. A free ball joint would let it swing all the way down to vertical
// (y ~ 2.0); a 20-degree cone holds it near y ~ 2.66. A broken/weld joint never
// droops at all (y ~ 3.0). The arm length is held the whole time.
// -----------------------------------------------------------------------------
TEST_F(RagdollTest, LimbSwingsWithinSwingTwistCone)
{
    const glm::vec3 pivot{ 0.0f, 3.0f, 0.0f };
    RagdollScene s = BuildStaticRootRagdoll(/*swing*/ 20.0f, /*twist*/ 10.0f);

    EnablePhysics3D();
    ASSERT_TRUE(s.m_LimbBone.HasComponent<Rigidbody3DComponent>());

    f32 maxDistErr = 0.0f;
    f32 minY = Pos(s.m_LimbBone).y;
    for (int i = 0; i < 300; ++i) // 5 s at 60 Hz
    {
        RunFrames(1);
        const glm::vec3 p = Pos(s.m_LimbBone);
        maxDistErr = std::max(maxDistErr, std::abs(glm::distance(p, pivot) - 1.0f));
        minY = std::min(minY, p.y);
    }

    const glm::vec3 end = Pos(s.m_LimbBone);
    EXPECT_TRUE(std::isfinite(end.x) && std::isfinite(end.y) && std::isfinite(end.z));
    // The ball joint must keep the limb pinned at arm's length from the pivot.
    EXPECT_LT(maxDistErr, 0.2f) << "ragdoll joint did not hold the limb at a fixed distance from the pivot";
    // The limb must droop under gravity (a frozen/weld joint stays at y ~ 3.0)...
    EXPECT_LT(minY, 2.9f) << "the limb never drooped — the SwingTwist joint behaved like a rigid weld";
    // ...but the 20-degree swing cone must clip the droop well above vertical
    // (a free ball joint would settle near y ~ 2.0). sin(20) ~ 0.34 -> y ~ 2.66.
    EXPECT_GT(end.y, 2.4f) << "the limb escaped the swing cone (free ball joint); y=" << end.y;
}

// -----------------------------------------------------------------------------
// Teardown — stopping physics removes the components the ragdoll generated
// (restoring the authored scene) while leaving pre-authored bone physics intact.
// -----------------------------------------------------------------------------
TEST_F(RagdollTest, TeardownRestoresAuthoredScene)
{
    RagdollScene s = BuildStaticRootRagdoll(/*swing*/ 30.0f, /*twist*/ 10.0f);

    EnablePhysics3D();
    ASSERT_TRUE(s.m_LimbBone.HasComponent<Rigidbody3DComponent>());
    ASSERT_TRUE(s.m_LimbBone.HasComponent<PhysicsJoint3DComponent>());
    ASSERT_TRUE(s.m_LimbBone.HasComponent<SphereCollider3DComponent>());

    GetScene().OnPhysics3DStop();

    ASSERT_NE(GetScene().GetPhysicsScene(), nullptr);
    EXPECT_EQ(GetScene().GetPhysicsScene()->GetRagdollCount(), 0u) << "ragdoll runtime tracking was not cleared on stop";

    // The generated components are gone again...
    EXPECT_FALSE(s.m_LimbBone.HasComponent<Rigidbody3DComponent>()) << "generated limb body was not removed on stop";
    EXPECT_FALSE(s.m_LimbBone.HasComponent<PhysicsJoint3DComponent>()) << "generated limb joint was not removed on stop";
    EXPECT_FALSE(s.m_LimbBone.HasComponent<SphereCollider3DComponent>()) << "generated limb collider was not removed on stop";
    EXPECT_EQ(s.m_RagdollRoot.GetComponent<RagdollComponent>().m_RuntimeRagdollToken, 0u);

    // ...but the pre-authored Static root body is untouched.
    ASSERT_TRUE(s.m_RootBone.HasComponent<Rigidbody3DComponent>()) << "the pre-authored root body was wrongly removed";
    EXPECT_EQ(s.m_RootBone.GetComponent<Rigidbody3DComponent>().m_Type, BodyType3D::Static);
}

// -----------------------------------------------------------------------------
// Save-game round-trip — the authored RagdollComponent data must survive
// CaptureSceneState -> RestoreSceneState (exercises SaveGameComponentSerializer).
// -----------------------------------------------------------------------------
TEST_F(RagdollTest, ComponentSurvivesSaveGameRoundTrip)
{
    constexpr f32 kEps = 1e-4f;

    Entity e = GetScene().CreateEntity("RagdollSaveGame");
    auto& r = e.AddComponent<RagdollComponent>();
    r.m_SkeletonEntity = UUID(0xC0FFEE42ULL);
    r.m_Enabled = false;
    r.m_BoneMass = 2.5f;
    r.m_BoneRadius = 0.12f;
    r.m_SwingLimitDeg = 33.0f;
    r.m_TwistLimitDeg = 17.0f;

    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    Entity re = restored->FindEntityByName("RagdollSaveGame");
    ASSERT_TRUE(re);
    ASSERT_TRUE(re.HasComponent<RagdollComponent>()) << "RagdollComponent dropped by the save-game round-trip";

    const auto& rr = re.GetComponent<RagdollComponent>();
    EXPECT_EQ(static_cast<u64>(rr.m_SkeletonEntity), 0xC0FFEE42ULL);
    EXPECT_FALSE(rr.m_Enabled);
    EXPECT_NEAR(rr.m_BoneMass, 2.5f, kEps);
    EXPECT_NEAR(rr.m_BoneRadius, 0.12f, kEps);
    EXPECT_NEAR(rr.m_SwingLimitDeg, 33.0f, kEps);
    EXPECT_NEAR(rr.m_TwistLimitDeg, 17.0f, kEps);
}
