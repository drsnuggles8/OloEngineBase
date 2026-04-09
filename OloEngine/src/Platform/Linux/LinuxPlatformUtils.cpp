#include "OloEnginePCH.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <GLFW/glfw3.h>

namespace OloEngine
{
    f32 Time::GetTime()
    {
        return static_cast<f32>(::glfwGetTime());
    }

    // Returns empty string on cancellation, or a sentinel on platform-not-implemented.
    // Callers should check for the sentinel to distinguish from user cancellation.
    std::string FileDialogs::OpenFile([[maybe_unused]] const char* const filter, [[maybe_unused]] const char* const initialDir)
    {
        // TODO(Linux): Implement native file dialog (zenity, kdialog, or nfd)
        OLO_CORE_ERROR("FileDialogs::OpenFile not implemented on Linux");
        return "<platform-file-dialog-not-implemented>";
    }

    // Returns empty string on cancellation, or a sentinel on platform-not-implemented.
    // Callers should check for the sentinel to distinguish from user cancellation.
    std::string FileDialogs::SaveFile([[maybe_unused]] const char* const filter, [[maybe_unused]] const char* const initialDir)
    {
        // TODO(Linux): Implement native file dialog (zenity, kdialog, or nfd)
        OLO_CORE_ERROR("FileDialogs::SaveFile not implemented on Linux");
        return "<platform-file-dialog-not-implemented>";
    }

} // namespace OloEngine
