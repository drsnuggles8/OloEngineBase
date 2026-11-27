#include "OloEnginePCH.h"
#include "OloEngine/Renderer/DisplayOutput.h"

namespace OloEngine
{
    // No platform query for the display's HDR state on Linux: X11 has none, and
    // Wayland's colour-management protocol is not something GLFW exposes. The
    // surface formats VulkanContext reads are the whole answer here, so Auto
    // stays SDR and an explicit HDR preference is honoured as requested
    // (SelectDisplayOutputMode, OsHdr == Unknown).
    void QueryPlatformDisplayInfo([[maybe_unused]] void* glfwWindow, DisplayCapabilities& caps)
    {
        caps.OsHdr = DisplayHdrState::Unknown;
    }

    bool PollPlatformDisplayChange([[maybe_unused]] void* glfwWindow)
    {
        return false;
    }
} // namespace OloEngine
