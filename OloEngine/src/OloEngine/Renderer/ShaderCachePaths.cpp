#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderCachePaths.h"

#include <cstdlib>

namespace OloEngine::ShaderCachePaths
{
    namespace
    {
        [[nodiscard]] std::filesystem::path ResolveRoot()
        {
            // A developer override always wins — this is a single, trusted,
            // locally-set value, so std::getenv is fine here.
            if (const char* override = std::getenv("OLO_SHADER_CACHE_DIR"); override != nullptr && *override != '\0')
            {
                return std::filesystem::path(override);
            }
            if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData != nullptr && *localAppData != '\0')
            {
                return std::filesystem::path(localAppData) / "OloEngine" / "ShaderCache";
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
