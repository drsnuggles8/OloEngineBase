
#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/RootMotion.h"
#include "OloEngine/Animation/FootIKComponent.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/IK/FootIKPostPass.h"
#include "OloEngine/Animation/IK/IKPostPass.h"
#include "OloEngine/Animation/SpringBoneComponent.h"
#include "OloEngine/Animation/Procedural/SpringBonePostPass.h"
#include "OloEngine/Animation/NoiseAnimationComponent.h"
#include "OloEngine/Animation/Procedural/NoisePostPass.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Core/Log.h"
#include <algorithm>
#include <optional>
#include <mutex>
#include <unordered_set>

namespace OloEngine::Animation
{
    namespace
    {
        // TRS components sampled from a clip for a single bone.
        struct TRSFrame
        {
            glm::vec3 translation;
            glm::quat rotation;
            glm::vec3 scale;
        };

        // Sample a clip at a given time and return the bone's TRS. A cached
        // BoneAnimation pointer skips the per-bone lookup when the caller has it.
        TRSFrame SampleClipTRS(const Ref<AnimationClip>& clip, f32 timeSeconds, const std::string& boneName,
                               const BoneAnimation* cachedBoneAnim)
        {
            TRSFrame result = { glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f) };
            if (!clip)
                return result;
            if (const auto* boneAnim = cachedBoneAnim ? cachedBoneAnim : clip->FindBoneAnimation(boneName); boneAnim)
            {
                result.translation = AnimatedModel::SampleBonePosition(boneAnim->PositionKeys, timeSeconds);
                result.rotation = AnimatedModel::SampleBoneRotation(boneAnim->RotationKeys, timeSeconds);
                result.scale = AnimatedModel::SampleBoneScale(boneAnim->ScaleKeys, timeSeconds);
            }
            return result;
        }

        // SampleClipTRS + root-motion in-place pinning: when this bone is the
        // clip's extraction root, the extracted (masked) motion is removed from
        // the sample so the mesh doesn't double-move once the delta is applied
        // to the entity (issue #631). boneIndex is the skeleton bone index.
        TRSFrame SampleClipTRSPinned(const Ref<AnimationClip>& clip, f32 timeSeconds, const std::string& boneName,
                                     const BoneAnimation* cachedBoneAnim, sizet boneIndex)
        {
            TRSFrame frame = SampleClipTRS(clip, timeSeconds, boneName, cachedBoneAnim);
            if (clip && clip->RootMotion.ExtractRootMotion &&
                boneIndex == static_cast<sizet>(clip->RootMotion.RootBoneIndex))
            {
                const TRSFrame reference = SampleClipTRS(clip, 0.0f, boneName, cachedBoneAnim);
                const BoneTransform pinned = RootMotionUtils::MakeInPlaceRootPose(
                    { frame.translation, frame.rotation, frame.scale },
                    { reference.translation, reference.rotation, reference.scale },
                    clip->RootMotion.RootTranslationMask, clip->RootMotion.RootRotationMask);
                frame.translation = pinned.Translation;
                frame.rotation = pinned.Rotation;
            }
            return frame;
        }

        [[nodiscard("composed matrix must be used")]] glm::mat4 TRSToMatrix(const TRSFrame& trs)
        {
            return glm::translate(glm::mat4(1.0f), trs.translation) *
                   glm::mat4_cast(trs.rotation) *
                   glm::scale(glm::mat4(1.0f), trs.scale);
        }

