// OloEngine Memory System
// Ported from Unreal Engine's HAL/Memory.h

#pragma once

#include "OloEngine/Memory/MemoryBase.h"

namespace OloEngine
{

#if defined(PLATFORM_USES_FIXED_GMalloc_CLASS) && PLATFORM_USES_FIXED_GMalloc_CLASS
    // Platform-specific fixed allocator would be defined here
    // #include "OloEngine/Memory/MallocBinned2.h"
    // #define FMEMORY_INLINE_GMalloc (FMallocBinned2::MallocBinned2)
#endif

#if !defined(FMEMORY_INLINE_GMalloc)
    #define FMEMORY_INLINE_GMalloc Private::GMalloc
#endif

} // namespace OloEngine

#include "OloEngine/Memory/FMemory.inl"
