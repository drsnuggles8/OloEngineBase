// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/HAL/PlatformMisc.h"

#ifdef OLO_PLATFORM_LINUX

#include <sched.h>

namespace OloEngine
{
    FProcessorGroupDesc FPlatformMisc::QueryProcessorGroupDesc()
    {
        FProcessorGroupDesc Result;
        Result.NumProcessorGroups = 1;

        // On Linux, use sched_getaffinity to get available CPUs
        cpu_set_t CpuSet;
        CPU_ZERO(&CpuSet);
        if (sched_getaffinity(0, sizeof(CpuSet), &CpuSet) == 0)
        {
            Result.ThreadAffinities[0] = 0;
            for (int i = 0; i < 64 && i < CPU_SETSIZE; ++i)
            {
                if (CPU_ISSET(i, &CpuSet))
                {
                    Result.ThreadAffinities[0] |= (1ULL << i);
                }
            }
        }
        else
        {
            // Fallback: assume all 64 bits available
            Result.ThreadAffinities[0] = ~0ULL;
        }

        return Result;
    }

} // namespace OloEngine

#endif // OLO_PLATFORM_LINUX
