#pragma once

// @file ConcurrentLinearAllocator.h
// @brief Fast lock-free linear allocator for OloEngine
//
// This fast linear allocator can be used for temporary allocations, and is best suited
// for allocations that are produced and consumed on different threads and within the
// lifetime of a frame. Although the lifetime of any individual allocation is not
// hard-tied to a frame (tracking is done using the BlockHeader::NumAllocations atomic
// variable), the application will eventually run OOM if allocations are not cleaned up
// in a timely manner.
//
// There is a fast-path version of the allocator that skips AllocationHeaders by aligning
// the BlockHeader with the BlockSize, so that headers can easily be found by AligningDown
// the address of the Allocation itself.
//
// The allocator works by allocating a larger block in TLS which has a Header at the front
// which contains the atomic, and all allocations are then allocated from this block:
//
// --------------------------------------------------------------------------------------------
// | BlockHeader(atomic counter etc.) | Alignment Waste | AllocationHeader(size, optional) |
// | Memory used for Allocation | Alignment Waste | AllocationHeader(size, optional) |
// | Memory used for Allocation | FreeSpace ...
// --------------------------------------------------------------------------------------------
//
// The allocator is most often used concurrently, but also supports single-threaded use cases,
// so it can be used for an array scratchpad.
//
// Ported from Unreal Engine's ConcurrentLinearAllocator.h

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Memory/AlignmentTemplates.h"
#include "OloEngine/Memory/TypeTraits.h"
#include "OloEngine/Memory/PageAllocator.h"
#include "OloEngine/Memory/LockFreeFixedSizeAllocator.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"

#include <atomic>
#include <type_traits>
#include <utility>
#include <cstring>
#include <limits>

// Tracy integration for memory profiling
#if TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

namespace OloEngine
{
    // ========================================================================
    // Memory Tracing via Tracy
    // ========================================================================
    // Memory tracing is integrated with Tracy profiler when TRACY_ENABLE is set.
    // This provides allocation tracking, heap visualization, and memory leak
    // detection in the Tracy profiler UI.

    // @brief Root heap identifiers for memory tracing
    enum class EMemoryTraceRootHeap : u8
    {
        SystemMemory = 0,
        VideoMemory = 1,
    };

#if TRACY_ENABLE
    // Tracy-based memory trace functions with named heap pools
    // TracyAllocN/TracyFreeN allow tracking allocations in named memory pools

    // @brief Mark an allocation as a heap/pool for Tracy tracking
    // @param Address The address of the allocation
    // @param Heap The heap type (used to select pool name)
    inline void MemoryTrace_MarkAllocAsHeap(u64 Address, EMemoryTraceRootHeap Heap)
    {
        const char* PoolName = (Heap == EMemoryTraceRootHeap::VideoMemory) ? "LinearAllocator-GPU" : "LinearAllocator";
        // Mark the block header allocation in Tracy
        TracyAllocN(reinterpret_cast<void*>(Address), 0, PoolName);
    }

    // @brief Unmark a heap allocation in Tracy
    // @param Address The address of the allocation being freed
    // @param Heap The heap type (used to select pool name)
    inline void MemoryTrace_UnmarkAllocAsHeap(u64 Address, EMemoryTraceRootHeap Heap)
    {
        const char* PoolName = (Heap == EMemoryTraceRootHeap::VideoMemory) ? "LinearAllocator-GPU" : "LinearAllocator";
        TracyFreeN(reinterpret_cast<void*>(Address), PoolName);
    }

    // @brief Trace a memory allocation in Tracy
    // @param Address The address of the allocation
    // @param Size The size of the allocation
    // @param Alignment The alignment of the allocation (unused by Tracy)
    // @param RootHeap The heap type (used to select pool name)
    inline void MemoryTrace_Alloc(u64 Address, u64 Size, [[maybe_unused]] u32 Alignment, EMemoryTraceRootHeap RootHeap = EMemoryTraceRootHeap::SystemMemory)
    {
        const char* PoolName = (RootHeap == EMemoryTraceRootHeap::VideoMemory) ? "LinearAllocator-GPU" : "LinearAllocator";
        TracyAllocN(reinterpret_cast<void*>(Address), Size, PoolName);
    }

    // @brief Trace a memory free in Tracy
    // @param Address The address being freed
    // @param RootHeap The heap type (used to select pool name)
    inline void MemoryTrace_Free(u64 Address, EMemoryTraceRootHeap RootHeap = EMemoryTraceRootHeap::SystemMemory)
    {
        const char* PoolName = (RootHeap == EMemoryTraceRootHeap::VideoMemory) ? "LinearAllocator-GPU" : "LinearAllocator";
        TracyFreeN(reinterpret_cast<void*>(Address), PoolName);
    }
#else
    // No-op memory trace functions when Tracy is disabled
    inline void MemoryTrace_MarkAllocAsHeap([[maybe_unused]] u64 Address, [[maybe_unused]] EMemoryTraceRootHeap Heap) {}
    inline void MemoryTrace_UnmarkAllocAsHeap([[maybe_unused]] u64 Address, [[maybe_unused]] EMemoryTraceRootHeap Heap) {}
    inline void MemoryTrace_Alloc([[maybe_unused]] u64 Address, [[maybe_unused]] u64 Size, [[maybe_unused]] u32 Alignment, [[maybe_unused]] EMemoryTraceRootHeap RootHeap = EMemoryTraceRootHeap::SystemMemory) {}
    inline void MemoryTrace_Free([[maybe_unused]] u64 Address, [[maybe_unused]] EMemoryTraceRootHeap RootHeap = EMemoryTraceRootHeap::SystemMemory) {}
#endif

    // ========================================================================
    // LLM (Low Level Memory Tracker) - Disabled
    // ========================================================================
    // OloEngine uses Tracy for memory profiling instead of UE's LLM system.
    // These macros are defined for API compatibility but expand to nothing.

#define LLM_IF_ENABLED(x)

    // Forward declarations
    template<typename BlockAllocationTag, ELinearAllocatorThreadPolicy ThreadPolicy>
    class TLinearAllocatorBase;

    // ========================================================================
    // Error Handling
    // ========================================================================

    namespace Private
    {
        // @brief Called when invalid parameters are passed to array allocator
        //
        // This is marked [[noreturn]] as it will always terminate the program.
        // In UE, this is declared in a separate compilation unit.
        //
        // @param NewNum The invalid number of elements requested
        // @param NumBytesPerElement Bytes per element
        [[noreturn]] inline void OnInvalidConcurrentLinearArrayAllocatorNum(i32 NewNum, sizet NumBytesPerElement)
        {
            OLO_CORE_ASSERT(false, "Invalid ConcurrentLinearArrayAllocator parameters: NewNum={}, NumBytesPerElement={}",
                            NewNum, NumBytesPerElement);
            std::abort(); // Ensure we never return
        }
    } // namespace Private

