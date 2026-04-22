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
        // FProcessorGroupDesc already zero-initialises its members via in-class
        // initialisers, so we don't need an explicit zeroing pass here.
        FProcessorGroupDesc Result{};

        constexpr u16 MaxGroups = FProcessorGroupDesc::MaxNumProcessorGroups;
        constexpr int MaxCpus = static_cast<int>(MaxGroups) * 64;

        // On Linux, use sched_getaffinity to get available CPUs.
        // Linux has no native processor-group concept, so we emulate Windows-style
        // groups by splitting the affinity bitmap into 64-bit chunks.
        //
        // Allocate a dynamic cpu_set via CPU_ALLOC so machines with more CPUs than
        // the stock cpu_set_t (CPU_SETSIZE, typically 1024) can still be queried.
        // Size it to cover MaxGroups * 64 CPUs, which matches the fixed affinity
        // array we have to fill anyway.
        cpu_set_t* DynSet = CPU_ALLOC(MaxCpus);
        const sizet DynSetSize = (DynSet != nullptr) ? CPU_ALLOC_SIZE(MaxCpus) : 0;
        bool QuerySucceeded = false;

        if (DynSet != nullptr)
        {
            CPU_ZERO_S(DynSetSize, DynSet);
            if (sched_getaffinity(0, DynSetSize, DynSet) == 0)
            {
                u16 MaxGroupUsed = 0;
                for (int i = 0; i < MaxCpus; ++i)
                {
                    if (!CPU_ISSET_S(i, DynSetSize, DynSet))
                    {
                        continue;
                    }
                    const u16 group = static_cast<u16>(i / 64);
                    const sizet bit = static_cast<sizet>(i) % 64;
                    if (group >= MaxGroups)
                    {
                        break; // truncate excess bits beyond our fixed array
                    }
                    Result.ThreadAffinities[group] |= (1ULL << bit);
                    if (group > MaxGroupUsed)
                    {
                        MaxGroupUsed = group;
                    }
                }
                Result.NumProcessorGroups = static_cast<u16>(MaxGroupUsed + 1);
                QuerySucceeded = true;
            }
            CPU_FREE(DynSet);
        }

        if (!QuerySucceeded)
        {
            // Fallback when CPU_ALLOC/sched_getaffinity failed: distribute the online
            // CPU count reported by sysconf across as many 64-bit groups as needed.
            long numCpus = sysconf(_SC_NPROCESSORS_ONLN);
            if (numCpus <= 0)
            {
                numCpus = 1;
            }
            if (static_cast<int>(numCpus) > MaxCpus)
            {
                numCpus = MaxCpus;
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
