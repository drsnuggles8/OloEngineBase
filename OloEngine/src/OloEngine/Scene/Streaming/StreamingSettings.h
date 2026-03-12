#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace OloEngine
{
    struct StreamingSettings
    {
        bool Enabled = false;
        f32 DefaultLoadRadius = 200.0f;
        f32 DefaultUnloadRadius = 250.0f;
        u32 MaxLoadedRegions = 16;
        std::string RegionDirectory; // Relative to working directory
    };

    inline void SanitizeStreamingSettings(StreamingSettings& ss)
    {
        if (!std::isfinite(ss.DefaultLoadRadius))
            ss.DefaultLoadRadius = 200.0f;
        if (!std::isfinite(ss.DefaultUnloadRadius))
            ss.DefaultUnloadRadius = 250.0f;

        ss.DefaultLoadRadius = std::max(ss.DefaultLoadRadius, 1.0f);
        ss.DefaultUnloadRadius = std::max(ss.DefaultUnloadRadius, ss.DefaultLoadRadius + 1.0f);
        ss.MaxLoadedRegions = std::max(ss.MaxLoadedRegions, 1u);
    }
} // namespace OloEngine