    // ========================================================================
    // Aligned Allocator
    // ========================================================================

    // @struct AlignedAllocator
    // @brief Default aligned allocator using standard memory functions
    struct AlignedAllocator
    {
        static constexpr bool SupportsAlignment = true;
        static constexpr bool UsesFMalloc = false;
        static constexpr u32 MaxAlignment = OLO_MAX_SMALL_POOL_ALIGNMENT;

        OLO_FINLINE static void* Malloc(sizet Size, u32 Alignment)
        {
            return FMemory::Malloc(Size, Alignment);
        }

        OLO_FINLINE static void Free(void* Pointer, [[maybe_unused]] sizet Size)
        {
            FMemory::Free(Pointer);
        }
    };

    // ========================================================================
    // Block Allocation Cache (TLS)
    // ========================================================================

    // @class TBlockAllocationCache
    // @brief Thread-local cache for single-block reuse
    //
    // Caches a single freed block in TLS to avoid allocator round-trips
    // for the common pattern of allocate-use-free.
    //
    // @tparam BlockSize Size of blocks to cache
    // @tparam Allocator Underlying allocator type
    template<u32 BlockSize, typename Allocator = AlignedAllocator>
    class TBlockAllocationCache
    {
        struct TLSCleanup
        {
            void* Block = nullptr;

            ~TLSCleanup()
            {
                if (Block)
                {
                    Allocator::Free(Block, BlockSize);
                }
            }
        };

        OLO_FINLINE static void* SwapBlock(void* NewBlock)
        {
            static thread_local TLSCleanup s_Tls;
            void* Ret = s_Tls.Block;
            s_Tls.Block = NewBlock;
            return Ret;
        }

      public:
        static constexpr bool SupportsAlignment = Allocator::SupportsAlignment;
        static constexpr bool UsesFMalloc = Allocator::UsesFMalloc;
        static constexpr u32 MaxAlignment = Allocator::MaxAlignment;

        OLO_FINLINE static void* Malloc(sizet Size, u32 Alignment)
        {
            if (Size == BlockSize)
            {
                void* Pointer = SwapBlock(nullptr);
                if (Pointer != nullptr)
                {
                    return Pointer;
                }
            }
            return Allocator::Malloc(Size, Alignment);
        }

        OLO_FINLINE static void Free(void* Pointer, sizet Size)
        {
            if (Size == BlockSize)
            {
                Pointer = SwapBlock(Pointer);
                if (Pointer == nullptr)
                {
                    return;
                }
            }
            return Allocator::Free(Pointer, Size);
        }
    };

    // ========================================================================
    // Block Allocation Lock-Free Cache
    // ========================================================================

    // @class TBlockAllocationLockFreeCache
    // @brief Lock-free page-based block cache using PageAllocator
    //
    // Uses the global PageAllocator for 64K blocks, falls back to
    // the provided allocator for other sizes.
    //
    // @tparam BlockSize Size of blocks (must be DEFAULT_PAGE_SIZE for page allocator)
    // @tparam Allocator Fallback allocator for non-page-sized allocations
    template<u32 BlockSize, typename Allocator = AlignedAllocator>
    class TBlockAllocationLockFreeCache
    {
      public:
        static_assert(BlockSize == DEFAULT_PAGE_SIZE, "Only 64k pages are supported with this cache.");

        static constexpr bool SupportsAlignment = Allocator::SupportsAlignment;
        static constexpr bool UsesFMalloc = Allocator::UsesFMalloc;
        static constexpr u32 MaxAlignment = Allocator::MaxAlignment;

        OLO_FINLINE static void* Malloc(sizet Size, u32 Alignment)
        {
            if (Size == BlockSize)
            {
                // Use page allocator for exact block size
                return FPageAllocator::Get().Alloc(static_cast<i32>(Alignment));
            }
            else
            {
                return Allocator::Malloc(Size, Alignment);
            }
        }

        OLO_FINLINE static void Free(void* Pointer, sizet Size)
        {
            if (Size == BlockSize)
            {
                FPageAllocator::Get().Free(Pointer);
            }
            else
            {
                Allocator::Free(Pointer, Size);
            }
        }
    };

    // ========================================================================
    // Default Block Allocation Tag
    // ========================================================================

    // @struct DefaultBlockAllocationTag
    // @brief Default configuration for the linear allocator
    //
    // Controls:
    // - Block size (64KB default)
    // - Whether oversized allocations are allowed
    // - Whether accurate allocation sizes are required
    // - Whether block allocation should be inlined
    struct DefaultBlockAllocationTag
    {
        static constexpr u32 BlockSize = 64 * 1024;          // Block size (64KB)
        static constexpr bool AllowOversizedBlocks = true;   // Support allocations > BlockSize
        static constexpr bool RequiresAccurateSize = true;   // GetAllocationSize returns exact size
        static constexpr bool InlineBlockAllocation = false; // NOINLINE block allocation for code size
        static constexpr const char* TagName = "DefaultLinear";

        using Allocator = TBlockAllocationLockFreeCache<BlockSize, AlignedAllocator>;
    };

    // ========================================================================
    // Thread Policy Enum
    // ========================================================================

    // @enum ELinearAllocatorThreadPolicy
    // @brief Controls thread-safety of the linear allocator
    enum class ELinearAllocatorThreadPolicy
    {
        ThreadSafe,   ///< Use atomic operations for thread safety
        NotThreadSafe ///< Single-threaded operation, no atomics
    };

    // ========================================================================
    // Linear Allocator Base
    // ========================================================================

    // @class TLinearAllocatorBase
    // @brief Core linear allocator implementation
    //
    // A bump allocator that allocates from 64KB blocks. Blocks are freed
    // when all allocations within them are freed.
    //
    // @tparam BlockAllocationTag Configuration tag (block size, allocator, etc.)
    // @tparam ThreadPolicy Thread safety policy
    template<typename BlockAllocationTag, ELinearAllocatorThreadPolicy ThreadPolicy>
    class TLinearAllocatorBase
    {
        // Determine if we can use the fast path (no allocation headers)
        static constexpr bool SupportsFastPath =
            ((BlockAllocationTag::BlockSize <= (64 * 1024)) && (OLO_MAX_VIRTUAL_MEMORY_ALIGNMENT >= 64 * 1024)) && IsPowerOfTwo(BlockAllocationTag::BlockSize) && !OLO_ASAN_ENABLED && !BlockAllocationTag::RequiresAccurateSize && BlockAllocationTag::Allocator::SupportsAlignment;

