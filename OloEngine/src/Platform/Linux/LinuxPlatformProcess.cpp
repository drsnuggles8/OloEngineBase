// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/HAL/PlatformProcess.h"

#ifdef OLO_PLATFORM_LINUX

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/resource.h> // For setpriority, PRIO_PROCESS
#include <cerrno>         // For errno (setpriority result check)
#include <cstring>        // For strncpy
#include <type_traits>    // For static_assert on pthread_t size

namespace OloEngine
{
    void* FPlatformProcess::GetCurrentThreadHandle()
    {
        // pthread_t is opaque and its concrete width is implementation-defined;
        // we require it fits into a uintptr_t so we can round-trip it through void*.
        static_assert(sizeof(pthread_t) <= sizeof(uintptr_t),
                      "pthread_t does not fit into uintptr_t; update GetCurrentThreadHandle encoding.");
        return reinterpret_cast<void*>(static_cast<uptr>(pthread_self()));
    }

    void FPlatformProcess::SetThreadAffinityMask(u64 AffinityMask)
    {
        if (AffinityMask != GetNoAffinityMask())
        {
            cpu_set_t CpuSet;
            CPU_ZERO(&CpuSet);
            for (u32 i = 0; i < 64; ++i)
            {
                if (AffinityMask & (1ULL << i))
                {
                    CPU_SET(i, &CpuSet);
                }
            }
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &CpuSet);
        }
    }

    namespace
    {
        // Map non-real-time priorities to a nice value. Lower nice = higher priority.
        // Range is [-20, 19]; we pick representative values for each step.
        int ThreadPriorityToNice(EThreadPriority Priority)
        {
            switch (Priority)
            {
                case EThreadPriority::TPri_AboveNormal:
                    return -5;
                case EThreadPriority::TPri_Normal:
                    return 0;
                case EThreadPriority::TPri_SlightlyBelowNormal:
                    return 1;
                case EThreadPriority::TPri_BelowNormal:
                    return 5;
                case EThreadPriority::TPri_Lowest:
                    return 10;
                default:
                    return 0;
            }
        }

        // Choose scheduling policy + priority for a given EThreadPriority.
        // Returns true if an RT policy (SCHED_RR) was selected, false for SCHED_OTHER.
        // For SCHED_OTHER the caller should additionally apply ThreadPriorityToNice()
        // via setpriority() to get a distinguishable priority (kernel ignores
        // sched_priority for SCHED_OTHER).
        bool SelectSchedPolicy(EThreadPriority Priority, int& OutPolicy, int& OutPriority)
        {
            if (Priority == EThreadPriority::TPri_TimeCritical ||
                Priority == EThreadPriority::TPri_Highest)
            {
                OutPolicy = SCHED_RR;
                const int MinPri = sched_get_priority_min(SCHED_RR);
                const int MaxPri = sched_get_priority_max(SCHED_RR);
                if (MinPri < 0 || MaxPri < 0)
                {
                    // sched_get_priority_* failed; fall back to a conservative value.
                    OutPriority = 1;
                }
                else if (Priority == EThreadPriority::TPri_TimeCritical)
                {
                    OutPriority = MaxPri;
                }
                else
                {
                    // Midpoint of the valid RR range instead of a hardcoded 50.
                    OutPriority = MinPri + (MaxPri - MinPri) / 2;
                }
                return true;
            }
            OutPolicy = SCHED_OTHER;
            OutPriority = 0; // Must be 0 for SCHED_OTHER.
            return false;
        }

        // Apply priority to a given pthread handle. Returns true if the kernel
        // accepted the change. Errors (typically EPERM without CAP_SYS_NICE) are
        // logged but not fatal — callers use Linux thread priority as advisory.
        bool ApplyThreadPriority(pthread_t Handle, EThreadPriority Priority, bool IsSelf)
        {
            int CurrentPolicy = 0;
            struct sched_param CurrentParam{};
            pthread_getschedparam(Handle, &CurrentPolicy, &CurrentParam);

            int NewPolicy = CurrentPolicy;
            int NewPri = CurrentParam.sched_priority;
            const bool isRealtime = SelectSchedPolicy(Priority, NewPolicy, NewPri);

            struct sched_param NewParam{};
            NewParam.sched_priority = NewPri;
            const int rc = pthread_setschedparam(Handle, NewPolicy, &NewParam);
            bool accepted = (rc == 0);
            if (!accepted)
            {
                OLO_CORE_WARN("[PlatformProcess] pthread_setschedparam failed ({}); "
                              "Linux thread priority changes typically require CAP_SYS_NICE.",
                              rc);
            }
            // For SCHED_OTHER apply a nice value so different non-RT priorities are
            // still distinguishable. This only affects the calling thread (setpriority
            // operates on the caller's tid via SYS_gettid), so we only do it when
            // IsSelf is true.
            if (!isRealtime && IsSelf)
            {
                const int nice = ThreadPriorityToNice(Priority);
                const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
                errno = 0;
                const int niceRc = setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice);
                if (niceRc == -1 && errno != 0)
                {
                    OLO_CORE_WARN("[PlatformProcess] setpriority(nice={}) failed (errno={}); "
                                  "non-RT priority changes typically require CAP_SYS_NICE.",
                                  nice, errno);
                }
            }
            return accepted;
        }
    } // namespace

    void FPlatformProcess::SetThreadPriority(EThreadPriority Priority)
    {
        (void)ApplyThreadPriority(pthread_self(), Priority, /*IsSelf=*/true);
    }

    void FPlatformProcess::SetThreadPriority(std::thread& Thread, EThreadPriority Priority)
    {
        if (Thread.joinable())
        {
            (void)ApplyThreadPriority(Thread.native_handle(), Priority, /*IsSelf=*/false);
        }
    }

    void FPlatformProcess::SetThreadName(const char* Name)
    {
        if (Name)
        {
            // Linux limits thread names to 16 characters including null terminator
            char TruncatedName[16];
            strncpy(TruncatedName, Name, 15);
            TruncatedName[15] = '\0';
            pthread_setname_np(pthread_self(), TruncatedName);
        }
    }

    void FPlatformProcess::YieldThread()
    {
        sched_yield();
    }

    void FPlatformProcess::SetThreadGroupAffinity(u64 AffinityMask, u16 ProcessorGroup)
    {
        // Linux doesn't have processor groups in the Windows sense
        // Instead, it uses NUMA nodes and CPU sets that can span all cores
        // We offset the affinity mask by the processor group to support >64 cores
        if (AffinityMask != GetNoAffinityMask())
        {
            cpu_set_t CpuSet;
            CPU_ZERO(&CpuSet);

            // Calculate the base CPU offset for this "group" (64 CPUs per group)
            u32 BaseOffset = static_cast<u32>(ProcessorGroup) * 64;

            for (u32 i = 0; i < 64; ++i)
            {
                if (AffinityMask & (1ULL << i))
                {
                    const u32 idx = BaseOffset + i;
                    if (idx < static_cast<u32>(CPU_SETSIZE))
                    {
                        CPU_SET(idx, &CpuSet);
                    }
                }
            }
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &CpuSet);
        }
    }

} // namespace OloEngine

#endif // OLO_PLATFORM_LINUX
