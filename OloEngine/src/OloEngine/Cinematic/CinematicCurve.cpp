#include "OloEnginePCH.h"
#include "OloEngine/Cinematic/CinematicCurve.h"

#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
    namespace
    {
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
            case CinematicInterp::Linear:
            default:
                return u;
        }
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
        const f32 blend = CinematicCurve::ApplyInterp(Keys[i].Interp, u);
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
        const f32 blend = CinematicCurve::ApplyInterp(Keys[i].Interp, u);
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
            return glm::normalize(Keys.front().Value);
        }
        if (time >= Keys.back().Time)
        {
            return glm::normalize(Keys.back().Value);
        }
        f32 u = 0.0f;
        const sizet i = FindSegment(Keys, time, u);
        const f32 blend = CinematicCurve::ApplyInterp(Keys[i].Interp, u);
        // glm::slerp takes the shortest arc; normalize endpoints so authored
        // (possibly denormal) quats don't skew the result.
        return glm::slerp(glm::normalize(Keys[i].Value), glm::normalize(Keys[i + 1].Value), blend);
    }
} // namespace OloEngine
