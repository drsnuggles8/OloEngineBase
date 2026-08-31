#include "OloEnginePCH.h"
#include "OloEngine/Core/BuildInfo.h"

// These are baked in as PRIVATE compile definitions on the OloEngine target
// (OloEngine/CMakeLists.txt, computed once in the root CMakeLists.txt) — the
// fallbacks below only matter for a TU built outside that target's definitions
// (e.g. a standalone compile invocation), not for a normal engine build.
#ifndef OLO_ENGINE_VERSION
#define OLO_ENGINE_VERSION "0.0.0"
#endif
#ifndef OLO_BUILD_GIT_HASH
#define OLO_BUILD_GIT_HASH "unknown"
#endif
#ifndef OLO_BUILD_GIT_DESCRIBE
#define OLO_BUILD_GIT_DESCRIBE "unknown"
#endif
#ifndef OLO_BUILD_TIMESTAMP
#define OLO_BUILD_TIMESTAMP "unknown"
#endif

namespace OloEngine::BuildInfo
{
    const char* GetEngineVersion()
    {
        return OLO_ENGINE_VERSION;
    }

    const char* GetGitHash()
    {
        return OLO_BUILD_GIT_HASH;
    }

    const char* GetGitDescribe()
    {
        return OLO_BUILD_GIT_DESCRIBE;
    }

    const char* GetBuildTimestamp()
    {
        return OLO_BUILD_TIMESTAMP;
    }

    std::string GetBuildId()
    {
        const std::string hash = GetGitHash();
        if (hash.empty() || hash == "unknown")
        {
            return GetEngineVersion();
        }
        return std::string(GetEngineVersion()) + "+" + hash;
    }
} // namespace OloEngine::BuildInfo
