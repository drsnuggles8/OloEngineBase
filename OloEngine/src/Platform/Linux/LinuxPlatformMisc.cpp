// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/HAL/PlatformMisc.h"

#ifdef OLO_PLATFORM_LINUX

#include <sched.h>
#include <unistd.h>
#include <cerrno>

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
        // sched_getaffinity returns EINVAL when the provided buffer is smaller than
        // the kernel's internal cpumask (kernels built with large CONFIG_NR_CPUS can
        // exceed CPU_SETSIZE). Size the initial buffer from sysconf and retry with
        // a progressively larger allocation on EINVAL so we still work on huge boxes.
        long ConfCpus = sysconf(_SC_NPROCESSORS_CONF);
        int InitialCpus = (ConfCpus > 0) ? static_cast<int>(ConfCpus) : MaxCpus;
        if (InitialCpus < MaxCpus)
        {
            InitialCpus = MaxCpus;
        }

        cpu_set_t* DynSet = nullptr;
        sizet DynSetSize = 0;
        bool QuerySucceeded = false;
        int AllocCpus = InitialCpus;
        // Cap the retry loop so a pathological EINVAL can't spin forever.
        constexpr int MaxRetryCpus = 1 << 20; // 1M CPUs

        while (AllocCpus <= MaxRetryCpus)
        {
            DynSet = CPU_ALLOC(AllocCpus);
            if (DynSet == nullptr)
            {
                break;
            }
            DynSetSize = CPU_ALLOC_SIZE(AllocCpus);
            CPU_ZERO_S(DynSetSize, DynSet);
            const int rc = sched_getaffinity(0, DynSetSize, DynSet);
            if (rc == 0)
            {
                // Only copy bits that fit into our fixed ThreadAffinities array;
                // anything beyond MaxCpus is intentionally truncated.
                const int IterCpus = (AllocCpus < MaxCpus) ? AllocCpus : MaxCpus;
                for (int i = 0; i < IterCpus; ++i)
                {
                    if (!CPU_ISSET_S(i, DynSetSize, DynSet))
                    {
                        continue;
                    }
                    const u16 group = static_cast<u16>(i / 64);
                    const sizet bit = static_cast<sizet>(i) % 64;
                    if (group >= MaxGroups)
                    {
                        break;
                    }
                    Result.ThreadAffinities[group] |= (1ULL << bit);
                }
                // Derive NumProcessorGroups directly from the populated mask array so
                // sparse affinity (e.g. cpuset that only enables CPUs above index 63)
                // can never leave a trailing zero group mistaken for a real entry.
                u16 NumGroups = 0;
                for (int g = static_cast<int>(MaxGroups) - 1; g >= 0; --g)
                {
                    if (Result.ThreadAffinities[g] != 0)
                    {
                        NumGroups = static_cast<u16>(g + 1);
                        break;
                    }
                }
                Result.NumProcessorGroups = NumGroups;
                // Only treat the query as successful if we actually captured at least
                // one CPU bit. A zero result typically means we truncated every set
                // bit to beyond MaxCpus, in which case the sysconf-based fallback
                // below produces a more useful mask than an all-zero affinity.
                QuerySucceeded = (NumGroups > 0);
                CPU_FREE(DynSet);
                DynSet = nullptr;
                break;
            }

            // Snapshot errno before any libc call (CPU_FREE) can overwrite it.
            const int savedErrno = errno;
            CPU_FREE(DynSet);
            DynSet = nullptr;
            if (savedErrno != EINVAL)
            {
                // Any other error (EFAULT, ESRCH, EPERM) isn't fixable by growing the
                // buffer, so bail out to the fallback path.
                break;
            }
            // Kernel cpumask is larger than our buffer — double and retry.
            AllocCpus *= 2;
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
