// OloEngine Memory System
// Ported from Unreal Engine's HAL/PlatformMemory.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/GenericPlatformMemory.h"

// TODO(platform-memory): port the platform-specific FPlatformMemory backends.
// In UE this header selects a per-OS implementation via the COMPILED_PLATFORM_HEADER
// macro (e.g. Windows/Linux/Mac PlatformMemory with large-page allocation, accurate
// stats, NUMA hints, etc.). OloEngine currently ships only the generic implementation
// below; when needed, add Platform/<OS>/<OS>PlatformMemory.h and select it here so
// FPlatformMemory picks up the OS-specific overrides instead of the generic fallbacks.

namespace OloEngine
{

    // FPlatformMemory is already aliased in GenericPlatformMemory.h

} // namespace OloEngine
