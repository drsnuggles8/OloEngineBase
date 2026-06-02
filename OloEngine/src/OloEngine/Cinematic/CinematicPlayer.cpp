#include "OloEnginePCH.h"
#include "OloEngine/Cinematic/CinematicPlayer.h"
#include "OloEngine/Cinematic/CinematicSequence.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace OloEngine::CinematicPlayer
{
    AdvanceResult AdvanceTime(f32 fromTime, f32 dt, f32 speed, f32 duration, bool loop) noexcept
    {
        OLO_PROFILE_FUNCTION();

        AdvanceResult result;

        if (duration <= 0.0f)
        {
            // Nothing to play — finish immediately and pin to the start.
            result.NewTime = 0.0f;
            result.JustFinished = true;
            return result;
        }

        const f32 effectiveSpeed = (speed > 0.0f) ? speed : 0.0f;
        const f32 raw = fromTime + dt * effectiveSpeed;

        if (raw < duration)
        {
            result.NewTime = raw;
            return result;
        }

        if (loop)
        {
            // Wrap into [0, duration). fmod(duration, duration) == 0, which is
            // the intended "land exactly on the start" behaviour.
            f32 wrapped = std::fmod(raw, duration);
            if (wrapped < 0.0f)
            {
                wrapped += duration;
            }
            result.NewTime = wrapped;
            result.Looped = true;
            return result;
        }

        result.NewTime = duration;
        result.JustFinished = true;
        return result;
    }

    void CollectEvents(const CinematicSequence& sequence, f32 lowerExclusive, f32 upperInclusive,
                       std::vector<std::string>& out)
    {
        OLO_PROFILE_FUNCTION();

        if (upperInclusive < lowerExclusive)
        {
            return;
        }

        // Gather (time, name) so events from different tracks fire in global
        // time order rather than track-declaration order.
        std::vector<std::pair<f32, const std::string*>> hits;
        for (const auto& track : sequence.EventTracks)
        {
            for (const auto& key : track.Keys)
            {
                if (key.Time > lowerExclusive && key.Time <= upperInclusive)
                {
                    hits.emplace_back(key.Time, &key.Name);
                }
            }
        }

        std::stable_sort(hits.begin(), hits.end(),
                         [](const auto& a, const auto& b)
                         { return a.first < b.first; });

        out.reserve(out.size() + hits.size());
        for (const auto& [time, name] : hits)
        {
            out.push_back(*name);
        }
    }

    TickResult Tick(const CinematicSequence& sequence, f32 fromTime, f32 eventFloor, f32 dt, f32 speed, bool loop)
    {
        OLO_PROFILE_FUNCTION();

        TickResult result;

        const f32 duration = sequence.GetEffectiveDuration();
        const AdvanceResult adv = AdvanceTime(fromTime, dt, speed, duration, loop);

        result.NewTime = adv.NewTime;
        result.Looped = adv.Looped;
        result.JustFinished = adv.JustFinished;

        if (duration <= 0.0f)
        {
            return result; // no timeline to fire events along
        }

        if (adv.Looped)
        {
            // Tail of the lap we just left, then the head of the new lap. The
            // negative sentinel lets events authored at exactly t == 0 re-fire
            // each loop.
            CollectEvents(sequence, eventFloor, duration, result.FiredEvents);
            CollectEvents(sequence, -1.0f, adv.NewTime, result.FiredEvents);
        }
        else
        {
            CollectEvents(sequence, eventFloor, adv.NewTime, result.FiredEvents);
        }

        return result;
    }
} // namespace OloEngine::CinematicPlayer
