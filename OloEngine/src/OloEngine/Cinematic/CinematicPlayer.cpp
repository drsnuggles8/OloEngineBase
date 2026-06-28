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

        // Non-finite inputs (a corrupt key time feeding `duration`, a NaN `dt`,
        // etc.) must never reach std::fmod or the lap-count cast below — both
        // produce NaN / undefined behaviour. Hold the playhead at the last good
        // value instead (no movement, no finish). speed is already finite-guarded
        // on every external write path, but this keeps the pure function total.
        if (!std::isfinite(fromTime) || !std::isfinite(dt) || !std::isfinite(speed) || !std::isfinite(duration))
        {
            result.NewTime = std::isfinite(fromTime) ? fromTime : 0.0f;
            return result;
        }

        if (duration <= 0.0f)
        {
            // Nothing to play — finish immediately and pin to the start.
            result.NewTime = 0.0f;
            result.JustFinished = true;
            return result;
        }

        const f32 raw = fromTime + dt * speed;

        if (speed > 0.0f)
        {
            // Forward: advance toward `duration`.
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
                // Number of full laps crossed this step (>=1). >1 when a single dt
                // spans the whole timeline more than once (e.g. a frame hitch).
                result.LoopCount = static_cast<u32>(raw / duration);
                return result;
            }

            result.NewTime = duration;
            result.JustFinished = true;
            return result;
        }

        if (speed < 0.0f)
        {
            // Reverse: advance toward 0. Mirror of the forward branch — the
            // playhead decreases, finishes at 0 (non-looping), and wraps
            // 0 -> duration when looping.
            if (raw > 0.0f)
            {
                result.NewTime = raw;
                return result;
            }

            if (loop)
            {
                // Wrap into (0, duration]. fmod of a non-positive `raw` against a
                // positive `duration` always lands in (-duration, 0], so the
                // unconditional shift up by duration maps it into (0, duration] —
                // mirroring the forward wrap and mapping an exact landing on 0 to
                // `duration` (the start of a backward lap).
                const f32 wrapped = std::fmod(raw, duration) + duration;
                result.NewTime = wrapped;
                result.Looped = true;
                // Full laps crossed this step (>=1). floor(-raw/duration)+1
                // mirrors the forward floor(raw/duration): a single backward
                // step that just crosses 0 is one lap, and an exact landing on
                // a -k*duration boundary counts the lap it completes.
                result.LoopCount = static_cast<u32>(std::floor(-raw / duration)) + 1u;
                return result;
            }

            result.NewTime = 0.0f;
            result.JustFinished = true;
            return result;
        }

        // speed == 0: hold the playhead — no movement, no finish. (NaN speed was
        // already handled by the finite guard above.)
        result.NewTime = fromTime;
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

    void CollectEventsReverse(const CinematicSequence& sequence, f32 lowerInclusive, f32 upperExclusive,
                              std::vector<std::string>& out)
    {
        OLO_PROFILE_FUNCTION();

        if (upperExclusive <= lowerInclusive)
        {
            return;
        }

        // Reverse mirror of CollectEvents: a backward step lands on the lower
        // bound (inclusive) and leaves the upper bound (exclusive, already fired
        // when the playhead last sat there), and the events fire in descending
        // time order — the order the receding playhead crosses them.
        std::vector<std::pair<f32, const std::string*>> hits;
        for (const auto& track : sequence.EventTracks)
        {
            for (const auto& key : track.Keys)
            {
                if (key.Time >= lowerInclusive && key.Time < upperExclusive)
                {
                    hits.emplace_back(key.Time, &key.Name);
                }
            }
        }

        std::stable_sort(hits.begin(), hits.end(),
                         [](const auto& a, const auto& b)
                         { return a.first > b.first; });

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

        if (speed < 0.0f)
        {
            // Reverse traversal: the playhead recedes, so events fire in
            // descending time order and the half-open window flips — events
            // land on the new (lower) playhead and leave the old (higher) one.
            if (adv.Looped)
            {
                // Mirror of the forward wrap below, walked the other way: the
                // head of the lap we're leaving (descent fromTime -> 0), every
                // full middle lap crossed by a huge dt, then the tail of the lap
                // we wrapped into (descent duration -> newTime). `duration + 1`
                // as the exclusive upper bound mirrors the forward `-1` lower
                // sentinel — it pulls the top-of-timeline event (t == duration)
                // into the window the same way `-1` pulls in t == 0.
                CollectEventsReverse(sequence, 0.0f, fromTime, result.FiredEvents);

                constexpr u32 kMaxMiddleLaps = 64;
                const u32 middleLaps = (adv.LoopCount > 1) ? std::min(adv.LoopCount - 1, kMaxMiddleLaps) : 0;
                for (u32 lap = 0; lap < middleLaps; ++lap)
                {
                    CollectEventsReverse(sequence, 0.0f, duration + 1.0f, result.FiredEvents);
                }

                CollectEventsReverse(sequence, adv.NewTime, duration + 1.0f, result.FiredEvents);
            }
            else
            {
                CollectEventsReverse(sequence, adv.NewTime, fromTime, result.FiredEvents);
            }
            return result;
        }

        if (adv.Looped)
        {
            // Tail of the lap we just left, every full lap crossed in between,
            // then the head of the new lap. The negative sentinel lets events
            // authored at exactly t == 0 re-fire each loop. Each *completed*
            // lap fires the whole timeline's events; a single huge dt (frame
            // hitch) can cross several. The middle-lap count is capped so a
            // pathological dt can't spin firing the same events unboundedly.
            CollectEvents(sequence, eventFloor, duration, result.FiredEvents);

            constexpr u32 kMaxMiddleLaps = 64;
            const u32 middleLaps = (adv.LoopCount > 1) ? std::min(adv.LoopCount - 1, kMaxMiddleLaps) : 0;
            for (u32 lap = 0; lap < middleLaps; ++lap)
            {
                CollectEvents(sequence, -1.0f, duration, result.FiredEvents);
            }

            CollectEvents(sequence, -1.0f, adv.NewTime, result.FiredEvents);
        }
        else
        {
            CollectEvents(sequence, eventFloor, adv.NewTime, result.FiredEvents);
        }

        return result;
    }
} // namespace OloEngine::CinematicPlayer
