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
#ifndef OLO_BUILD_GIT_DIRTY
#define OLO_BUILD_GIT_DIRTY 0
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

    bool IsWorkingTreeDirty()
    {
        return OLO_BUILD_GIT_DIRTY != 0;
    }

    std::string GetBuildId()
    {
        const std::string hash = GetGitHash();
        if (hash.empty() || hash == "unknown")
        {
            return GetEngineVersion();
        }

        std::string id = std::string(GetEngineVersion()) + "+" + hash;

        // GetGitHash() alone can't distinguish a build made from a dirty
        // working tree from one made from the exact commit it names — the
        // hash is the same either way. IsWorkingTreeDirty() is computed
        // independently in CMake (git diff-index, not a parse of
        // GetGitDescribe()'s text) precisely so a real tag that happens to
        // end in "-dirty" can't be mistaken for an actually-dirty checkout.
        if (IsWorkingTreeDirty())
        {
            id += "-dirty";
        }

        return id;
    }
} // namespace OloEngine::BuildInfo
