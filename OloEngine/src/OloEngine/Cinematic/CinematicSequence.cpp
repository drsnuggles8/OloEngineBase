#include "OloEnginePCH.h"
#include "OloEngine/Cinematic/CinematicSequence.h"

#include <algorithm>

namespace OloEngine
{
    namespace
    {
        template<typename KeyVec>
        void AccumulateMaxTime(const KeyVec& keys, f32& maxTime) noexcept
        {
            if (!keys.empty())
            {
                maxTime = std::max(maxTime, keys.back().Time);
            }
        }
    } // namespace

    f32 CinematicSequence::ComputeDuration() const
    {
        // Keys are authored sorted, so the last key carries the channel's max
        // time. Take the max across every channel of every track.
        f32 maxTime = 0.0f;

        for (const auto& track : TransformTracks)
        {
            AccumulateMaxTime(track.Translation.Keys, maxTime);
            AccumulateMaxTime(track.Rotation.Keys, maxTime);
            AccumulateMaxTime(track.Scale.Keys, maxTime);
        }
        for (const auto& track : CameraTracks)
        {
            AccumulateMaxTime(track.Position.Keys, maxTime);
            AccumulateMaxTime(track.Rotation.Keys, maxTime);
            AccumulateMaxTime(track.VerticalFovRadians.Keys, maxTime);
        }
        for (const auto& track : VisibilityTracks)
        {
            AccumulateMaxTime(track.Keys, maxTime);
        }
        for (const auto& track : EventTracks)
        {
            AccumulateMaxTime(track.Keys, maxTime);
        }

        return maxTime;
    }
} // namespace OloEngine
