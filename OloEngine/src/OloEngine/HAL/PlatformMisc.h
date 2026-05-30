// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/HAL/PlatformProcess.h" // for EThreadPriority

#include <atomic>
#include <cstring>
#include <utility>

// Platform detection for asymmetric fence support
// Asymmetric fences are primarily useful on ARM architectures where memory ordering
// can benefit from separating producer and consumer barriers.

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__)
#define PLATFORM_SUPPORTS_ASYMMETRIC_FENCES 1
#else
#define PLATFORM_SUPPORTS_ASYMMETRIC_FENCES 0
#endif

// CPU-architecture intrinsics for inline helpers (MemoryBarrier, CountBits)
#ifdef _MSC_VER
#include <intrin.h>
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
         *
         * Implementation lives in Platform/<OS>/<OS>PlatformMisc.cpp.
         * @return Populated processor group descriptor
         */
        static FProcessorGroupDesc QueryProcessorGroupDesc();
    };

    // Forward declare EThreadPriority - actual enum is in PlatformProcess.h
    // We use the TPri_ prefixed enum values there to match UE5.7's naming

    /**
     * @enum EThreadCreateFlags
     * @brief Flags for thread creation
     *
     * Defined here in PlatformMisc.h since it's included by other HAL headers
     * that need to use these flags (RunnableThread.h, Thread.h, Fork.h).
     */
    enum class EThreadCreateFlags : i8
    {
        None = 0,
        SMTExclusive = (1 << 0), // Request exclusive access to SMT core
    };

    OLO_FINLINE EThreadCreateFlags operator|(EThreadCreateFlags A, EThreadCreateFlags B)
    {
        return static_cast<EThreadCreateFlags>(std::to_underlying(A) | std::to_underlying(B));
    }

    OLO_FINLINE EThreadCreateFlags operator&(EThreadCreateFlags A, EThreadCreateFlags B)
    {
        return static_cast<EThreadCreateFlags>(std::to_underlying(A) & std::to_underlying(B));
    }

    /**
     * @class FGenericPlatformAffinity
     * @brief Generic platform affinity - base class providing default implementations
     *
     * Ported from UE5.7 GenericPlatform/GenericPlatformAffinity.h
     *
     * The generic implementation returns 0xFFFFFFFFFFFFFFFF (all cores) for all masks.
     * Platform-specific subclasses can override to provide optimal core placement,
     * especially on big.LITTLE architectures (iOS/Android).
     *
     * Thread Priority Methods (GetRenderingThreadPriority, etc.):
     * - Return EThreadPriority values from PlatformProcess.h
     * - Windows overrides elevate critical threads to TPri_AboveNormal
     * - Task workers use TPri_SlightlyBelowNormal / TPri_BelowNormal
     *
     * Affinity Mask Methods (GetMainGameMask, etc.):
     * - Return bitmasks indicating allowed CPU cores
     * - Generic returns 0xFFFFFFFFFFFFFFFF (all cores)
     * - Mobile platforms (iOS/Android) override for big.LITTLE optimization
     */
    class FGenericPlatformAffinity
    {
      public:
        // =============================================================================
        // Affinity Masks - Bitmasks indicating which CPU cores a thread can run on
        // =============================================================================

        /**
         * @brief Get the affinity mask for the main game thread
         * @return Affinity mask (0xFFFF...F = any core)
         */
        static u64 GetMainGameMask()
        {
            return 0xFFFFFFFFFFFFFFFF;
        }

        /**
         * @brief Get the affinity mask for the rendering thread
         * @return Affinity mask for the render thread
         */
        static u64 GetRenderingThreadMask()
        {
            return 0xFFFFFFFFFFFFFFFF;
        }

        /**
         * @brief Get the affinity mask for the RHI thread
         * @return Affinity mask for the RHI thread
         *
         * Note: OloEngine is OpenGL-only and doesn't use a separate RHI thread,
         * but this is provided for API compatibility with UE patterns.
         */
        static u64 GetRHIThreadMask()
        {
            return 0xFFFFFFFFFFFFFFFF;
        }

        /**
         * @brief Get the affinity mask for the render heartbeat thread
         * @return Affinity mask for the RT heartbeat (watchdog) thread
         */
        static u64 GetRTHeartBeatMask()
        {
            return 0xFFFFFFFFFFFFFFFF;
        }

        /**
         * @brief Get the affinity mask for thread pool workers
         * @return Affinity mask for generic pool threads
         */
        static u64 GetPoolThreadMask()
        {
            return 0xFFFFFFFFFFFFFFFF;
        }

        /**
         * @brief Get the affinity mask for task graph foreground workers
         * @return Affinity mask for task graph workers (normal/high priority)
         */
        static u64 GetTaskGraphThreadMask()
        {
            return 0xFFFFFFFFFFFFFFFF;
        }

        /**
         * @brief Get the affinity mask for task graph background workers
         * @return Affinity mask for background task graph workers
         *
         * On big.LITTLE platforms, this may return only efficiency cores.
         */
        static u64 GetTaskGraphBackgroundTaskMask()
        {
            return 0xFFFFFFFFFFFFFFFF;
        }

        /**
         * @brief Get the affinity mask for high priority task graph tasks
         * @return Affinity mask for high priority tasks
         */
        static u64 GetTaskGraphHighPriorityTaskMask()
        {
            return 0xFFFFFFFFFFFFFFFF;
        }

        /**
         * @brief Get the affinity mask for the audio render thread
         * @return Affinity mask for audio thread
         */
        static u64 GetAudioRenderThreadMask()
        {
            return 0xFFFFFFFFFFFFFFFF;
        }

        /**
         * @brief Get the affinity mask for async loading threads
         * @return Affinity mask for async loading
         */
        static u64 GetAsyncLoadingThreadMask()
        {
            return 0xFFFFFFFFFFFFFFFF;
        }

        /**
         * @brief Get the "no affinity" mask - thread can run on any core
         * @return Mask meaning no restriction (typically 0xFFFF...F or 0)
         *
         * Note: Different from UE5.7 which uses 0xFFFFFFFFFFFFFFFF.
         * We use 0 as "no restriction" to match Windows' default behavior.
         */
        static u64 GetNoAffinityMask()
        {
            return 0xFFFFFFFFFFFFFFFF;
        }

        // =============================================================================
        // Thread Priorities - OS-level scheduling priorities
        // =============================================================================

        /**
         * @brief Get the priority for the rendering thread
         * @return Thread priority for the render thread
         */
        static EThreadPriority GetRenderingThreadPriority()
        {
            return EThreadPriority::TPri_Normal;
        }

        /**
         * @brief Get the flags for rendering thread creation
         * @return Creation flags for the render thread
         */
        static EThreadCreateFlags GetRenderingThreadFlags()
        {
            return EThreadCreateFlags::None;
        }

        /**
         * @brief Get the priority for the RHI thread
         * @return Thread priority for the RHI thread
         */
        static EThreadPriority GetRHIThreadPriority()
        {
            return EThreadPriority::TPri_Normal;
        }

        /**
         * @brief Get the flags for RHI thread creation
         * @return Creation flags for the RHI thread
         */
        static EThreadCreateFlags GetRHIThreadFlags()
        {
            return EThreadCreateFlags::None;
        }

        /**
         * @brief Get the priority for the game thread
         * @return Thread priority for the game thread
         */
        static EThreadPriority GetGameThreadPriority()
        {
            return EThreadPriority::TPri_Normal;
        }

        /**
         * @brief Get the priority for foreground task graph workers
         * @return Thread priority for task workers
         */
        static EThreadPriority GetTaskThreadPriority()
        {
            return EThreadPriority::TPri_SlightlyBelowNormal;
        }

        /**
         * @brief Get the priority for background task graph workers
         * @return Thread priority for background workers
         */
        static EThreadPriority GetTaskBPThreadPriority()
        {
            return EThreadPriority::TPri_BelowNormal;
        }
    };

    /**
     * @class FWindowsPlatformAffinity
     * @brief Windows-specific thread affinity settings
     *
     * Ported from UE5.7 Windows/WindowsPlatformAffinity.h
     *
     * On Windows, the scheduler is sophisticated enough that we typically
     * don't need to pin threads to specific cores. Instead, we just elevate
     * the priority of critical threads (game, render, RHI) to AboveNormal.
     */
    class FWindowsPlatformAffinity : public FGenericPlatformAffinity
    {
      public:
        static EThreadPriority GetRenderingThreadPriority()
        {
            return EThreadPriority::TPri_AboveNormal;
        }

        static EThreadPriority GetRHIThreadPriority()
        {
            return EThreadPriority::TPri_AboveNormal;
        }

        static EThreadPriority GetGameThreadPriority()
        {
            return EThreadPriority::TPri_AboveNormal;
        }
    };

    // =============================================================================
    // Platform Selection - typedef the appropriate platform class
    // =============================================================================
#ifdef _WIN32
    using FPlatformAffinity = FWindowsPlatformAffinity;
#else
    using FPlatformAffinity = FGenericPlatformAffinity;
#endif

} // namespace OloEngine
