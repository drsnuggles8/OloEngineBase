// Windows implementation of PlatformMemoryBackend.
// Owns all <Windows.h> / <Psapi.h> usage for the memory subsystem.

#include "OloEnginePCH.h"
#include "OloEngine/Memory/PlatformMemoryBackend.h"

#ifdef OLO_PLATFORM_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>

namespace OloEngine::PlatformMemoryBackend
{
    bool QueryStats(BackendStats& outStats)
    {
        MEMORYSTATUSEX memStatus{};
        memStatus.dwLength = sizeof(memStatus);
        if (GlobalMemoryStatusEx(&memStatus))
        {
            outStats.TotalPhysical = memStatus.ullTotalPhys;
            outStats.TotalVirtual = memStatus.ullTotalVirtual;
            outStats.AvailablePhysical = memStatus.ullAvailPhys;
            outStats.AvailableVirtual = memStatus.ullAvailVirtual;
        }

        if (PROCESS_MEMORY_COUNTERS pmc{}; GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        {
            outStats.UsedPhysical = pmc.WorkingSetSize;
            outStats.PeakUsedPhysical = pmc.PeakWorkingSetSize;
            outStats.UsedVirtual = pmc.PagefileUsage;
            outStats.PeakUsedVirtual = pmc.PeakPagefileUsage;
        }

        return outStats.TotalPhysical != 0;
    }

    void QueryConstants(BackendConstants& outConstants)
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        outConstants.PageSize = si.dwPageSize;
        outConstants.OsAllocationGranularity = si.dwAllocationGranularity;

        MEMORYSTATUSEX memStatus{};
        memStatus.dwLength = sizeof(memStatus);
        if (GlobalMemoryStatusEx(&memStatus))
        {
            outConstants.TotalPhysical = memStatus.ullTotalPhys;
            outConstants.TotalVirtual = memStatus.ullTotalVirtual;
        }
    }

    void* AllocateFromOS(sizet size)
    {
        return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }

    void FreeToOS(void* ptr, sizet /*size*/)
    {
        VirtualFree(ptr, 0, MEM_RELEASE);
    }

    bool PageProtect(void* ptr, sizet size, bool canRead, bool canWrite)
    {
        DWORD newProtect = PAGE_NOACCESS;
        if (canRead && canWrite)
        {
            newProtect = PAGE_READWRITE;
        }
        else if (canRead)
        {
            newProtect = PAGE_READONLY;
        }
        else if (canWrite)
        {
            newProtect = PAGE_READWRITE; // No write-only on Windows
        }

        DWORD oldProtect = 0;
        return VirtualProtect(ptr, size, newProtect, &oldProtect) != 0;
    }

} // namespace OloEngine::PlatformMemoryBackend

#endif // OLO_PLATFORM_WINDOWS
