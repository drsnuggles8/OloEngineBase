// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

// @file ConcurrentLinearAllocator.h
// @brief Fast linear allocator for temporary allocations
// 
// This fast linear allocator can be used for temporary allocations, and is best suited
// for allocations that are produced and consumed on different threads and within the
// lifetime of a frame. Although the lifetime of any individual allocation is not hard-tied
// to a frame (tracking is done using atomic counters), the application will eventually
// run OOM if allocations are not cleaned up in a timely manner.
// 
// The allocator works by allocating a larger block in TLS which has a Header at the front
// containing an atomic counter, and all allocations are then allocated from this block.
// 
// Ported from UE5.7's Experimental/ConcurrentLinearAllocator.h

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Memory/AlignmentTemplates.h"
#include "OloEngine/Templates/UnrealTemplate.h"

#include <atomic>
#include <type_traits>
#include <cstring>

namespace OloEngine
{
    // ============================================================================
    // Thread Policy (forward declared for use in tags)
    // ============================================================================

    enum class ELinearAllocatorThreadPolicy
    {
        ThreadSafe,
        NotThreadSafe
    };

    // ============================================================================
    // Block Allocation Tags
    // ============================================================================

    // @struct FDefaultBlockAllocationTag
    // @brief Default configuration for the linear allocator
    struct FDefaultBlockAllocationTag
    {
        static constexpr u32 BlockSize = 64 * 1024;          // Block size used to allocate from
        static constexpr bool AllowOversizedBlocks = true;   // Support oversized allocations
        static constexpr bool RequiresAccurateSize = true;   // GetAllocationSize returns accurate size
        static constexpr bool InlineBlockAllocation = false; // Inline or noinline block allocation
        static constexpr u32 MaxAlignment = 256;             // Maximum supported alignment
        static constexpr const char* TagName = "DefaultLinear";
    };

    // Forward declare allocator template for Allocator typedef
    template <typename BlockAllocationTag, ELinearAllocatorThreadPolicy ThreadPolicy>
    class TLinearAllocatorBase;

    // @struct TBlockAllocationCache
    // @brief TLS cache for single-block reuse to avoid allocator round-trips
    // 
    // @tparam BlockSize Size of blocks to cache
    template<u32 BlockSize>
    class TBlockAllocationCache
    {
        struct TLSCleanup
        {
            void* Block = nullptr;

            ~TLSCleanup()
            {
                if (Block)
                {
                    FMemory::Free(Block);
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
        static constexpr bool SupportsAlignment = true;
        static constexpr bool UsesFMalloc = false;
        static constexpr u32 MaxAlignment = 256;

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
            return FMemory::Malloc(Size, Alignment);
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
            return FMemory::Free(Pointer);
        }
    };

    // @struct FLowLevelTasksBlockAllocationTag
    // @brief Configuration optimized for LowLevelTasks allocations
    // 
    // Uses TBlockAllocationCache for TLS caching to reduce allocator round-trips.
    struct FLowLevelTasksBlockAllocationTag : FDefaultBlockAllocationTag
    {
        static constexpr u32 BlockSize = 64 * 1024;
        static constexpr bool AllowOversizedBlocks = true;
        static constexpr bool RequiresAccurateSize = false;
        static constexpr bool InlineBlockAllocation = true;
        static constexpr const char* TagName = "LowLevelTasksLinear";
        
        // Use TBlockAllocationCache for TLS caching (matching UE5.7 behavior)
        using Allocator = TBlockAllocationCache<BlockSize>;
    };

    // ============================================================================
    // TLinearAllocatorBase Implementation
    // ============================================================================

