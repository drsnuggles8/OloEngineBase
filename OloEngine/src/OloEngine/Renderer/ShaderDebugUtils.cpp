#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderDebugUtils.h"

// Forward declare the internal Utils functions
namespace OloEngine
{
    namespace Utils
    {
        extern void SetDisableShaderCache(bool disable);
        extern bool IsShaderCacheDisabled();
    } // namespace Utils
} // namespace OloEngine

namespace OloEngine
{
    namespace ShaderDebugUtils
    {
        void SetDisableShaderCache(bool disable)
        {
            Utils::SetDisableShaderCache(disable);
            if (disable)
            {
                OLO_CORE_WARN("Shader cache disabled for debugging - all shaders will be recompiled!");
            }
            else
            {
                OLO_CORE_INFO("Shader cache re-enabled");
            }
        }

        bool IsShaderCacheDisabled()
        {
            return Utils::IsShaderCacheDisabled();
        }
    } // namespace ShaderDebugUtils
} // namespace OloEngine
