
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Animation/AnimationClip.h"
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
        auto LoopTime = [](float t, const Ref<AnimationClip>& clip) -> float {
            if (clip && clip->Duration > 0.0f)
            {
                while (t >= clip->Duration) t -= clip->Duration;
                while (t < 0.0f) t += clip->Duration;
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
        struct TRSFrame {
            glm::vec3 translation;
            glm::quat rotation;
            glm::vec3 scale;
        };

        // Helper lambda to sample a clip at a given time and return TRS components
        auto SampleClipTRS = [](const Ref<AnimationClip>& clip, float time, const std::string& boneName) -> TRSFrame {
            TRSFrame result = { glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f) };
            if (!clip) return result;
            const auto* boneAnim = clip->FindBoneAnimation(boneName);
            if (boneAnim && !boneAnim->Keyframes.empty())
            {
                const auto& keyframes = boneAnim->Keyframes;
                size_t k0 = 0, k1 = 0;
                
                // Use binary search for better performance with large keyframe arrays
                auto it = std::lower_bound(keyframes.begin(), keyframes.end(), time,
                    [](const BoneKeyframe& keyframe, float t) { return keyframe.Time < t; });
                
                if (it == keyframes.end())
                {
                    // Time is beyond all keyframes, use last keyframe
                    k0 = k1 = keyframes.size() - 1;
                }
                else if (it == keyframes.begin())
                {
                    // Time is before all keyframes, use first keyframe
                    k0 = k1 = 0;
                }
                else
                {
                    // Found the interval
                    k1 = std::distance(keyframes.begin(), it);
                    k0 = k1 - 1;
                }
                
                const auto& frame0 = keyframes[k0];
                const auto& frame1 = keyframes[k1];
                float t = 0.0f;
                float dt = frame1.Time - frame0.Time;
                if (dt > 0.0f)
                    t = (time - frame0.Time) / dt;
                result.translation = glm::mix(frame0.Translation, frame1.Translation, t);
                result.rotation = glm::slerp(frame0.Rotation, frame1.Rotation, t);
                result.scale = glm::mix(frame0.Scale, frame1.Scale, t);
            }
            return result;
        };

        // Helper lambda to convert TRS to matrix
        auto TRSToMatrix = [](const TRSFrame& trs) -> glm::mat4 {
            return glm::translate(glm::mat4(1.0f), trs.translation) * 
                   glm::mat4_cast(trs.rotation) * 
                   glm::scale(glm::mat4(1.0f), trs.scale);
        };

        // For each bone, sample and blend if needed
        for (size_t i = 0; i < skeleton.m_BoneNames.size(); ++i)
        {
            const std::string& boneName = skeleton.m_BoneNames[i];
            if (animState.m_Blending && animState.m_NextClip)
            {
                // Sample both clips and blend at TRS level (more efficient than matrix decomposition)
                TRSFrame trsA = SampleClipTRS(animState.m_CurrentClip, animState.m_CurrentTime, boneName);
                TRSFrame trsB = SampleClipTRS(animState.m_NextClip, animState.m_NextTime, boneName);
                
                TRSFrame blendedTRS;
                blendedTRS.translation = glm::mix(trsA.translation, trsB.translation, animState.m_BlendFactor);
                blendedTRS.rotation = glm::slerp(trsA.rotation, trsB.rotation, animState.m_BlendFactor);
                blendedTRS.scale = glm::mix(trsA.scale, trsB.scale, animState.m_BlendFactor);
                
                skeleton.m_LocalTransforms[i] = TRSToMatrix(blendedTRS);
            }
            else if (animState.m_CurrentClip)
            {
                TRSFrame trs = SampleClipTRS(animState.m_CurrentClip, animState.m_CurrentTime, boneName);
                skeleton.m_LocalTransforms[i] = TRSToMatrix(trs);
            }
            else
            {
                // No animation: use identity transform
                skeleton.m_LocalTransforms[i] = glm::mat4(1.0f);
            }
        }

        // Compute global transforms
        for (size_t i = 0; i < skeleton.m_LocalTransforms.size(); ++i)
        {
            int parent = skeleton.m_ParentIndices[i];
            if (parent >= 0)
                skeleton.m_GlobalTransforms[i] = skeleton.m_GlobalTransforms[parent] * skeleton.m_LocalTransforms[i];
            else
                skeleton.m_GlobalTransforms[i] = skeleton.m_LocalTransforms[i];
        }

        // Compute final bone matrices for GPU skinning (GlobalTransform * InverseBindPose)
        for (size_t i = 0; i < skeleton.m_GlobalTransforms.size(); ++i)
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
}
