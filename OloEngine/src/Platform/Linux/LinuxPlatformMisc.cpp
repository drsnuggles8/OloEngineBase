// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/HAL/PlatformMisc.h"

#ifdef OLO_PLATFORM_LINUX

#include <sched.h>
#include <unistd.h>

namespace OloEngine
{
    FProcessorGroupDesc FPlatformMisc::QueryProcessorGroupDesc()
    {
        FProcessorGroupDesc Result;
        Result.NumProcessorGroups = 0;

        // On Linux, use sched_getaffinity to get available CPUs.
        // Linux has no native processor-group concept, so we emulate Windows-style
        // groups by splitting the affinity bitmap into 64-bit chunks.
        cpu_set_t CpuSet;
        CPU_ZERO(&CpuSet);
        if (sched_getaffinity(0, sizeof(CpuSet), &CpuSet) == 0)
        {
            constexpr sizet MaxGroups = sizeof(Result.ThreadAffinities) / sizeof(Result.ThreadAffinities[0]);
            for (sizet g = 0; g < MaxGroups; ++g)
            {
                Result.ThreadAffinities[g] = 0;
            }

            u16 MaxGroupUsed = 0;
            for (int i = 0; i < CPU_SETSIZE; ++i)
            {
                if (!CPU_ISSET(i, &CpuSet))
                {
                    continue;
                }
                const sizet group = static_cast<sizet>(i) / 64;
                const sizet bit = static_cast<sizet>(i) % 64;
                if (group >= MaxGroups)
                {
                    break;
                }
                Result.ThreadAffinities[group] |= (1ULL << bit);
                if (static_cast<u16>(group) > MaxGroupUsed)
                {
                    MaxGroupUsed = static_cast<u16>(group);
                }
            }
            Result.NumProcessorGroups = static_cast<u16>(MaxGroupUsed + 1);
        }
        else
        {
            // Fallback when sched_getaffinity failed: query the number of online CPUs
            // via sysconf and build a mask matching the real count instead of advertising
            // a fictional 64-CPU machine with ~0ULL.
            long numCpus = sysconf(_SC_NPROCESSORS_ONLN);
            if (numCpus <= 0)
            {
                numCpus = 1;
            }
            if (numCpus > 64)
            {
                numCpus = 64;
            }
            Result.NumProcessorGroups = 1;
            Result.ThreadAffinities[0] = (numCpus >= 64)
                                             ? ~0ULL
                                             : ((1ULL << static_cast<u32>(numCpus)) - 1ULL);
        }

        return Result;
    }

} // namespace OloEngine

#endif // OLO_PLATFORM_LINUX
