#include "OloEnginePCH.h"
#include "MorphTargetSystem.h"
#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <functional>

namespace OloEngine
{
    namespace
    {
        // One track's weight at timeSeconds: held before the first and after
        // the last key, linear between keys.
        f32 SampleMorphTrack(const std::vector<std::pair<f64, f32>>& keys, f64 timeSeconds)
        {
            // Binary search for the first key with time >= 'timeSeconds'
            auto it = std::ranges::lower_bound(keys, timeSeconds, std::ranges::less{},
                                               &std::pair<f64, f32>::first);
            if (it == keys.end())
            {
                // Past all keyframes — use last value
                return keys.back().second;
            }
            if (it == keys.begin())
            {
                // Before first keyframe — use first value
                return it->second;
            }
            auto prev = std::prev(it);
            f64 dt = it->first - prev->first;
            if (dt < 1e-6)
            {
                return prev->second;
            }
            f32 t = static_cast<f32>((timeSeconds - prev->first) / dt);
            return prev->second + t * (it->second - prev->second);
        }
    } // namespace

    void MorphTargetSystem::SampleMorphKeyframes(
        const Ref<AnimationClip>& clip,
        f64 timeSeconds,
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

            morphComp.SetWeight(targetName, SampleMorphTrack(keys, timeSeconds));
        }
    }

    void MorphTargetSystem::SampleMorphKeyframesBlended(
        const Ref<AnimationClip>& from,
        f64 fromSeconds,
        const Ref<AnimationClip>& to,
        f64 toSeconds,
        f32 alpha,
        MorphTargetComponent& morphComp)
    {
        OLO_PROFILE_FUNCTION();

        const bool fromKeys = from && !from->MorphKeyframes.empty();
        const bool toKeys = to && !to->MorphKeyframes.empty();
        if (!fromKeys && !toKeys)
        {
            return;
        }
        if (!toKeys)
        {
            SampleMorphKeyframes(from, fromSeconds, morphComp);
            return;
        }
        if (!fromKeys)
        {
            SampleMorphKeyframes(to, toSeconds, morphComp);
            return;
        }

        alpha = std::clamp(alpha, 0.0f, 1.0f);
        const auto& fromTracks = from->GetMorphTracks();
        const auto& toTracks = to->GetMorphTracks();
        const auto sideWeight = [&morphComp](const auto& tracks, const std::string& name, f64 seconds)
        {
            const auto it = tracks.find(name);
            return (it != tracks.end() && !it->second.empty()) ? SampleMorphTrack(it->second, seconds)
                                                               : morphComp.GetWeight(name);
        };

        // Every weight is computed before any is written: a target only one
        // clip keys reads its CURRENT weight for the other side.
        std::vector<std::pair<const std::string*, f32>> blended;
        blended.reserve(fromTracks.size() + toTracks.size());
        for (const auto& [name, keys] : fromTracks)
        {
            blended.emplace_back(&name, glm::mix(sideWeight(fromTracks, name, fromSeconds),
                                                 sideWeight(toTracks, name, toSeconds), alpha));
        }
        for (const auto& [name, keys] : toTracks)
        {
            if (!fromTracks.contains(name))
            {
                blended.emplace_back(&name, glm::mix(sideWeight(fromTracks, name, fromSeconds),
                                                     sideWeight(toTracks, name, toSeconds), alpha));
            }
        }
        for (const auto& [name, weight] : blended)
        {
            morphComp.SetWeight(*name, weight);
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
