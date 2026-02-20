#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <array>

namespace OloEngine
{
    // Piecewise linear curve with up to 8 keys, for animating particle properties over lifetime
    struct ParticleCurve
    {
        struct Key
        {
            f32 Time = 0.0f; // Normalized 0..1
            f32 Value = 0.0f;
        };

        std::array<Key, 8> Keys{};
        u32 KeyCount = 0;

        ParticleCurve() = default;

        // Convenience: constant value
        explicit ParticleCurve(f32 constant)
        {
            Keys[0] = { 0.0f, constant };
            Keys[1] = { 1.0f, constant };
            KeyCount = 2;
        }

        // Convenience: linear ramp
        ParticleCurve(f32 start, f32 end)
        {
            Keys[0] = { 0.0f, start };
            Keys[1] = { 1.0f, end };
            KeyCount = 2;
        }

        [[nodiscard]] f32 Evaluate(f32 t) const
        {
            if (KeyCount == 0)
            {
                return 0.0f;
            }
            if (KeyCount == 1 || t <= Keys[0].Time)
            {
                return Keys[0].Value;
            }
            if (t >= Keys[KeyCount - 1].Time)
            {
                return Keys[KeyCount - 1].Value;
            }

            // Binary search for the segment containing t
            // Find first key with Time > t, then the segment is [i-1, i]
            u32 lo = 0;
            u32 hi = KeyCount;
            while (lo < hi)
            {
                u32 mid = (lo + hi) / 2;
                if (Keys[mid].Time <= t)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            // lo is now the index of the first key with Time > t
            u32 i = lo - 1;
            f32 segLen = Keys[i + 1].Time - Keys[i].Time;
            if (segLen <= 0.0f)
            {
                return Keys[i].Value;
            }
            f32 alpha = (t - Keys[i].Time) / segLen;
            return Keys[i].Value + alpha * (Keys[i + 1].Value - Keys[i].Value);
        }
    };

    // Curve for vec4 (e.g., color over lifetime)
    struct ParticleCurve4
    {
        ParticleCurve R;
        ParticleCurve G;
        ParticleCurve B;
        ParticleCurve A;

        ParticleCurve4() = default;

        explicit ParticleCurve4(const glm::vec4& constant)
            : R(constant.r), G(constant.g), B(constant.b), A(constant.a)
        {
        }

        ParticleCurve4(const glm::vec4& start, const glm::vec4& end)
            : R(start.r, end.r), G(start.g, end.g), B(start.b, end.b), A(start.a, end.a)
        {
        }

        [[nodiscard]] glm::vec4 Evaluate(f32 t) const
        {
            return { R.Evaluate(t), G.Evaluate(t), B.Evaluate(t), A.Evaluate(t) };
        }
    };
} // namespace OloEngine
