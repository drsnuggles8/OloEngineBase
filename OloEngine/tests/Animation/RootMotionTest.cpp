// OLO_TEST_LAYER: unit
// =============================================================================
// RootMotionTest — CPU contract tests for the root-motion consumption pipeline
// (issue #631, part 1). Pins the extraction math the runtime relies on:
//   * per-clip wrap-aware delta extraction (single tick, loop wrap, multi-loop,
//     non-looping clamp, rotation accumulation),
//   * translation / rotation masking and the in-place pose pinning that removes
//     the extracted motion from the sampled pose,
//   * model-space conversion via bind-pose ancestors / pre-transforms,
//   * blend behavior across the legacy two-clip cross-fade, state-machine
//     transitions, and 1D/2D blend trees (weights must MATCH the pose blend),
//   * the AnimationSystem / AnimationStateMachine / AnimationGraph plumbing
//     that publishes the per-tick delta.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Animation/AnimationStateMachine.h"
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Animation/AnimationTransition.h"
#include "OloEngine/Animation/BlendTree.h"
#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Animation/RootMotion.h"
#include "OloEngine/Animation/Skeleton.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace OloEngine::Animation
{
    namespace
    {
        constexpr f32 kEps = 1e-4f;

        void ExpectVec3Near(const glm::vec3& actual, const glm::vec3& expected, f32 eps = kEps)
        {
            EXPECT_NEAR(actual.x, expected.x, eps);
            EXPECT_NEAR(actual.y, expected.y, eps);
            EXPECT_NEAR(actual.z, expected.z, eps);
        }

        // Quat comparison aware of the q / -q double cover.
        void ExpectQuatNear(const glm::quat& actual, const glm::quat& expected, f32 eps = kEps)
        {
            const f32 dot = std::abs(glm::dot(actual, expected));
            EXPECT_NEAR(dot, 1.0f, eps);
        }

        // A clip whose root bone translates linearly at `velocity` units/sec over
        // `duration`, with constant identity rotation. Sampling is exactly linear
        // between the two keys.
        Ref<AnimationClip> MakeLinearClip(const glm::vec3& velocity, f32 duration = 1.0f,
                                          const std::string& rootBone = "Root", const std::string& name = "Linear")
        {
            auto clip = Ref<AnimationClip>::Create();
            clip->Name = name;
            clip->Duration = duration;
            BoneAnimation root;
            root.BoneName = rootBone;
            root.PositionKeys.push_back({ 0.0, glm::vec3(0.0f) });
            root.PositionKeys.push_back({ static_cast<f64>(duration), velocity * duration });
            root.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
            root.RotationKeys.push_back({ static_cast<f64>(duration), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
            clip->BoneAnimations.push_back(std::move(root));
            clip->InitializeBoneCache();
            return clip;
        }

        // A clip whose root bone yaws `totalYawDegrees` about +Y over one loop,
        // no translation.
        Ref<AnimationClip> MakeTurningClip(f32 totalYawDegrees, f32 duration = 1.0f,
                                           const std::string& rootBone = "Root", const std::string& name = "Turn")
        {
            auto clip = Ref<AnimationClip>::Create();
            clip->Name = name;
            clip->Duration = duration;
            BoneAnimation root;
            root.BoneName = rootBone;
            root.PositionKeys.push_back({ 0.0, glm::vec3(0.0f) });
            root.PositionKeys.push_back({ static_cast<f64>(duration), glm::vec3(0.0f) });
            const glm::quat half = glm::angleAxis(glm::radians(totalYawDegrees * 0.5f), glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::quat full = glm::angleAxis(glm::radians(totalYawDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
            // Three keys so slerp sampling stays on the short arc even for
            // totalYawDegrees close to 360.
            root.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
            root.RotationKeys.push_back({ static_cast<f64>(duration) * 0.5, half });
            root.RotationKeys.push_back({ static_cast<f64>(duration), full });
            clip->BoneAnimations.push_back(std::move(root));
            clip->InitializeBoneCache();
            return clip;
        }

        // Non-const Ref: the engine's Ref<T> propagates constness to operator->.
        void EnableExtraction(Ref<AnimationClip> clip, u32 rootBoneIndex = 0) // NOLINT(performance-unnecessary-value-param)
        {
            clip->RootMotion.ExtractRootMotion = true;
            clip->RootMotion.RootBoneIndex = rootBoneIndex;
        }

        // Single-root-bone skeleton named "Root" with identity bind pose.
        Ref<Skeleton> MakeRootSkeleton()
        {
            auto skeleton = Ref<Skeleton>::Create(static_cast<sizet>(1));
            skeleton->m_BoneNames[0] = "Root";
            skeleton->m_ParentIndices[0] = -1;
            skeleton->m_GlobalTransforms[0] = glm::mat4(1.0f);
            skeleton->SetBindPose();
            return skeleton;
        }

        const std::vector<std::string> s_RootNames = { "Root" };
        const PoseEvalContext s_RootCtx{ s_RootNames, {} };

        f32 YawDegrees(const glm::quat& q)
        {
            // Rotate +Z and measure the heading angle in the XZ plane.
            const glm::vec3 forward = q * glm::vec3(0.0f, 0.0f, 1.0f);
            return glm::degrees(std::atan2(forward.x, forward.z));
        }
    } // namespace

    // ── Low-level extraction ────────────────────────────────────────────────

    TEST(RootMotionExtract, LinearDeltaWithinClip)
    {
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        const RootMotionDelta delta = RootMotionUtils::ExtractClipRootDelta(*clip, "Root", 0.2f, 0.3f, true);
        EXPECT_TRUE(delta.HasMotion);
        ExpectVec3Near(delta.Translation, glm::vec3(0.0f, 0.0f, 0.3f));
        ExpectQuatNear(delta.Rotation, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    }

    TEST(RootMotionExtract, DeltaAccumulatesAcrossLoopWrap)
    {
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        // t0 = 0.9, dt = 0.2 wraps: naive S(0.1) - S(0.9) would be -0.8 — the
        // wrap jump must NOT count as motion.
        const RootMotionDelta delta = RootMotionUtils::ExtractClipRootDelta(*clip, "Root", 0.9f, 0.2f, true);
        ExpectVec3Near(delta.Translation, glm::vec3(0.0f, 0.0f, 0.2f));
    }

    TEST(RootMotionExtract, DeltaAccumulatesMultipleWholeLoops)
    {
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        // 0.5 tail + 1 full loop + 0.5 head = 2.5 units.
        const RootMotionDelta delta = RootMotionUtils::ExtractClipRootDelta(*clip, "Root", 0.5f, 2.5f, true);
        ExpectVec3Near(delta.Translation, glm::vec3(0.0f, 0.0f, 2.5f), 1e-3f);
    }

    TEST(RootMotionExtract, NonLoopingClampsAtClipEnd)
    {
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        const RootMotionDelta delta = RootMotionUtils::ExtractClipRootDelta(*clip, "Root", 0.8f, 0.5f, false);
        // Only 0.2s of motion remain before the last key clamps.
        ExpectVec3Near(delta.Translation, glm::vec3(0.0f, 0.0f, 0.2f));
    }

    TEST(RootMotionExtract, RotationDeltaAccumulatesAcrossWrap)
    {
        auto clip = MakeTurningClip(90.0f);
        // Tail 0.75→1.0 turns 22.5°, head 0→0.25 turns 22.5° — total 45°.
        const RootMotionDelta delta = RootMotionUtils::ExtractClipRootDelta(*clip, "Root", 0.75f, 0.5f, true);
        EXPECT_NEAR(YawDegrees(delta.Rotation), 45.0f, 0.1f);
    }

    TEST(RootMotionExtract, MissingRootTrackYieldsNoMotion)
    {
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        const RootMotionDelta delta = RootMotionUtils::ExtractClipRootDelta(*clip, "Hips", 0.0f, 0.5f, true);
        EXPECT_FALSE(delta.HasMotion);
        ExpectVec3Near(delta.Translation, glm::vec3(0.0f));
    }

    TEST(RootMotionExtract, ZeroOrNegativeDeltaTimeYieldsNoMotion)
    {
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        EXPECT_FALSE(RootMotionUtils::ExtractClipRootDelta(*clip, "Root", 0.5f, 0.0f, true).HasMotion);
        EXPECT_FALSE(RootMotionUtils::ExtractClipRootDelta(*clip, "Root", 0.5f, -0.1f, true).HasMotion);
    }

    // ── Masks ───────────────────────────────────────────────────────────────

    TEST(RootMotionMask, TranslationMaskSelectsChannels)
    {
        RootMotionDelta delta;
        delta.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
        delta.HasMotion = true;
        const RootMotionDelta masked = RootMotionUtils::ApplyMasks(delta, glm::vec3(1.0f, 0.0f, 1.0f), glm::vec3(1.0f));
        ExpectVec3Near(masked.Translation, glm::vec3(1.0f, 0.0f, 3.0f));
    }

    TEST(RootMotionMask, ZeroRotationMaskYieldsIdentity)
    {
        RootMotionDelta delta;
        delta.Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        delta.HasMotion = true;
        const RootMotionDelta masked = RootMotionUtils::ApplyMasks(delta, glm::vec3(1.0f), glm::vec3(0.0f));
        ExpectQuatNear(masked.Rotation, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    }

    TEST(RootMotionMask, YawOnlyRotationMaskKeepsYaw)
    {
        RootMotionDelta delta;
        delta.Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        delta.HasMotion = true;
        // Mask selects only the Y euler component — a pure yaw passes through.
        const RootMotionDelta masked = RootMotionUtils::ApplyMasks(delta, glm::vec3(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        EXPECT_NEAR(YawDegrees(masked.Rotation), 30.0f, 0.1f);
    }

    // ── Blend ───────────────────────────────────────────────────────────────

    TEST(RootMotionBlend, MixesTranslationAndRotation)
    {
        RootMotionDelta a;
        a.Translation = glm::vec3(0.0f, 0.0f, 1.0f);
        a.HasMotion = true;
        RootMotionDelta b;
        b.Translation = glm::vec3(1.0f, 0.0f, 0.0f);
        b.Rotation = glm::angleAxis(glm::radians(40.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        b.HasMotion = true;

        const RootMotionDelta mid = RootMotionUtils::Blend(a, b, 0.5f);
        ExpectVec3Near(mid.Translation, glm::vec3(0.5f, 0.0f, 0.5f));
        EXPECT_NEAR(YawDegrees(mid.Rotation), 20.0f, 0.1f);
    }

    TEST(RootMotionBlend, NonContributingSideBlendsAsZeroMotion)
    {
        RootMotionDelta a;
        a.Translation = glm::vec3(0.0f, 0.0f, 1.0f);
        a.HasMotion = true;
        const RootMotionDelta mid = RootMotionUtils::Blend(a, {}, 0.5f);
        EXPECT_TRUE(mid.HasMotion);
        ExpectVec3Near(mid.Translation, glm::vec3(0.0f, 0.0f, 0.5f));
    }

    // ── Model-space conversion ──────────────────────────────────────────────

    TEST(RootMotionSpace, PreTransformConvertsDeltaToModelSpace)
    {
        // A -90° X pre-rotation on the root (the fox.gltf pattern).
        const glm::mat4 pre = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        const std::vector<int> parents = { -1 };
        const std::vector<glm::mat4> bindGlobals = { pre };
        const std::vector<glm::mat4> preTransforms = { pre };

        RootMotionDelta local;
        local.Translation = glm::vec3(0.0f, 0.0f, 1.0f);
        local.Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        local.HasMotion = true;

        const RootMotionDelta model = RootMotionUtils::ToModelSpace(local, 0, parents, bindGlobals, preTransforms);

        const glm::mat3 linear(pre);
        ExpectVec3Near(model.Translation, linear * local.Translation);
        const glm::quat frame = glm::quat_cast(linear);
        ExpectQuatNear(model.Rotation, frame * local.Rotation * glm::inverse(frame));
    }

    TEST(RootMotionSpace, EmptySkeletonSpansDegradeToIdentity)
    {
        RootMotionDelta local;
        local.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
        local.HasMotion = true;
        const RootMotionDelta model = RootMotionUtils::ToModelSpace(local, 0, {}, {}, {});
        ExpectVec3Near(model.Translation, local.Translation);
    }

    // ── In-place pose pinning ───────────────────────────────────────────────

    TEST(RootMotionPin, FullMaskPinsToReference)
    {
        const BoneTransform sampled{ glm::vec3(0.0f, 1.0f, 5.0f),
                                     glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                                     glm::vec3(1.0f) };
        const BoneTransform reference{ glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f) };
        const BoneTransform pinned = RootMotionUtils::MakeInPlaceRootPose(sampled, reference, glm::vec3(1.0f), glm::vec3(1.0f));
        ExpectVec3Near(pinned.Translation, reference.Translation);
        ExpectQuatNear(pinned.Rotation, reference.Rotation);
    }

    TEST(RootMotionPin, ZeroMaskKeepsSampledPose)
    {
        const BoneTransform sampled{ glm::vec3(0.0f, 1.0f, 5.0f),
                                     glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                                     glm::vec3(1.0f) };
        const BoneTransform reference{ glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f) };
        const BoneTransform pinned = RootMotionUtils::MakeInPlaceRootPose(sampled, reference, glm::vec3(0.0f), glm::vec3(0.0f));
        ExpectVec3Near(pinned.Translation, sampled.Translation);
        ExpectQuatNear(pinned.Rotation, sampled.Rotation);
    }

    TEST(RootMotionPin, PartialTranslationMaskPinsOnlyMaskedChannels)
    {
        const BoneTransform sampled{ glm::vec3(2.0f, 1.5f, 5.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f) };
        const BoneTransform reference{ glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f) };
        // Extract XZ (pinned to reference), keep Y (hip bob stays in the pose).
        const BoneTransform pinned = RootMotionUtils::MakeInPlaceRootPose(sampled, reference, glm::vec3(1.0f, 0.0f, 1.0f), glm::vec3(1.0f));
        ExpectVec3Near(pinned.Translation, glm::vec3(0.0f, 1.5f, 0.0f));
    }

    // ── Configured extraction ───────────────────────────────────────────────

    TEST(RootMotionConfigured, RequiresExtractFlag)
    {
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        // Extract flag left off — settings round-trip but must not move anything.
        const RootMotionDelta delta = RootMotionUtils::ExtractConfiguredDelta(*clip, 0.0f, 0.5f, true, s_RootCtx);
        EXPECT_FALSE(delta.HasMotion);
    }

    TEST(RootMotionConfigured, DiscardYieldsNoMotion)
    {
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        EnableExtraction(clip);
        clip->RootMotion.DiscardRootMotion = true;
        const RootMotionDelta delta = RootMotionUtils::ExtractConfiguredDelta(*clip, 0.0f, 0.5f, true, s_RootCtx);
        EXPECT_FALSE(delta.HasMotion);
    }

    TEST(RootMotionConfigured, AppliesConfiguredMasks)
    {
        auto clip = MakeLinearClip(glm::vec3(1.0f, 1.0f, 1.0f));
        EnableExtraction(clip);
        clip->RootMotion.RootTranslationMask = glm::vec3(1.0f, 0.0f, 1.0f);
        const RootMotionDelta delta = RootMotionUtils::ExtractConfiguredDelta(*clip, 0.0f, 0.5f, true, s_RootCtx);
        EXPECT_TRUE(delta.HasMotion);
        ExpectVec3Near(delta.Translation, glm::vec3(0.5f, 0.0f, 0.5f));
    }

    TEST(RootMotionConfigured, OutOfRangeRootBoneIndexIsSafe)
    {
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        EnableExtraction(clip, 7); // only one bone name in the context
        const RootMotionDelta delta = RootMotionUtils::ExtractConfiguredDelta(*clip, 0.0f, 0.5f, true, s_RootCtx);
        EXPECT_FALSE(delta.HasMotion);
    }

    // ── AnimationSystem (legacy two-clip path) ──────────────────────────────

    TEST(RootMotionAnimationSystem, UpdateExtractsDeltaAndPinsRootPose)
    {
        auto skeleton = MakeRootSkeleton();
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        EnableExtraction(clip);

        AnimationStateComponent animState(clip);
        animState.m_IsPlaying = true;

        AnimationSystem::Update(animState, *skeleton, 0.25f);

        EXPECT_TRUE(animState.m_HasRootMotion);
        ExpectVec3Near(animState.m_RootMotionTranslation, glm::vec3(0.0f, 0.0f, 0.25f));

        // The pose must be pinned in place: the root's sampled translation at
        // t=0.25 would be (0,0,0.25) without extraction.
        const BoneTransform rootPose = BlendUtils::DecomposeMatrix(skeleton->m_LocalTransforms[0]);
        ExpectVec3Near(rootPose.Translation, glm::vec3(0.0f));
    }

    TEST(RootMotionAnimationSystem, ExtractionOffKeepsLegacyMovingPose)
    {
        auto skeleton = MakeRootSkeleton();
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f)); // extraction NOT enabled

        AnimationStateComponent animState(clip);
        animState.m_IsPlaying = true;

        AnimationSystem::Update(animState, *skeleton, 0.25f);

        EXPECT_FALSE(animState.m_HasRootMotion);
        const BoneTransform rootPose = BlendUtils::DecomposeMatrix(skeleton->m_LocalTransforms[0]);
        ExpectVec3Near(rootPose.Translation, glm::vec3(0.0f, 0.0f, 0.25f));
    }

    TEST(RootMotionAnimationSystem, PerTickDeltasSumAcrossLoopWraps)
    {
        auto skeleton = MakeRootSkeleton();
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        EnableExtraction(clip);

        AnimationStateComponent animState(clip);
        animState.m_IsPlaying = true;

        glm::vec3 total(0.0f);
        for (int i = 0; i < 5; ++i)
        {
            AnimationSystem::Update(animState, *skeleton, 0.4f);
            total += animState.m_RootMotionTranslation;
        }
        // 5 × 0.4s at 1 unit/s crosses the 1s loop twice; motion must not reset.
        ExpectVec3Near(total, glm::vec3(0.0f, 0.0f, 2.0f), 1e-3f);
    }

    TEST(RootMotionAnimationSystem, CrossFadeBlendsBothClipDeltas)
    {
        auto skeleton = MakeRootSkeleton();
        auto current = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f), 1.0f, "Root", "Walk");
        auto next = MakeLinearClip(glm::vec3(2.0f, 0.0f, 0.0f), 1.0f, "Root", "Strafe");
        EnableExtraction(current);
        EnableExtraction(next);

        AnimationStateComponent animState(current);
        animState.m_IsPlaying = true;
        animState.m_NextClip = next;
        animState.m_Blending = true;
        animState.m_BlendDuration = 1.0f;
        animState.m_BlendTime = 0.25f; // post-advance factor = 0.5

        AnimationSystem::Update(animState, *skeleton, 0.25f);

        EXPECT_TRUE(animState.m_HasRootMotion);
        // current contributes (0,0,0.25), next (0.5,0,0); factor 0.5 → mix.
        ExpectVec3Near(animState.m_RootMotionTranslation, glm::vec3(0.25f, 0.0f, 0.125f));
    }

    // ── State machine ───────────────────────────────────────────────────────

    namespace
    {
        AnimationState MakeSingleClipState(const std::string& name, const Ref<AnimationClip>& clip)
        {
            AnimationState state;
            state.Name = name;
            state.Type = AnimationState::MotionType::SingleClip;
            state.Clip = clip;
            state.Looping = true;
            return state;
        }
    } // namespace

    TEST(RootMotionStateMachine, SingleClipStateExtractsPerTickDelta)
    {
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        EnableExtraction(clip);

        auto sm = Ref<AnimationStateMachine>::Create();
        sm->AddState(MakeSingleClipState("Walk", clip));
        sm->SetDefaultState("Walk");

        AnimationParameterSet params;
        sm->Start(params);

        std::vector<BoneTransform> pose;
        glm::vec3 total(0.0f);
        for (int i = 0; i < 8; ++i)
        {
            sm->Update(0.4f, params, 1, s_RootCtx, pose);
            EXPECT_TRUE(sm->GetLastRootMotion().HasMotion);
            total += sm->GetLastRootMotion().Translation;
        }
        // 3.2 s at 1 unit/s across three loop wraps.
        ExpectVec3Near(total, glm::vec3(0.0f, 0.0f, 3.2f), 1e-3f);

        // The evaluated pose stays pinned at the clip's reference frame.
        const BoneTransform rootPose = pose[0];
        ExpectVec3Near(rootPose.Translation, glm::vec3(0.0f));
    }

    TEST(RootMotionStateMachine, TransitionBlendsSourceAndTargetDeltas)
    {
        auto walk = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f), 1.0f, "Root", "Walk");
        auto strafe = MakeLinearClip(glm::vec3(1.0f, 0.0f, 0.0f), 1.0f, "Root", "Strafe");
        EnableExtraction(walk);
        EnableExtraction(strafe);

        auto sm = Ref<AnimationStateMachine>::Create();
        sm->AddState(MakeSingleClipState("Walk", walk));
        sm->AddState(MakeSingleClipState("Strafe", strafe));
        sm->SetDefaultState("Walk");

        AnimationTransition transition;
        transition.SourceState = "Walk";
        transition.DestinationState = "Strafe";
        transition.BlendDuration = 0.5f;
        TransitionCondition condition;
        condition.ParameterName = "go";
        condition.Op = TransitionCondition::Comparison::TriggerSet;
        transition.Conditions.push_back(condition);
        sm->AddTransition(transition);

        AnimationParameterSet params;
        params.DefineTrigger("go");
        sm->Start(params);

        std::vector<BoneTransform> pose;

        // Tick 1: steady walk; the trigger starts the transition at tick end.
        params.SetTrigger("go");
        sm->Update(0.25f, params, 1, s_RootCtx, pose);
        ExpectVec3Near(sm->GetLastRootMotion().Translation, glm::vec3(0.0f, 0.0f, 0.25f));
        EXPECT_TRUE(sm->IsInTransition());

        // Tick 2: factor = 0.25/0.5 = 0.5 — walk (0,0,0.25) and strafe
        // (0.25,0,0) blend at 0.5.
        sm->Update(0.25f, params, 1, s_RootCtx, pose);
        ExpectVec3Near(sm->GetLastRootMotion().Translation, glm::vec3(0.125f, 0.0f, 0.125f));

        // Tick 3: factor reaches 1 — the delta is fully the target's.
        sm->Update(0.25f, params, 1, s_RootCtx, pose);
        ExpectVec3Near(sm->GetLastRootMotion().Translation, glm::vec3(0.25f, 0.0f, 0.0f));
        EXPECT_FALSE(sm->IsInTransition());
    }

    // ── Blend trees ─────────────────────────────────────────────────────────

    TEST(RootMotionBlendTree, Blend1DUsesPoseWeights)
    {
        auto slow = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f), 1.0f, "Root", "Slow");
        auto fast = MakeLinearClip(glm::vec3(2.0f, 0.0f, 0.0f), 0.5f, "Root", "Fast");
        EnableExtraction(slow);
        EnableExtraction(fast);

        auto tree = Ref<BlendTree>::Create();
        tree->Type = BlendTree::BlendType::Simple1D;
        tree->BlendParameterX = "Speed";
        tree->Children.push_back({ slow, 0.0f, { 0.0f, 0.0f }, 1.0f, "Slow" });
        tree->Children.push_back({ fast, 1.0f, { 0.0f, 0.0f }, 1.0f, "Fast" });

        AnimationParameterSet params;
        params.DefineFloat("Speed", 0.25f);

        // Normalized window [0, 0.5]: slow advances 0→0.5s → (0,0,0.5);
        // fast advances 0→0.25s at 4 units/s → (1,0,0)... its duration is 0.5s
        // covering 1 unit, so 0.25s = 0.5 unit → (0.5+0.5): recompute — the fast
        // clip covers velocity*duration = 2*0.5 = 1 unit per loop, so half the
        // loop is 0.5 unit along X.
        const RootMotionDelta delta = tree->ExtractRootMotion(0.0f, 0.5f, params, true, s_RootCtx);
        EXPECT_TRUE(delta.HasMotion);
        ExpectVec3Near(delta.Translation, glm::mix(glm::vec3(0.0f, 0.0f, 0.5f), glm::vec3(0.5f, 0.0f, 0.0f), 0.25f));
    }

    TEST(RootMotionBlendTree, Blend2DExactMatchUsesSingleChild)
    {
        auto north = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f), 1.0f, "Root", "N");
        auto east = MakeLinearClip(glm::vec3(1.0f, 0.0f, 0.0f), 1.0f, "Root", "E");
        EnableExtraction(north);
        EnableExtraction(east);

        auto tree = Ref<BlendTree>::Create();
        tree->Type = BlendTree::BlendType::FreeformCartesian2D;
        tree->BlendParameterX = "X";
        tree->BlendParameterY = "Y";
        tree->Children.push_back({ north, 0.0f, { 0.0f, 1.0f }, 1.0f, "N" });
        tree->Children.push_back({ east, 0.0f, { 1.0f, 0.0f }, 1.0f, "E" });

        AnimationParameterSet params;
        params.DefineFloat("X", 0.0f);
        params.DefineFloat("Y", 1.0f);

        const RootMotionDelta delta = tree->ExtractRootMotion(0.0f, 0.5f, params, true, s_RootCtx);
        ExpectVec3Near(delta.Translation, glm::vec3(0.0f, 0.0f, 0.5f));
    }

    TEST(RootMotionBlendTree, Blend2DWeightsMirrorInverseDistance)
    {
        auto north = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f), 1.0f, "Root", "N");
        auto east = MakeLinearClip(glm::vec3(1.0f, 0.0f, 0.0f), 1.0f, "Root", "E");
        EnableExtraction(north);
        EnableExtraction(east);

        auto tree = Ref<BlendTree>::Create();
        tree->Type = BlendTree::BlendType::FreeformCartesian2D;
        tree->BlendParameterX = "X";
        tree->BlendParameterY = "Y";
        tree->Children.push_back({ north, 0.0f, { 0.0f, 1.0f }, 1.0f, "N" });
        tree->Children.push_back({ east, 0.0f, { 1.0f, 0.0f }, 1.0f, "E" });

        const glm::vec2 paramPos(0.25f, 0.75f);
        AnimationParameterSet params;
        params.DefineFloat("X", paramPos.x);
        params.DefineFloat("Y", paramPos.y);

        // Analytic inverse-distance-squared weights (matches Evaluate2D).
        const f32 wN = 1.0f / glm::dot(paramPos - glm::vec2(0.0f, 1.0f), paramPos - glm::vec2(0.0f, 1.0f));
        const f32 wE = 1.0f / glm::dot(paramPos - glm::vec2(1.0f, 0.0f), paramPos - glm::vec2(1.0f, 0.0f));
        const f32 total = wN + wE;

        const RootMotionDelta delta = tree->ExtractRootMotion(0.0f, 0.5f, params, true, s_RootCtx);
        const glm::vec3 expected = (wN / total) * glm::vec3(0.0f, 0.0f, 0.5f) + (wE / total) * glm::vec3(0.5f, 0.0f, 0.0f);
        ExpectVec3Near(delta.Translation, expected, 1e-3f);
    }

    // ── Animation graph (layers) ────────────────────────────────────────────

    namespace
    {
        Ref<AnimationGraph> MakeSingleLayerGraph(const Ref<AnimationClip>& clip, AnimationLayer::BlendMode mode, f32 weight)
        {
            auto sm = Ref<AnimationStateMachine>::Create();
            sm->AddState(MakeSingleClipState("Walk", clip));
            sm->SetDefaultState("Walk");

            auto graph = Ref<AnimationGraph>::Create();
            AnimationLayer layer;
            layer.Name = "Base";
            layer.StateMachine = sm;
            layer.Mode = mode;
            layer.Weight = weight;
            graph->Layers.push_back(std::move(layer));
            return graph;
        }

        void TickGraph(Ref<AnimationGraph> graph, f32 dt) // NOLINT(performance-unnecessary-value-param)
        {
            std::vector<glm::mat4> localTransforms;
            const std::vector<i32> parents = { -1 };
            graph->Update(dt, 1, localTransforms, s_RootNames, parents, {});
        }
    } // namespace

    TEST(RootMotionGraph, BaseLayerPublishesRootMotion)
    {
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        EnableExtraction(clip);
        auto graph = MakeSingleLayerGraph(clip, AnimationLayer::BlendMode::Override, 1.0f);
        graph->Start();

        TickGraph(graph, 0.25f);
        EXPECT_TRUE(graph->GetLastRootMotion().HasMotion);
        ExpectVec3Near(graph->GetLastRootMotion().Translation, glm::vec3(0.0f, 0.0f, 0.25f));
    }

    TEST(RootMotionGraph, PartialBaseLayerWeightScalesMotion)
    {
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        EnableExtraction(clip);
        auto graph = MakeSingleLayerGraph(clip, AnimationLayer::BlendMode::Override, 0.5f);
        graph->Start();

        TickGraph(graph, 0.25f);
        ExpectVec3Near(graph->GetLastRootMotion().Translation, glm::vec3(0.0f, 0.0f, 0.125f));
    }

    TEST(RootMotionGraph, AdditiveBaseLayerContributesNoRootMotion)
    {
        auto clip = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f));
        EnableExtraction(clip);
        auto graph = MakeSingleLayerGraph(clip, AnimationLayer::BlendMode::Additive, 1.0f);
        graph->Start();

        TickGraph(graph, 0.25f);
        EXPECT_FALSE(graph->GetLastRootMotion().HasMotion);
    }

    TEST(RootMotionGraph, UpperLayerDoesNotContributeRootMotion)
    {
        auto walk = MakeLinearClip(glm::vec3(0.0f, 0.0f, 1.0f), 1.0f, "Root", "Walk");
        auto upper = MakeLinearClip(glm::vec3(5.0f, 0.0f, 0.0f), 1.0f, "Root", "Upper");
        EnableExtraction(walk);
        EnableExtraction(upper);

        auto graph = MakeSingleLayerGraph(walk, AnimationLayer::BlendMode::Override, 1.0f);

        auto upperSm = Ref<AnimationStateMachine>::Create();
        upperSm->AddState(MakeSingleClipState("Upper", upper));
        upperSm->SetDefaultState("Upper");
        AnimationLayer upperLayer;
        upperLayer.Name = "Upper";
        upperLayer.StateMachine = upperSm;
        upperLayer.Mode = AnimationLayer::BlendMode::Override;
        upperLayer.Weight = 1.0f;
        graph->Layers.push_back(std::move(upperLayer));

        graph->Start();
        TickGraph(graph, 0.25f);

        // Only the base layer's +Z walk motion is published.
        ExpectVec3Near(graph->GetLastRootMotion().Translation, glm::vec3(0.0f, 0.0f, 0.25f));
    }
} // namespace OloEngine::Animation
