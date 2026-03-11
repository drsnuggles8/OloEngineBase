#pragma once

#include "OloEngine/Core/Base.h"

#include <string>

namespace OloEngine
{
    struct StreamingSettings
    {
        bool Enabled = false;
        f32 DefaultLoadRadius = 200.0f;
        f32 DefaultUnloadRadius = 250.0f;
        u32 MaxLoadedRegions = 16;
        std::string RegionDirectory; // Relative to scene file
    };
} // namespace OloEngine
