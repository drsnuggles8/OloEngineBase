// Windows implementation of JoltCapturePlatform.

#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/JoltCapturePlatform.h"

#ifdef OLO_PLATFORM_WINDOWS

#include <cstdlib>

namespace OloEngine::JoltCapturePlatform
{
    CapturePaths GetDefaultCapturePaths()
    {
        CapturePaths result;
        const char* appData = std::getenv("APPDATA");
        if (appData != nullptr)
        {
            result.CapturesPath = std::filesystem::path(appData) / "OloEngine" / "Captures";
            result.ExpectedRoot = std::filesystem::path(appData);
        }
        else
        {
            result.CapturesPath = std::filesystem::current_path() / "Captures";
            result.ExpectedRoot = std::filesystem::current_path();
        }
        return result;
    }

    std::vector<std::filesystem::path> GetAllowedBasePaths()
    {
        std::vector<std::filesystem::path> result;
        const char* appData = std::getenv("APPDATA");
        if (appData != nullptr)
        {
            try
            {
                result.push_back(std::filesystem::weakly_canonical(std::filesystem::path(appData)));
            }
            catch (const std::filesystem::filesystem_error&)
            {
                // Ignore if canonicalization fails.
            }
        }
        return result;
    }

} // namespace OloEngine::JoltCapturePlatform

#endif // OLO_PLATFORM_WINDOWS