        struct BlockHeader;

        // @class AllocationHeader
        // @brief Per-allocation header storing block offset and size
        //
        // Only used when SupportsFastPath is false.
        class AllocationHeader
        {
          public:
            OLO_FINLINE AllocationHeader(BlockHeader* InBlockHeader, sizet InAllocationSize)
            {
                uptr Offset = uptr(this) - uptr(InBlockHeader);
                OLO_CORE_ASSERT(Offset < std::numeric_limits<u32>::max(), "Offset exceeds 32-bit range");
                m_BlockHeaderOffset = static_cast<u32>(Offset);

                OLO_CORE_ASSERT(InAllocationSize < std::numeric_limits<u32>::max(), "Size exceeds 32-bit range");
                m_AllocationSize = static_cast<u32>(InAllocationSize);
            }

            OLO_FINLINE BlockHeader* GetBlockHeader() const
            {
                return reinterpret_cast<BlockHeader*>(uptr(this) - m_BlockHeaderOffset);
            }

            OLO_FINLINE sizet GetAllocationSize() const
            {
                return static_cast<sizet>(m_AllocationSize);
            }

          private:
            u32 m_BlockHeaderOffset; // Negative offset from allocation to BlockHeader
            u32 m_AllocationSize;    // Size of the allocation following this header
        };

        static OLO_FINLINE AllocationHeader* GetAllocationHeader(void* Pointer)
        {
            if constexpr (SupportsFastPath)
            {
                return nullptr;
            }
            else
            {
                return reinterpret_cast<AllocationHeader*>(Pointer) - 1;
            }
        }

        // @struct BlockHeader
        // @brief Header at the start of each allocated block
        //
        // Contains the allocation counter and next allocation pointer.
        struct BlockHeader
        {
            OLO_FINLINE BlockHeader()
                : NextAllocationPtr(uptr(this) + sizeof(BlockHeader) + sizeof(AllocationHeader))
            {
            }

            // Plain (non-atomic) counter for single-threaded use
            struct PlainUInt
            {
                unsigned int FetchSub(unsigned int N, [[maybe_unused]] std::memory_order Order)
                {
                    unsigned int OriginalValue = Value;
                    Value -= N;
                    return OriginalValue;
                }

                void Store(unsigned int N, [[maybe_unused]] std::memory_order Order)
                {
                    Value = N;
                }

                unsigned int Value;
            };

            // Atomic counter for thread-safe use
            struct AtomicUInt
            {
                unsigned int FetchSub(unsigned int N, std::memory_order Order)
                {
                    return Value.fetch_sub(N, Order);
                }

                void Store(unsigned int N, std::memory_order Order)
                {
                    Value.store(N, Order);
                }

                std::atomic_uint Value;
            };

            // Select counter type based on thread policy
            using NumAllocationsType = std::conditional_t<
                ThreadPolicy == ELinearAllocatorThreadPolicy::ThreadSafe,
                AtomicUInt,
                PlainUInt>;

            // Tracks live allocations + UINT_MAX (fixed up when block is closed)
            NumAllocationsType NumAllocations{ .Value = std::numeric_limits<unsigned int>::max() };

            // Padding to avoid false sharing between NumAllocations and other fields
            u8 Padding[OLO_PLATFORM_CACHE_LINE_SIZE - sizeof(std::atomic_uint)];

            // Next address to allocate from
            uptr NextAllocationPtr;

            // TLS-local count of allocations from this block
            unsigned int Num = 0;
        };

        // @struct TLSCleanup
        // @brief RAII cleanup for TLS block on thread exit
        struct TLSCleanup
        {
            BlockHeader* Header = nullptr;

            ~TLSCleanup()
            {
                if (Header)
                {
                    Header->NextAllocationPtr = uptr(Header) + BlockAllocationTag::BlockSize;
                    const u32 DeltaCount = std::numeric_limits<unsigned int>::max() - Header->Num;

                    // Single atomic operation to reduce contention
                    if (Header->NumAllocations.FetchSub(DeltaCount, std::memory_order_acq_rel) == DeltaCount)
                    {
                        // All allocations freed - can reuse block
                        Header->~BlockHeader();
                        OLO_ASAN_UNPOISON_MEMORY_REGION(Header, BlockAllocationTag::BlockSize);
                        MemoryTrace_UnmarkAllocAsHeap(u64(Header), EMemoryTraceRootHeap::SystemMemory);
                        BlockAllocationTag::Allocator::Free(Header, BlockAllocationTag::BlockSize);
                    }
                }
            }
        };

        OLO_NOINLINE static void AllocateBlock(BlockHeader*& Header)
        {
            if constexpr (!BlockAllocationTag::InlineBlockAllocation)
            {
                static_assert(BlockAllocationTag::BlockSize >= sizeof(BlockHeader) + sizeof(AllocationHeader),
                              "BlockSize must be large enough for headers");

                u32 BlockAlignment = SupportsFastPath ? BlockAllocationTag::BlockSize : alignof(BlockHeader);
                Header = new (BlockAllocationTag::Allocator::Malloc(BlockAllocationTag::BlockSize, BlockAlignment)) BlockHeader;
                MemoryTrace_MarkAllocAsHeap(u64(Header), EMemoryTraceRootHeap::SystemMemory);
                OLO_CORE_ASSERT(IsAligned(Header, BlockAlignment), "Block not properly aligned");

                if constexpr (!SupportsFastPath)
                {
                    OLO_ASAN_POISON_MEMORY_REGION(Header + 1, BlockAllocationTag::BlockSize - sizeof(BlockHeader));
                }

                static thread_local TLSCleanup s_Cleanup;
                new (&s_Cleanup) TLSCleanup{ Header };
            }
        }

      public:
        // @brief Allocate memory with compile-time known alignment
        // @tparam Alignment Alignment requirement
        // @param Size Number of bytes to allocate
        // @return Pointer to allocated memory
        template<u32 Alignment>
        static OLO_FINLINE void* Malloc(sizet Size)
        {
            return Malloc(Size, Alignment);
        }

        // @brief Allocate memory for type T
        // @tparam T Type to allocate for
        // @return Pointer to allocated memory
        template<typename T>
        static OLO_FINLINE void* Malloc()
        {
            return Malloc(sizeof(T), alignof(T));
        }

