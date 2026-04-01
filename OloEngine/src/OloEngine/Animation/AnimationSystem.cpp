
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/IK/AimIKSolver.h"
#include "OloEngine/Animation/IK/LimbIKSolver.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Core/Log.h"
#include <algorithm>
#include <glm/gtx/matrix_decompose.hpp>

namespace OloEngine::Animation
{
    // Animation update: advances time, samples animation, computes bone transforms
    void AnimationSystem::Update(
        AnimationStateComponent& animState,
        Skeleton& skeleton,
        f32 deltaTime,
        const IKTargetComponent* ikTarget,
        const glm::mat4& entityWorldTransform)
    {

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

        // Helper structure to hold TRS components
        struct TRSFrame
        {
            glm::vec3 translation;
            glm::quat rotation;
            glm::vec3 scale;
        };

        // Helper lambda to sample a clip at a given time and return TRS components
        auto SampleClipTRS = [](const Ref<AnimationClip>& clip, f32 time, const std::string& boneName,
                                const BoneAnimation* cachedBoneAnim = nullptr) -> TRSFrame
        {
            TRSFrame result = { glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f) };
            if (!clip)
                return result;
            const auto* boneAnim = cachedBoneAnim ? cachedBoneAnim : clip->FindBoneAnimation(boneName);
            if (boneAnim)
            {
                // Sample each channel separately using the new optimized functions
                result.translation = AnimatedModel::SampleBonePosition(boneAnim->PositionKeys, time);
                result.rotation = AnimatedModel::SampleBoneRotation(boneAnim->RotationKeys, time);
                result.scale = AnimatedModel::SampleBoneScale(boneAnim->ScaleKeys, time);
            }
            return result;
        };

        // Helper lambda to convert TRS to matrix
        auto TRSToMatrix = [](const TRSFrame& trs) -> glm::mat4
        {
            return glm::translate(glm::mat4(1.0f), trs.translation) *
                   glm::mat4_cast(trs.rotation) *
                   glm::scale(glm::mat4(1.0f), trs.scale);
        };

        // Reset all local transforms to bind-pose so bones not animated in
        // the current clip fall back to their rest pose (not stale values
        // from a previously active clip).
        if (!skeleton.m_BindPoseLocalTransforms.empty())
        {
            skeleton.m_LocalTransforms = skeleton.m_BindPoseLocalTransforms;
        }

