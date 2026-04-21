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
#include <cstring>        // For strncpy

namespace OloEngine
{
    int FPlatformProcess::TranslateThreadPriority(EThreadPriority Priority)
    {
        // Linux uses nice values: -20 (highest) to 19 (lowest)
        // Normal threads typically use nice 0
        // Note: Setting thread priority on Linux often requires root or CAP_SYS_NICE
        switch (Priority)
        {
            case EThreadPriority::TPri_TimeCritical:
                return -15;
            case EThreadPriority::TPri_Highest:
                return -10;
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

    void* FPlatformProcess::GetCurrentThreadHandle()
    {
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

    void FPlatformProcess::SetThreadPriority(EThreadPriority Priority)
    {
        // setpriority affects the thread's nice value
        // Note: This may fail without appropriate permissions
        int NiceValue = TranslateThreadPriority(Priority);
        pid_t Tid = static_cast<pid_t>(syscall(SYS_gettid));
        setpriority(PRIO_PROCESS, static_cast<id_t>(Tid), NiceValue);
    }

    void FPlatformProcess::SetThreadPriority(std::thread& Thread, EThreadPriority Priority)
    {
        if (Thread.joinable())
        {
            // On Linux, we need to use pthread_setschedparam for external threads
            pthread_t Handle = Thread.native_handle();
            int Policy;
            struct sched_param Param;
            pthread_getschedparam(Handle, &Policy, &Param);

            // For SCHED_OTHER (normal scheduling), priority must be 0
            // We use SCHED_RR or SCHED_FIFO for elevated priorities (requires root)
            if (Priority == EThreadPriority::TPri_TimeCritical ||
                Priority == EThreadPriority::TPri_Highest)
            {
                Policy = SCHED_RR;
                Param.sched_priority = (Priority == EThreadPriority::TPri_TimeCritical) ? 99 : 50;
            }
            else
            {
                Policy = SCHED_OTHER;
                Param.sched_priority = 0; // Must be 0 for SCHED_OTHER
            }
            pthread_setschedparam(Handle, Policy, &Param);
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
                    CPU_SET(BaseOffset + i, &CpuSet);
                }
            }
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &CpuSet);
        }
    }

} // namespace OloEngine

#endif // OLO_PLATFORM_LINUX