    // @class TLinearAllocatorBase
    // @brief Core linear allocator implementation
    // 
    // @tparam BlockAllocationTag Configuration tag for block sizes and behavior
    // @tparam ThreadPolicy Thread safety policy
    template <typename BlockAllocationTag, ELinearAllocatorThreadPolicy ThreadPolicy>
    class TLinearAllocatorBase
    {
        // Fast path requires aligned blocks and no ASAN
        static constexpr bool SupportsFastPath = 
            ((BlockAllocationTag::BlockSize <= (64 * 1024)) && 
             (OLO_MAX_VIRTUAL_MEMORY_ALIGNMENT >= 64 * 1024)) &&
            IsPowerOfTwo(BlockAllocationTag::BlockSize) &&
            !OLO_ASAN_ENABLED &&
            !BlockAllocationTag::RequiresAccurateSize;

        struct FBlockHeader;

        // @class FAllocationHeader
        // @brief Header stored before each allocation (slow path only)
        class FAllocationHeader
        {
        public:
            OLO_FINLINE FAllocationHeader(FBlockHeader* InBlockHeader, sizet InAllocationSize)
            {
                uptr Offset = reinterpret_cast<uptr>(this) - reinterpret_cast<uptr>(InBlockHeader);
                OLO_CORE_ASSERT(Offset < UINT32_MAX);
                m_BlockHeaderOffset = static_cast<u32>(Offset);

                OLO_CORE_ASSERT(InAllocationSize < UINT32_MAX);
                m_AllocationSize = static_cast<u32>(InAllocationSize);
            }

            OLO_FINLINE FBlockHeader* GetBlockHeader() const
            {
                return reinterpret_cast<FBlockHeader*>(reinterpret_cast<uptr>(this) - m_BlockHeaderOffset);
            }

            OLO_FINLINE sizet GetAllocationSize() const
            {
                return static_cast<sizet>(m_AllocationSize);
            }

        private:
            u32 m_BlockHeaderOffset;  // Negative offset from allocation to BlockHeader
            u32 m_AllocationSize;     // Size of the allocation following this header
        };

        static OLO_FINLINE FAllocationHeader* GetAllocationHeader(void* Pointer)
        {
            if constexpr (SupportsFastPath)
            {
                return nullptr;
            }
            else
            {
                return reinterpret_cast<FAllocationHeader*>(Pointer) - 1;
            }
        }

        // @struct FBlockHeader
        // @brief Header at the start of each allocation block
        struct FBlockHeader
        {
            OLO_FINLINE FBlockHeader()
                : NextAllocationPtr(reinterpret_cast<uptr>(this) + sizeof(FBlockHeader) + sizeof(FAllocationHeader))
            {
            }

            struct FPlainUInt
            {
                unsigned int FetchSub(unsigned int N, std::memory_order)
                {
                    unsigned int OriginalValue = Value;
                    Value -= N;
                    return OriginalValue;
                }

                void Store(unsigned int N, std::memory_order)
                {
                    Value = N;
                }

                unsigned int Value;
            };

            struct FAtomicUInt
            {
                unsigned int FetchSub(unsigned int N, std::memory_order Order)
                {
                    return Value.fetch_sub(N, Order);
                }

                void Store(unsigned int N, std::memory_order Order)
                {
                    return Value.store(N, Order);
                }

                std::atomic_uint Value;
            };

            using NumAllocationsType = std::conditional_t<
                ThreadPolicy == ELinearAllocatorThreadPolicy::ThreadSafe,
                FAtomicUInt,
                FPlainUInt>;

            NumAllocationsType NumAllocations{ .Value = UINT_MAX };  // Tracks live allocations + UINT_MAX
            u8 Padding[OLO_PLATFORM_CACHE_LINE_SIZE - sizeof(std::atomic_uint)];  // Avoid false sharing
            uptr NextAllocationPtr;  // Next address to allocate from
            unsigned int Num = 0;    // TLS local number of allocations from this block
        };

        // @struct FTLSCleanup
        // @brief Handles cleanup of TLS block on thread exit
        struct FTLSCleanup
        {
            FBlockHeader* Header = nullptr;

