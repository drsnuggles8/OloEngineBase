// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Task/TaskShared.h"

#include <atomic>
#include <thread>

#ifdef _WIN32
#include "Platform/Windows/WindowsHWrapper.h"
#include <malloc.h> // For _alloca
#include <intrin.h> // For _mm_pause, __rdtsc
#if defined(_M_ARM64) || defined(_M_ARM)
#include <arm64intr.h>
#endif
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <cstring> // For strncpy
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#elif defined(__APPLE__)
#include <pthread.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#include <mach/mach_time.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#endif

// Detect CPU architecture
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define OLO_PLATFORM_CPU_X86_FAMILY 1
#else
#define OLO_PLATFORM_CPU_X86_FAMILY 0
#endif

#if defined(_M_ARM64) || defined(_M_ARM) || defined(__aarch64__) || defined(__arm__)
#define OLO_PLATFORM_CPU_ARM_FAMILY 1
#else
#define OLO_PLATFORM_CPU_ARM_FAMILY 0
#endif

namespace OloEngine
{
    /**
     * @enum EThreadPriority
     * @brief Thread priority levels matching UE5.7
     */
    enum class EThreadPriority : u8
    {
        TPri_Normal,
        TPri_AboveNormal,
        TPri_BelowNormal,
        TPri_Highest,
        TPri_Lowest,
        TPri_SlightlyBelowNormal,
        TPri_TimeCritical,
        TPri_Num,
    };

    /**
     * @class FPlatformProcess
     * @brief Platform-specific process and thread utilities
     */
    class FPlatformProcess
    {
      public:
        /**
         * @brief Check if the current platform supports multithreading
         * @return true on all modern platforms
         */
        static bool SupportsMultithreading()
        {
            // All modern platforms support multithreading
            // On UE5.7 this can return false for single-core platforms or during fork
            return true;
        }

        /**
         * @brief Set the affinity mask for the current thread
         * @param AffinityMask Bitmask of allowed CPU cores (0 = no affinity restriction)
         */
        static void SetThreadAffinityMask(u64 AffinityMask);

        /**
         * @brief Set the priority of the current thread
         * @param Priority The desired priority level
         */
        static void SetThreadPriority(EThreadPriority Priority);

        /**
         * @brief Set the priority of a specific thread
         * @param Thread The thread handle
         * @param Priority The desired priority level
         */
        static void SetThreadPriority(std::thread& Thread, EThreadPriority Priority);

        /**
         * @brief Set the name of the current thread (for debugging/profiling)
         * @param Name The thread name (will be truncated on some platforms)
         */
        static void SetThreadName(const char* Name);

        /**
         * @brief Set the affinity mask and processor group for the current thread
         * @param AffinityMask Bitmask of allowed CPU cores within the group (0 = no affinity restriction)
         * @param ProcessorGroup The processor group index (for systems with >64 cores)
         *
         * On systems with more than 64 logical processors (Windows), processors are organized
         * into groups of up to 64. This function allows setting affinity within a specific group.
         * On other platforms or single-group systems, ProcessorGroup is ignored.
         */
        static void SetThreadGroupAffinity(u64 AffinityMask, u16 ProcessorGroup);

        /**
         * @brief Get the "no affinity" mask value (all cores allowed)
         * @return Platform-specific value indicating no affinity restriction
         */
        static constexpr u64 GetNoAffinityMask()
        {
            return 0;
        }

        /**
         * @brief Get the current thread's native handle
         */
        static void* GetCurrentThreadHandle();

        /**
         * @brief Yield the current thread's time slice
         */
        static void YieldThread();

        /**
         * @brief Tells the processor to pause for implementation-specific amount of time.
         *
         * Used for spin-loops to:
         * - Improve the speed at which the code detects the release of a lock
         * - Reduce power consumption during busy-wait
         * - Avoid memory order violations on some architectures
         */
        static OLO_FINLINE void Yield()
        {
#if OLO_PLATFORM_CPU_X86_FAMILY
            _mm_pause();
#elif OLO_PLATFORM_CPU_ARM_FAMILY
#if defined(_MSC_VER)
            __isb(0);
#else
            __asm__ __volatile__("isb");
#endif
#else
            // Fallback for unknown architectures
            std::this_thread::yield();
#endif
        }

