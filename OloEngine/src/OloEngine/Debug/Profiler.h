#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    /**
     * @brief Simple profiler namespace placeholder
     * 
     * This provides basic profiling functionality for the asset system.
     * Currently just wraps the existing Instrumentor functionality.
     */
    namespace Profiler
    {
        // For now, just use the existing profiling macros
        // In the future, this could provide more specialized asset profiling
    }
}

// Simple profiler macros for asset loading
#define OLO_ASSET_PROFILE_SCOPE(name) OLO_PROFILE_SCOPE(name)
#define OLO_ASSET_PROFILE_FUNCTION() OLO_PROFILE_FUNCTION()
