#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderCachePaths.h"

#include "OloEngine/Core/Environment.h"

namespace OloEngine::ShaderCachePaths
{
    namespace
    {
        [[nodiscard("Store this!")]] std::filesystem::path ResolveRoot()
        {
            // Routed through OloEngine::Env rather than a raw std::getenv call
            // — see that header's rationale (cpp:S990, one suppression instead
            // of one per call site). A developer override always wins.
            if (const std::optional<std::string> dirOverride = Env::Get("OLO_SHADER_CACHE_DIR"); dirOverride)
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