        // @brief Main allocation function
        // @param Size Number of bytes to allocate
        // @param Alignment Alignment requirement (must be power of two)
        // @return Pointer to allocated memory
        static void* Malloc(sizet Size, u32 Alignment)
        {
            OLO_CORE_ASSERT(Alignment >= 1 && IsPowerOfTwo(Alignment), "Alignment must be power of two");

            if constexpr (!SupportsFastPath)
            {
                Alignment = (Alignment < alignof(AllocationHeader)) ? static_cast<u32>(alignof(AllocationHeader)) : Alignment;
#if OLO_ASAN_ENABLED
                // Ensure 8-byte alignment for ASAN
                Alignment = Align(Alignment, 8u);
#endif
            }

            static thread_local BlockHeader* s_Header = nullptr;

            if (s_Header == nullptr)
            {
            AllocateNewBlock:
                if constexpr (BlockAllocationTag::InlineBlockAllocation)
                {
                    static_assert(BlockAllocationTag::BlockSize >= sizeof(BlockHeader) + sizeof(AllocationHeader),
                                  "BlockSize must be large enough for headers");

                    u32 BlockAlignment = SupportsFastPath ? BlockAllocationTag::BlockSize : static_cast<u32>(alignof(BlockHeader));
                    s_Header = new (BlockAllocationTag::Allocator::Malloc(BlockAllocationTag::BlockSize, BlockAlignment)) BlockHeader;
                    MemoryTrace_MarkAllocAsHeap(u64(s_Header), EMemoryTraceRootHeap::SystemMemory);
                    OLO_CORE_ASSERT(IsAligned(s_Header, BlockAlignment), "Block not properly aligned");

                    if constexpr (!SupportsFastPath)
                    {
                        OLO_ASAN_POISON_MEMORY_REGION(s_Header + 1, BlockAllocationTag::BlockSize - sizeof(BlockHeader));
                    }

                    static thread_local TLSCleanup s_Cleanup;
                    new (&s_Cleanup) TLSCleanup{ s_Header };
                }
                else
                {
                    AllocateBlock(s_Header);
                }
            }

        AllocateNewItem:
            if constexpr (SupportsFastPath)
            {
                // Fast path: no allocation headers
                uptr AlignedOffset = Align(s_Header->NextAllocationPtr, static_cast<u64>(Alignment));
                if (AlignedOffset + Size <= uptr(s_Header) + BlockAllocationTag::BlockSize)
                {
                    s_Header->NextAllocationPtr = AlignedOffset + Size;
                    s_Header->Num++;
                    MemoryTrace_Alloc(u64(AlignedOffset), Size, Alignment);
                    LLM_IF_ENABLED(/* FLowLevelMemTracker::Get().OnLowLevelAlloc(...) */);
                    return reinterpret_cast<void*>(AlignedOffset);
                }

                // Cold path: block is full or allocation is oversized
                constexpr sizet HeaderSize = sizeof(BlockHeader);
                if constexpr (BlockAllocationTag::AllowOversizedBlocks)
                {
                    // Support for oversized blocks
                    if (HeaderSize + Size + Alignment > BlockAllocationTag::BlockSize)
                    {
                        BlockHeader* LargeHeader = new (BlockAllocationTag::Allocator::Malloc(
                            HeaderSize + Size + Alignment, BlockAllocationTag::BlockSize)) BlockHeader;
                        MemoryTrace_MarkAllocAsHeap(u64(LargeHeader), EMemoryTraceRootHeap::SystemMemory);
                        OLO_CORE_ASSERT(IsAligned(LargeHeader, alignof(BlockHeader)), "Large block not aligned");

                        uptr LargeAlignedOffset = Align(LargeHeader->NextAllocationPtr, static_cast<u64>(Alignment));
                        LargeHeader->NextAllocationPtr = uptr(LargeHeader) + HeaderSize + Size + Alignment;
                        LargeHeader->NumAllocations.Store(1, std::memory_order_release);

                        OLO_CORE_ASSERT(LargeAlignedOffset + Size <= LargeHeader->NextAllocationPtr,
                                        "Oversized allocation overflow");
                        MemoryTrace_Alloc(u64(LargeAlignedOffset), Size, Alignment);
                        LLM_IF_ENABLED(/* FLowLevelMemTracker::Get().OnLowLevelAlloc(...) */);
                        return reinterpret_cast<void*>(LargeAlignedOffset);
                    }
                }
                OLO_CORE_ASSERT(HeaderSize + Size + Alignment <= BlockAllocationTag::BlockSize,
                                "Allocation too large for block");
            }
            else
            {
                // Slow path: uses allocation headers
                uptr AlignedOffset = Align(s_Header->NextAllocationPtr, static_cast<u64>(Alignment));
                if (AlignedOffset + Size <= uptr(s_Header) + BlockAllocationTag::BlockSize)
                {
                    s_Header->NextAllocationPtr = AlignedOffset + Size + sizeof(AllocationHeader);
                    s_Header->Num++;

                    AllocationHeader* AllocHeader = reinterpret_cast<AllocationHeader*>(AlignedOffset) - 1;
                    OLO_ASAN_UNPOISON_MEMORY_REGION(AllocHeader, sizeof(AllocationHeader) + Size);
                    new (AllocHeader) AllocationHeader(s_Header, Size);
                    OLO_ASAN_POISON_MEMORY_REGION(AllocHeader, sizeof(AllocationHeader));

                    MemoryTrace_Alloc(u64(AllocHeader), Size + sizeof(AllocationHeader), Alignment);
                    LLM_IF_ENABLED(/* FLowLevelMemTracker::Get().OnLowLevelAlloc(...) */);
                    return reinterpret_cast<void*>(AlignedOffset);
                }

                // Cold path
                constexpr sizet HeaderSize = sizeof(BlockHeader) + sizeof(AllocationHeader);
                if constexpr (BlockAllocationTag::AllowOversizedBlocks)
                {
                    if (HeaderSize + Size + Alignment > BlockAllocationTag::BlockSize)
                    {
                        BlockHeader* LargeHeader = new (BlockAllocationTag::Allocator::Malloc(
                            HeaderSize + Size + Alignment, static_cast<u32>(alignof(BlockHeader)))) BlockHeader;
                        MemoryTrace_MarkAllocAsHeap(u64(LargeHeader), EMemoryTraceRootHeap::SystemMemory);
                        OLO_CORE_ASSERT(IsAligned(LargeHeader, alignof(BlockHeader)), "Large block not aligned");

                        uptr LargeAlignedOffset = Align(LargeHeader->NextAllocationPtr, static_cast<u64>(Alignment));
                        LargeHeader->NextAllocationPtr = uptr(LargeHeader) + HeaderSize + Size + Alignment;
                        LargeHeader->NumAllocations.Store(1, std::memory_order_release);

                        OLO_CORE_ASSERT(LargeAlignedOffset + Size <= LargeHeader->NextAllocationPtr,
                                        "Oversized allocation overflow");
                        AllocationHeader* AllocHeader = new (reinterpret_cast<AllocationHeader*>(LargeAlignedOffset) - 1)
                            AllocationHeader(LargeHeader, Size);
                        OLO_ASAN_POISON_MEMORY_REGION(AllocHeader, sizeof(AllocationHeader));

                        MemoryTrace_Alloc(u64(AllocHeader), Size + sizeof(AllocationHeader), Alignment);
                        LLM_IF_ENABLED(/* FLowLevelMemTracker::Get().OnLowLevelAlloc(...) */);
                        return reinterpret_cast<void*>(LargeAlignedOffset);
                    }
                }
                OLO_CORE_ASSERT(HeaderSize + Size + Alignment <= BlockAllocationTag::BlockSize,
                                "Allocation too large for block");
            }

            // Block is full - close it and allocate a new one
            s_Header->NextAllocationPtr = uptr(s_Header) + BlockAllocationTag::BlockSize;
            const u32 DeltaCount = std::numeric_limits<unsigned int>::max() - s_Header->Num;

            if (s_Header->NumAllocations.FetchSub(DeltaCount, std::memory_order_acq_rel) == DeltaCount)
            {
                // All allocations freed - reuse block
                s_Header->~BlockHeader();
                s_Header = new (s_Header) BlockHeader;
                OLO_ASAN_POISON_MEMORY_REGION(s_Header + 1, BlockAllocationTag::BlockSize - sizeof(BlockHeader));
                goto AllocateNewItem;
            }

            goto AllocateNewBlock;
        }

