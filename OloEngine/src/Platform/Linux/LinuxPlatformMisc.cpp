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
            // Fallback when sched_getaffinity failed: distribute the online CPU count
            // reported by sysconf across as many 64-bit groups as needed instead of
            // capping at a single group of 64.
            constexpr sizet MaxGroups = sizeof(Result.ThreadAffinities) / sizeof(Result.ThreadAffinities[0]);
            for (sizet g = 0; g < MaxGroups; ++g)
            {
                Result.ThreadAffinities[g] = 0;
            }

            long numCpus = sysconf(_SC_NPROCESSORS_ONLN);
            if (numCpus <= 0)
            {
                numCpus = 1;
            }
            const sizet maxCpus = MaxGroups * 64;
            if (static_cast<sizet>(numCpus) > maxCpus)
            {
                numCpus = static_cast<long>(maxCpus);
            }
            const sizet total = static_cast<sizet>(numCpus);
            const sizet fullGroups = total / 64;
            const sizet remainder = total % 64;
            for (sizet g = 0; g < fullGroups; ++g)
            {
                Result.ThreadAffinities[g] = ~0ULL;
            }
            if (remainder != 0 && fullGroups < MaxGroups)
            {
                Result.ThreadAffinities[fullGroups] = (1ULL << remainder) - 1ULL;
            }
            const sizet groupsUsed = fullGroups + (remainder != 0 ? 1 : 0);
            Result.NumProcessorGroups = static_cast<u16>(groupsUsed == 0 ? 1 : groupsUsed);
            if (Result.ThreadAffinities[0] == 0)
            {
                // Guarantee at least one runnable CPU so callers never see an empty mask.
                Result.ThreadAffinities[0] = 1ULL;
            }
        }

        return Result;
    }

} // namespace OloEngine

#endif // OLO_PLATFORM_LINUX