        /**
         * @brief Tells the processor to pause for at least the specified number of cycles.
         *
         * Used for spin-loops to:
         * - Improve the speed at which the code detects lock release
         * - Reduce power consumption during busy-wait
         *
         * @param Cycles Approximate number of CPU cycles to pause
         *
         * @note On x86, uses RDTSC for cycle counting.
         *       On ARM, issues yield instructions approximately Cycles times.
         */
        static OLO_FINLINE void YieldCycles(u64 Cycles)
        {
#if OLO_PLATFORM_CPU_X86_FAMILY
            auto ReadCycleCounter = []() -> u64
            {
#if defined(_MSC_VER)
                return __rdtsc();
#elif defined(__APPLE__)
                return mach_absolute_time();
#elif __has_builtin(__builtin_readcyclecounter)
                return __builtin_readcyclecounter();
#else
                u32 lo, hi;
                __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
                return (static_cast<u64>(hi) << 32) | lo;
#endif
            };

            u64 start = ReadCycleCounter();
            // Some 32-bit implementations return 0 for cycle counter
            // Just to be safe, we protect against this
            Cycles = (start != 0) ? Cycles : 0;

            // Standard loop using pause instruction
            do
            {
                Yield();
            } while ((ReadCycleCounter() - start) < Cycles);

#elif OLO_PLATFORM_CPU_ARM_FAMILY
            // We can't read cycle counter from user mode on ARM
            // So we just issue yield instructions approximately Cycles times
            for (u64 i = 0; i < Cycles; i++)
            {
#if defined(_MSC_VER)
                __yield();
#else
                __builtin_arm_yield();
#endif
            }
#else
            // Fallback for unknown architectures - approximate with std::this_thread::yield
            for (u64 i = 0; i < Cycles; i++)
            {
                std::this_thread::yield();
            }
#endif
        }

      private:
#ifdef _WIN32
        static int TranslateThreadPriority(EThreadPriority Priority);
#elif defined(__linux__) || defined(__APPLE__)
        static int TranslateThreadPriority(EThreadPriority Priority);
#endif
    };

    //=============================================================================
    // IMPLEMENTATION
    //=============================================================================

#ifdef _WIN32

    inline int FPlatformProcess::TranslateThreadPriority(EThreadPriority Priority)
    {
        switch (Priority)
        {
            case EThreadPriority::TPri_AboveNormal:
                return THREAD_PRIORITY_ABOVE_NORMAL;
            case EThreadPriority::TPri_Normal:
                return THREAD_PRIORITY_NORMAL;
            case EThreadPriority::TPri_BelowNormal:
                return THREAD_PRIORITY_BELOW_NORMAL;
            case EThreadPriority::TPri_Highest:
                return THREAD_PRIORITY_HIGHEST;
            case EThreadPriority::TPri_TimeCritical:
                return THREAD_PRIORITY_HIGHEST;
            case EThreadPriority::TPri_Lowest:
                return THREAD_PRIORITY_LOWEST;
            // There is no such thing as slightly below normal on Windows.
            // This can't be below normal since we don't want latency sensitive tasks
            // to go to efficient cores on Alder Lake (hybrid architecture).
            case EThreadPriority::TPri_SlightlyBelowNormal:
                return THREAD_PRIORITY_NORMAL;
            default:
                return THREAD_PRIORITY_NORMAL;
        }
    }

    inline void* FPlatformProcess::GetCurrentThreadHandle()
    {
        return ::GetCurrentThread();
    }

    inline void FPlatformProcess::SetThreadAffinityMask(u64 AffinityMask)
    {
        if (AffinityMask != GetNoAffinityMask())
        {
            ::SetThreadAffinityMask(::GetCurrentThread(), static_cast<DWORD_PTR>(AffinityMask));
        }
    }

    inline void FPlatformProcess::SetThreadPriority(EThreadPriority Priority)
    {
        ::SetThreadPriority(::GetCurrentThread(), TranslateThreadPriority(Priority));
    }

