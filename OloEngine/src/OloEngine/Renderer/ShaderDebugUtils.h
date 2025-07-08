#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    namespace ShaderDebugUtils
    {
        // Debug function to disable shader caching for development
        void SetDisableShaderCache(bool disable);
        
        // Check if shader cache is disabled
        bool IsShaderCacheDisabled();
    }
}