            ~FTLSCleanup()
            {
                if (Header)
                {
                    Header->NextAllocationPtr = reinterpret_cast<uptr>(Header) + BlockAllocationTag::BlockSize;
                    const u32 DeltaCount = UINT_MAX - Header->Num;

                    // Single atomic to reduce contention with deletions
                    if (Header->NumAllocations.FetchSub(DeltaCount, std::memory_order_acq_rel) == DeltaCount)
                    {
                        // All allocations already freed, we can free the block
                        Header->~FBlockHeader();
                        OLO_ASAN_UNPOISON_MEMORY_REGION(Header, BlockAllocationTag::BlockSize);
                        FMemory::Free(Header);
                    }
                }
            }
        };

        OLO_NOINLINE static void AllocateBlock(FBlockHeader*& Header)
        {
            if constexpr (!BlockAllocationTag::InlineBlockAllocation)
            {
                static_assert(BlockAllocationTag::BlockSize >= sizeof(FBlockHeader) + sizeof(FAllocationHeader));
                u32 BlockAlignment = SupportsFastPath ? BlockAllocationTag::BlockSize : alignof(FBlockHeader);
                Header = new (FMemory::Malloc(BlockAllocationTag::BlockSize, BlockAlignment)) FBlockHeader;
                OLO_CORE_ASSERT(IsAligned(Header, BlockAlignment));
                if constexpr (!SupportsFastPath)
                {
                    OLO_ASAN_POISON_MEMORY_REGION(Header + 1, BlockAllocationTag::BlockSize - sizeof(FBlockHeader));
                }

                static thread_local FTLSCleanup Cleanup;
                new (&Cleanup) FTLSCleanup{ Header };
            }
        }

    public:
        template<u32 Alignment>
        static OLO_FINLINE void* Malloc(sizet Size)
        {
            return Malloc(Size, Alignment);
        }

        template<typename T>
        static OLO_FINLINE void* Malloc()
        {
            return Malloc(sizeof(T), alignof(T));
        }

