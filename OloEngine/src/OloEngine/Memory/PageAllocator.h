#pragma once

/**
 * @file PageAllocator.h
 * @brief Lock-free page-based memory allocator for OloEngine
 * 
 * Provides a simple page allocator that manages fixed-size memory pages.
 * Used as the backing allocator for higher-level allocators like
 * the concurrent linear allocator.
 * 
 * Ported from Unreal Engine's MemStack.h (FPageAllocator)
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Memory/AlignmentTemplates.h"
#include "OloEngine/Memory/LockFreeFixedSizeAllocator.h"
#include "OloEngine/Memory/NoopCounter.h"

namespace OloEngine
{
    // ========================================================================
    // Forward Declaration
    // ========================================================================
    
    class FPageAllocator;

    // ========================================================================
    // Page Allocator
    // ========================================================================

    /**
     * @class FPageAllocator
     * @brief A lock-free allocator for fixed-size memory pages (singleton)
     * 
     * This allocator maintains a free list of pages and uses lock-free
     * operations for thread-safe allocation and deallocation.
     * 
     * Pages are typically 64KB and are used as building blocks for
     * higher-level allocators like linear allocators and memory stacks.
     */
    class FPageAllocator
    {
    public:
        enum
        {
            PageSize = 64 * 1024,
            SmallPageSize = 1024 - 16  // allow a little extra space for allocator headers, etc
        };

        // In shipping builds, use FNoopCounter for no overhead
        // In debug/development builds, use FAtomicCounter for tracking
#if defined(OLO_DIST)
        using TPageAllocator = TLockFreeFixedSizeAllocator<PageSize, OLO_PLATFORM_CACHE_LINE_SIZE, FNoopCounter>;
#else
        using TPageAllocator = TLockFreeFixedSizeAllocator<PageSize, OLO_PLATFORM_CACHE_LINE_SIZE, FAtomicCounter>;
#endif

        /**
         * @brief Get the global page allocator instance
         */
        static FPageAllocator& Get()
        {
            static FPageAllocator Instance;
            return Instance;
        }

        ~FPageAllocator() = default;

        /**
         * @brief Allocate a page with specified alignment
         * @param Alignment Alignment requirement (default: MIN_ALIGNMENT)
         * @return Pointer to allocated page, or nullptr on failure
         */
        void* Alloc(i32 Alignment = OLO_DEFAULT_ALIGNMENT)
        {
            return m_TheAllocator.Allocate(Alignment);
        }

        /**
         * @brief Free a previously allocated page
         * @param Mem Pointer to page to free
         */
        void Free(void* Mem)
        {
            m_TheAllocator.Free(Mem);
        }

        /**
         * @brief Allocate a small page (for small allocations)
         * @return Pointer to allocated small page
         */
        void* AllocSmall()
        {
            return FMemory::Malloc(SmallPageSize);
        }

        /**
         * @brief Free a small page
         * @param Mem Pointer to small page to free
         */
        void FreeSmall(void* Mem)
        {
            FMemory::Free(Mem);
        }

        /**
         * @brief Get total bytes currently in use
         * @return Bytes used
         */
        u64 BytesUsed()
        {
            return static_cast<u64>(m_TheAllocator.GetNumUsed()) * PageSize;
        }

        /**
         * @brief Get total bytes in free list
         * @return Bytes free
         */
        u64 BytesFree()
        {
            return static_cast<u64>(m_TheAllocator.GetNumFree()) * PageSize;
        }

        /**
         * @brief Latch into protected mode (no-op in this implementation)
         * 
         * In UE, this locks the allocator into a mode where it cannot
         * be modified. Used during critical sections.
         */
        void LatchProtectedMode()
        {
            // No-op in this simplified implementation
        }

    private:
        FPageAllocator() = default;
        
        // Non-copyable
        FPageAllocator(const FPageAllocator&) = delete;
        FPageAllocator& operator=(const FPageAllocator&) = delete;

        TPageAllocator m_TheAllocator;
    };

} // namespace OloEngine