        // @brief Free a previously allocated pointer
        // @param Pointer Pointer to free (can be nullptr)
        static void Free(void* Pointer)
        {
            if (Pointer != nullptr)
            {
                MemoryTrace_Free(u64(Pointer));
                LLM_IF_ENABLED(/* FLowLevelMemTracker::Get().OnLowLevelFree(...) */);

                if constexpr (SupportsFastPath)
                {
                    BlockHeader* Header = reinterpret_cast<BlockHeader*>(
                        AlignDown(Pointer, static_cast<u64>(BlockAllocationTag::BlockSize)));

                    if (Header->NumAllocations.FetchSub(1, std::memory_order_acq_rel) == 1)
                    {
                        const uptr NextAllocationPtr = Header->NextAllocationPtr;
                        Header->~BlockHeader();
                        BlockAllocationTag::Allocator::Free(Header, NextAllocationPtr - uptr(Header));
                    }
                }
                else
                {
                    AllocationHeader* AllocHeader = GetAllocationHeader(Pointer);
                    OLO_ASAN_UNPOISON_MEMORY_REGION(AllocHeader, sizeof(AllocationHeader));
                    BlockHeader* Header = AllocHeader->GetBlockHeader();
                    OLO_ASAN_POISON_MEMORY_REGION(AllocHeader, sizeof(AllocationHeader) + AllocHeader->GetAllocationSize());

                    if (Header->NumAllocations.FetchSub(1, std::memory_order_acq_rel) == 1)
                    {
                        const uptr NextAllocationPtr = Header->NextAllocationPtr;
                        Header->~BlockHeader();
                        OLO_ASAN_UNPOISON_MEMORY_REGION(Header, NextAllocationPtr - uptr(Header));
                        BlockAllocationTag::Allocator::Free(Header, NextAllocationPtr - uptr(Header));
                    }
                }
            }
        }

        // @brief Get the size of an allocation
        // @param Pointer Pointer to query
        // @return Size of the allocation, or space to end of block in fast path
        static sizet GetAllocationSize(void* Pointer)
        {
            if (Pointer)
            {
                if constexpr (SupportsFastPath)
                {
                    return Align(uptr(Pointer), static_cast<u64>(BlockAllocationTag::BlockSize)) - uptr(Pointer);
                }
                else
                {
                    AllocationHeader* AllocHeader = GetAllocationHeader(Pointer);
                    OLO_ASAN_UNPOISON_MEMORY_REGION(AllocHeader, sizeof(AllocationHeader));
                    sizet Size = AllocHeader->GetAllocationSize();
                    OLO_ASAN_POISON_MEMORY_REGION(AllocHeader, sizeof(AllocationHeader));
                    return Size;
                }
            }
            return 0;
        }

        // @brief Reallocate memory
        // @param Old Pointer to existing allocation (can be nullptr)
        // @param Size New size
        // @param Alignment New alignment
        // @return Pointer to new allocation
        static void* Realloc(void* Old, sizet Size, u32 Alignment)
        {
            void* New = nullptr;
            if (Size != 0)
            {
                New = Malloc(Size, Alignment);
                sizet OldSize = GetAllocationSize(Old);
                if (OldSize != 0)
                {
                    std::memcpy(New, Old, Size < OldSize ? Size : OldSize);
                }
            }
            Free(Old);
            return New;
        }
    };

    // ========================================================================
    // Type Aliases
    // ========================================================================

    // @typedef TConcurrentLinearAllocator
    // @brief Thread-safe linear allocator with custom tag
    template<typename T>
    using TConcurrentLinearAllocator = TLinearAllocatorBase<T, ELinearAllocatorThreadPolicy::ThreadSafe>;

    // @typedef ConcurrentLinearAllocator
    // @brief Default thread-safe linear allocator
    using ConcurrentLinearAllocator = TLinearAllocatorBase<DefaultBlockAllocationTag, ELinearAllocatorThreadPolicy::ThreadSafe>;

    // @typedef NonconcurrentLinearAllocator
    // @brief Single-threaded linear allocator (no atomics)
    using NonconcurrentLinearAllocator = TLinearAllocatorBase<DefaultBlockAllocationTag, ELinearAllocatorThreadPolicy::NotThreadSafe>;

    // ========================================================================
    // Concurrent Linear Object (CRTP)
    // ========================================================================

