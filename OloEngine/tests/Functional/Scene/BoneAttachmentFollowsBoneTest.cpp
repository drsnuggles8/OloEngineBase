// OLO_TEST_LAYER: Functional
#include "OloEnginePCH.h"

// =============================================================================
// BoneAttachmentFollowsBoneTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Animation (the skeleton pose) × Scene::PropagateWorldTransforms (issue
//   #1533). A BoneAttachmentComponent child of a skinned character composes
//   world = parentWorld * boneGlobal * local, so a separate eye entity can ride
//   the head bone of a character whose bones are not scene entities.
//
// The contract has a TIMING half, and it is the part a unit test of the compose
// rule could not see: the child must follow THIS frame's pose.
//   - Runtime: the scheduler orders PropagateTransforms after every writer of
//     the SkeletonPose channel. Composing before the animation systems ran
//     would leave the eye one pose behind its head, which only shows as a
//     slight lag on screen.
//   - Edit mode: OnUpdateEditor composes world matrices BEFORE its preview
//     loop poses the skeletons, then re-composes the attached subtrees.
// Both are asserted against the skeleton's live m_GlobalTransforms after the
// frame, so a stale pose fails the equality rather than passing a tolerance.
//
// Fallbacks (no parent, a parent without a skeleton, an unknown bone, disabled)
// compose parent-relative and are REPORTED: the status Scene::ResolveBoneAttachment
// returns, and one warning per (entity, reason) in the log.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "PropertyTests/ScopedWarningCapture.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SystemScheduler.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr f32 kDt = 1.0f / 60.0f;

    // Composition goes through glm float products in a different association
    // than the expected value below, so compare with a tolerance scaled to the
    // matrix's own magnitude rather than bit-exactly.
    ::testing::AssertionResult MatrixNear(const glm::mat4& actual, const glm::mat4& expected, f32 eps = 1e-4f)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                const f32 a = actual[c][r];
                const f32 e = expected[c][r];
                if (!std::isfinite(a) || std::abs(a - e) > eps * std::max(1.0f, std::abs(e)))
                {
                    return ::testing::AssertionFailure()
                           << "element [" << c << "][" << r << "] is " << a << ", expected " << e;
                }
            }
        }
        return ::testing::AssertionSuccess();
    }

    // Root + Head. Head carries a bind offset so its global transform is never
    // the identity, which would make "composed against the bone" and "composed
    // parent-relative" indistinguishable.
    Ref<Skeleton> MakeHeadSkeleton(const char* headName = "Head")
    {
        auto skeleton = Ref<Skeleton>::Create(2);
        skeleton->m_BoneNames = { "Root", headName };
        skeleton->m_ParentIndices = { -1, 0 };
        skeleton->m_LocalTransforms = { glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)) };
        skeleton->m_BonePreTransforms = { glm::mat4(1.0f), glm::mat4(1.0f) };
        skeleton->m_GlobalTransforms[0] = skeleton->m_LocalTransforms[0];
        skeleton->m_GlobalTransforms[1] = skeleton->m_GlobalTransforms[0] * skeleton->m_LocalTransforms[1];
        skeleton->SetBindPose();
        return skeleton;
    }

    // The head nods about +Y AND rises over one second, so the bone's global
    // transform changes in both rotation and translation every frame: a compose
    // in the wrong order, or against last frame's pose, cannot coincide with it.
    Ref<AnimationClip> MakeHeadTurnClip(const char* headName = "Head")
    {
        auto clip = Ref<AnimationClip>::Create();
        clip->Name = "BoneAttachment_HeadTurn";
        clip->Duration = 1.0f;

        BoneAnimation head;
        head.BoneName = headName;
        head.PositionKeys.push_back({ 0.0, glm::vec3(0.0f, 1.0f, 0.0f) });
        head.PositionKeys.push_back({ 1.0, glm::vec3(0.0f, 1.5f, 0.25f) });
        head.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        head.RotationKeys.push_back({ 1.0, glm::angleAxis(glm::radians(80.0f), glm::vec3(0.0f, 1.0f, 0.0f)) });
        head.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
        head.ScaleKeys.push_back({ 1.0, glm::vec3(1.0f) });
        clip->BoneAnimations.push_back(std::move(head));
        clip->InitializeBoneCache();
        return clip;
    }
} // namespace

class BoneAttachmentFollowsBoneTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Skeleton = MakeHeadSkeleton();

        m_Character = GetScene().CreateEntity("Character");
        auto& characterTransform = m_Character.GetComponent<TransformComponent>();
        characterTransform.Translation = { 3.0f, 0.5f, -2.0f };
        characterTransform.SetRotationEuler({ 0.0f, 0.6f, 0.0f });
        characterTransform.Scale = { 1.5f, 1.5f, 1.5f };
        m_Character.AddComponent<SkeletonComponent>(m_Skeleton);
        auto& anim = m_Character.AddComponent<AnimationStateComponent>();
        anim.m_CurrentClip = MakeHeadTurnClip();
        anim.m_IsPlaying = true;

        m_Eye = GetScene().CreateEntity("Eye");
        m_Eye.SetParent(m_Character);
        m_Eye.GetComponent<TransformComponent>().Translation = { 0.1f, 0.05f, 0.2f };
        m_Eye.GetComponent<TransformComponent>().SetRotationEuler({ 0.2f, 0.0f, 0.0f });
        m_Eye.AddComponent<BoneAttachmentComponent>("Head");

        // A plain descendant of the attached entity: composes on top of it.
        m_Pupil = GetScene().CreateEntity("Pupil");
        m_Pupil.SetParent(m_Eye);
        m_Pupil.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.03f };
    }

    [[nodiscard]] glm::mat4 World(Entity entity) const
    {
        return GetScene().GetWorldTransform(static_cast<entt::entity>(entity));
    }

    [[nodiscard]] static glm::mat4 Local(Entity entity)
    {
        return entity.GetComponent<TransformComponent>().GetTransform();
    }

    // What the eye MUST be after a frame: the character's world, times the head
    // bone's pose as the skeleton holds it NOW, times the eye's own offset.
    [[nodiscard]] glm::mat4 ExpectedEyeWorld() const
    {
        return World(m_Character) * m_Skeleton->m_GlobalTransforms[1] * Local(m_Eye);
    }

    void ExpectAttachedToHead(const char* when) const
    {
        SCOPED_TRACE(when);
        EXPECT_TRUE(MatrixNear(World(m_Eye), ExpectedEyeWorld()))
            << "the attached eye did not compose against the head bone's CURRENT pose";
        EXPECT_TRUE(MatrixNear(World(m_Pupil), World(m_Eye) * Local(m_Pupil)))
            << "a descendant of the attached entity did not compose on top of it";
    }

    Ref<Skeleton> m_Skeleton;
    Entity m_Character;
    Entity m_Eye;
    Entity m_Pupil;
};

TEST_F(BoneAttachmentFollowsBoneTest, RuntimeTickComposesTheChildAgainstThisTicksBonePose)
{
    const auto resolution = GetScene().ResolveBoneAttachment(static_cast<entt::entity>(m_Eye));
    ASSERT_TRUE(resolution.has_value());
    EXPECT_EQ(resolution->Status, BoneAttachmentStatus::Attached);
    EXPECT_EQ(resolution->BoneIndex, 1);

    glm::vec3 previousEye(std::numeric_limits<f32>::quiet_NaN());
    for (u32 frame = 0; frame < 12; ++frame)
    {
        RunFrames(1, kDt);
        ExpectAttachedToHead(("runtime frame " + std::to_string(frame)).c_str());

        // The prediction must not be a coincidence of a static pose: the bone
        // moves every frame, so the eye's world position does too.
        const glm::vec3 eye(World(m_Eye)[3]);
        if (frame > 0)
        {
            EXPECT_GT(glm::length(eye - previousEye), 1e-4f)
                << "the head bone did not move between frames " << frame - 1 << " and " << frame
                << " — the equality above would then hold for a stale pose too";
        }
        previousEye = eye;
    }

    // And it is not the parent-relative composition the fallback would produce.
    EXPECT_FALSE(MatrixNear(World(m_Eye), World(m_Character) * Local(m_Eye), 1e-3f))
        << "the attached eye composed exactly as an ordinary child would — the bone was ignored";
}

TEST_F(BoneAttachmentFollowsBoneTest, EditModePreviewRecomposesTheChildAfterPosing)
{
    // OnUpdateEditor's full PropagateWorldTransforms runs BEFORE its animation
    // preview loop writes the pose; without the targeted recompose after the
    // loop the eye would trail its head by one pose, frame after frame.
    EditorCamera camera;
    glm::vec3 previousEye(std::numeric_limits<f32>::quiet_NaN());
    for (u32 frame = 0; frame < 8; ++frame)
    {
        GetScene().OnUpdateEditor(Timestep(kDt), camera);
        ExpectAttachedToHead(("edit-mode frame " + std::to_string(frame)).c_str());

        const glm::vec3 eye(World(m_Eye)[3]);
        if (frame > 0)
        {
            EXPECT_GT(glm::length(eye - previousEye), 1e-4f)
                << "the editor preview did not advance the head bone between frames " << frame - 1 << " and "
                << frame;
        }
        previousEye = eye;
    }
}

