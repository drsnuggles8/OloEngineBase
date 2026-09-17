#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace OloEngine
{
    // Zero weights retain the legacy bend. Stiffness progressively replaces
    // the linear blade bend with an anchored quadratic bend.
    inline glm::vec4 SanitizeFoliageWind(f32 stiffness, f32 branch, f32 leaf, bool debug = false)
    {
        const auto weight = [](f32 value)
        { return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f; };
        return { weight(stiffness), weight(branch), weight(leaf), debug ? 1.0f : 0.0f };
    }

    inline f32 FoliageWindPhase(u64 id)
    {
        id ^= id >> 30;
        id *= 0xbf58476d1ce4e5b9ull;
        id ^= id >> 27;
        id *= 0x94d049bb133111ebull;
        id ^= id >> 31;
        return static_cast<f32>(id & 0xffffffu) * (6.2831853f / 16777216.0f);
    }

    // Shared with FoliageWind.glsl: global velocity is capped to length 20,
    // trunk response <= 2*strength, branch <= .35*strength, leaf <= .15*strength.
    inline f32 FoliageWindMaximumDisplacement(f32 strength, const glm::vec4& weights, f32 legacyEnvelope = 2.0f)
    {
        const f32 trunk = weights.x + weights.y + weights.z > 0.0f ? 2.0f : std::max(legacyEnvelope, 1.118034f);
        return std::abs(std::isfinite(strength) ? strength : 0.0f) *
               (trunk + 0.35f * weights.y + 0.15f * weights.z);
    }

    inline f32 FoliageImpostorBoundsRadius(f32 cardRadius)
    {
        return cardRadius * 1.41421356237f;
    }

    struct FoliageWindHistory
    {
        f32 Time = 0.0f;
        f32 PreviousTime = 0.0f;
        bool ResetPending = true;

        void Advance(f32 time, f32 previousTime)
        {
            Time = std::isfinite(time) ? time : 0.0f;
            PreviousTime = ResetPending || !std::isfinite(previousTime) || previousTime > Time ? Time : previousTime;
            ResetPending = false;
        }

        void Reset()
        {
            ResetPending = true;
        }
    };
} // namespace OloEngine
