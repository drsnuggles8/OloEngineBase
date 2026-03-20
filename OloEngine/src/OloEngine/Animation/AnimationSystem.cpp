
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Core/Log.h"
#include <algorithm>

namespace OloEngine::Animation
{
    // Animation update: advances time, samples animation, computes bone transforms
    void AnimationSystem::Update(
        AnimationStateComponent& animState,
        Skeleton& skeleton,
        f32 deltaTime)
    {

        // Advance and loop animation time for current and next clips
        auto LoopTime = [](float t, const Ref<AnimationClip>& clip) -> float
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
            float blendAlpha = glm::clamp(animState.m_BlendTime / animState.m_BlendDuration, 0.0f, 1.0f);
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
        auto SampleClipTRS = [](const Ref<AnimationClip>& clip, float time, const std::string& boneName) -> TRSFrame
        {
            TRSFrame result = { glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f) };
            if (!clip)
                return result;
            const auto* boneAnim = clip->FindBoneAnimation(boneName);
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
                    TRSFrame trsA = SampleClipTRS(animState.m_CurrentClip, animState.m_CurrentTime, boneName);
                    TRSFrame trsB = SampleClipTRS(animState.m_NextClip, animState.m_NextTime, boneName);

                    TRSFrame blendedTRS;
                    blendedTRS.translation = glm::mix(trsA.translation, trsB.translation, animState.m_BlendFactor);
                    blendedTRS.rotation = glm::slerp(trsA.rotation, trsB.rotation, animState.m_BlendFactor);
                    blendedTRS.scale = glm::mix(trsA.scale, trsB.scale, animState.m_BlendFactor);

                    skeleton.m_LocalTransforms[i] = TRSToMatrix(blendedTRS);
                }
                else if (boneAnimA)
                {
                    TRSFrame trs = SampleClipTRS(animState.m_CurrentClip, animState.m_CurrentTime, boneName);
                    skeleton.m_LocalTransforms[i] = TRSToMatrix(trs);
                }
                else if (boneAnimB)
                {
                    TRSFrame trs = SampleClipTRS(animState.m_NextClip, animState.m_NextTime, boneName);
                    skeleton.m_LocalTransforms[i] = TRSToMatrix(trs);
                }
                // else: neither clip animates this bone — keep bind-pose local transform
            }
            else if (animState.m_CurrentClip)
            {
                if (animState.m_CurrentClip->FindBoneAnimation(boneName))
                {
                    TRSFrame trs = SampleClipTRS(animState.m_CurrentClip, animState.m_CurrentTime, boneName);
                    skeleton.m_LocalTransforms[i] = TRSToMatrix(trs);
                }
                // else: bone not animated in this clip — keep bind-pose local transform
            }
            // If no current clip: keep existing bind-pose transforms
        }

        // Compute global transforms, applying pre-transforms for non-bone ancestor nodes
        static const glm::mat4 identityTransform(1.0f);
        for (sizet i = 0; i < skeleton.m_LocalTransforms.size(); ++i)
        {
            const glm::mat4& preTransform = (i < skeleton.m_BonePreTransforms.size())
                                                ? skeleton.m_BonePreTransforms[i]
                                                : identityTransform;
            int parent = skeleton.m_ParentIndices[i];
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