TEST_F(BoneAttachmentFollowsBoneTest, SwappingTheParentSkeletonReResolvesTheBone)
{
    RunFrames(2, kDt);
    ExpectAttachedToHead("before the swap");

    // A skeleton whose "Head" sits at a DIFFERENT index: a cached index keyed
    // only on the name would now read the wrong bone.
    auto swapped = Ref<Skeleton>::Create(3);
    swapped->m_BoneNames = { "Root", "Neck", "Head" };
    swapped->m_ParentIndices = { -1, 0, 1 };
    swapped->m_LocalTransforms = { glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f)),
                                   glm::translate(glm::mat4(1.0f), glm::vec3(0.3f, 0.4f, 0.0f)) };
    swapped->m_BonePreTransforms = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
    swapped->m_GlobalTransforms[0] = swapped->m_LocalTransforms[0];
    swapped->m_GlobalTransforms[1] = swapped->m_GlobalTransforms[0] * swapped->m_LocalTransforms[1];
    swapped->m_GlobalTransforms[2] = swapped->m_GlobalTransforms[1] * swapped->m_LocalTransforms[2];
    swapped->SetBindPose();
    m_Character.GetComponent<SkeletonComponent>().SetSkeleton(swapped);
    m_Character.GetComponent<AnimationStateComponent>().m_IsPlaying = false;

    RunFrames(1, kDt);
    EXPECT_TRUE(MatrixNear(World(m_Eye), World(m_Character) * swapped->m_GlobalTransforms[2] * Local(m_Eye)))
        << "after the skeleton swap the eye did not follow the NEW skeleton's Head bone";
    const auto resolution = GetScene().ResolveBoneAttachment(static_cast<entt::entity>(m_Eye));
    ASSERT_TRUE(resolution.has_value());
    EXPECT_EQ(resolution->BoneIndex, 2);
}

TEST_F(BoneAttachmentFollowsBoneTest, UnknownBoneFallsBackToParentRelativeAndIsReportedOnce)
{
    const Tests::ScopedWarningCapture warnings;
    m_Eye.GetComponent<BoneAttachmentComponent>().m_BoneName = "NoSuchBone";

    RunFrames(5, kDt);

    EXPECT_TRUE(MatrixNear(World(m_Eye), World(m_Character) * Local(m_Eye)))
        << "an unresolvable attachment must compose exactly like an ordinary child";
    EXPECT_TRUE(MatrixNear(World(m_Pupil), World(m_Eye) * Local(m_Pupil)));

    const auto resolution = GetScene().ResolveBoneAttachment(static_cast<entt::entity>(m_Eye));
    ASSERT_TRUE(resolution.has_value());
    EXPECT_EQ(resolution->Status, BoneAttachmentStatus::UnknownBone);
    EXPECT_EQ(resolution->BoneIndex, -1);

    // Reported, once: five ticks, one warning naming the bone and the reason.
    EXPECT_EQ(warnings.Count("cannot follow bone 'NoSuchBone'"), 1u)
        << "the fallback must be reported exactly once per (entity, reason), not silently and not every frame";
    EXPECT_EQ(warnings.Count("UnknownBone"), 1u);
}

TEST_F(BoneAttachmentFollowsBoneTest, ParentWithoutSkeletonFallsBackAndIsReported)
{
    const Tests::ScopedWarningCapture warnings;
    m_Character.RemoveComponent<AnimationStateComponent>();
    m_Character.RemoveComponent<SkeletonComponent>();

    RunFrames(3, kDt);

    EXPECT_TRUE(MatrixNear(World(m_Eye), World(m_Character) * Local(m_Eye)));
    const auto resolution = GetScene().ResolveBoneAttachment(static_cast<entt::entity>(m_Eye));
    ASSERT_TRUE(resolution.has_value());
    EXPECT_EQ(resolution->Status, BoneAttachmentStatus::ParentHasNoSkeleton);
    EXPECT_EQ(warnings.Count("ParentHasNoSkeleton"), 1u);
}

