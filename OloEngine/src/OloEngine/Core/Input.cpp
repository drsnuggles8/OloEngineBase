#include "OloEnginePCH.h"
#include "OloEngine/Core/Input.h"

#ifdef OLO_PLATFORM_WINDOWS
	#include "Platform/Windows/WindowsInput.h"
#endif

namespace OloEngine {

	Scope<Input> Input::s_Instance = Input::Create();

	Scope<Input> Input::Create()
	{
#ifdef OLO_PLATFORM_WINDOWS
		return CreateScope<WindowsInput>();
#else
		OLO_CORE_ASSERT(false, "Unknown platform!");
		return nullptr;
#endif
	}
}