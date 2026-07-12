#include "OloEnginePCH.h"
// OLO_TEST_LAYER: Functional

// =============================================================================
// LiveRetargetingTest — Functional Test (issue #631 part 2).
//
// Cross-subsystem seam under test:
//   Scene tick × RetargetingSystem × AnimationSystem × root-motion pipeline.
//   A RetargetingComponent pointing at a source entity (foreign rig + clips,
//   bone names sharing NOTHING with the target) must — with no offline bake —
//   splice role-mapped retargeted clips into the target's available clips,
//   drive the target skeleton through the normal AnimationSystem path, carry
//   the source clip's root-motion settings across the bake, and rebake when
//   the authored settings change.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Animation/Retargeting/RetargetingComponent.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Build a two-bone skeleton (root + one child) with FK-composed globals and
    // a captured bind pose — what the model loaders produce.
    Ref<Skeleton> BuildTwoBoneSkeleton(const std::string& rootName, const std::string& childName,
                                       const glm::vec3& childOffset)
    {
        auto skeleton = Ref<Skeleton>::Create(static_cast<sizet>(2));
        skeleton->m_BoneNames = { rootName, childName };
        skeleton->m_ParentIndices = { -1, 0 };
        skeleton->m_LocalTransforms[0] = glm::mat4(1.0f);
        skeleton->m_LocalTransforms[1] = glm::translate(glm::mat4(1.0f), childOffset);
        skeleton->m_GlobalTransforms[0] = skeleton->m_LocalTransforms[0];
        skeleton->m_GlobalTransforms[1] = skeleton->m_GlobalTransforms[0] * skeleton->m_LocalTransforms[1];
        skeleton->SetBindPose();
        return skeleton;
    }
} // namespace

class LiveRetargetingTest : public FunctionalTest
{
  protected:
    // Disjoint naming conventions: 3ds Max Biped source vs Mixamo-style target.
    // Name matching maps zero bones; only the humanoid-role layer bridges them.
    static constexpr const char* kSourceRoot = "Bip01 Pelvis";
    static constexpr const char* kSourceLeg = "Bip01 L Thigh";
    static constexpr const char* kTargetRoot = "Hips";
    static constexpr const char* kTargetLeg = "LeftUpLeg";