TEST_F(BoneAttachmentFollowsBoneTest, UnparentedAttachmentComposesAsItsLocalTransformAndIsReported)
{
    const Tests::ScopedWarningCapture warnings;
    m_Eye.SetParent(Entity{});

    RunFrames(2, kDt);

    EXPECT_TRUE(MatrixNear(World(m_Eye), Local(m_Eye)));
    EXPECT_TRUE(MatrixNear(World(m_Pupil), World(m_Eye) * Local(m_Pupil)));
    const auto resolution = GetScene().ResolveBoneAttachment(static_cast<entt::entity>(m_Eye));
    ASSERT_TRUE(resolution.has_value());
    EXPECT_EQ(resolution->Status, BoneAttachmentStatus::NoParent);
    EXPECT_EQ(warnings.Count("NoParent"), 1u);
}

TEST_F(BoneAttachmentFollowsBoneTest, DisabledAttachmentComposesParentRelative)
{
    m_Eye.GetComponent<BoneAttachmentComponent>().m_Enabled = false;

    RunFrames(2, kDt);

    EXPECT_TRUE(MatrixNear(World(m_Eye), World(m_Character) * Local(m_Eye)));
    const auto resolution = GetScene().ResolveBoneAttachment(static_cast<entt::entity>(m_Eye));
    ASSERT_TRUE(resolution.has_value());
    EXPECT_EQ(resolution->Status, BoneAttachmentStatus::Disabled);

    // Re-enabling takes effect on the next frame, with no re-authoring.
    m_Eye.GetComponent<BoneAttachmentComponent>().m_Enabled = true;
    RunFrames(1, kDt);
    ExpectAttachedToHead("after re-enabling");
}

// The ordering half of the contract, pinned on the graph itself. DependsOn
// alone would pass without the SkeletonPose channel — PropagateTransforms was
// already reachable from Animation through RootMotionApply — so this asserts
// the edges the channel adds (each pose writer directly before
// PropagateTransforms, or before the pose writer that is), and that every
// pose writer declares it.
TEST(BoneAttachmentScheduling, PropagateTransformsReadsThePoseEveryAnimationSystemWrites)
{
    const SystemScheduler::GraphSnapshot graph = Scene::GetGameplayScheduler().ExportGraph();

    const auto findNode = [&graph](std::string_view name) -> const SystemScheduler::GraphNode*
    {
        for (const auto& node : graph.Nodes)
        {
            if (node.Name == name)
                return &node;
        }
        return nullptr;
    };
    const auto declares = [](const std::vector<std::string>& channels, std::string_view channel)
    {
        return std::ranges::find(channels, channel) != channels.end();
    };
    const auto hasEdge = [&graph](std::string_view from, std::string_view to)
    {
        return std::ranges::any_of(graph.Edges, [&](const SystemScheduler::GraphEdge& edge)
                                   { return edge.From == from && edge.To == to; });
    };

    const auto* propagate = findNode("PropagateTransforms");
    ASSERT_NE(propagate, nullptr);
    EXPECT_TRUE(declares(propagate->Reads, "SkeletonPose"));

    // The scheduler orders the writers of one channel among themselves and
    // links a reader to the LAST of them, so the channel's edges are
    // Animation -> AnimationGraph -> PropagateTransforms: each writer reaches
    // the reader directly or through the other pose writer, never through an
    // unrelated system.
    constexpr std::array<const char*, 2> kPoseWriters{ "Animation", "AnimationGraph" };
    for (const char* writer : kPoseWriters)
    {
        SCOPED_TRACE(writer);
        const auto* node = findNode(writer);
        ASSERT_NE(node, nullptr);
        EXPECT_TRUE(declares(node->Writes, "SkeletonPose"))
            << "a system that writes the skeleton pose must declare it, or PropagateTransforms can run before it";
        const bool viaOtherWriter = std::ranges::any_of(kPoseWriters, [&](const char* other)
                                                        { return std::string_view(other) != writer && hasEdge(writer, other) &&
                                                                 hasEdge(other, "PropagateTransforms"); });
        EXPECT_TRUE(hasEdge(writer, "PropagateTransforms") || viaOtherWriter)
            << "the SkeletonPose channel must order this writer before PropagateTransforms";
    }

    // The cloth attachment drive inside the physics kick reads the pose too.
    const auto* kick = findNode("PhysicsKick");
    ASSERT_NE(kick, nullptr);
    EXPECT_TRUE(declares(kick->Reads, "SkeletonPose"));
}