    inline void FPlatformProcess::SetThreadPriority(std::thread& Thread, EThreadPriority Priority)
    {
        if (Thread.joinable())
        {
            HANDLE Handle = static_cast<HANDLE>(Thread.native_handle());
            ::SetThreadPriority(Handle, TranslateThreadPriority(Priority));
        }
    }

    inline void FPlatformProcess::SetThreadName(const char* Name)
    {
        // Windows 10 1607+ supports SetThreadDescription for thread naming
        // This shows up in debuggers and profilers
        if (Name)
        {
            // Convert UTF-8 to wide string
            int WideLen = ::MultiByteToWideChar(CP_UTF8, 0, Name, -1, nullptr, 0);
            if (WideLen > 0)
            {
                wchar_t* WideName = static_cast<wchar_t*>(_alloca(WideLen * sizeof(wchar_t)));
                ::MultiByteToWideChar(CP_UTF8, 0, Name, -1, WideName, WideLen);

                // SetThreadDescription is available on Windows 10 1607+
                // We use GetProcAddress to avoid requiring the newer SDK
                typedef HRESULT(WINAPI * SetThreadDescriptionFn)(HANDLE, PCWSTR);
                static SetThreadDescriptionFn SetThreadDescriptionPtr = nullptr;
                static bool bChecked = false;

                if (!bChecked)
                {
                    HMODULE hKernel32 = ::GetModuleHandleW(L"kernel32.dll");
                    if (hKernel32)
                    {
                        SetThreadDescriptionPtr = reinterpret_cast<SetThreadDescriptionFn>(
                            ::GetProcAddress(hKernel32, "SetThreadDescription"));
                    }
                    bChecked = true;
                }

                if (SetThreadDescriptionPtr)
                {
                    SetThreadDescriptionPtr(::GetCurrentThread(), WideName);
                }
            }
        }
    }

    inline void FPlatformProcess::YieldThread()
    {
        ::SwitchToThread();
    }

    inline void FPlatformProcess::SetThreadGroupAffinity(u64 AffinityMask, u16 ProcessorGroup)
    {
        if (AffinityMask == GetNoAffinityMask())
        {
            return;
        }

        // Use SetThreadGroupAffinity for multi-group systems (>64 cores)
        // This API is available on Windows 7+ and is required for systems with >64 logical processors
        GROUP_AFFINITY GroupAffinity = {};
        GroupAffinity.Mask = static_cast<KAFFINITY>(AffinityMask);
        GroupAffinity.Group = ProcessorGroup;

        ::SetThreadGroupAffinity(::GetCurrentThread(), &GroupAffinity, nullptr);
    }

#elif defined(__linux__)

    inline int FPlatformProcess::TranslateThreadPriority(EThreadPriority Priority)
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

    inline void* FPlatformProcess::GetCurrentThreadHandle()
    {
        return reinterpret_cast<void*>(static_cast<uptr>(pthread_self()));
    }

    inline void FPlatformProcess::SetThreadAffinityMask(u64 AffinityMask)
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

    inline void FPlatformProcess::SetThreadPriority(EThreadPriority Priority)
    {
        // setpriority affects the thread's nice value
        // Note: This may fail without appropriate permissions
        int NiceValue = TranslateThreadPriority(Priority);
        pid_t Tid = static_cast<pid_t>(syscall(SYS_gettid));
        setpriority(PRIO_PROCESS, static_cast<id_t>(Tid), NiceValue);
    }

    inline void FPlatformProcess::SetThreadPriority(std::thread& Thread, EThreadPriority Priority)
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

    inline void FPlatformProcess::SetThreadName(const char* Name)
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

    inline void FPlatformProcess::YieldThread()
    {
        sched_yield();
    }

    inline void FPlatformProcess::SetThreadGroupAffinity(u64 AffinityMask, u16 ProcessorGroup)
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

#elif defined(__APPLE__)

