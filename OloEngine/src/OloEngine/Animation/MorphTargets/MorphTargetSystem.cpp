#include "OloEnginePCH.h"
#include "MorphTargetSystem.h"
#include "OloEngine/Core/Log.h"

#include <algorithm>

namespace OloEngine
{
    void MorphTargetSystem::SampleMorphKeyframes(
        const Ref<AnimationClip>& clip,
        f64 time,
        MorphTargetComponent& morphComp)
    {
        OLO_PROFILE_FUNCTION();

        if (!clip || clip->MorphKeyframes.empty())
            return;

        const auto& tracks = clip->GetMorphTracks();

        for (const auto& [targetName, keys] : tracks)
        {
            if (keys.empty())
                continue;

            // Binary search for the first key with time >= 'time'
            auto it = std::lower_bound(keys.begin(), keys.end(), time,
                                       [](const std::pair<f64, f32>& key, f64 t)
                                       { return key.first < t; });

            f32 weight = 0.0f;
            if (it == keys.end())
            {
                // Past all keyframes — use last value
                weight = keys.back().second;
            }
            else if (it == keys.begin())
            {
                // Before first keyframe — use first value
                weight = it->second;
            }
            else
            {
                auto prev = std::prev(it);
                f64 dt = it->first - prev->first;
                if (dt < 1e-6)
                {
                    weight = prev->second;
                }
                else
                {
                    f32 t = static_cast<f32>((time - prev->first) / dt);
                    weight = prev->second + t * (it->second - prev->second);
                }
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
