#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <vector>

namespace OloEngine
{
    class CinematicSequence;

    /**
     * @brief Pure playback math for a cinematic sequence.
     *
     * No Scene / component dependency: every function takes plain time values
     * and returns the advanced playhead plus the events that fired during the
     * step. CinematicSystem owns the per-entity bookkeeping and applies the
     * sampled tracks to the Scene. Keeping this layer pure makes the timeline
     * semantics (loop wrap, clamp-on-end, edge-triggered events) directly
     * unit-testable — see CinematicPlayerTest.cpp.
     */
    namespace CinematicPlayer
    {
        struct AdvanceResult
        {
            f32 NewTime = 0.0f;
            bool Looped = false;       ///< playhead wrapped past the end this step
            bool JustFinished = false; ///< reached the end this step (non-looping / empty)
        };

        /// Advance `fromTime` by `dt * max(speed, 0)`, honouring `duration` and
        /// `loop`. Reverse playback (speed < 0) is treated as a hold for now.
        /// A non-positive `duration` finishes immediately.
        [[nodiscard]] AdvanceResult AdvanceTime(f32 fromTime, f32 dt, f32 speed, f32 duration, bool loop) noexcept;

        /// Append event identifiers whose key time lies in
        /// (lowerExclusive, upperInclusive], in ascending time order.
        void CollectEvents(const CinematicSequence& sequence, f32 lowerExclusive, f32 upperInclusive,
                           std::vector<std::string>& out);

        struct TickResult
        {
            f32 NewTime = 0.0f;
            bool Looped = false;
            bool JustFinished = false;
            std::vector<std::string> FiredEvents;
        };

        /// One playback step. `eventFloor` is the exclusive lower bound below
        /// which events have already fired (the playhead's prior position; pass
        /// a negative sentinel on the first tick so events at t == 0 fire).
        /// Handles loop wrap by firing the tail of the old lap and the head of
        /// the new one.
        [[nodiscard]] TickResult Tick(const CinematicSequence& sequence, f32 fromTime, f32 eventFloor,
                                      f32 dt, f32 speed, bool loop);
    } // namespace CinematicPlayer
} // namespace OloEngine
