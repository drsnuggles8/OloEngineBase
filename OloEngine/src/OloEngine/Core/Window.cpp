#include "OloEnginePCH.h"
#include "OloEngine/Core/Window.h"

#ifdef OLO_PLATFORM_WINDOWS
#include "Platform/Windows/WindowsWindow.h"
#elif defined(OLO_PLATFORM_LINUX)
#include "Platform/Linux/LinuxWindow.h"
#endif

namespace OloEngine
{

    Scope<Window> Window::Create(const WindowProps& props)
    {
#ifdef OLO_PLATFORM_WINDOWS
        return CreateScope<WindowsWindow>(props);
#elif defined(OLO_PLATFORM_LINUX)
        return CreateScope<LinuxWindow>(props);
#else
        OLO_CORE_ASSERT(false, "Unknown platform!");
        return nullptr;
#endif
    }

} // namespace OloEngine