    // @class TConcurrentLinearObject
    // @brief CRTP base class for objects that use the concurrent linear allocator
    //
    // Inherit from this class to override operator new/delete to use
    // the concurrent linear allocator.
    //
    // @tparam ObjectType The derived class type (CRTP)
    // @tparam BlockAllocationTag Allocator configuration tag
    template<typename ObjectType, typename BlockAllocationTag = DefaultBlockAllocationTag>
    class TConcurrentLinearObject
    {
      public:
        static void* operator new(sizet Size)
        {
            static_assert(TIsDerivedFrom<ObjectType, TConcurrentLinearObject<ObjectType, BlockAllocationTag>>::value,
                          "TConcurrentLinearObject must be base of its ObjectType (see CRTP)");
            static_assert(alignof(ObjectType) <= BlockAllocationTag::Allocator::MaxAlignment,
                          "ObjectType alignment exceeds allocator maximum");
            return TConcurrentLinearAllocator<BlockAllocationTag>::template Malloc<alignof(ObjectType)>(Size);
        }

        static void* operator new(sizet /*Size*/, void* Object)
        {
            static_assert(TIsDerivedFrom<ObjectType, TConcurrentLinearObject<ObjectType, BlockAllocationTag>>::value,
                          "TConcurrentLinearObject must be base of its ObjectType (see CRTP)");
            return Object;
        }

        static void* operator new[](sizet Size)
        {
            static_assert(alignof(ObjectType) <= BlockAllocationTag::Allocator::MaxAlignment,
                          "ObjectType alignment exceeds allocator maximum");
            return TConcurrentLinearAllocator<BlockAllocationTag>::template Malloc<alignof(ObjectType)>(Size);
        }

        static void* operator new(sizet Size, std::align_val_t AlignVal)
        {
            static_assert(alignof(ObjectType) <= BlockAllocationTag::Allocator::MaxAlignment,
                          "ObjectType alignment exceeds allocator maximum");
            OLO_CORE_ASSERT(sizet(AlignVal) == alignof(ObjectType), "Alignment mismatch");
            return TConcurrentLinearAllocator<BlockAllocationTag>::template Malloc<alignof(ObjectType)>(Size);
        }

        static void* operator new[](sizet Size, std::align_val_t AlignVal)
        {
            static_assert(alignof(ObjectType) <= BlockAllocationTag::Allocator::MaxAlignment,
                          "ObjectType alignment exceeds allocator maximum");
            OLO_CORE_ASSERT(sizet(AlignVal) == alignof(ObjectType), "Alignment mismatch");
            return TConcurrentLinearAllocator<BlockAllocationTag>::template Malloc<alignof(ObjectType)>(Size);
        }

        OLO_FINLINE static void operator delete(void* Ptr)
        {
            return TConcurrentLinearAllocator<BlockAllocationTag>::Free(Ptr);
        }

        OLO_FINLINE static void operator delete[](void* Ptr)
        {
            return TConcurrentLinearAllocator<BlockAllocationTag>::Free(Ptr);
        }
    };

    // ========================================================================
    // Linear Array Allocator Base
    // ========================================================================

    // @brief Default calculator for array slack when growing
    inline i32 DefaultCalculateSlackGrow(i32 NewMax, i32 CurrentMax, sizet NumBytesPerElement, bool /*bAllowQuantize*/)
    {
        i32 Retval;

        // Aggressive growth to avoid many reallocations
        if (CurrentMax || NewMax > 0)
        {
            // Grow by 50%
            Retval = NewMax + 3 * NewMax / 8 + 16;
        }
        else
        {
            Retval = NewMax;
        }

        // Don't exceed 32-bit bounds
        if (static_cast<i64>(Retval) * NumBytesPerElement > std::numeric_limits<i32>::max())
        {
            Retval = std::numeric_limits<i32>::max() / static_cast<i32>(NumBytesPerElement);
        }

        return Retval;
    }

    // @brief Default calculator for array slack when shrinking
    inline i32 DefaultCalculateSlackShrink(i32 NewMax, i32 CurrentMax, sizet /*NumBytesPerElement*/, bool /*bAllowQuantize*/)
    {
        // Shrink aggressively
        return NewMax != 0 ? NewMax : 0;
    }

    // @brief Default calculator for array slack reserve
    inline i32 DefaultCalculateSlackReserve(i32 NewMax, sizet /*NumBytesPerElement*/, bool /*bAllowQuantize*/)
    {
        return NewMax;
    }

    // Forward declaration for array allocator
    struct ScriptContainerElement
    {
        alignas(8) u8 Padding[8];
    };

    // @class TLinearArrayAllocatorBase
    // @brief Array allocator using the linear allocator for backing storage
    //
    // Provides an STL-compatible allocator interface for dynamic arrays
    // that use the concurrent linear allocator.
    //
    // @tparam BlockAllocationTag Allocator configuration
    // @tparam ThreadPolicy Thread safety policy
    template<typename BlockAllocationTag, ELinearAllocatorThreadPolicy ThreadPolicy>
    class TLinearArrayAllocatorBase
    {
      public:
        using SizeType = i32;

        static constexpr bool NeedsElementType = true;
        static constexpr bool RequireRangeCheck = true;

        template<typename ElementType>
        class ForElementType
        {
          public:
            ForElementType() = default;

            ~ForElementType()
            {
                if (m_Data)
                {
                    TLinearAllocatorBase<BlockAllocationTag, ThreadPolicy>::Free(m_Data);
                }
            }

            // @brief Move state from another allocator
            // @param Other Source allocator (will be left empty)
            void MoveToEmpty(ForElementType& Other)
            {
                OLO_CORE_ASSERT(this != &Other, "Cannot move to self");

                if (m_Data)
                {
                    TLinearAllocatorBase<BlockAllocationTag, ThreadPolicy>::Free(m_Data);
                }

                m_Data = Other.m_Data;
                Other.m_Data = nullptr;
            }

            ElementType* GetAllocation() const
            {
                return m_Data;
            }

            void ResizeAllocation(SizeType /*CurrentNum*/, SizeType NewMax, sizet NumBytesPerElement)
            {
                static_assert(sizeof(i32) <= sizeof(sizet), "sizet expected to be larger than i32");

                // Check for under/overflow
                if (OLO_UNLIKELY(NewMax < 0 || NumBytesPerElement < 1 || NumBytesPerElement > static_cast<sizet>(std::numeric_limits<i32>::max())))
                {
                    Private::OnInvalidConcurrentLinearArrayAllocatorNum(NewMax, NumBytesPerElement);
                }

                static_assert(alignof(ElementType) <= BlockAllocationTag::Allocator::MaxAlignment,
                              "Element alignment exceeds allocator maximum");
                m_Data = static_cast<ElementType*>(TLinearAllocatorBase<BlockAllocationTag, ThreadPolicy>::Realloc(
                    m_Data, NewMax * NumBytesPerElement, alignof(ElementType)));
            }

