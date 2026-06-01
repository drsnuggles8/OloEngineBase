#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

namespace OloEngine
{
    /// How a keyframe segment is interpolated towards the NEXT keyframe.
    /// The mode stored on key[i] governs the segment [key[i], key[i+1]].
    enum class CinematicInterp : u8
    {
        Constant = 0, ///< Hold key[i].Value until key[i+1] (step).
        Linear = 1,   ///< Straight lerp / slerp.
        EaseInOut = 2 ///< Smoothstep ease on the segment parameter.
    };

    // ---------------------------------------------------------------------
    // Keyframe channels. Each channel is an independently-keyed property
    // (e.g. an entity's translation). Keys are assumed sorted by Time
    // ascending; Evaluate() is robust to empty channels and degenerate
    // (zero-length) segments. Before the first key the channel holds the
    // first value; after the last key it holds the last value.
    //
    // These are pure CPU math types — no scene / GL dependency — so the
    // sampling contract is pinned by CinematicCurveTest.cpp.
    // ---------------------------------------------------------------------

    struct CinematicFloatKey
    {
        f32 Time = 0.0f;
        f32 Value = 0.0f;
        CinematicInterp Interp = CinematicInterp::Linear;
    };

    struct CinematicFloatChannel
    {
        std::vector<CinematicFloatKey> Keys;

        [[nodiscard]] bool HasKeys() const noexcept
        {
            return !Keys.empty();
        }
        /// Sample the channel at `time`. Returns `fallback` when there are no keys.
        [[nodiscard]] f32 Evaluate(f32 time, f32 fallback = 0.0f) const;
    };

    struct CinematicVec3Key
    {
        f32 Time = 0.0f;
        glm::vec3 Value{ 0.0f };
        CinematicInterp Interp = CinematicInterp::Linear;
    };

    struct CinematicVec3Channel
    {
        std::vector<CinematicVec3Key> Keys;

        [[nodiscard]] bool HasKeys() const noexcept
        {
            return !Keys.empty();
        }
        [[nodiscard]] glm::vec3 Evaluate(f32 time, const glm::vec3& fallback = glm::vec3(0.0f)) const;
    };

    struct CinematicQuatKey
    {
        f32 Time = 0.0f;
        glm::quat Value{ 1.0f, 0.0f, 0.0f, 0.0f };
        CinematicInterp Interp = CinematicInterp::Linear;
    };

    struct CinematicQuatChannel
    {
        std::vector<CinematicQuatKey> Keys;

        [[nodiscard]] bool HasKeys() const noexcept
        {
            return !Keys.empty();
        }
        [[nodiscard]] glm::quat Evaluate(f32 time, const glm::quat& fallback = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) const;
    };

    namespace CinematicCurve
    {
        /// Map a 0..1 segment parameter through the given interpolation mode.
        /// Constant => 0 (hold the left key), Linear => u, EaseInOut => smoothstep(u).
        [[nodiscard]] f32 ApplyInterp(CinematicInterp mode, f32 u) noexcept;
    } // namespace CinematicCurve
} // namespace OloEngine
