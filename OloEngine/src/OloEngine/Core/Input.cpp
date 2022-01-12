// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
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