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
        Constant = 0,  ///< Hold key[i].Value until key[i+1] (step).
        Linear = 1,    ///< Straight lerp / slerp.
        EaseInOut = 2, ///< Smoothstep ease on the segment parameter.
        /// Cubic-Hermite ease shaped by per-key in/out tangent handles. The
        /// segment leaving key[i] uses key[i].OutTangent and key[i+1].InTangent
        /// (slopes of the normalized 0..1 ease curve). Zero tangents reproduce
        /// EaseInOut (smoothstep); unit tangents reproduce Linear. The shaped
        /// blend is applied uniformly to the value the same way the other modes
        /// are, so tangents control a segment's *timing* (ease / overshoot), not
        /// a curved spatial path between keys.
        Bezier = 3
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
        // Tangent handles, used only when this key (OutTangent) or the previous
        // key (InTangent) drives a CinematicInterp::Bezier segment. Slopes of the
        // normalized 0..1 ease curve; 0 => smoothstep (see CinematicInterp::Bezier).
        f32 InTangent = 0.0f;
        f32 OutTangent = 0.0f;
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
        // See CinematicFloatKey — the tangents shape the segment's ease uniformly
        // across x/y/z (they do not bend the straight path between key values).
        f32 InTangent = 0.0f;
        f32 OutTangent = 0.0f;
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
        // See CinematicFloatKey — the tangents ease the slerp parameter; the
        // shaped blend is clamped to [0,1] before slerp so rotations never whip
        // past their endpoints on a steep handle.
        f32 InTangent = 0.0f;
        f32 OutTangent = 0.0f;
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
        /// Bezier is treated as Linear here because tangents are not available at
        /// this call site — segment evaluation routes Bezier through BezierBlend
        /// instead (it owns both endpoints' tangents).
        [[nodiscard]] f32 ApplyInterp(CinematicInterp mode, f32 u) noexcept;

        /// Cubic-Hermite ease on the normalized segment parameter `u` in [0,1].
        /// `outTangent` is the slope leaving the left key, `inTangent` the slope
        /// arriving at the right key, both as slopes of the unit (0->1) ease
        /// curve. (0, 0) reproduces smoothstep (EaseInOut); (1, 1) reproduces a
        /// straight line (Linear). Large tangents may push the result outside
        /// [0,1] (anticipation / overshoot) — callers that cannot tolerate that
        /// (e.g. quaternion slerp) clamp the result.
        [[nodiscard]] f32 BezierBlend(f32 u, f32 outTangent, f32 inTangent) noexcept;
    } // namespace CinematicCurve
} // namespace OloEngine