            SizeType CalculateSlackReserve(SizeType NewMax, sizet NumBytesPerElement) const
            {
                return DefaultCalculateSlackReserve(NewMax, NumBytesPerElement, false);
            }

            SizeType CalculateSlackShrink(SizeType NewMax, SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                return DefaultCalculateSlackShrink(NewMax, CurrentMax, NumBytesPerElement, false);
            }

            SizeType CalculateSlackGrow(SizeType NewMax, SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                return DefaultCalculateSlackGrow(NewMax, CurrentMax, NumBytesPerElement, false);
            }

            sizet GetAllocatedSize(SizeType CurrentMax, sizet NumBytesPerElement) const
            {
                return CurrentMax * NumBytesPerElement;
            }

            bool HasAllocation() const
            {
                return !!m_Data;
            }

            SizeType GetInitialCapacity() const
            {
                return 0;
            }

          private:
            ForElementType(const ForElementType&) = delete;
            ForElementType& operator=(const ForElementType&) = delete;
            ElementType* m_Data = nullptr;
        };

        using ForAnyElementType = ForElementType<ScriptContainerElement>;
    };

    // Array allocator type aliases
    template<typename BlockAllocationTag>
    using TConcurrentLinearArrayAllocator = TLinearArrayAllocatorBase<BlockAllocationTag, ELinearAllocatorThreadPolicy::ThreadSafe>;

    template<typename BlockAllocationTag>
    using TNonconcurrentLinearArrayAllocator = TLinearArrayAllocatorBase<BlockAllocationTag, ELinearAllocatorThreadPolicy::NotThreadSafe>;

    using ConcurrentLinearArrayAllocator = TConcurrentLinearArrayAllocator<DefaultBlockAllocationTag>;
    using NonconcurrentLinearArrayAllocator = TNonconcurrentLinearArrayAllocator<DefaultBlockAllocationTag>;

    // ========================================================================
    // Allocator Traits
    // ========================================================================

    // @struct TAllocatorTraitsBase
    // @brief Base traits for allocators
    template<typename AllocatorType>
    struct TAllocatorTraitsBase
    {
        static constexpr bool IsZeroConstruct = false;
        static constexpr bool SupportsMove = false;
    };

    // @struct TAllocatorTraits
    // @brief Traits specialization for allocators
    template<typename AllocatorType>
    struct TAllocatorTraits : TAllocatorTraitsBase<AllocatorType>
    {
    };

    // @brief Specialization for TConcurrentLinearArrayAllocator - zero constructs
    template<typename BlockAllocationTag>
    struct TAllocatorTraits<TConcurrentLinearArrayAllocator<BlockAllocationTag>>
        : TAllocatorTraitsBase<TConcurrentLinearArrayAllocator<BlockAllocationTag>>
    {
        static constexpr bool IsZeroConstruct = true;
    };

    // ========================================================================
    // Composite Allocator Type Aliases (for UE container compatibility)
    // ========================================================================

    // @brief Bit array allocator using inline storage backed by concurrent linear allocator
    //
    // Uses TInlineAllocator with 4 inline elements for small bit arrays,
    // falling back to TConcurrentLinearArrayAllocator for larger allocations.
    template<typename BlockAllocationTag>
    using TConcurrentLinearBitArrayAllocator = TInlineAllocator<4, TConcurrentLinearArrayAllocator<BlockAllocationTag>>;

    // @brief Sparse array allocator using concurrent linear allocation
    //
    // Uses TConcurrentLinearArrayAllocator for elements and
    // TConcurrentLinearBitArrayAllocator for the free list bitmap.
    template<typename BlockAllocationTag>
    using TConcurrentLinearSparseArrayAllocator = TSparseArrayAllocator<
        TConcurrentLinearArrayAllocator<BlockAllocationTag>,
        TConcurrentLinearBitArrayAllocator<BlockAllocationTag>>;

    // @brief Set allocator using concurrent linear allocation
    //
    // Uses TConcurrentLinearSparseArrayAllocator for sparse storage and
    // TInlineAllocator with 1 inline element for hash buckets.
    template<typename BlockAllocationTag>
    using TConcurrentLinearSetAllocator = TSetAllocator<
        TConcurrentLinearSparseArrayAllocator<BlockAllocationTag>,
        TInlineAllocator<1, TConcurrentLinearBitArrayAllocator<BlockAllocationTag>>>;

    // Default allocator type aliases
    using ConcurrentLinearBitArrayAllocator = TConcurrentLinearBitArrayAllocator<DefaultBlockAllocationTag>;
    using ConcurrentLinearSparseArrayAllocator = TConcurrentLinearSparseArrayAllocator<DefaultBlockAllocationTag>;
    using ConcurrentLinearSetAllocator = TConcurrentLinearSetAllocator<DefaultBlockAllocationTag>;

    // ========================================================================
    // Bulk Object Allocator
    // ========================================================================

    // @class TConcurrentLinearBulkObjectAllocator
    // @brief Allocator that tracks objects for bulk destruction
    //
    // All allocated objects are linked together and can be destroyed
    // atomically with BulkDelete(). Useful for frame-lifetime allocations.
    //
    // @tparam BlockAllocationTag Allocator configuration
    template<typename BlockAllocationTag>
    class TConcurrentLinearBulkObjectAllocator
    {
        using ThisType = TConcurrentLinearBulkObjectAllocator<BlockAllocationTag>;

        struct Allocation
        {
            virtual ~Allocation() = default;
            Allocation* Next = nullptr;
        };

        template<typename T>
        struct TObject final : Allocation
        {
            TObject() = default;

            virtual ~TObject() override
            {
                T* Alloc = reinterpret_cast<T*>(uptr(this) + Align(sizeof(TObject<T>), static_cast<sizet>(alignof(T))));
                OLO_CORE_ASSERT(IsAligned(Alloc, alignof(T)), "Object not properly aligned");

                using DestructorType = T;
                Alloc->DestructorType::~T();
            }
        };

        template<typename T>
        struct TObjectArray final : Allocation
        {
            explicit TObjectArray(sizet InNum)
                : Num(InNum)
            {
            }

            virtual ~TObjectArray() override
            {
                T* Alloc = reinterpret_cast<T*>(uptr(this) + Align(sizeof(TObjectArray<T>), static_cast<sizet>(alignof(T))));
                OLO_CORE_ASSERT(IsAligned(Alloc, alignof(T)), "Array not properly aligned");

                for (sizet i = 0; i < Num; i++)
                {
                    using DestructorType = T;
                    Alloc[i].DestructorType::~T();
                }
            }