        // For each bone, sample and blend if needed.
        // Bones without animation channels in a clip keep their bind-pose
        // local transform (e.g. b_Root_00 carries a -90° X rotation in the
        // fox.gltf model but has no keyframes in the animation).
        for (sizet i = 0; i < skeleton.m_BoneNames.size(); ++i)
        {
            const std::string& boneName = skeleton.m_BoneNames[i];

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

                    skeleton.m_LocalTransforms[i] = TRSToMatrix(blendedTRS);
                }
                else if (boneAnimA)
                {
                    TRSFrame trs = SampleClipTRS(animState.m_CurrentClip, animState.m_CurrentTime, boneName, boneAnimA);
                    skeleton.m_LocalTransforms[i] = TRSToMatrix(trs);
                }
                else if (boneAnimB)
                {
                    TRSFrame trs = SampleClipTRS(animState.m_NextClip, animState.m_NextTime, boneName, boneAnimB);
                    skeleton.m_LocalTransforms[i] = TRSToMatrix(trs);
                }
                // else: neither clip animates this bone — keep bind-pose local transform
            }
            else if (animState.m_CurrentClip)
            {
                const auto* boneAnim = animState.m_CurrentClip->FindBoneAnimation(boneName);
                if (boneAnim)
                {
                    TRSFrame trs = SampleClipTRS(animState.m_CurrentClip, animState.m_CurrentTime, boneName, boneAnim);
                    skeleton.m_LocalTransforms[i] = TRSToMatrix(trs);
                }
                // else: bone not animated in this clip — keep bind-pose local transform
            }
            // If no current clip: keep existing bind-pose transforms
        }

        // Apply IK pass between pose evaluation and forward kinematics
        if (ikTarget && (ikTarget->AimIKEnabled || ikTarget->LimbIKEnabled))
        {
            sizet boneCount = skeleton.m_BoneNames.size();

            // Track which bones the IK chains will modify so we only write those
            // back — avoids the lossy glm::decompose round-trip on untouched bones.
            std::vector<bool> ikModified(boneCount, false);

            std::vector<BoneTransform> localPose(boneCount);
            for (sizet i = 0; i < boneCount; ++i)
            {
                glm::vec3 scale;
                glm::vec3 translation;
                glm::quat rotation;
                glm::vec3 skew;
                glm::vec4 perspective;
                if (!glm::decompose(skeleton.m_LocalTransforms[i], scale, rotation, translation, skew, perspective))
                {
                    localPose[i] = { glm::vec3(0.0f), glm::identity<glm::quat>(), glm::vec3(1.0f) };
                    continue;
                }
                localPose[i] = { translation, rotation, scale };
            }

            auto isFiniteVec3 = [](const glm::vec3& v)
            { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };

            if (ikTarget->AimIKEnabled && ikTarget->AimBoneIndex < static_cast<u32>(boneCount))
            {
                if (isFiniteVec3(ikTarget->AimTarget) && isFiniteVec3(ikTarget->AimAxis) && isFiniteVec3(ikTarget->AimOffset) && isFiniteVec3(ikTarget->AimPoleVector) && std::isfinite(ikTarget->AimWeight) && std::isfinite(ikTarget->AimChainFactor))
                {
                    // Mark affected bones only after validation passes
                    auto bone = ikTarget->AimBoneIndex;
                    for (u32 j = 0; j < std::max(1u, ikTarget->AimChainLength) && bone < static_cast<u32>(boneCount); ++j)
                    {
                        ikModified[bone] = true;
                        if (auto parent = skeleton.m_ParentIndices[bone]; parent < 0)
                        {
                            break;
                        }
                        else
                        {
                            bone = static_cast<u32>(parent);
                        }
                    }

                    AimIKParams params;
                    params.TargetBoneIndex = ikTarget->AimBoneIndex;
                    params.TargetPosition = BlendUtils::WorldToModelSpace(ikTarget->AimTarget, entityWorldTransform);
                    params.AimAxis = ikTarget->AimAxis;
                    params.AimOffset = ikTarget->AimOffset;
                    params.PoleVector = ikTarget->AimPoleVector;
                    params.ChainLength = std::max(1u, ikTarget->AimChainLength);
                    params.ChainFactor = glm::clamp(ikTarget->AimChainFactor, 0.0f, 1.0f);
                    params.Weight = glm::clamp(ikTarget->AimWeight, 0.0f, 1.0f);
                    AimIKSolver::Solve(localPose, skeleton.m_ParentIndices, params, skeleton.m_BonePreTransforms);
                }
            }

            if (ikTarget->LimbIKEnabled && ikTarget->LimbBoneIndex < static_cast<u32>(boneCount))
            {
                if (isFiniteVec3(ikTarget->LimbTarget) && std::isfinite(ikTarget->LimbWeight))
                {
                    // Mark affected bones only after validation passes
                    auto bone = ikTarget->LimbBoneIndex;
                    for (u32 j = 0; j < std::max(1u, ikTarget->LimbChainLength) && bone < static_cast<u32>(boneCount); ++j)
                    {
                        ikModified[bone] = true;
                        if (auto parent = skeleton.m_ParentIndices[bone]; parent < 0)
                        {
                            break;
                        }
                        else
                        {
                            bone = static_cast<u32>(parent);
                        }
                    }

                    LimbIKParams params;
                    params.TargetBoneIndex = ikTarget->LimbBoneIndex;
                    params.TargetPosition = BlendUtils::WorldToModelSpace(ikTarget->LimbTarget, entityWorldTransform);
                    params.ChainLength = std::max(1u, ikTarget->LimbChainLength);
                    params.Weight = glm::clamp(ikTarget->LimbWeight, 0.0f, 1.0f);
                    LimbIKSolver::Solve(localPose, skeleton.m_ParentIndices, params, skeleton.m_BonePreTransforms);
                }
            }

            // Only write back bones that IK actually modified
            for (sizet i = 0; i < boneCount; ++i)
            {
                if (ikModified[i])
                {
                    skeleton.m_LocalTransforms[i] =
                        glm::translate(glm::mat4(1.0f), localPose[i].Translation) * glm::mat4_cast(localPose[i].Rotation) * glm::scale(glm::mat4(1.0f), localPose[i].Scale);
                }
            }
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
