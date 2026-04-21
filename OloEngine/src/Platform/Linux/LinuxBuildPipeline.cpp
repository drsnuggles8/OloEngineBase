// Linux implementation of BuildPipelinePlatform.
// Icon embedding in ELF binaries is not currently supported.

#include "OloEnginePCH.h"
#include "OloEngine/Build/BuildPipelinePlatform.h"

#ifdef OLO_PLATFORM_LINUX

namespace OloEngine::BuildPipelinePlatform
{
    bool EmbedCustomIcon(const std::filesystem::path& /*exePath*/,
                         const std::filesystem::path& /*iconPath*/,
                         std::string& outError)
    {
        outError = "Icon embedding is not supported on Linux";
        return false;
    }

} // namespace OloEngine::BuildPipelinePlatform

#endif // OLO_PLATFORM_LINUX
