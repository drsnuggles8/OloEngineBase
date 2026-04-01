#include "OloEnginePCH.h"
#include "OloEngine/Animation/OneShotBlend.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Renderer/AnimatedModel.h"

namespace OloEngine::Animation
{
    void OneShotBlend::Trigger()
    {
        if (!Clip || Clip->Duration <= 0.0f)
        {
            return;
        }
        m_Phase = Phase::BlendIn;
        m_PlaybackTime = 0.0f;
        m_PhaseTime = 0.0f;
    }

    void OneShotBlend::Cancel()
    {
        if (m_Phase == Phase::Idle)
        {
            return;
        }
        // Jump straight to blend-out from wherever we are
        m_Phase = Phase::BlendOut;
        m_PhaseTime = 0.0f;
    }

    void OneShotBlend::Update(
        f32 dt,
        std::span<BoneTransform> basePose,
        std::span<const int> parentIndices,
        std::span<const std::string> boneNames)
    {
        if (m_Phase == Phase::Idle || !Clip || basePose.empty())
        {
            return;
        }

        // Advance playback time (always moving forward, no looping — one-shot)
        m_PlaybackTime += dt;
        m_PhaseTime += dt;

        // Determine phase transitions — loop to handle instant transitions
        // (e.g., zero-duration blend-in/out can skip multiple phases in one frame)
        bool transitioned = true;
        while (transitioned)
        {
            transitioned = false;
            switch (m_Phase)
            {
                case Phase::BlendIn:
                    if (BlendInDuration <= 0.0f || m_PhaseTime >= BlendInDuration)
                    {
                        m_Phase = Phase::Playing;
                        m_PhaseTime = (BlendInDuration > 0.0f) ? (m_PhaseTime - BlendInDuration) : 0.0f;
                        transitioned = true;
                    }
                    break;
                case Phase::Playing:
                {
                    f32 blendOutStart = Clip->Duration - BlendOutDuration;
                    if (m_PlaybackTime >= blendOutStart)
                    {
                        m_Phase = Phase::BlendOut;
                        m_PhaseTime = m_PlaybackTime - blendOutStart;
                        transitioned = true;
                    }
                    break;
                }
                case Phase::BlendOut:
                    if (BlendOutDuration <= 0.0f || m_PhaseTime >= BlendOutDuration || m_PlaybackTime >= Clip->Duration)
                    {
                        m_Phase = Phase::Idle;
                        m_PlaybackTime = 0.0f;
                        m_PhaseTime = 0.0f;
                        if (OnFinished)
                        {
                            OnFinished();
                        }
                        return;
                    }
                    break;
                case Phase::Idle:
                    return;
            }
        }

        // Compute effective blend weight for this frame
        f32 phaseWeight = 1.0f;
        if (m_Phase == Phase::BlendIn && BlendInDuration > 0.0f)
        {
            phaseWeight = glm::clamp(m_PhaseTime / BlendInDuration, 0.0f, 1.0f);
        }
        else if (m_Phase == Phase::BlendOut && BlendOutDuration > 0.0f)
        {
            phaseWeight = 1.0f - glm::clamp(m_PhaseTime / BlendOutDuration, 0.0f, 1.0f);
        }
        f32 effectiveWeight = Weight * phaseWeight;

        if (effectiveWeight <= 0.0f)
        {
            return;
        }

        // Clamp playback time to clip duration
        f32 sampleTime = glm::clamp(m_PlaybackTime, 0.0f, Clip->Duration);

        // Sample the one-shot clip into a temporary pose
        sizet boneCount = basePose.size();
        std::vector<BoneTransform> oneShotPose(boneCount);

        for (sizet i = 0; i < boneCount; ++i)
        {
            oneShotPose[i] = basePose[i]; // Default to base pose for bones without animation
        }

        // Rebuild cache if bone count changed (skeleton swapped)
        if (m_CachedBoneCount != boneCount)
        {
            m_BoneNameToIndex.clear();
            for (sizet i = 0; i < boneCount && i < boneNames.size(); ++i)
            {
                m_BoneNameToIndex[boneNames[i]] = i;
            }
            m_CachedBoneCount = boneCount;
        }

        for (const auto& boneAnim : Clip->BoneAnimations)
        {
            if (auto it = m_BoneNameToIndex.find(boneAnim.BoneName); it != m_BoneNameToIndex.end())
            {
                auto i = it->second;
                oneShotPose[i].Translation = AnimatedModel::SampleBonePosition(boneAnim.PositionKeys, sampleTime);
                oneShotPose[i].Rotation = AnimatedModel::SampleBoneRotation(boneAnim.RotationKeys, sampleTime);
                oneShotPose[i].Scale = AnimatedModel::SampleBoneScale(boneAnim.ScaleKeys, sampleTime);
            }
        }

        // Blend the one-shot pose into the base pose
        if (Additive)
        {
            // For additive, we need a rest pose. Use basePose as the "rest" for simplicity
            // (the delta is oneShotPose - basePose, applied on top of basePose).
            BlendUtils::AdditivePose(basePose, oneShotPose, basePose, effectiveWeight,
                                     BlendRootBone, parentIndices, basePose);
        }
        else if (BlendRootBone > 0)
        {
            BlendUtils::MaskedLerpPose(basePose, oneShotPose, effectiveWeight,
                                       BlendRootBone, parentIndices, basePose);
        }
        else
        {
            BlendUtils::LerpPose(basePose, oneShotPose, effectiveWeight, basePose);
        }
    }
} // namespace OloEngine::Animation
