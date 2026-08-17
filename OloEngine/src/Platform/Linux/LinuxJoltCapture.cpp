// Linux implementation of JoltCapturePlatform.

#include "OloEnginePCH.h"
#include <optional>
#include "OloEngine/Core/Environment.h"
#include "OloEngine/Physics3D/JoltCapturePlatform.h"

#ifdef OLO_PLATFORM_LINUX

#include <cstdlib>

namespace OloEngine::JoltCapturePlatform
{
    namespace
    {
        std::filesystem::path GetUserDataRoot()
        {
            // OS-provided, not an engine knob: this IS the documented way to
            // find the user data directory on Linux.
            if (const std::optional<std::string> xdgDataHome = Env::Get("XDG_DATA_HOME"))
            {
                return std::filesystem::path(*xdgDataHome);
            }
            if (const std::optional<std::string> home = Env::Get("HOME"))
            {
                return std::filesystem::path(*home) / ".local" / "share";
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

        if (const std::optional<std::string> xdgDataHome = Env::Get("XDG_DATA_HOME"))
        {
            try
            {
                result.push_back(std::filesystem::weakly_canonical(std::filesystem::path(*xdgDataHome)));
            }
            catch (const std::filesystem::filesystem_error&)
            {
                // Ignore if canonicalization fails.
            }
        }

        if (const std::optional<std::string> home = Env::Get("HOME"))
        {
            try
            {
                result.push_back(std::filesystem::weakly_canonical(std::filesystem::path(*home) / ".local" / "share"));
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
