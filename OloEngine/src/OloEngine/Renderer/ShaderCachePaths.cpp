#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderCachePaths.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Core/Environment.h"

namespace OloEngine::ShaderCachePaths
{
    namespace
    {
        [[nodiscard("Store this!")]] std::filesystem::path ResolveRoot()
        {
            // OLO_SHADER_CACHE_DIR is a registered lever (Core/DebugLevers.inl)
            // rather than a raw Env::Get, so it gets the startup log, the MCP
            // tool and DebugLevers.NoEngineCodeReadsAnOloVariableOutsideTheRegistry
            // coverage for free. LOCALAPPDATA below is a real OS environment
            // variable, not an engine lever, so it stays a direct Env::Get.
            if (const std::optional<std::string> dirOverride = Levers::ShaderCacheDir(); dirOverride)
            {
                return std::filesystem::path(*dirOverride);
            }
            if (const std::optional<std::string> localAppData = Env::Get("LOCALAPPDATA"); localAppData)
            {
                return std::filesystem::path(*localAppData) / "OloEngine" / "ShaderCache";
            }
            // No LOCALAPPDATA (non-Windows, or a stripped test environment) —
            // fall back to the historical in-tree location rather than failing
            // outright. This path is per-worktree again, but that only costs
            // performance (a cold recompile), never correctness.
            return std::filesystem::path("assets") / "cache" / "shader";
        }
    } // namespace

    const std::filesystem::path& Root()
    {
        static const std::filesystem::path s_Root = ResolveRoot();
        return s_Root;
    }
} // namespace OloEngine::ShaderCachePaths
