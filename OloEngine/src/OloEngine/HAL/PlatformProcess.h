// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Task/TaskShared.h"

#include <atomic>
#include <thread>

// CPU-architecture intrinsics for inline Yield/YieldCycles helpers
#ifdef _MSC_VER
#include <intrin.h> // For _mm_pause, __rdtsc
#if defined(_M_ARM64) || defined(_M_ARM)
#include <arm64intr.h>
#endif
#else
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
     *
     * Implementations live in Platform/<OS>/<OS>PlatformProcess.cpp.
     * The inline Yield()/YieldCycles() helpers are CPU-architecture-gated, not OS-gated,
     * so they stay in this header.
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
        // Platform-specific translation of EThreadPriority to OS native priority value.
        // Implementations live in Platform/<OS>/<OS>PlatformProcess.cpp.
        static int TranslateThreadPriority(EThreadPriority Priority);
    };

} // namespace OloEngine