            sizet Num;
        };

        std::atomic<Allocation*> m_Next{ nullptr };

      public:
        TConcurrentLinearBulkObjectAllocator() = default;

        ~TConcurrentLinearBulkObjectAllocator()
        {
            BulkDelete();
        }

        // @brief Delete all allocated objects
        //
        // Calls destructors for all objects and frees memory.
        void BulkDelete()
        {
            Allocation* Current = m_Next.exchange(nullptr, std::memory_order_acquire);
            while (Current != nullptr)
            {
                Allocation* NextAllocation = Current->Next;
                Current->~Allocation();
                TConcurrentLinearAllocator<BlockAllocationTag>::Free(Current);
                Current = NextAllocation;
            }
        }

        // @brief Allocate raw memory (no destructor tracking)
        // @param Size Size in bytes
        // @param AlignmentVal Alignment requirement
        // @return Pointer to allocated memory
        void* Malloc(sizet Size, u32 AlignmentVal)
        {
            sizet TotalSize = Align(sizeof(Allocation), static_cast<sizet>(AlignmentVal)) + Size;
            Allocation* Alloc = new (TConcurrentLinearAllocator<BlockAllocationTag>::Malloc(
                TotalSize, std::max(static_cast<u32>(alignof(Allocation)), AlignmentVal))) Allocation();

            void* Result = reinterpret_cast<void*>(Align(uptr(Alloc + 1), static_cast<sizet>(AlignmentVal)));
            OLO_CORE_ASSERT(uptr(Result) + Size - uptr(Alloc) <= TotalSize, "Allocation overflow");

            // Link into list atomically
            Alloc->Next = m_Next.load(std::memory_order_relaxed);
            while (!m_Next.compare_exchange_strong(Alloc->Next, Alloc,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed))
            {
            }
            return Result;
        }

        // @brief Allocate and zero-initialize memory
        void* MallocAndMemset(sizet Size, u32 AlignmentVal, u8 MemsetChar)
        {
            void* Ptr = Malloc(Size, AlignmentVal);
            FMemory::Memset(Ptr, MemsetChar, Size);
            return Ptr;
        }

        // @brief Allocate memory for type T
        template<typename T>
        T* Malloc()
        {
            return reinterpret_cast<T*>(Malloc(sizeof(T), alignof(T)));
        }

        // @brief Allocate and zero-initialize for type T
        template<typename T>
        T* MallocAndMemset(u8 MemsetChar)
        {
            void* Ptr = Malloc(sizeof(T), alignof(T));
            FMemory::Memset(Ptr, MemsetChar, sizeof(T));
            return reinterpret_cast<T*>(Ptr);
        }

        // @brief Allocate array of T
        template<typename T>
        T* MallocArray(sizet Num)
        {
            return reinterpret_cast<T*>(Malloc(sizeof(T) * Num, alignof(T)));
        }

        // @brief Allocate and zero-initialize array
        template<typename T>
        T* MallocAndMemsetArray(sizet Num, u8 MemsetChar)
        {
            void* Ptr = Malloc(sizeof(T) * Num, alignof(T));
            FMemory::Memset(Ptr, MemsetChar, sizeof(T) * Num);
            return reinterpret_cast<T*>(Ptr);
        }

        // @brief Create and construct an object (destructor will be called on BulkDelete)
        // @tparam T Object type
        // @tparam TArgs Constructor argument types
        // @param Args Constructor arguments
        // @return Pointer to constructed object
        template<typename T, typename... TArgs>
        T* Create(TArgs&&... Args)
        {
            T* Alloc = CreateNoInit<T>();
            new (static_cast<void*>(Alloc)) T(std::forward<TArgs>(Args)...);
            return Alloc;
        }

        // @brief Create array of objects (destructors called on BulkDelete)
        template<typename T, typename... TArgs>
        T* CreateArray(sizet Num, const TArgs&... Args)
        {
            T* Alloc = CreateArrayNoInit<T>(Num);
            for (sizet i = 0; i < Num; i++)
            {
                new (static_cast<void*>(&Alloc[i])) T(Args...);
            }
            return Alloc;
        }

      private:
        template<typename T>
        T* CreateNoInit()
        {
            sizet TotalSize = Align(sizeof(TObject<T>), static_cast<sizet>(alignof(T))) + sizeof(T);
            TObject<T>* Object = new (TConcurrentLinearAllocator<BlockAllocationTag>::template Malloc<
                                      (alignof(TObject<T>) > alignof(T) ? alignof(TObject<T>) : alignof(T))>(TotalSize)) TObject<T>();

            T* Alloc = reinterpret_cast<T*>(uptr(Object) + Align(sizeof(TObject<T>), static_cast<sizet>(alignof(T))));
            OLO_CORE_ASSERT(IsAligned(Alloc, alignof(T)), "Object not aligned");
            OLO_CORE_ASSERT(uptr(Alloc + 1) - uptr(Object) <= TotalSize, "Allocation overflow");

            Object->Next = m_Next.load(std::memory_order_relaxed);
            while (!m_Next.compare_exchange_weak(Object->Next, Object,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed))
            {
            }
            return Alloc;
        }

        template<typename T>
        T* CreateArrayNoInit(sizet Num)
        {
            sizet TotalSize = Align(sizeof(TObjectArray<T>), static_cast<sizet>(alignof(T))) + (sizeof(T) * Num);
            TObjectArray<T>* Array = new (TConcurrentLinearAllocator<BlockAllocationTag>::template Malloc<
                                          (alignof(TObjectArray<T>) > alignof(T) ? alignof(TObjectArray<T>) : alignof(T))>(TotalSize)) TObjectArray<T>(Num);

            T* Alloc = reinterpret_cast<T*>(uptr(Array) + Align(sizeof(TObjectArray<T>), static_cast<sizet>(alignof(T))));
            OLO_CORE_ASSERT(IsAligned(Alloc, alignof(T)), "Array not aligned");
            OLO_CORE_ASSERT(uptr(Alloc + Num) - uptr(Array) <= TotalSize, "Allocation overflow");

            Array->Next = m_Next.load(std::memory_order_relaxed);
            while (!m_Next.compare_exchange_weak(Array->Next, Array,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed))
            {
            }
            return Alloc;
        }
    };

    // @typedef ConcurrentLinearBulkObjectAllocator
    // @brief Default bulk object allocator
    using ConcurrentLinearBulkObjectAllocator = TConcurrentLinearBulkObjectAllocator<DefaultBlockAllocationTag>;

} // namespace OloEngine
