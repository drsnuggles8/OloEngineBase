// Linux implementation of JoltCapturePlatform.

#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/JoltCapturePlatform.h"

#ifdef OLO_PLATFORM_LINUX

#include <cstdlib>

namespace OloEngine::JoltCapturePlatform
{
    namespace
    {
        std::filesystem::path GetUserDataRoot()
        {
            if (const char* xdgDataHome = std::getenv("XDG_DATA_HOME"); xdgDataHome != nullptr)
            {
                return std::filesystem::path(xdgDataHome);
            }
            if (const char* home = std::getenv("HOME"); home != nullptr)
            {
                return std::filesystem::path(home) / ".local" / "share";
            }
            return {};
        }
    } // namespace

    CapturePaths GetDefaultCapturePaths()
    {
        CapturePaths result;
        const std::filesystem::path dataRoot = GetUserDataRoot();
        if (!dataRoot.empty())
        {
            result.CapturesPath = dataRoot / "OloEngine" / "Captures";
            result.ExpectedRoot = dataRoot;
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

        if (const char* xdgDataHome = std::getenv("XDG_DATA_HOME"); xdgDataHome != nullptr)
        {
            try
            {
                result.push_back(std::filesystem::weakly_canonical(std::filesystem::path(xdgDataHome)));
            }
            catch (const std::filesystem::filesystem_error&)
            {
                // Ignore if canonicalization fails.
            }
        }

        if (const char* home = std::getenv("HOME"); home != nullptr)
        {
            try
            {
                result.push_back(std::filesystem::weakly_canonical(std::filesystem::path(home) / ".local" / "share"));
            }
            catch (const std::filesystem::filesystem_error&)
            {
                // Ignore if canonicalization fails.
            }
        }

        return result;
    }

} // namespace OloEngine::JoltCapturePlatform

#endif // OLO_PLATFORM_LINUX