        static void* Malloc(sizet Size, u32 Alignment)
        {
            OLO_CORE_ASSERT(Alignment >= 1 && IsPowerOfTwo(Alignment));
            if constexpr (!SupportsFastPath)
            {
                Alignment = (Alignment < alignof(FAllocationHeader)) ? alignof(FAllocationHeader) : Alignment;
#if OLO_ASAN_ENABLED
                // Ensure FAllocationHeader is 8-byte aligned for ASAN
                Alignment = Align(Alignment, 8u);
#endif
            }

            static thread_local FBlockHeader* Header = nullptr;
            if (Header == nullptr)
            {
            AllocateNewBlock:
                if constexpr (BlockAllocationTag::InlineBlockAllocation)
                {
                    static_assert(BlockAllocationTag::BlockSize >= sizeof(FBlockHeader) + sizeof(FAllocationHeader));
                    u32 BlockAlignment = SupportsFastPath ? BlockAllocationTag::BlockSize : alignof(FBlockHeader);
                    Header = new (FMemory::Malloc(BlockAllocationTag::BlockSize, BlockAlignment)) FBlockHeader;
                    OLO_CORE_ASSERT(IsAligned(Header, BlockAlignment));
                    if constexpr (!SupportsFastPath)
                    {
                        OLO_ASAN_POISON_MEMORY_REGION(Header + 1, BlockAllocationTag::BlockSize - sizeof(FBlockHeader));
                    }

                    static thread_local FTLSCleanup Cleanup;
                    new (&Cleanup) FTLSCleanup{ Header };
                }
                else
                {
                    AllocateBlock(Header);
                }
            }

        AllocateNewItem:
            if constexpr (SupportsFastPath)
            {
                uptr AlignedOffset = Align(Header->NextAllocationPtr, static_cast<uptr>(Alignment));
                if (AlignedOffset + Size <= reinterpret_cast<uptr>(Header) + BlockAllocationTag::BlockSize)
                {
                    Header->NextAllocationPtr = AlignedOffset + Size;
                    Header->Num++;
                    return reinterpret_cast<void*>(AlignedOffset);
                }

                // Cold path: oversized blocks
                constexpr sizet HeaderSize = sizeof(FBlockHeader);
                if constexpr (BlockAllocationTag::AllowOversizedBlocks)
                {
                    if (HeaderSize + Size + Alignment > BlockAllocationTag::BlockSize)
                    {
                        FBlockHeader* LargeHeader = new (FMemory::Malloc(HeaderSize + Size + Alignment, BlockAllocationTag::BlockSize)) FBlockHeader;
                        OLO_CORE_ASSERT(IsAligned(LargeHeader, alignof(FBlockHeader)));

                        uptr LargeAlignedOffset = Align(LargeHeader->NextAllocationPtr, static_cast<uptr>(Alignment));
                        LargeHeader->NextAllocationPtr = reinterpret_cast<uptr>(LargeHeader) + HeaderSize + Size + Alignment;
                        LargeHeader->NumAllocations.Store(1, std::memory_order_release);

                        OLO_CORE_ASSERT(LargeAlignedOffset + Size <= LargeHeader->NextAllocationPtr);
                        return reinterpret_cast<void*>(LargeAlignedOffset);
                    }
                }
                OLO_CORE_ASSERT(HeaderSize + Size + Alignment <= BlockAllocationTag::BlockSize);
            }
            else
            {
                uptr AlignedOffset = Align(Header->NextAllocationPtr, static_cast<uptr>(Alignment));
                if (AlignedOffset + Size <= reinterpret_cast<uptr>(Header) + BlockAllocationTag::BlockSize)
                {
                    Header->NextAllocationPtr = AlignedOffset + Size + sizeof(FAllocationHeader);
                    Header->Num++;

                    FAllocationHeader* AllocationHeader = reinterpret_cast<FAllocationHeader*>(AlignedOffset) - 1;
                    OLO_ASAN_UNPOISON_MEMORY_REGION(AllocationHeader, sizeof(FAllocationHeader) + Size);
                    new (AllocationHeader) FAllocationHeader(Header, Size);
                    OLO_ASAN_POISON_MEMORY_REGION(AllocationHeader, sizeof(FAllocationHeader));

                    return reinterpret_cast<void*>(AlignedOffset);
                }

                // Cold path: oversized blocks
                constexpr sizet HeaderSize = sizeof(FBlockHeader) + sizeof(FAllocationHeader);
                if constexpr (BlockAllocationTag::AllowOversizedBlocks)
                {
                    if (HeaderSize + Size + Alignment > BlockAllocationTag::BlockSize)
                    {
                        FBlockHeader* LargeHeader = new (FMemory::Malloc(HeaderSize + Size + Alignment, alignof(FBlockHeader))) FBlockHeader;
                        OLO_CORE_ASSERT(IsAligned(LargeHeader, alignof(FBlockHeader)));

                        uptr LargeAlignedOffset = Align(LargeHeader->NextAllocationPtr, static_cast<uptr>(Alignment));
                        LargeHeader->NextAllocationPtr = reinterpret_cast<uptr>(LargeHeader) + HeaderSize + Size + Alignment;
                        LargeHeader->NumAllocations.Store(1, std::memory_order_release);

                        OLO_CORE_ASSERT(LargeAlignedOffset + Size <= LargeHeader->NextAllocationPtr);
                        FAllocationHeader* AllocationHeader = new (reinterpret_cast<FAllocationHeader*>(LargeAlignedOffset) - 1) FAllocationHeader(LargeHeader, Size);
                        OLO_ASAN_POISON_MEMORY_REGION(AllocationHeader, sizeof(FAllocationHeader));

                        return reinterpret_cast<void*>(LargeAlignedOffset);
                    }
                }
                OLO_CORE_ASSERT(HeaderSize + Size + Alignment <= BlockAllocationTag::BlockSize);
            }

            // Allocation of a new block
            Header->NextAllocationPtr = reinterpret_cast<uptr>(Header) + BlockAllocationTag::BlockSize;
            const u32 DeltaCount = UINT_MAX - Header->Num;

            // Single atomic to reduce contention with deletions
            if (Header->NumAllocations.FetchSub(DeltaCount, std::memory_order_acq_rel) == DeltaCount)
            {
                // All allocations already freed, reuse the block
                Header->~FBlockHeader();
                Header = new (Header) FBlockHeader;
                OLO_ASAN_POISON_MEMORY_REGION(Header + 1, BlockAllocationTag::BlockSize - sizeof(FBlockHeader));
                goto AllocateNewItem;
            }

            goto AllocateNewBlock;
        }

