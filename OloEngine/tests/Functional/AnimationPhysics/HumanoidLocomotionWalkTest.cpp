#include "OloEnginePCH.h"
// OLO_TEST_LAYER: Functional

// =============================================================================
// HumanoidLocomotionWalkTest — Functional Test (issue #631 capstone).
//
// The whole locomotion stack in one scene, driven by real Scene::OnUpdateRuntime
// ticks against real Jolt physics:
//   * a foreign-named source rig (3ds Max Biped convention) carrying a walk
//     clip with root motion — live-retargeted (humanoid-role mapping) onto a
//     Mixamo-named, differently-indexed target humanoid (part 2),
//   * the baked clip's root motion extracted and applied to the entity, so the
//     character actually crosses the floor (part 1),
//   * FootIKComponent ground raycasts against the physics floor + a raised
//     step, pulling the feet onto uneven ground while the pose stays pinned
//     (part 3).
// A regression anywhere in the retarget→extract→apply→adapt chain leaves the
// character frozen at the origin, sliding without ground contact, or with
// feet ignoring the step.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Animation/FootIKComponent.h"
#include "OloEngine/Animation/Retargeting/RetargetingComponent.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/gtc/matrix_transform.hpp>

#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Hips + two 3-bone legs (thigh → calf → foot). The feet rest at model
    // y ≈ 0.1 — the FootHeight of a planted foot on y = 0 ground.
    Ref<Skeleton> BuildBipedSkeleton(const std::vector<std::string>& names)
    {
        auto skeleton = Ref<Skeleton>::Create(static_cast<sizet>(7));
        skeleton->m_BoneNames = names;
        skeleton->m_ParentIndices = { -1, 0, 1, 2, 0, 4, 5 };
        const glm::vec3 locals[7] = {
            { 0.0f, 1.05f, 0.0f },    // hips
            { 0.1f, -0.05f, 0.0f },   // L thigh
            { 0.0f, -0.45f, 0.02f },  // L calf (slight bend for the solver)
            { 0.0f, -0.45f, -0.02f }, // L foot
            { -0.1f, -0.05f, 0.0f },  // R thigh
            { 0.0f, -0.45f, 0.02f },  // R calf
            { 0.0f, -0.45f, -0.02f }, // R foot
        };
        for (sizet i = 0; i < 7; ++i)
        {
            skeleton->m_LocalTransforms[i] = glm::translate(glm::mat4(1.0f), locals[i]);
        }
        for (sizet i = 0; i < 7; ++i)
        {
            const int p = skeleton->m_ParentIndices[i];
            skeleton->m_GlobalTransforms[i] = (p >= 0)
                                                  ? skeleton->m_GlobalTransforms[static_cast<sizet>(p)] * skeleton->m_LocalTransforms[i]
                                                  : skeleton->m_LocalTransforms[i];
        }
        skeleton->SetBindPose();
        return skeleton;
    }

    glm::vec3 BoneWorldPos(Entity entity, u32 bone)
    {
        const auto& skeleton = entity.GetComponent<SkeletonComponent>().m_Skeleton;
        const glm::mat4 world = entity.GetComponent<TransformComponent>().GetTransform();
        return glm::vec3(world * skeleton->m_GlobalTransforms[bone][3]);
    }
} // namespace

class HumanoidLocomotionWalkTest : public FunctionalTest
{
  protected:
    static constexpr u32 kLeftFoot = 3;
    static constexpr u32 kRightFoot = 6;
    static constexpr f32 kStepTopY = 0.15f;

