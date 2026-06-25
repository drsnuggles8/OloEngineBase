#include "OloEnginePCH.h"
#include "OloEngine/Animation/OneShotBlend.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Renderer/AnimatedModel.h"

namespace OloEngine::Animation
{
    void OneShotBlend::Trigger()
    {
        if (!Clip || Clip->Duration <= 0.0f || !std::isfinite(Clip->Duration))
        {
            return;
        }
        m_Phase = Phase::BlendIn;
        m_PlaybackTime = 0.0f;
        m_PhaseTime = 0.0f;
        m_AdditiveRestPose.clear();
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

    bool OneShotBlend::AdvancePhase()
    {
        // Loop to handle instant transitions (e.g., zero-duration blend-in/out can
        // skip multiple phases in one frame).
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
                    if (f32 blendOutStart = Clip->Duration - BlendOutDuration; m_PlaybackTime >= blendOutStart)
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
                        return false;
                    }
                    break;
                case Phase::Idle:
                    return false;
                default:
                    break;
            }
        }
        return true;
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

        // Validate floating-point parameters before any arithmetic
        if (!std::isfinite(Weight) || !std::isfinite(BlendInDuration) || !std::isfinite(BlendOutDuration))
        {
            m_Phase = Phase::Idle;
            m_PlaybackTime = 0.0f;
            m_PhaseTime = 0.0f;
            return;
        }

        // Advance playback time (always moving forward, no looping — one-shot)
        m_PlaybackTime += dt;
        m_PhaseTime += dt;

        // Determine phase transitions; bail out if the one-shot finished this frame.
        if (!AdvancePhase())
        {
            return;
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
        else
        {
            // No additional handling required.
        }
        f32 effectiveWeight = Weight * phaseWeight;

        if (effectiveWeight <= 0.0f)
        {
            return;
        }

        // Clamp playback time to clip duration
        f32 sampleTime = glm::clamp(m_PlaybackTime, 0.0f, Clip->Duration);

        // Sample the one-shot clip into a reusable pose buffer
        sizet boneCount = basePose.size();
        m_OneShotPose.resize(boneCount);

        for (sizet i = 0; i < boneCount; ++i)
        {
            m_OneShotPose[i] = basePose[i]; // Default to base pose for bones without animation
        }

        // Rebuild cache if bone count or skeleton identity changed
        sizet nameHash = 0;
        for (auto const& name : boneNames)
        {
            nameHash ^= std::hash<std::string_view>{}(name) + 0x9e3779b9U + (nameHash << 6) + (nameHash >> 2);
        }
        if (m_CachedBoneCount != boneCount || m_CachedBoneNamesHash != nameHash)
        {
            m_BoneNameToIndex.clear();
            for (sizet i = 0; i < boneCount && i < boneNames.size(); ++i)
            {
                m_BoneNameToIndex[boneNames[i]] = i;
            }
            m_CachedBoneCount = boneCount;
            m_CachedBoneNamesHash = nameHash;
            m_AdditiveRestPose.clear();
        }

        for (const auto& boneAnim : Clip->BoneAnimations)
        {
            if (auto it = m_BoneNameToIndex.find(boneAnim.BoneName); it != m_BoneNameToIndex.end())
            {
                auto i = it->second;
                m_OneShotPose[i].Translation = AnimatedModel::SampleBonePosition(boneAnim.PositionKeys, sampleTime);
                m_OneShotPose[i].Rotation = AnimatedModel::SampleBoneRotation(boneAnim.RotationKeys, sampleTime);
                m_OneShotPose[i].Scale = AnimatedModel::SampleBoneScale(boneAnim.ScaleKeys, sampleTime);
            }
        }

        // Blend the one-shot pose into the base pose
        if (Additive)
        {
            // Capture a stable rest pose snapshot on the first additive frame.
            // Using basePose as rest every frame would reduce to linear interpolation
            // since the delta (m_OneShotPose - basePose) changes with basePose.
            if (m_AdditiveRestPose.size() != boneCount)
            {
                m_AdditiveRestPose.assign(basePose.begin(), basePose.end());
            }
            BlendUtils::AdditivePose(basePose, m_OneShotPose, m_AdditiveRestPose, effectiveWeight,
                                     BlendRootBone, parentIndices, basePose);
        }
        else if (BlendRootBone > 0)
        {
            BlendUtils::MaskedLerpPose(basePose, m_OneShotPose, effectiveWeight,
                                       BlendRootBone, parentIndices, basePose);
        }
        else
        {
            BlendUtils::LerpPose(basePose, m_OneShotPose, effectiveWeight, basePose);
        }
    }
} // namespace OloEngine::Animation