        static void Free(void* Pointer)
        {
            if (Pointer != nullptr)
            {
                if constexpr (SupportsFastPath)
                {
                    FBlockHeader* Header = reinterpret_cast<FBlockHeader*>(AlignDown(Pointer, BlockAllocationTag::BlockSize));

                    // Delete complete blocks when the last allocation is freed
                    if (Header->NumAllocations.FetchSub(1, std::memory_order_acq_rel) == 1)
                    {
                        const uptr NextAllocationPtr = Header->NextAllocationPtr;
                        Header->~FBlockHeader();
                        FMemory::Free(Header);
                    }
                }
                else
                {
                    FAllocationHeader* AllocationHeader = GetAllocationHeader(Pointer);
                    OLO_ASAN_UNPOISON_MEMORY_REGION(AllocationHeader, sizeof(FAllocationHeader));
                    FBlockHeader* Header = AllocationHeader->GetBlockHeader();
                    OLO_ASAN_POISON_MEMORY_REGION(AllocationHeader, sizeof(FAllocationHeader) + AllocationHeader->GetAllocationSize());

                    // Delete complete blocks when the last allocation is freed
                    if (Header->NumAllocations.FetchSub(1, std::memory_order_acq_rel) == 1)
                    {
                        const uptr NextAllocationPtr = Header->NextAllocationPtr;
                        Header->~FBlockHeader();
                        OLO_ASAN_UNPOISON_MEMORY_REGION(Header, NextAllocationPtr - reinterpret_cast<uptr>(Header));
                        FMemory::Free(Header);
                    }
                }
            }
        }

        static sizet GetAllocationSize(void* Pointer)
        {
            if (Pointer)
            {
                if constexpr (SupportsFastPath)
                {
                    return Align(reinterpret_cast<uptr>(Pointer), static_cast<uptr>(BlockAllocationTag::BlockSize)) - reinterpret_cast<uptr>(Pointer);
                }
                else
                {
                    FAllocationHeader* AllocationHeader = GetAllocationHeader(Pointer);
                    OLO_ASAN_UNPOISON_MEMORY_REGION(AllocationHeader, sizeof(FAllocationHeader));
                    sizet Size = AllocationHeader->GetAllocationSize();
                    OLO_ASAN_POISON_MEMORY_REGION(AllocationHeader, sizeof(FAllocationHeader));
                    return Size;
                }
            }
            return 0;
        }

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

    // ============================================================================
    // Type Aliases
    // ============================================================================

    // @brief Thread-safe concurrent linear allocator
    // @tparam T Block allocation tag for configuration
    template <typename T>
    using TConcurrentLinearAllocator = TLinearAllocatorBase<T, ELinearAllocatorThreadPolicy::ThreadSafe>;

    // @brief Default thread-safe concurrent linear allocator
    using FConcurrentLinearAllocator = TLinearAllocatorBase<FDefaultBlockAllocationTag, ELinearAllocatorThreadPolicy::ThreadSafe>;

    // @brief Single-threaded linear allocator
    using FNonconcurrentLinearAllocator = TLinearAllocatorBase<FDefaultBlockAllocationTag, ELinearAllocatorThreadPolicy::NotThreadSafe>;

} // namespace OloEngine

