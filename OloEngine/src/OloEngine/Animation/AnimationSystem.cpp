
#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/IK/IKPostPass.h"
#include "OloEngine/Animation/SpringBoneComponent.h"
#include "OloEngine/Animation/Procedural/SpringBonePostPass.h"
#include "OloEngine/Animation/NoiseAnimationComponent.h"
#include "OloEngine/Animation/Procedural/NoisePostPass.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Core/Log.h"
#include <algorithm>
#include <optional>

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
                               const BoneAnimation* cachedBoneAnim = nullptr)
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

        glm::mat4 TRSToMatrix(const TRSFrame& trs)
        {
            return glm::translate(glm::mat4(1.0f), trs.translation) *
                   glm::mat4_cast(trs.rotation) *
                   glm::scale(glm::mat4(1.0f), trs.scale);
        }

        // Compute the animated local transform for a single bone, blending the
        // current/next clip as needed. Returns nullopt when no active clip
        // animates this bone (the caller keeps the bind-pose transform).
        std::optional<glm::mat4> EvaluateBoneLocalTransform(const AnimationStateComponent& animState, const std::string& boneName)
        {
            if (animState.m_Blending && animState.m_NextClip)
            {
                const auto* boneAnimA = animState.m_CurrentClip
                                            ? animState.m_CurrentClip->FindBoneAnimation(boneName)
                                            : nullptr;
                const auto* boneAnimB = animState.m_NextClip->FindBoneAnimation(boneName);

                if (boneAnimA && boneAnimB)
                {
                    TRSFrame trsA = SampleClipTRS(animState.m_CurrentClip, animState.m_CurrentTime, boneName, boneAnimA);
                    TRSFrame trsB = SampleClipTRS(animState.m_NextClip, animState.m_NextTime, boneName, boneAnimB);

                    TRSFrame blendedTRS;
                    blendedTRS.translation = glm::mix(trsA.translation, trsB.translation, animState.m_BlendFactor);
                    blendedTRS.rotation = glm::slerp(trsA.rotation, trsB.rotation, animState.m_BlendFactor);
                    blendedTRS.scale = glm::mix(trsA.scale, trsB.scale, animState.m_BlendFactor);

                    return TRSToMatrix(blendedTRS);
                }
                if (boneAnimA)
                {
                    return TRSToMatrix(SampleClipTRS(animState.m_CurrentClip, animState.m_CurrentTime, boneName, boneAnimA));
                }
                if (boneAnimB)
                {
                    return TRSToMatrix(SampleClipTRS(animState.m_NextClip, animState.m_NextTime, boneName, boneAnimB));
                }
                // Neither clip animates this bone — keep bind-pose local transform.
                return std::nullopt;
            }

            if (animState.m_CurrentClip)
            {
                if (const auto* boneAnim = animState.m_CurrentClip->FindBoneAnimation(boneName); boneAnim)
                {
                    return TRSToMatrix(SampleClipTRS(animState.m_CurrentClip, animState.m_CurrentTime, boneName, boneAnim));
                }
            }
            // No current clip / bone not animated — keep bind-pose local transform.
            return std::nullopt;
        }
    } // namespace

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
        NoiseAnimationState* noiseState)
    {
        OLO_PROFILE_FUNCTION();

        // Rotate current final bones into the previous-frame slot so the
        // G-Buffer skinned pass can compute per-bone motion vectors.
        skeleton.RotateBoneHistory();

        // Advance and loop animation time for current and next clips
        auto LoopTime = [](f32 t, const Ref<AnimationClip>& clip) -> f32
        {
            if (clip && clip->Duration > 0.0f)
            {
                while (t >= clip->Duration)
                    t -= clip->Duration;
                while (t < 0.0f)
                    t += clip->Duration;
            }
            return t;
        };

        animState.m_CurrentTime += deltaTime;
        animState.m_CurrentTime = LoopTime(animState.m_CurrentTime, animState.m_CurrentClip);

        if (animState.m_Blending && animState.m_NextClip)
        {
            animState.m_BlendTime += deltaTime;
            animState.m_NextTime += deltaTime;
            animState.m_NextTime = LoopTime(animState.m_NextTime, animState.m_NextClip);
            f32 blendAlpha = glm::clamp(animState.m_BlendTime / animState.m_BlendDuration, 0.0f, 1.0f);
            animState.m_BlendFactor = blendAlpha;
            if (blendAlpha >= 1.0f)
            {
                // Finish blend
                animState.m_CurrentClip = animState.m_NextClip;
                animState.m_CurrentTime = animState.m_NextTime;
                animState.m_NextClip = nullptr;
                animState.m_Blending = false;
                animState.m_BlendTime = 0.0f;
                animState.m_BlendFactor = 0.0f;
            }
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
        for (sizet i = 0; i < skeleton.m_BoneNames.size(); ++i)
        {
            if (auto animatedLocal = EvaluateBoneLocalTransform(animState, skeleton.m_BoneNames[i]); animatedLocal)
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

        // Apply spring-bone secondary motion after IK so springs react to the
        // IK-corrected pose
        if (springBone && springBoneState && springBone->Enabled)
        {
            ApplySpringBonePostPass(skeleton, *springBone, *springBoneState, entityWorldTransform, deltaTime);
        }

        // Compute global transforms, applying pre-transforms for non-bone ancestor nodes
        static const glm::mat4 identityTransform(1.0f);
        for (sizet i = 0; i < skeleton.m_LocalTransforms.size(); ++i)
        {
            const glm::mat4& preTransform = (i < skeleton.m_BonePreTransforms.size())
                                                ? skeleton.m_BonePreTransforms[i]
                                                : identityTransform;
            i32 parent = skeleton.m_ParentIndices[i];
            if (parent >= 0)
                skeleton.m_GlobalTransforms[i] = skeleton.m_GlobalTransforms[parent] * preTransform * skeleton.m_LocalTransforms[i];
            else
                skeleton.m_GlobalTransforms[i] = preTransform * skeleton.m_LocalTransforms[i];
        }

        // Compute final bone matrices for GPU skinning (GlobalTransform * InverseBindPose)
        for (sizet i = 0; i < skeleton.m_GlobalTransforms.size(); ++i)
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
