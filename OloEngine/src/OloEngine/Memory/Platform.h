#pragma once

/**
 * @file Platform.h
 * @brief Platform-specific definitions for the OloEngine memory system
 * 
 * This file provides platform abstractions for:
 * - Cache line size
 * - Memory alignment requirements
 * - Compiler-specific attributes
 * - Platform detection macros
 * 
 * Ported from Unreal Engine's platform abstraction layer.
 */

#include "OloEngine/Core/Base.h"

// ============================================================================
// Platform Cache Line Size
// ============================================================================

#if defined(OLO_PLATFORM_WINDOWS)
    // x64 Windows typically has 64-byte cache lines
    #define OLO_PLATFORM_CACHE_LINE_SIZE 64
#elif defined(OLO_PLATFORM_LINUX)
    // Most x86-64 Linux systems have 64-byte cache lines
    #define OLO_PLATFORM_CACHE_LINE_SIZE 64
#elif defined(OLO_PLATFORM_MACOS)
    // Apple Silicon (M1/M2) has 128-byte cache lines, Intel Macs have 64
    #if defined(__arm64__) || defined(__aarch64__)
        #define OLO_PLATFORM_CACHE_LINE_SIZE 128
    #else
        #define OLO_PLATFORM_CACHE_LINE_SIZE 64
    #endif
#else
    // Default to 64 bytes for unknown platforms
    #define OLO_PLATFORM_CACHE_LINE_SIZE 64
#endif

// ============================================================================
// Memory Alignment Macros
// ============================================================================

// Default memory alignment (matches most platform's malloc alignment)
#if !defined(OLO_DEFAULT_ALIGNMENT)
    #define OLO_DEFAULT_ALIGNMENT 16
#endif

// Minimum small pool alignment (for binned allocators)
#define OLO_MIN_SMALL_POOL_ALIGNMENT 8

// Maximum small pool alignment
#define OLO_MAX_SMALL_POOL_ALIGNMENT 256

// Standard allocation alignment
#define OLO_STANDARD_ALIGNMENT 16

// ============================================================================
// Virtual Memory Alignment
// ============================================================================

// Maximum supported virtual memory alignment by the platform
// This affects fast-path decisions in linear allocators
#if defined(OLO_PLATFORM_WINDOWS)
    // Windows can support 64KB aligned allocations via VirtualAlloc
    #define OLO_MAX_VIRTUAL_MEMORY_ALIGNMENT (64 * 1024)
#elif defined(OLO_PLATFORM_LINUX)
    // Linux mmap can support large page alignments
    #define OLO_MAX_VIRTUAL_MEMORY_ALIGNMENT (64 * 1024)
#elif defined(OLO_PLATFORM_MACOS)
    // macOS supports 16KB pages on Apple Silicon
    #if defined(__arm64__) || defined(__aarch64__)
        #define OLO_MAX_VIRTUAL_MEMORY_ALIGNMENT (16 * 1024)
    #else
        #define OLO_MAX_VIRTUAL_MEMORY_ALIGNMENT (64 * 1024)
    #endif
#else
    #define OLO_MAX_VIRTUAL_MEMORY_ALIGNMENT (64 * 1024)
#endif

// ============================================================================
// Compiler Hints and Attributes
// ============================================================================

// Force no-inline (for cold paths)
// Note: OLO_NOINLINE is defined in Base.h, we just alias it here for clarity
#ifndef OLO_FORCENOINLINE
    #define OLO_FORCENOINLINE OLO_NOINLINE
#endif

// Note: OLO_LIKELY and OLO_UNLIKELY are defined in Base.h

// Alignment attribute for struct/class
#if OLO_COMPILER_MSVC
    #define OLO_ALIGN(x) __declspec(align(x))
    #define OLO_GCC_ALIGN(x)
#elif OLO_COMPILER_GCC || OLO_COMPILER_CLANG
    #define OLO_ALIGN(x)
    #define OLO_GCC_ALIGN(x) __attribute__((aligned(x)))
#else
    #define OLO_ALIGN(x)
    #define OLO_GCC_ALIGN(x)
#endif

// ============================================================================
// Address Sanitizer Support
// ============================================================================

#if defined(__has_feature)
    #if __has_feature(address_sanitizer)
        #define OLO_ASAN_ENABLED 1
    #endif
#endif

#if defined(__SANITIZE_ADDRESS__)
    #define OLO_ASAN_ENABLED 1
#endif

#if !defined(OLO_ASAN_ENABLED)
    #define OLO_ASAN_ENABLED 0
#endif

// ASAN memory poisoning macros
#if OLO_ASAN_ENABLED && __has_include(<sanitizer/asan_interface.h>)
    #include <sanitizer/asan_interface.h>
    #define OLO_ASAN_POISON_MEMORY_REGION(addr, size) ASAN_POISON_MEMORY_REGION(addr, size)
    #define OLO_ASAN_UNPOISON_MEMORY_REGION(addr, size) ASAN_UNPOISON_MEMORY_REGION(addr, size)
#else
    #define OLO_ASAN_POISON_MEMORY_REGION(addr, size) ((void)(addr), (void)(size))
    #define OLO_ASAN_UNPOISON_MEMORY_REGION(addr, size) ((void)(addr), (void)(size))
#endif

// ============================================================================
// Platform Properties Utility Class
// ============================================================================

namespace OloEngine
{
    /**
     * @brief Platform-specific property queries
     * 
     * Provides static methods to query platform capabilities and properties
     * at compile-time and runtime.
     */
    struct PlatformProperties
    {
        /**
         * @brief Get the maximum supported virtual memory alignment
         * @return Maximum alignment in bytes that the platform can support for virtual allocations
         */
        static constexpr sizet GetMaxSupportedVirtualMemoryAlignment()
        {
            return OLO_MAX_VIRTUAL_MEMORY_ALIGNMENT;
        }

        /**
         * @brief Get the cache line size for the current platform
         * @return Cache line size in bytes
         */
        static constexpr sizet GetCacheLineSize()
        {
            return OLO_PLATFORM_CACHE_LINE_SIZE;
        }

        /**
         * @brief Get the default memory alignment
         * @return Default alignment in bytes
         */
        static constexpr sizet GetDefaultAlignment()
        {
            return OLO_DEFAULT_ALIGNMENT;
        }
    };

} // namespace OloEngine
