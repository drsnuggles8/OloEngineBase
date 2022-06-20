// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "OloEngine/Core/Window.h"

#ifdef OLO_PLATFORM_WINDOWS
#include "Platform/Windows/WindowsWindow.h"
#endif

namespace OloEngine
{

	Scope<Window> Window::Create(const WindowProps& props)
	{
#ifdef OLO_PLATFORM_WINDOWS
		return CreateScope<WindowsWindow>(props);
#else
		OLO_CORE_ASSERT(false, "Unknown platform!");
		return nullptr;
#endif
	}

}