    void BuildScene() override
    {
        // Source entity: the foreign rig carrying one clip ("Walk") that yaws
        // the left thigh and drives the pelvis forward with root motion.
        m_Source = GetScene().CreateEntity("AnimationLibrary");
        auto sourceSkeleton = BuildTwoBoneSkeleton(kSourceRoot, kSourceLeg, glm::vec3(0.0f, -0.5f, 0.0f));
        m_Source.AddComponent<SkeletonComponent>(sourceSkeleton);

        auto clip = Ref<AnimationClip>::Create();
        clip->Name = "Walk";
        clip->Duration = 1.0f;
        {
            BoneAnimation pelvis;
            pelvis.BoneName = kSourceRoot;
            pelvis.PositionKeys.push_back({ 0.0, glm::vec3(0.0f) });
            pelvis.PositionKeys.push_back({ 1.0, glm::vec3(0.0f, 0.0f, 1.0f) }); // 1 unit/loop forward
            clip->BoneAnimations.push_back(std::move(pelvis));

            BoneAnimation thigh;
            thigh.BoneName = kSourceLeg;
            m_ThighPose = glm::angleAxis(glm::radians(35.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            thigh.RotationKeys.push_back({ 0.0, m_ThighPose });
            thigh.RotationKeys.push_back({ 1.0, m_ThighPose });
            clip->BoneAnimations.push_back(std::move(thigh));
        }
        clip->InitializeBoneCache();
        clip->RootMotion.ExtractRootMotion = true;
        clip->RootMotion.RootBoneIndex = 0;

        auto& sourceAnim = m_Source.AddComponent<AnimationStateComponent>();
        sourceAnim.m_AvailableClips.push_back(clip);

        // Target entity: differently-named, twice-proportioned rig with an
        // empty clip list and a live RetargetingComponent pointing at the
        // source entity.
        m_Target = GetScene().CreateEntity("Character");
        m_Target.AddComponent<SkeletonComponent>(BuildTwoBoneSkeleton(kTargetRoot, kTargetLeg, glm::vec3(0.0f, -1.0f, 0.0f)));
        m_Target.AddComponent<AnimationStateComponent>();

        auto& retarget = m_Target.AddComponent<RetargetingComponent>();
        retarget.m_SourceEntity = m_Source.GetUUID();
    }

    Entity m_Source;
    Entity m_Target;
    glm::quat m_ThighPose{ 1.0f, 0.0f, 0.0f, 0.0f };
};

TEST_F(LiveRetargetingTest, BakesRoleMappedClipsIntoTargetOnFirstTick)
{
    RunFrames(1);

    const auto& animState = m_Target.GetComponent<AnimationStateComponent>();
    ASSERT_EQ(animState.m_AvailableClips.size(), 1u)
        << "the retargeting system did not splice the baked clip into the "
           "target's available clips on the first tick.";
    const Ref<AnimationClip>& baked = animState.m_AvailableClips[0];
    ASSERT_TRUE(baked);
    EXPECT_EQ(baked->Name, "Walk") << "baked clip must keep the SOURCE clip name";
    EXPECT_TRUE(baked->FindBoneAnimation(kTargetLeg))
        << "role mapping failed — the baked clip carries no track for the "
           "target's leg bone despite disjoint naming (BuildByHumanoidRole).";

    // Root-motion settings carried across the bake, root index remapped into
    // the TARGET skeleton (still bone 0 here — both rigs order root first).
    EXPECT_TRUE(baked->RootMotion.ExtractRootMotion);
    EXPECT_EQ(baked->RootMotion.RootBoneIndex, 0u);

    // The state twin holds the bake and doesn't rebake while settings stand.
    ASSERT_TRUE(m_Target.HasComponent<RetargetingStateComponent>());
    const auto* bakedPtr = m_Target.GetComponent<RetargetingStateComponent>().BakedClips[0].get();
    RunFrames(2);
    EXPECT_EQ(m_Target.GetComponent<RetargetingStateComponent>().BakedClips[0].get(), bakedPtr)
        << "the system rebaked without any settings change.";
}

TEST_F(LiveRetargetingTest, BakedClipDrivesTargetSkeletonAndRootMotion)
{
    RunFrames(1);

    // Start playing the baked clip on the target.
    auto& animState = m_Target.GetComponent<AnimationStateComponent>();
    ASSERT_FALSE(animState.m_AvailableClips.empty());
    animState.m_CurrentClip = animState.m_AvailableClips[0];
    animState.m_IsPlaying = true;

    const glm::vec3 startPos = m_Target.GetComponent<TransformComponent>().Translation;
    RunFrames(30); // 0.5 s at the default 1/60 step

    // The thigh rotation carried across (rest poses are identity on both rigs,
    // so the re-base degenerates to a copy).
    const auto& skeleton = m_Target.GetComponent<SkeletonComponent>().m_Skeleton;
    const BoneTransform leg = Animation::BlendUtils::DecomposeMatrix(skeleton->m_LocalTransforms[1]);
    const f32 dot = std::abs(glm::dot(leg.Rotation, m_ThighPose));
    EXPECT_GT(dot, 0.999f) << "retargeted thigh rotation did not reach the target skeleton";

    // Root motion flowed through the FULL pipeline: retarget bake → extraction
    // → RootMotionApply moved the entity forward. The 2x-proportioned target
    // gets the extent-derived translation scale, so ~2 units/s; 0.5 s ≈ 1.0.
    const glm::vec3 moved = m_Target.GetComponent<TransformComponent>().Translation - startPos;
    EXPECT_GT(moved.z, 0.5f) << "root motion did not move the retargeted character";
    EXPECT_LT(moved.z, 2.0f) << "root motion moved the character implausibly far";

    // And the pose stays pinned in place — the mesh must not double-move.
    const BoneTransform root = Animation::BlendUtils::DecomposeMatrix(skeleton->m_LocalTransforms[0]);
    EXPECT_LT(std::abs(root.Translation.z), 0.05f)
        << "baked root track was not pinned — the pose carries the motion too";
}

TEST_F(LiveRetargetingTest, SettingsChangeTriggersRebake)
{
    RunFrames(1);
    ASSERT_TRUE(m_Target.HasComponent<RetargetingStateComponent>());
    const auto* before = m_Target.GetComponent<RetargetingStateComponent>().BakedClips[0].get();

    // Flip a bake-relevant setting; the next tick must produce fresh clips.
    m_Target.GetComponent<RetargetingComponent>().PerBoneTranslation = false;
    RunFrames(1);
    const auto* after = m_Target.GetComponent<RetargetingStateComponent>().BakedClips[0].get();
    EXPECT_NE(before, after) << "settings change did not trigger a rebake";
}