    inline int FPlatformProcess::TranslateThreadPriority(EThreadPriority Priority)
    {
        // macOS uses relative thread priority (0-63 for QoS)
        switch (Priority)
        {
            case EThreadPriority::TPri_TimeCritical:
                return 63;
            case EThreadPriority::TPri_Highest:
                return 55;
            case EThreadPriority::TPri_AboveNormal:
                return 45;
            case EThreadPriority::TPri_Normal:
                return 31;
            case EThreadPriority::TPri_SlightlyBelowNormal:
                return 25;
            case EThreadPriority::TPri_BelowNormal:
                return 15;
            case EThreadPriority::TPri_Lowest:
                return 5;
            default:
                return 31;
        }
    }

    inline void* FPlatformProcess::GetCurrentThreadHandle()
    {
        return reinterpret_cast<void*>(static_cast<uptr>(pthread_self()));
    }

    inline void FPlatformProcess::SetThreadAffinityMask(u64 AffinityMask)
    {
        if (AffinityMask != GetNoAffinityMask())
        {
            // macOS doesn't support direct CPU affinity, but we can use thread affinity policies
            thread_affinity_policy_data_t Policy = { static_cast<integer_t>(AffinityMask) };
            thread_policy_set(pthread_mach_thread_np(pthread_self()),
                              THREAD_AFFINITY_POLICY,
                              reinterpret_cast<thread_policy_t>(&Policy),
                              THREAD_AFFINITY_POLICY_COUNT);
        }
    }

    inline void FPlatformProcess::SetThreadPriority(EThreadPriority Priority)
    {
        // Use pthread_setschedparam on macOS
        pthread_t Self = pthread_self();
        int Policy;
        struct sched_param Param;
        pthread_getschedparam(Self, &Policy, &Param);
        Param.sched_priority = TranslateThreadPriority(Priority);
        pthread_setschedparam(Self, Policy, &Param);
    }

    inline void FPlatformProcess::SetThreadPriority(std::thread& Thread, EThreadPriority Priority)
    {
        if (Thread.joinable())
        {
            pthread_t Handle = Thread.native_handle();
            int Policy;
            struct sched_param Param;
            pthread_getschedparam(Handle, &Policy, &Param);
            Param.sched_priority = TranslateThreadPriority(Priority);
            pthread_setschedparam(Handle, Policy, &Param);
        }
    }

    inline void FPlatformProcess::SetThreadName(const char* Name)
    {
        if (Name)
        {
            // macOS uses pthread_setname_np but only for the current thread
            // and it only takes the thread name, not the thread handle
            pthread_setname_np(Name);
        }
    }

    inline void FPlatformProcess::YieldThread()
    {
        sched_yield();
    }

    inline void FPlatformProcess::SetThreadGroupAffinity(u64 AffinityMask, u16 ProcessorGroup)
    {
        // macOS doesn't support processor groups, just use regular affinity
        (void)ProcessorGroup;
        SetThreadAffinityMask(AffinityMask);
    }

#else
    // Fallback for unsupported platforms

    inline int FPlatformProcess::TranslateThreadPriority(EThreadPriority Priority)
    {
        (void)Priority;
        return 0;
    }

    inline void* FPlatformProcess::GetCurrentThreadHandle()
    {
        return nullptr;
    }

    inline void FPlatformProcess::SetThreadAffinityMask(u64 AffinityMask)
    {
        (void)AffinityMask;
        // Not supported
    }

    inline void FPlatformProcess::SetThreadPriority(EThreadPriority Priority)
    {
        (void)Priority;
        // Not supported
    }

    inline void FPlatformProcess::SetThreadPriority(std::thread& Thread, EThreadPriority Priority)
    {
        (void)Thread;
        (void)Priority;
        // Not supported
    }

    inline void FPlatformProcess::SetThreadName(const char* Name)
    {
        (void)Name;
        // Not supported
    }

    inline void FPlatformProcess::YieldThread()
    {
        std::this_thread::yield();
    }

    inline void FPlatformProcess::SetThreadGroupAffinity(u64 AffinityMask, u16 ProcessorGroup)
    {
        (void)AffinityMask;
        (void)ProcessorGroup;
        // Not supported
    }

#endif

} // namespace OloEngine
