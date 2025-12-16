// SmallTaskAllocator.h - Optimized allocator for small tasks
// Ported from UE5.7 Tasks/TaskPrivate.h small task allocator

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Memory/LockFreeFixedSizeAllocator.h"

namespace OloEngine::Tasks
{
    namespace Private
    {
        /**
         * @brief Size threshold for small task optimization
         * 
         * Tasks smaller than or equal to this size will use the pooled allocator
         * for faster allocation/deallocation. Matches UE5.7's SmallTaskSize exactly.
         * 
         * From UE5.7 TaskPrivate.h:
         *   inline constexpr int32 SmallTaskSize = 256;
         */
        inline constexpr i32 SmallTaskSize = 256;

        /**
         * @brief Type alias for executable task allocator
         * 
         * Matches UE5.7's FExecutableTaskAllocator:
         *   using FExecutableTaskAllocator = TLockFreeFixedSizeAllocator_TLSCache<SmallTaskSize, PLATFORM_CACHE_LINE_SIZE>;
         */
        using FExecutableTaskAllocator = TLockFreeFixedSizeAllocator_TLSCache<SmallTaskSize, OLO_PLATFORM_CACHE_LINE_SIZE>;

        /**
         * @brief Global small task allocator instance
         * 
         * This allocator is used by TExecutableTask for efficient task allocation.
         * Tasks larger than SmallTaskSize fall back to regular heap allocation.
         * 
         * UE5.7 declares this as an extern global:
         *   CORE_API extern FExecutableTaskAllocator SmallTaskAllocator;
         * 
         * We use a function returning a static reference for header-only usage.
         */
        inline FExecutableTaskAllocator& GetSmallTaskAllocator()
        {
            static FExecutableTaskAllocator s_Allocator;
            return s_Allocator;
        }

        /**
         * @brief Generic pooled allocator for fixed-size task objects
         * 
         * Used for task events and other fixed-size task-related allocations.
         * Wraps TLockFreeFixedSizeAllocator_TLSCache with lazy initialization.
         * 
         * @tparam Size Size of each allocation block
         * @tparam Alignment Alignment requirement (defaults to cache line size)
         */
        template<i32 Size, i32 Alignment = OLO_PLATFORM_CACHE_LINE_SIZE>
        class TFixedSizeTaskAllocator
        {
        public:
            void* Allocate()
            {
                return GetAllocator().Allocate();
            }

            void Free(void* Ptr)
            {
                GetAllocator().Free(Ptr);
            }

        private:
            using FAllocator = TLockFreeFixedSizeAllocator_TLSCache<Size, Alignment>;
            
            static FAllocator& GetAllocator()
            {
                static FAllocator s_Allocator;
                return s_Allocator;
            }
        };

        /**
         * @brief RAII helper for small task allocation
         * 
         * Provides operator new/delete overloads for tasks that should use
         * the small task allocator. Derived classes automatically get optimized
         * memory allocation.
         * 
         * @tparam DerivedType The CRTP derived type (unused but kept for pattern consistency)
         */
        template<typename DerivedType>
        struct TSmallTaskAllocationMixin
        {
            static void* operator new(size_t Size)
            {
                if (Size <= static_cast<size_t>(SmallTaskSize))
                {
                    return GetSmallTaskAllocator().Allocate();
                }
                else
                {
                    // Large allocation fallback - trace it for profiling if enabled
                    // UE5.7 uses: TASKGRAPH_VERBOSE_EVENT_SCOPE(TExecutableTask::LargeAlloc);
                    return FMemory::Malloc(Size, OLO_PLATFORM_CACHE_LINE_SIZE);
                }
            }

            static void operator delete(void* Ptr, size_t Size)
            {
                if (Size <= static_cast<size_t>(SmallTaskSize))
                {
                    GetSmallTaskAllocator().Free(Ptr);
                }
                else
                {
                    FMemory::Free(Ptr);
                }
            }
        };

    } // namespace Private
} // namespace OloEngine::Tasks
