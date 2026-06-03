#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace OloEngine
{
    // ---------------------------------------------------------------------
    // Pure keyframe-editing operations on the cinematic key vectors.
    //
    // CinematicCurve / CinematicVisibilityTrack / CinematicEventTrack all
    // evaluate keys under a single invariant: the key vector is sorted by
    // `Time` ascending (see CinematicSequenceSerializer::SortKeysByTime and
    // CinematicCurve::Evaluate). The timeline editor mutates those vectors
    // live — dragging a key changes its time, inserting/deleting adds or
    // removes keys — and every such edit must preserve the sort invariant or
    // playback / scrubbing reads garbage.
    //
    // These helpers are the single place that invariant is enforced. They are
    // deliberately Scene/GL-free templates over any vector whose element has a
    // `f32 Time` member (CinematicFloatKey, CinematicVec3Key, CinematicQuatKey,
    // CinematicVisibilityKey, CinematicEventKey), so the contract is unit-
    // testable in isolation — see CinematicEditTest.cpp. The panel is left as
    // pure UI on top.
    // ---------------------------------------------------------------------
    namespace CinematicEdit
    {
        /// Insert `key` into the already-sorted `keys`, keeping it sorted by
        /// Time ascending. A key whose Time ties existing keys is placed
        /// *after* them (upper-bound), so a freshly-inserted key never reorders
        /// equal-time neighbours. Returns the index the key landed at.
        template<typename KeyVec>
        sizet InsertKeySorted(KeyVec& keys, typename KeyVec::value_type key)
        {
            const auto pos = std::upper_bound(keys.begin(), keys.end(), key,
                                              [](const auto& a, const auto& b)
                                              { return a.Time < b.Time; });
            const auto it = keys.insert(pos, std::move(key));
            return static_cast<sizet>(std::distance(keys.begin(), it));
        }

        /// Move `keys[index]` to `newTime` and restore the sort order. The time
        /// is clamped to >= 0; a non-finite `newTime` is rejected (the key is
        /// left untouched). Returns the moved key's new index — selection in the
        /// editor follows the key as it slides past its neighbours. An
        /// out-of-range `index` is a no-op that returns `index` unchanged.
        template<typename KeyVec>
        sizet MoveKeyTime(KeyVec& keys, sizet index, f32 newTime)
        {
            if (index >= keys.size() || !std::isfinite(newTime))
            {
                return index;
            }
            newTime = std::max(0.0f, newTime);

            // Erase + re-insert rather than mutate-in-place-then-sort: this keeps
            // the upper-bound tie rule consistent with InsertKeySorted and yields
            // the new index directly.
            typename KeyVec::value_type moved = keys[index];
            moved.Time = newTime;
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(index));
            return InsertKeySorted(keys, std::move(moved));
        }

        /// Remove `keys[index]` if in range. Returns true when a key was erased.
        template<typename KeyVec>
        bool RemoveKeyAt(KeyVec& keys, sizet index)
        {
            if (index >= keys.size())
            {
                return false;
            }
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }

        /// True when `keys` is sorted by Time ascending (the playback invariant).
        /// Used by tests and as a cheap debug assert after bulk edits.
        template<typename KeyVec>
        [[nodiscard]] bool IsSortedByTime(const KeyVec& keys) noexcept
        {
            return std::is_sorted(keys.begin(), keys.end(),
                                  [](const auto& a, const auto& b)
                                  { return a.Time < b.Time; });
        }
    } // namespace CinematicEdit
} // namespace OloEngine