        // Compute the animated local transform for a single bone, blending the
        // current/next clip as needed. Returns nullopt when no active clip
        // animates this bone (the caller keeps the bind-pose transform).
        // boneIndex feeds per-clip root-motion pinning (issue #631).
        std::optional<glm::mat4> EvaluateBoneLocalTransform(const AnimationStateComponent& animState, const std::string& boneName, sizet boneIndex)
        {
            if (animState.m_Blending && animState.m_NextClip)
            {
                const auto* boneAnimA = animState.m_CurrentClip
                                            ? animState.m_CurrentClip->FindBoneAnimation(boneName)
                                            : nullptr;
                const auto* boneAnimB = animState.m_NextClip->FindBoneAnimation(boneName);

                if (boneAnimA && boneAnimB)
                {
                    TRSFrame trsA = SampleClipTRSPinned(animState.m_CurrentClip, animState.m_CurrentTime, boneName, boneAnimA, boneIndex);
                    TRSFrame trsB = SampleClipTRSPinned(animState.m_NextClip, animState.m_NextTime, boneName, boneAnimB, boneIndex);

                    TRSFrame blendedTRS;
                    blendedTRS.translation = glm::mix(trsA.translation, trsB.translation, animState.m_BlendFactor);
                    blendedTRS.rotation = glm::slerp(trsA.rotation, trsB.rotation, animState.m_BlendFactor);
                    blendedTRS.scale = glm::mix(trsA.scale, trsB.scale, animState.m_BlendFactor);

                    return TRSToMatrix(blendedTRS);
                }
                if (boneAnimA)
                {
                    return TRSToMatrix(SampleClipTRSPinned(animState.m_CurrentClip, animState.m_CurrentTime, boneName, boneAnimA, boneIndex));
                }
                if (boneAnimB)
                {
                    return TRSToMatrix(SampleClipTRSPinned(animState.m_NextClip, animState.m_NextTime, boneName, boneAnimB, boneIndex));
                }
                // Neither clip animates this bone — keep bind-pose local transform.
                return std::nullopt;
            }

            if (animState.m_CurrentClip)
            {
                if (const auto* boneAnim = animState.m_CurrentClip->FindBoneAnimation(boneName); boneAnim)
                {
                    return TRSToMatrix(SampleClipTRSPinned(animState.m_CurrentClip, animState.m_CurrentTime, boneName, boneAnim, boneIndex));
                }
            }
            // No current clip / bone not animated — keep bind-pose local transform.
            return std::nullopt;
        }
    } // namespace

    void AnimationSystem::ApplyClipRequest(AnimationStateComponent& animState)
    {
        if (animState.m_RequestedClip.empty())
        {
            return;
        }
        std::string requested;
        requested.swap(animState.m_RequestedClip); // consumed whatever happens next
        const bool loop = animState.m_RequestedLoop;

        const auto found = std::ranges::find_if(animState.m_AvailableClips, [&requested](const Ref<AnimationClip>& clip)
                                                { return clip && clip->Name == requested; });
        if (found == animState.m_AvailableClips.end())
        {
            // LOUD, once per name: a mistyped clip name in a script would
            // otherwise read as an animation system that ignores it.
            static std::mutex s_ReportedMutex;
            static std::unordered_set<std::string> s_Reported;
            const std::scoped_lock lock(s_ReportedMutex);
            if (s_Reported.insert(requested).second)
            {
                std::string available;
                for (const Ref<AnimationClip>& clip : animState.m_AvailableClips)
                {
                    if (clip)
                    {
                        available += available.empty() ? clip->Name : (", " + clip->Name);
                    }
                }
                OLO_CORE_WARN("AnimationSystem: no clip named '{}' on this model (clips: {}); the request is ignored",
                              requested, available.empty() ? std::string("none") : available);
            }
            return;
        }
        const Ref<AnimationClip>& target = *found;
        const i32 index = static_cast<i32>(std::distance(animState.m_AvailableClips.begin(), found));

        // Already the current clip and not leaving it: only the loop flag moves.
        // This is what lets a script write the request every frame.
        if (animState.m_CurrentClip == target && !animState.m_Blending)
        {
            animState.m_Loop = loop;
            return;
        }
        // Already on its way there: keep the blend, take the new loop flag.
        if (animState.m_Blending && animState.m_NextClip == target)
        {
            animState.m_NextLoop = loop;
            return;
        }

        animState.m_CurrentClipIndex = index;
        if (!animState.m_CurrentClip)
        {
            // Nothing to blend from.
            animState.m_CurrentClip = target;
            animState.m_CurrentTime = 0.0f;
            animState.m_Loop = loop;
            return;
        }
        // Interrupting a blend that is more than half done: the pose is mostly
        // the blend target already, so that clip is promoted to current before
        // the new blend starts. Starting from the old current clip instead would
        // snap the pose back towards where the blend began.
        if (animState.m_Blending && animState.m_NextClip && animState.m_BlendFactor > 0.5f)
        {
            animState.m_CurrentClip = animState.m_NextClip;
            animState.m_CurrentTime = animState.m_NextTime;
            animState.m_Loop = animState.m_NextLoop;
        }
        animState.m_NextClip = target;
        animState.m_NextTime = 0.0f;
        animState.m_NextLoop = loop;
        animState.m_BlendTime = 0.0f;
        animState.m_BlendFactor = 0.0f;
        animState.m_Blending = true;
    }

    // Animation update: advances time, samples animation, computes bone transforms
    void AnimationSystem::Update(
        AnimationStateComponent& animState,
        Skeleton& skeleton,
        f32 deltaTime,
        const IKTargetComponent* ikTarget,
        const glm::mat4& entityWorldTransform,
        const SpringBoneComponent* springBone,
        SpringBoneState* springBoneState,
        const NoiseAnimationComponent* noise,
        NoiseAnimationState* noiseState,
        const FootIKComponent* footIK,
        FootIKStateComponent* footIKState)
    {
        OLO_PROFILE_FUNCTION();

        // Deformation history is NOT advanced here. SkeletalDeformationSystem
        // advances it once per tick for every skinned entity, animating or not
        // (#1226) — doing it here would tie the previous pose to whether this
        // entity animated, and a paused one would then emit last tick's delta
        // for as long as the pause lasted.

        // A clip requested from outside (issue #1533) starts its blend HERE, at
        // the one place every pose update passes through, so the editor combo, a
        // script and an MCP write behave the same way in edit mode and in Play.
        ApplyClipRequest(animState);

        // Advance animation time for current and next clips: a looping clip
        // wraps, a non-looping one (issue #1533) holds its final pose.
        auto LoopTime = [](f32 t, const Ref<AnimationClip>& clip, bool loop)
        {
            if (clip && clip->Duration > 0.0f)
            {
                if (!loop)
                {
                    return std::clamp(t, 0.0f, clip->Duration);
                }
                while (t >= clip->Duration)
                    t -= clip->Duration;
                while (t < 0.0f)
                    t += clip->Duration;
            }
            return t;
        };

        // Capture pre-advance clip times for root-motion extraction (issue #631);
        // the wrap/blend bookkeeping below rewrites them.
        const f32 rootMotionStartCurrent = animState.m_CurrentTime;
        const f32 rootMotionStartNext = animState.m_NextTime;
        const bool wasBlending = animState.m_Blending && animState.m_NextClip;
        const Ref<AnimationClip> blendTargetClip = animState.m_NextClip; // survives the completion swap

        // The clip clock runs at the entity's playback speed (issue #1533);
        // the post passes below keep real time. A non-finite or negative speed
        // (a corrupt write that bypassed every clamp) plays as authored.
        const f32 playbackSpeed = std::isfinite(animState.m_PlaybackSpeed) ? std::clamp(animState.m_PlaybackSpeed, 0.0f, 10.0f) : 1.0f;
        const f32 clipSeconds = deltaTime * playbackSpeed;

        animState.m_CurrentTime += clipSeconds;
        animState.m_CurrentTime = LoopTime(animState.m_CurrentTime, animState.m_CurrentClip, animState.m_Loop);

        f32 blendAlpha = 0.0f;
        if (wasBlending)
        {
            animState.m_BlendTime += clipSeconds;
            animState.m_NextTime += clipSeconds;
            animState.m_NextTime = LoopTime(animState.m_NextTime, animState.m_NextClip, animState.m_NextLoop);
            blendAlpha = glm::clamp(animState.m_BlendTime / animState.m_BlendDuration, 0.0f, 1.0f);
            animState.m_BlendFactor = blendAlpha;
        }

        // Extract this tick's root-motion delta (wrap-aware, per clip) before the
        // blend-completion swap discards the source clip. Each contributing clip
        // extracts against its own settings; the deltas blend with the same
        // factor the pose blend uses. Extraction is wrap-aware exactly when the
        // clip loops (m_Loop / m_NextLoop, issue #1533); a clip that holds its
        // final pose extracts no motion past its end.
        {
            const PoseEvalContext rootMotionCtx{
                .BoneNames = skeleton.m_BoneNames,
                .BindPose = {},
                .ParentIndices = skeleton.m_ParentIndices,
                .BindPoseGlobals = skeleton.m_BindPoseMatrices,
                .PreTransforms = skeleton.m_BonePreTransforms
            };
            RootMotionDelta delta;
            if (animState.m_CurrentClip)
            {
                delta = RootMotionUtils::ExtractConfiguredDelta(
                    *animState.m_CurrentClip, rootMotionStartCurrent, clipSeconds, animState.m_Loop, rootMotionCtx);
            }
            if (wasBlending && blendTargetClip)
            {
                const RootMotionDelta nextDelta = RootMotionUtils::ExtractConfiguredDelta(
                    *blendTargetClip, rootMotionStartNext, clipSeconds, animState.m_NextLoop, rootMotionCtx);
                delta = RootMotionUtils::Blend(delta, nextDelta, animState.m_BlendFactor);
            }
            animState.m_RootMotionTranslation = delta.Translation;
            animState.m_RootMotionRotation = delta.Rotation;
            animState.m_HasRootMotion = delta.HasMotion;
        }

        if (wasBlending && blendAlpha >= 1.0f)
        {
            // Finish blend
            animState.m_CurrentClip = animState.m_NextClip;
            animState.m_CurrentTime = animState.m_NextTime;
            animState.m_Loop = animState.m_NextLoop;
            animState.m_NextClip = nullptr;
            animState.m_Blending = false;
            animState.m_BlendTime = 0.0f;
            animState.m_BlendFactor = 0.0f;
        }

        // Reset all local transforms to bind-pose so bones not animated in
        // the current clip fall back to their rest pose (not stale values
        // from a previously active clip).
        if (!skeleton.m_BindPoseLocalTransforms.empty())
        {
            skeleton.m_LocalTransforms = skeleton.m_BindPoseLocalTransforms;
        }

        // For each bone, sample and blend if needed. Bones without animation
        // channels in the active clip(s) keep their bind-pose local transform
        // (e.g. b_Root_00 carries a -90° X rotation in the fox.gltf model but has
        // no keyframes in the animation).
        auto boneNameCount = skeleton.m_BoneNames.size();
        for (sizet i = 0; i < boneNameCount; ++i)
        {
            if (auto animatedLocal = EvaluateBoneLocalTransform(animState, skeleton.m_BoneNames[i], i); animatedLocal)
            {
                skeleton.m_LocalTransforms[i] = *animatedLocal;
            }
        }

        // Apply procedural noise (breathing / idle sway) before IK so the noise
        // produces the organic "intent" pose that IK then corrects — end-effector
        // constraints (planted feet, hands on target) stay satisfied while the
        // body sways.
        if (noise && noiseState && noise->Enabled)
        {
            ApplyNoisePostPass(skeleton, *noise, *noiseState, deltaTime);
        }

        // Apply IK pass between pose evaluation and forward kinematics
        if (ikTarget && (ikTarget->AimIKEnabled || ikTarget->LimbIKEnabled || ikTarget->ChainIKEnabled))
        {
            ApplyIKPostPass(skeleton, *ikTarget, entityWorldTransform);
        }

        // Ground-adaptation foot/hand IK after aim/limb/chain IK so it corrects
        // the final intent pose (issue #631)
        if (footIK && footIKState && footIK->Enabled)
        {
            ApplyFootIKPostPass(skeleton, *footIK, *footIKState, entityWorldTransform, deltaTime);
        }

        // Apply spring-bone secondary motion after IK so springs react to the
        // IK-corrected pose
        if (springBone && springBoneState && springBone->Enabled)
        {
            ApplySpringBonePostPass(skeleton, *springBone, *springBoneState, entityWorldTransform, deltaTime);
        }

        // Compute global transforms, applying pre-transforms for non-bone ancestor nodes
        static const glm::mat4 identityTransform(1.0f);
        auto localTransformCount = skeleton.m_LocalTransforms.size();
        for (sizet i = 0; i < localTransformCount; ++i)
        {
            const glm::mat4& preTransform = (i < skeleton.m_BonePreTransforms.size())
                                                ? skeleton.m_BonePreTransforms[i]
                                                : identityTransform;
            i32 parent = skeleton.m_ParentIndices[i];
            if (parent >= 0)
                skeleton.m_GlobalTransforms[i] = skeleton.m_GlobalTransforms[static_cast<sizet>(parent)] * preTransform * skeleton.m_LocalTransforms[i];
            else
                skeleton.m_GlobalTransforms[i] = preTransform * skeleton.m_LocalTransforms[i];
        }

        // Compute final bone matrices for GPU skinning (GlobalTransform * InverseBindPose)
        auto globalTransformCount = skeleton.m_GlobalTransforms.size();
        for (sizet i = 0; i < globalTransformCount; ++i)
        {
            if (i < skeleton.m_InverseBindPoses.size())
            {
                skeleton.m_FinalBoneMatrices[i] = skeleton.m_GlobalTransforms[i] * skeleton.m_InverseBindPoses[i];
            }
            else
            {
                // Fallback if no bind pose data available
                skeleton.m_FinalBoneMatrices[i] = skeleton.m_GlobalTransforms[i];
            }
        }
    }
} // namespace OloEngine::Animation
