#include "OloEnginePCH.h"
#include "OloEngine/Cinematic/CinematicCurve.h"

#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
    namespace
    {
        // Guard glm::normalize against zero / denormal quaternions (||q||^2 below
        // EPS), which would divide by zero and propagate NaN into transforms.
        // A degenerate key falls back to a safe (identity-ish) quaternion.
        glm::quat SafeNormalizeQuat(const glm::quat& q, const glm::quat& fallback) noexcept
        {
            constexpr f32 kQuatLenEps = 1e-8f;
            return (glm::dot(q, q) > kQuatLenEps) ? glm::normalize(q) : fallback;
        }

        // Find the segment [i, i+1] bracketing `time` for a sorted key vector.
        // Returns the index `i` of the left key and writes the normalized
        // segment parameter `u` in [0,1]. Callers guarantee size() >= 2 and
        // Keys.front().Time < time < Keys.back().Time before calling.
        template<typename KeyVec>
        sizet FindSegment(const KeyVec& keys, f32 time, f32& outU) noexcept
        {
            // Linear scan — cinematic channels hold a handful of keys, so a
            // branch-predictable scan beats a binary search's setup cost.
            sizet i = 0;
            const sizet last = keys.size() - 1;
            while (i < last && keys[i + 1].Time <= time)
            {
                ++i;
            }
            const f32 t0 = keys[i].Time;
            const f32 t1 = keys[i + 1].Time;
            const f32 span = t1 - t0;
            outU = (span > 1e-8f) ? glm::clamp((time - t0) / span, 0.0f, 1.0f) : 0.0f;
            return i;
        }

        // Blend factor for the segment [left, right] at normalized parameter `u`.
        // The left key's Interp governs the segment (matching the documented
        // contract); a Bezier segment reads the left key's OutTangent and the
        // right key's InTangent, every other mode ignores tangents. All three
        // keyed key types expose Interp / InTangent / OutTangent, so one template
        // serves the float / vec3 / quat channels.
        template<typename KeyT>
        f32 SegmentBlend(const KeyT& left, const KeyT& right, f32 u) noexcept
        {
            if (left.Interp == CinematicInterp::Bezier)
            {
                return CinematicCurve::BezierBlend(u, left.OutTangent, right.InTangent);
            }
            return CinematicCurve::ApplyInterp(left.Interp, u);
        }
    } // namespace

    f32 CinematicCurve::ApplyInterp(CinematicInterp mode, f32 u) noexcept
    {
        switch (mode)
        {
            case CinematicInterp::Constant:
                return 0.0f; // hold the left key
            case CinematicInterp::EaseInOut:
                // Smoothstep: 3u^2 - 2u^3.
                return u * u * (3.0f - 2.0f * u);
            case CinematicInterp::Bezier: // no tangents here — see BezierBlend
            case CinematicInterp::Linear:
            default:
                return u;
        }
    }

    f32 CinematicCurve::BezierBlend(f32 u, f32 outTangent, f32 inTangent) noexcept
    {
        // Cubic-Hermite basis from p0 = 0 to p1 = 1 over the unit interval:
        //   H(u) = h00*0 + h10*m0 + h01*1 + h11*m1
        // with m0 = outTangent (slope at u=0), m1 = inTangent (slope at u=1).
        // The p0 term drops out (p0 = 0), so only h01 + h10*m0 + h11*m1 remain.
        const f32 u2 = u * u;
        const f32 u3 = u2 * u;
        const f32 h10 = u3 - 2.0f * u2 + u;     // weights m0
        const f32 h01 = -2.0f * u3 + 3.0f * u2; // weights p1 (== 1)
        const f32 h11 = u3 - u2;                // weights m1
        return h01 + h10 * outTangent + h11 * inTangent;
    }

    f32 CinematicFloatChannel::Evaluate(f32 time, f32 fallback) const
    {
        if (Keys.empty())
        {
            return fallback;
        }
        if (time <= Keys.front().Time)
        {
            return Keys.front().Value;
        }
        if (time >= Keys.back().Time)
        {
            return Keys.back().Value;
        }
        f32 u = 0.0f;
        const sizet i = FindSegment(Keys, time, u);
        const f32 blend = SegmentBlend(Keys[i], Keys[i + 1], u);
        return glm::mix(Keys[i].Value, Keys[i + 1].Value, blend);
    }

    glm::vec3 CinematicVec3Channel::Evaluate(f32 time, const glm::vec3& fallback) const
    {
        if (Keys.empty())
        {
            return fallback;
        }
        if (time <= Keys.front().Time)
        {
            return Keys.front().Value;
        }
        if (time >= Keys.back().Time)
        {
            return Keys.back().Value;
        }
        f32 u = 0.0f;
        const sizet i = FindSegment(Keys, time, u);
        const f32 blend = SegmentBlend(Keys[i], Keys[i + 1], u);
        return glm::mix(Keys[i].Value, Keys[i + 1].Value, blend);
    }

    glm::quat CinematicQuatChannel::Evaluate(f32 time, const glm::quat& fallback) const
    {
        if (Keys.empty())
        {
            return fallback;
        }
        if (time <= Keys.front().Time)
        {
            return SafeNormalizeQuat(Keys.front().Value, fallback);
        }
        if (time >= Keys.back().Time)
        {
            return SafeNormalizeQuat(Keys.back().Value, fallback);
        }
        f32 u = 0.0f;
        const sizet i = FindSegment(Keys, time, u);
        // A Bezier ease can overshoot [0,1]; clamp before slerp so a steep tangent
        // can't extrapolate the rotation past its endpoints (the other modes stay
        // within [0,1] already, so the clamp is a no-op for them).
        const f32 blend = glm::clamp(SegmentBlend(Keys[i], Keys[i + 1], u), 0.0f, 1.0f);
        // glm::slerp takes the shortest arc; normalize endpoints so authored
        // (possibly denormal) quats don't skew the result, and guard against
        // zero-length keys so a degenerate quat can't seed a NaN.
        return glm::slerp(SafeNormalizeQuat(Keys[i].Value, fallback),
                          SafeNormalizeQuat(Keys[i + 1].Value, fallback), blend);
    }
} // namespace OloEngine