    void BuildScene() override
    {
        // ── Ground: a flat floor plus a raised step across the walk path ────
        {
            Entity floor = GetScene().CreateEntity("Floor");
            floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
            floor.GetComponent<TransformComponent>().Scale = { 40.0f, 1.0f, 40.0f };
            auto& rb = floor.AddComponent<Rigidbody3DComponent>();
            rb.m_Type = BodyType3D::Static;
            floor.AddComponent<BoxCollider3DComponent>();

            Entity step = GetScene().CreateEntity("Step");
            step.GetComponent<TransformComponent>().Translation = { 0.0f, kStepTopY - 0.5f, 2.2f };
            step.GetComponent<TransformComponent>().Scale = { 4.0f, 1.0f, 1.4f };
            auto& stepRb = step.AddComponent<Rigidbody3DComponent>();
            stepRb.m_Type = BodyType3D::Static;
            step.AddComponent<BoxCollider3DComponent>();
        }

        // ── Source rig (Biped names) + walk clip with root motion ───────────
        m_Source = GetScene().CreateEntity("AnimationLibrary");
        m_Source.AddComponent<SkeletonComponent>(BuildBipedSkeleton(
            { "Bip01 Pelvis", "Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot",
              "Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot" }));

        auto clip = Ref<AnimationClip>::Create();
        clip->Name = "Walk";
        clip->Duration = 1.0f;
        {
            BoneAnimation pelvis;
            pelvis.BoneName = "Bip01 Pelvis";
            // 1 unit forward per loop from the rest pose = 1 m/s of root motion.
            pelvis.PositionKeys.push_back({ 0.0, glm::vec3(0.0f, 1.05f, 0.0f) });
            pelvis.PositionKeys.push_back({ 1.0, glm::vec3(0.0f, 1.05f, 1.0f) });
            clip->BoneAnimations.push_back(std::move(pelvis));
        }
        clip->InitializeBoneCache();
        clip->RootMotion.ExtractRootMotion = true;
        clip->RootMotion.RootBoneIndex = 0;
        m_Source.AddComponent<AnimationStateComponent>().m_AvailableClips.push_back(clip);

        // ── Target humanoid (Mixamo-style names, live-retargeted) ───────────
        m_Character = GetScene().CreateEntity("Character");
        m_Character.AddComponent<SkeletonComponent>(BuildBipedSkeleton(
            { "Hips", "LeftUpLeg", "LeftLeg", "LeftFoot",
              "RightUpLeg", "RightLeg", "RightFoot" }));
        m_Character.AddComponent<AnimationStateComponent>();
        m_Character.AddComponent<RetargetingComponent>().m_SourceEntity = m_Source.GetUUID();

        auto& footIK = m_Character.AddComponent<FootIKComponent>();
        footIK.LeftFootBone = kLeftFoot;
        footIK.RightFootBone = kRightFoot;
        footIK.ChainLength = 3;
        footIK.PelvisBone = 0;
        footIK.FootHeight = 0.1f;
        footIK.RaycastUp = 0.6f;
        footIK.RaycastDown = 1.2f;
        footIK.FootLock = false; // constant-velocity walk: conformance is the contract here
        footIK.AlignFootToSlope = false;

        EnablePhysics3D();
    }

    Entity m_Source;
    Entity m_Character;
};

TEST_F(HumanoidLocomotionWalkTest, RetargetedHumanoidWalksWithRootMotionAndGroundedFeet)
{
    // Tick once: the live retarget bakes "Walk" onto the character.
    RunFrames(1);
    auto& animState = m_Character.GetComponent<AnimationStateComponent>();
    ASSERT_FALSE(animState.m_AvailableClips.empty()) << "live retarget produced no clips";
    animState.m_CurrentClip = animState.m_AvailableClips[0];
    animState.m_IsPlaying = true;

    const f32 startZ = m_Character.GetComponent<TransformComponent>().Translation.z;

    // Walk ~1.2 s on the flat floor.
    TickFor(1.2f);

    // Root motion moved the character (~1 m/s, same-proportioned rigs → scale 1).
    const f32 flatZ = m_Character.GetComponent<TransformComponent>().Translation.z;
    EXPECT_GT(flatZ - startZ, 0.8f) << "root motion is not moving the retargeted character";
    EXPECT_LT(flatZ - startZ, 1.7f);

    // The feet found the physics floor and sit at FootHeight above it.
    ASSERT_TRUE(m_Character.HasComponent<FootIKStateComponent>());
    const auto& ikState = m_Character.GetComponent<FootIKStateComponent>();
    EXPECT_TRUE(ikState.Left.HasGround) << "left foot raycast never hit the floor";
    EXPECT_TRUE(ikState.Right.HasGround);
    EXPECT_NEAR(ikState.Left.GroundPoint.y, 0.0f, 0.02f) << "flat floor is at y = 0";
    EXPECT_NEAR(BoneWorldPos(m_Character, kLeftFoot).y, 0.1f, 0.05f)
        << "foot is not resting FootHeight above the flat floor";

    // Keep walking onto the raised step (top at y = 0.15, spanning z 1.5–2.9).
    TickFor(1.2f);
    const f32 stepZ = m_Character.GetComponent<TransformComponent>().Translation.z;
    ASSERT_GT(stepZ, 1.6f) << "character did not reach the step";
    ASSERT_LT(stepZ, 2.8f) << "character overshot the step";

    // Uneven-ground adaptation: the ground cache found the step top and the
    // feet climbed onto it while the body origin stayed on the floor plane.
    EXPECT_NEAR(ikState.Left.GroundPoint.y, kStepTopY, 0.03f) << "ground cache missed the step";
    EXPECT_NEAR(BoneWorldPos(m_Character, kLeftFoot).y, kStepTopY + 0.1f, 0.06f)
        << "foot did not adapt onto the raised step";
    EXPECT_NEAR(BoneWorldPos(m_Character, kRightFoot).y, kStepTopY + 0.1f, 0.06f);

    // And the pose is pinned: the root bone's LOCAL translation carries no
    // accumulated forward motion (the entity transform does).
    const auto& skeleton = m_Character.GetComponent<SkeletonComponent>().m_Skeleton;
    const BoneTransform hips = Animation::BlendUtils::DecomposeMatrix(skeleton->m_LocalTransforms[0]);
    EXPECT_LT(std::abs(hips.Translation.z), 0.05f) << "root track not pinned — the mesh double-moves";
}
