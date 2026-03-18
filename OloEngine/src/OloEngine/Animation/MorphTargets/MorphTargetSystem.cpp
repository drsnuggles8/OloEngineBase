#include "OloEnginePCH.h"
#include "MorphTargetSystem.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    void MorphTargetSystem::SampleMorphKeyframes(
        const Ref<AnimationClip>& clip,
        f32 time,
        MorphTargetComponent& morphComp)
    {
        OLO_PROFILE_FUNCTION();

        if (!clip || clip->MorphKeyframes.empty())
            return;

        const auto& keyframes = clip->MorphKeyframes;

        // Group keyframes by target name, find the two surrounding keyframes, and lerp
        // Since keyframes are stored flat, we iterate and pick the closest pair per target
        std::unordered_map<std::string, std::pair<const MorphTargetKeyframe*, const MorphTargetKeyframe*>> bracketMap;

        for (const auto& kf : keyframes)
        {
            auto& bracket = bracketMap[kf.TargetName];

            // Find the last keyframe at or before 'time' (lower bracket)
            if (kf.Time <= time)
            {
                if (!bracket.first || kf.Time > bracket.first->Time)
                    bracket.first = &kf;
            }

            // Find the first keyframe at or after 'time' (upper bracket)
            if (kf.Time >= time)
            {
                if (!bracket.second || kf.Time < bracket.second->Time)
                    bracket.second = &kf;
            }
        }

        for (const auto& [targetName, bracket] : bracketMap)
        {
            f32 weight = 0.0f;
            if (bracket.first && bracket.second)
            {
                if (bracket.first == bracket.second || std::abs(bracket.second->Time - bracket.first->Time) < 1e-6f)
                {
                    weight = bracket.first->Weight;
                }
                else
                {
                    f32 t = (time - bracket.first->Time) / (bracket.second->Time - bracket.first->Time);
                    weight = bracket.first->Weight + t * (bracket.second->Weight - bracket.first->Weight);
                }
            }
            else if (bracket.first)
            {
                weight = bracket.first->Weight;
            }
            else if (bracket.second)
            {
                weight = bracket.second->Weight;
            }

            morphComp.SetWeight(targetName, weight);
        }
    }

    bool MorphTargetSystem::EvaluateMorphTargets(
        MorphTargetComponent& morphComp,
        const std::vector<glm::vec3>& basePositions,
        const std::vector<glm::vec3>& baseNormals,
        std::vector<glm::vec3>& outPositions,
        std::vector<glm::vec3>& outNormals)
    {
        OLO_PROFILE_FUNCTION();

        if (!morphComp.MorphTargets || !morphComp.HasActiveWeights())
            return false;

        auto orderedWeights = morphComp.GetOrderedWeights();

        MorphTargetEvaluator::EvaluateCPU(
            basePositions, baseNormals,
            *morphComp.MorphTargets,
            orderedWeights,
            outPositions, outNormals);

        return true;
    }
} // namespace OloEngine
