// Semaphore.h - Counting semaphore for thread synchronization
// Platform implementations live under Platform/<OS>/<OS>Semaphore.h

#pragma once

#include "OloEngine/Core/PlatformDetection.h"

#ifdef OLO_PLATFORM_WINDOWS
#include "Platform/Windows/WindowsSemaphore.h"
#elif defined(OLO_PLATFORM_LINUX)
#include "Platform/Linux/LinuxSemaphore.h"
#else
#error "Platform-specific semaphore implementation required"
#endif

// Public type: OloEngine::FSemaphore (alias defined in the included platform header)
