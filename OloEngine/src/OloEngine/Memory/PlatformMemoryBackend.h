// Platform-specific OS memory backend used by FGenericPlatformMemory.
// Implementations live in Platform/<OS>/<OS>PlatformMemory.cpp and encapsulate
// all raw OS calls (VirtualAlloc, GlobalMemoryStatusEx, posix_memalign, sysconf, etc.).

#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine::PlatformMemoryBackend
{
    /// Raw, OS-observable memory statistics. Fields are zero when the OS cannot provide them.
    struct BackendStats
    {
        u64 TotalPhysical = 0;
        u64 TotalVirtual = 0;
        u64 AvailablePhysical = 0;
        u64 AvailableVirtual = 0;
        u64 UsedPhysical = 0; // Current working set (0 if unknown)
        u64 PeakUsedPhysical = 0;
        u64 UsedVirtual = 0; // Current pagefile / commit charge (0 if unknown)
        u64 PeakUsedVirtual = 0;
    };

    /// OS-defined constants queried once at startup.
    struct BackendConstants
    {
        u32 PageSize = 4096;
        u32 OsAllocationGranularity = 65536;
        u64 TotalPhysical = 0;
        u64 TotalVirtual = 0;
    };

    /// Populate `outStats` with current OS memory info. Returns true on success.
    bool QueryStats(BackendStats& outStats);

    /// Query OS constants that don't change after process start.
    void QueryConstants(BackendConstants& outConstants);

    /// Allocate `size` bytes of OS memory with appropriate alignment (64KB granularity).
    /// Returns nullptr on failure.
    void* AllocateFromOS(sizet size);

    /// Free a pointer previously returned by AllocateFromOS.
    /// `size` is the original allocation size (ignored on some platforms).
    void FreeToOS(void* ptr, sizet size);

    /// Change page-protection flags on a region. Returns true on success.
    bool PageProtect(void* ptr, sizet size, bool canRead, bool canWrite);

} // namespace OloEngine::PlatformMemoryBackend
