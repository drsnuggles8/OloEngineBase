#include "OloEnginePCH.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <GLFW/glfw3.h>

namespace OloEngine
{
    f32 Time::GetTime()
    {
        return static_cast<f32>(GLFWAPI::glfwGetTime());
    }

    std::string FileDialogs::OpenFile([[maybe_unused]] const char* const filter, [[maybe_unused]] const char* const initialDir)
    {
        // TODO(Linux): Implement native file dialog (zenity, kdialog, or nfd)
        OLO_CORE_WARN("FileDialogs::OpenFile not yet implemented on Linux");
        return {};
    }

    std::string FileDialogs::SaveFile([[maybe_unused]] const char* const filter, [[maybe_unused]] const char* const initialDir)
    {
        // TODO(Linux): Implement native file dialog (zenity, kdialog, or nfd)
        OLO_CORE_WARN("FileDialogs::SaveFile not yet implemented on Linux");
        return {};
    }

} // namespace OloEngine
