// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"

#include <atomic>
#include <cstring>

// Platform detection for asymmetric fence support
// Asymmetric fences are primarily useful on ARM architectures where memory ordering
// can benefit from separating producer and consumer barriers.

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__)
#define PLATFORM_SUPPORTS_ASYMMETRIC_FENCES 1
#else
#define PLATFORM_SUPPORTS_ASYMMETRIC_FENCES 0
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#elif defined(__linux__)
#include <sched.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace OloEngine
{
    /**
     * @struct FProcessorGroupDesc
     * @brief Describes the processor groups in the system for NUMA/large core systems
     *
     * On Windows systems with more than 64 logical processors, processors are organized
     * into processor groups. This struct provides the affinity masks for each group.
     */
    struct FProcessorGroupDesc
    {
        static constexpr u16 MaxNumProcessorGroups = 16;
        u64 ThreadAffinities[MaxNumProcessorGroups] = {};
        u16 NumProcessorGroups = 0;
    };

    /**
     * @struct FThreadAffinity
     * @brief Describes the thread affinity including processor group for multi-group systems
     */
    struct FThreadAffinity
    {
        u64 ThreadAffinityMask = 0; // 0 = no affinity restriction
        u16 ProcessorGroup = 0;
    };

    /**
     * @class FPlatformMisc
     * @brief Platform-specific miscellaneous utilities
     */
    class FPlatformMisc
    {
      public:
        /**
         * @brief Light asymmetric fence for producers.
         *
         * On ARM platforms, this provides a lighter-weight fence that pairs with
         * AsymmetricThreadFenceHeavy() on consumer threads. The producer uses a light
         * fence while the consumer uses a heavy fence, providing correct ordering
         * with better performance than using full barriers on both sides.
         *
         * On x86/x64, this is a no-op since the strong memory model handles this.
         */
        static OLO_FINLINE void AsymmetricThreadFenceLight()
        {
#if PLATFORM_SUPPORTS_ASYMMETRIC_FENCES
#if defined(__aarch64__)
            // ARM64: Use store-load barrier
            __asm__ __volatile__("dmb ishst" ::: "memory");
#elif defined(__arm__)
            // ARM32: Use full data memory barrier (no asymmetric option)
            __asm__ __volatile__("dmb ish" ::: "memory");
#elif defined(_M_ARM64)
            // MSVC ARM64
            __dmb(_ARM64_BARRIER_ISHST);
#endif
#else
            // x86/x64: No-op - strong memory model
            std::atomic_thread_fence(std::memory_order_release);
#endif
        }

        /**
         * @brief Heavy asymmetric fence for consumers.
         *
         * On ARM platforms, this is the heavy counterpart to AsymmetricThreadFenceLight().
         * It ensures all stores from other cores are visible before proceeding.
         *
         * This is typically used in wait loops to ensure we see updates from producers
         * who only used light fences.
         */
        static OLO_FINLINE void AsymmetricThreadFenceHeavy()
        {
#if PLATFORM_SUPPORTS_ASYMMETRIC_FENCES
#if defined(__aarch64__)
            // ARM64: Full inner-shareable barrier
            __asm__ __volatile__("dmb ish" ::: "memory");
#elif defined(__arm__)
            // ARM32: Full data memory barrier
            __asm__ __volatile__("dmb ish" ::: "memory");
#elif defined(_M_ARM64)
            // MSVC ARM64
            __dmb(_ARM64_BARRIER_ISH);
#endif
#else
            // x86/x64: seq_cst fence for maximum compatibility
            std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
        }

        /**
         * @brief Full memory barrier.
         *
         * Ensures all memory operations before the barrier complete before any
         * operations after the barrier begin.
         */
        static OLO_FINLINE void MemoryBarrier()
        {
#if defined(_MSC_VER)
            _ReadWriteBarrier();
#if defined(_M_ARM64)
            __dmb(_ARM64_BARRIER_ISH);
#elif defined(_M_ARM)
            __dmb(_ARM_BARRIER_ISH);
#else
            // x86/x64
            _mm_mfence();
#endif
#elif defined(__GNUC__)
            __asm__ __volatile__("" ::: "memory");
#if defined(__aarch64__) || defined(__arm__)
            __asm__ __volatile__("dmb ish" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
            __asm__ __volatile__("mfence" ::: "memory");
#endif
#else
            std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
        }

        /**
         * @brief Gets the processor group description for systems with >64 logical processors.
         *
         * On Windows systems with more than 64 logical processors, this returns
         * information about each processor group including affinity masks.
         * On other platforms or single-group systems, returns a single group with full affinity.
         *
         * @return Reference to static processor group descriptor
         */
        static const FProcessorGroupDesc& GetProcessorGroupDesc()
        {
            static FProcessorGroupDesc ProcessorGroupDesc = QueryProcessorGroupDesc();
            return ProcessorGroupDesc;
        }

        /**
         * @brief Counts the number of set bits in a value.
         * @param Value The value to count bits in
         * @return The number of set bits
         */
        static OLO_FINLINE u32 CountBits(u64 Value)
        {
#if defined(_MSC_VER) && defined(_M_X64)
            return static_cast<u32>(__popcnt64(Value));
#elif defined(__GNUC__) || defined(__clang__)
            return static_cast<u32>(__builtin_popcountll(Value));
#else
            // Fallback bit counting
            u32 Count = 0;
            while (Value)
            {
                Count += Value & 1;
                Value >>= 1;
            }
            return Count;
#endif
        }

        /**
         * @brief Get the number of logical cores (including hyperthreads)
         * @return Total number of logical processors
         */
        static u32 NumberOfCoresIncludingHyperthreads()
        {
            const FProcessorGroupDesc& Desc = GetProcessorGroupDesc();
            u32 TotalCores = 0;
            for (u16 i = 0; i < Desc.NumProcessorGroups; ++i)
            {
                TotalCores += CountBits(Desc.ThreadAffinities[i]);
            }
            return TotalCores > 0 ? TotalCores : 1;
        }

        /**
         * @brief Get the number of physical cores (excluding hyperthreads)
         * @return Number of physical processor cores
         */
        static u32 NumberOfCores()
        {
            // On systems without CPUID or detailed topology info,
            // assume 2 threads per core as a reasonable default
            return (NumberOfCoresIncludingHyperthreads() + 1) / 2;
        }

        /**
         * @brief Get the number of worker threads to spawn for the task system
         * @return Recommended number of worker threads
         */
        static u32 NumberOfWorkerThreadsToSpawn()
        {
            // Reserve 1-2 cores for main thread and render thread
            u32 NumCores = NumberOfCoresIncludingHyperthreads();
            return NumCores > 2 ? NumCores - 1 : 1;
        }

      private:
        /**
         * @brief Queries the system for processor group information.
         * @return Populated processor group descriptor
         */
        static FProcessorGroupDesc QueryProcessorGroupDesc()
        {
            FProcessorGroupDesc Result;
#ifdef _WIN32
            // Use GetLogicalProcessorInformationEx to query processor groups
            using FnGetLogicalProcessorInformationEx = BOOL(WINAPI*)(
                LOGICAL_PROCESSOR_RELATIONSHIP,
                PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                PDWORD);

            static FnGetLogicalProcessorInformationEx GetLogicalProcessorInformationExFn =
                reinterpret_cast<FnGetLogicalProcessorInformationEx>(
                    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetLogicalProcessorInformationEx"));

            if (GetLogicalProcessorInformationExFn)
            {
                DWORD BufferSize = 0;
                GetLogicalProcessorInformationExFn(RelationGroup, nullptr, &BufferSize);

                if (BufferSize > 0)
                {
                    auto* Buffer = static_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                        HeapAlloc(GetProcessHeap(), 0, BufferSize));

                    if (Buffer && GetLogicalProcessorInformationExFn(RelationGroup, Buffer, &BufferSize))
                    {
                        Result.NumProcessorGroups = static_cast<u16>(Buffer->Group.ActiveGroupCount);

                        for (u16 GroupIndex = 0;
                             GroupIndex < Result.NumProcessorGroups &&
                             GroupIndex < FProcessorGroupDesc::MaxNumProcessorGroups;
                             ++GroupIndex)
                        {
                            Result.ThreadAffinities[GroupIndex] =
                                Buffer->Group.GroupInfo[GroupIndex].ActiveProcessorMask;
                        }
                    }

                    if (Buffer)
                    {
                        HeapFree(GetProcessHeap(), 0, Buffer);
                    }
                }
            }

            // Fallback for single-group systems or if query failed
            if (Result.NumProcessorGroups == 0)
            {
                SYSTEM_INFO SysInfo;
                GetSystemInfo(&SysInfo);
                Result.NumProcessorGroups = 1;
                Result.ThreadAffinities[0] = SysInfo.dwActiveProcessorMask;
            }
#else
            // Non-Windows platforms: single group with all available CPUs
            Result.NumProcessorGroups = 1;

#if defined(__linux__)
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
#elif defined(__APPLE__)
            // macOS: use hw.ncpu or hw.activecpu
            int NumCpus = 0;
            size_t Size = sizeof(NumCpus);
            if (sysctlbyname("hw.ncpu", &NumCpus, &Size, nullptr, 0) == 0 && NumCpus > 0)
            {
                Result.ThreadAffinities[0] = (NumCpus >= 64) ? ~0ULL : ((1ULL << NumCpus) - 1);
            }
            else
            {
                Result.ThreadAffinities[0] = ~0ULL;
            }
#else
            // Generic fallback
            Result.ThreadAffinities[0] = ~0ULL;
#endif
#endif
            return Result;
        }
    };

    /**
     * @class FPlatformAffinity
     * @brief Platform-specific thread affinity masks for different thread types
     *
     * Ported from UE5.7 HAL/PlatformAffinity.h
     * This provides default affinity masks for various thread types in the engine.
     */
    class FPlatformAffinity
    {
      public:
        /**
         * @brief Get the affinity mask for task graph worker threads
         * @return Affinity mask for foreground task graph workers
         */
        static u64 GetTaskGraphThreadMask()
        {
            // By default, use all available cores
            return FPlatformMisc::GetProcessorGroupDesc().ThreadAffinities[0];
        }

        /**
         * @brief Get the affinity mask for background task graph workers
         * @return Affinity mask for background task graph workers
         */
        static u64 GetTaskGraphBackgroundTaskMask()
        {
            // Background tasks can run on all cores by default
            // Platforms may override to restrict to efficiency cores
            return ~0ULL;
        }

        /**
         * @brief Get the affinity mask for the main game thread
         * @return Affinity mask for the game thread
         */
        static u64 GetMainGameMask()
        {
            return FPlatformMisc::GetProcessorGroupDesc().ThreadAffinities[0];
        }

        /**
         * @brief Get the affinity mask for the render thread
         * @return Affinity mask for the render thread
         */
        static u64 GetRenderingThreadMask()
        {
            return FPlatformMisc::GetProcessorGroupDesc().ThreadAffinities[0];
        }

        /**
         * @brief Get the affinity mask for the RHI thread
         * @return Affinity mask for the RHI thread
         */
        static u64 GetRHIThreadMask()
        {
            return FPlatformMisc::GetProcessorGroupDesc().ThreadAffinities[0];
        }

        /**
         * @brief Get the affinity mask for pool threads
         * @return Affinity mask for thread pool threads
         */
        static u64 GetPoolThreadMask()
        {
            return FPlatformMisc::GetProcessorGroupDesc().ThreadAffinities[0];
        }

        /**
         * @brief Get no affinity (run on any core)
         * @return Affinity mask meaning no restriction
         */
        static u64 GetNoAffinityMask()
        {
            return 0;
        }
    };

} // namespace OloEngine
