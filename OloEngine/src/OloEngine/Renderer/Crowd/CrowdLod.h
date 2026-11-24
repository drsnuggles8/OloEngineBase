#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <array>

namespace OloEngine
{
    struct CrowdVisibilityCounts
    {
        u32 Members = 0;
        std::array<u32, 3> Visible{};
    };

    enum class CrowdLod : u32
    {
        Near,
        Mid,
        Far,
        Unseen
    };

    // CPU reference and shadow-view policy. The GPU classification uses the
    // same thresholds. Test both bounds so a camera cut can cross two tiers.
    [[nodiscard]] inline CrowdLod SelectCrowdLod(f32 diameter, CrowdLod previous, f32 nearPixels, f32 farPixels, f32 hysteresis)
    {
        const f32 h = previous == CrowdLod::Unseen ? 0.0f : std::clamp(hysteresis, 0.0f, 0.45f);
        const f32 farThreshold = std::max(farPixels, 1.0f);
        const f32 nearThreshold = std::max(nearPixels, farThreshold);
        if (diameter >= nearThreshold * (previous == CrowdLod::Near ? 1.0f - h : 1.0f + h))
            return CrowdLod::Near;
        if (diameter >= farThreshold * (previous == CrowdLod::Far ? 1.0f + h : 1.0f - h))
            return CrowdLod::Mid;
        return CrowdLod::Far;
    }
} // namespace OloEngine
