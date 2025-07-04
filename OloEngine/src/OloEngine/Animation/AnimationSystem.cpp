
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Core/Log.h"
#include <glm/gtx/matrix_decompose.hpp>

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

        animState.CurrentTime += deltaTime;
        animState.CurrentTime = LoopTime(animState.CurrentTime, animState.m_CurrentClip);

        if (animState.Blending && animState.m_NextClip)
        {
            animState.BlendTime += deltaTime;
            animState.NextTime += deltaTime;
            animState.NextTime = LoopTime(animState.NextTime, animState.m_NextClip);
            float blendAlpha = glm::clamp(animState.BlendTime / animState.BlendDuration, 0.0f, 1.0f);
            animState.BlendFactor = blendAlpha;
            if (blendAlpha >= 1.0f)
            {
                // Finish blend
                animState.m_CurrentClip = animState.m_NextClip;
                animState.CurrentTime = animState.NextTime;
                animState.m_NextClip = nullptr;
                animState.Blending = false;
                animState.BlendTime = 0.0f;
                animState.BlendFactor = 0.0f;
            }
        }

        // Helper lambda to sample a clip at a given time
        auto SampleClip = [](const Ref<AnimationClip>& clip, float time, const std::string& boneName) -> glm::mat4 {
            if (!clip) return glm::mat4(1.0f);
            const auto* boneAnim = clip->FindBoneAnimation(boneName);
            if (boneAnim && !boneAnim->Keyframes.empty())
            {
                const auto& keyframes = boneAnim->Keyframes;
                size_t k0 = 0, k1 = 0;
                for (size_t k = 0; k < keyframes.size(); ++k)
                {
                    if (keyframes[k].Time > time)
                    {
                        k1 = k;
                        k0 = (k == 0) ? 0 : k - 1;
                        break;
                    }
                }
                if (k1 == 0) k1 = keyframes.size() - 1;
                const auto& frame0 = keyframes[k0];
                const auto& frame1 = keyframes[k1];
                float t = 0.0f;
                float dt = frame1.Time - frame0.Time;
                if (dt > 0.0f)
                    t = (time - frame0.Time) / dt;
                glm::vec3 trans = glm::mix(frame0.Translation, frame1.Translation, t);
                glm::quat rot = glm::slerp(frame0.Rotation, frame1.Rotation, t);
                glm::vec3 scale = glm::mix(frame0.Scale, frame1.Scale, t);
                return glm::translate(glm::mat4(1.0f), trans) * glm::mat4_cast(rot) * glm::scale(glm::mat4(1.0f), scale);
            }
            return glm::mat4(1.0f);
        };

        // For each bone, sample and blend if needed
        for (size_t i = 0; i < skeleton.m_BoneNames.size(); ++i)
        {
            const std::string& boneName = skeleton.m_BoneNames[i];
            if (animState.Blending && animState.m_NextClip)
            {
                glm::mat4 localA = SampleClip(animState.m_CurrentClip, animState.CurrentTime, boneName);
                glm::mat4 localB = SampleClip(animState.m_NextClip, animState.NextTime, boneName);
                // Decompose, blend, and recompose
                glm::vec3 tA, tB, sA, sB;
                glm::quat rA, rB;
                glm::vec3 skew;
                glm::vec4 persp;
                glm::decompose(localA, sA, rA, tA, skew, persp);
                glm::decompose(localB, sB, rB, tB, skew, persp);
                glm::vec3 t = glm::mix(tA, tB, animState.BlendFactor);
                glm::quat r = glm::slerp(rA, rB, animState.BlendFactor);
                glm::vec3 s = glm::mix(sA, sB, animState.BlendFactor);
                skeleton.m_LocalTransforms[i] = glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
            }
            else if (animState.m_CurrentClip)
            {
                skeleton.m_LocalTransforms[i] = SampleClip(animState.m_CurrentClip, animState.CurrentTime, boneName);
            }
            else
            {
                // No animation: oscillate root bone for demo
                if (i == 0)
                {
                    float phase = std::sin(animState.CurrentTime * 2.0f) * 0.5f;
                    skeleton.m_LocalTransforms[i] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, phase, 0.0f));
                }
                else
                {
                    skeleton.m_LocalTransforms[i] = glm::mat4(1.0f);
                }
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
