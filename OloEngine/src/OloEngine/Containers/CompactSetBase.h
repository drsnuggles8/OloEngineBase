#pragma once

/**
 * @file CompactSetBase.h
 * @brief Base class for TCompactSet providing memory management
 * 
 * TCompactSetBase manages the memory layout of the compact set:
 * - Data Array: Element storage (contiguous, no holes)
 * - Hash Size: 4-byte integer for hash table size
 * - Collision List: Per-element next index for hash collisions
 * - Hash Table: Power of 2 table for first-index lookup
 * 
 * Memory layout:
 * [Data Array][Hash Size (4 bytes)][Collision List][Hash Table]
 * 
 * Ported from Unreal Engine's Containers/CompactSetBase.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Containers/CompactHashTable.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Serialization/Archive.h"
#include <algorithm>
#include <limits>

namespace OloEngine
{
    /**
     * @struct FCompactSetLayout
     * @brief Describes the data layout of the compact set contents
     */
    struct FCompactSetLayout
    {
        i32 Size;
        i32 Alignment;
    };

    /**
     * @class TCompactSetBase
     * @brief Base class providing common functionality for TCompactSet
     * 
     * Uses FCompactSetLayout to describe element layout for type-erased operations.
     * 
     * @tparam Allocator The allocator type (e.g., FDefaultAllocator)
     */
    template <typename Allocator>
    class TCompactSetBase
    {
    public:
        using AllocatorType = Allocator;
        using SizeType = typename AllocatorType::SizeType;

        /**
         * @brief Check if set is in unset optional state
         */
        [[nodiscard]] bool operator==(FIntrusiveUnsetOptionalState Tag) const
        {
            return MaxElements == INDEX_NONE;
        }

        /**
         * @return true if the set is empty
         */
        [[nodiscard]] OLO_FINLINE bool IsEmpty() const
        {
            return NumElements == 0;
        }

        /**
         * @return The number of elements in the set
         */
        [[nodiscard]] OLO_FINLINE i32 Num() const
        {
            return NumElements;
        }

        /**
         * @return The maximum capacity before reallocation
         */
        [[nodiscard]] OLO_FINLINE i32 Max() const
        {
            return MaxElements;
        }

        /**
         * @return The max valid index (same as Num() for compact set, no holes)
         */
        [[nodiscard]] OLO_FINLINE i32 GetMaxIndex() const
        {
            return NumElements;
        }

        /**
         * @brief Calculate memory allocated by this container
         * @param Layout The element layout description
         * @return Number of bytes allocated
         */
        [[nodiscard]] OLO_FINLINE sizet GetAllocatedSize(const FCompactSetLayout Layout) const
        {
            if (MaxElements == 0)
            {
                return 0;
            }

            return GetTotalMemoryRequiredInBytes(MaxElements, *GetHashTableMemory(Layout), Layout);
        }

    protected:
        using HashCountType = u32;
        static constexpr sizet HashCountSize = sizeof(HashCountType);
        using ElementAllocatorType = typename AllocatorType::template ForElementType<u8>;

        static_assert(std::is_same_v<SizeType, i32>, "TCompactSet currently only supports 32-bit allocators");
        static_assert(sizeof(HashCountType) == CompactHashTable::GetMemoryAlignment(), 
                      "Hashtable alignment changed, need to update HashCountType");

        // ====================================================================
        // Constructors
        // ====================================================================

        [[nodiscard]] OLO_FINLINE TCompactSetBase() = default;

        [[nodiscard]] explicit consteval TCompactSetBase(EConstEval)
            : Elements(ConstEval)
            , NumElements(0)
            , MaxElements(0)
        {
        }

        /**
         * @brief Constructor for intrusive optional unset state
         */
        [[nodiscard]] explicit TCompactSetBase(FIntrusiveUnsetOptionalState Tag)
            : NumElements(0)
            , MaxElements(INDEX_NONE)
        {
        }

        // ====================================================================
        // Memory Layout Calculations
        // ====================================================================

        /**
         * @brief Get pointer to hash count storage
         */
        [[nodiscard]] OLO_FINLINE const HashCountType* GetHashTableMemory(const FCompactSetLayout Layout) const
        {
            return reinterpret_cast<const HashCountType*>(
                Elements.GetAllocation() + GetElementsSizeInBytes(MaxElements, Layout));
        }

        /**
         * @brief Get mutable view of hash table
         */
        [[nodiscard]] inline FCompactHashTableView GetHashTableView(const FCompactSetLayout Layout)
        {
            OLO_CORE_ASSERT(MaxElements > 0, "Cannot get hash table view of empty set");
            const HashCountType* HashTable = GetHashTableMemory(Layout);
            return FCompactHashTableView(
                const_cast<u8*>(reinterpret_cast<const u8*>(HashTable + 1)),
                MaxElements,
                *HashTable,
                CompactHashTable::GetMemoryRequiredInBytes(MaxElements, *HashTable));
        }

        /**
         * @brief Get const view of hash table
         */
        [[nodiscard]] inline FConstCompactHashTableView GetConstHashTableView(const FCompactSetLayout Layout) const
        {
            OLO_CORE_ASSERT(MaxElements > 0, "Cannot get hash table view of empty set");
            const HashCountType* HashTable = GetHashTableMemory(Layout);
            return FConstCompactHashTableView(
                reinterpret_cast<const u8*>(HashTable + 1),
                MaxElements,
                *HashTable,
                CompactHashTable::GetMemoryRequiredInBytes(MaxElements, *HashTable));
        }

        /**
         * @brief Calculate hash count for given element count
         */
        [[nodiscard]] OLO_FINLINE static constexpr SizeType GetHashCount(u32 InNumElements)
        {
            return static_cast<SizeType>(CompactHashTable::GetHashCount(InNumElements));
        }

        /**
         * @brief Calculate bytes required for element storage (with alignment padding)
         */
        [[nodiscard]] OLO_FINLINE static constexpr sizet GetElementsSizeInBytes(u32 InNumElements, const FCompactSetLayout Layout)
        {
            return Align(static_cast<sizet>(Layout.Size) * InNumElements, CompactHashTable::GetMemoryAlignment());
        }

        /**
         * @brief Calculate total memory required for N elements with given hash count
         */
        [[nodiscard]] OLO_FINLINE static constexpr sizet GetTotalMemoryRequiredInBytes(
            u32 InNumElements, u32 InHashCount, const FCompactSetLayout Layout)
        {
            return InNumElements ? 
                GetElementsSizeInBytes(InNumElements, Layout) + 
                CompactHashTable::GetMemoryRequiredInBytes(InNumElements, InHashCount) + 
                HashCountSize : 0;
        }

        /**
         * @brief Calculate total memory with default hash count
         */
        [[nodiscard]] OLO_FINLINE static constexpr sizet GetTotalMemoryRequiredInBytes(
            u32 InNumElements, const FCompactSetLayout Layout)
        {
            return InNumElements ? 
                GetElementsSizeInBytes(InNumElements, Layout) + 
                CompactHashTable::GetMemoryRequiredInBytes(InNumElements, GetHashCount(InNumElements)) + 
                HashCountSize : 0;
        }

        /**
         * @brief Calculate max elements for available space
         * @param TotalBytes Total bytes available
         * @param HashCount Size of hash table
         * @param MinElementCount Minimum element count (affects index type size)
         * @param Layout Element layout description
         * @return Maximum number of elements that fit
         */
        [[nodiscard]] static constexpr SizeType GetMaxElementsForAvailableSpace(
            sizet TotalBytes, u32 HashCount, u32 MinElementCount, const FCompactSetLayout Layout)
        {
            // Given some space in memory and a requested size for hash table, figure out how many elements we can fit in the remaining space
            const u32 TypeSize = CompactHashTable::GetTypeSize(MinElementCount);
            const u32 TypeShift = CompactHashTable::GetTypeShift(MinElementCount);
            const sizet AvailableBytes = TotalBytes - sizeof(HashCountType) - (static_cast<sizet>(HashCount) << TypeShift); // Remove hashtable and HashCount data
            const sizet MaxElements = AvailableBytes / (Layout.Size + TypeSize); // Calculate the max available ignoring the fact that the hash data has to be aligned
            const sizet RealAvailableBytes = AlignDown(AvailableBytes - (MaxElements << TypeShift), CompactHashTable::GetMemoryAlignment()); // Remove the max required indexes and align down
            return FMath::Min<SizeType>(static_cast<SizeType>(MaxElements), static_cast<SizeType>(RealAvailableBytes / Layout.Size)); // Now we can get the true number of elements we could potentially use within the aligned space
        }

        // ====================================================================
        // Memory Allocation
        // ====================================================================

        /**
         * @brief Calculate slack growth for allocation
         * @param NewMaxElements Desired element count
         * @param Layout Element layout description
         * @return Optimal element count including slack
         */
        [[nodiscard]] i32 AllocatorCalculateSlackGrow(i32 NewMaxElements, const FCompactSetLayout& Layout) const
        {
            const sizet OldHashCount = MaxElements > 0 ? 
                *reinterpret_cast<u32*>(Elements.GetAllocation() + GetElementsSizeInBytes(MaxElements, Layout)) : 0;
            const sizet OldSize = MaxElements > 0 ? 
                GetTotalMemoryRequiredInBytes(MaxElements, static_cast<u32>(OldHashCount), Layout) : 0;

            const sizet NewHashCount = NewMaxElements > 0 ? GetHashCount(NewMaxElements) : 0;
            const sizet NewSize = NewMaxElements > 0 ? 
                GetTotalMemoryRequiredInBytes(NewMaxElements, static_cast<u32>(NewHashCount), Layout) : 0;
            sizet NewSlackSize = 0;

            if constexpr (TAllocatorTraits<AllocatorType>::SupportsElementAlignment)
            {
                NewSlackSize = Elements.CalculateSlackGrow(NewSize, OldSize, 1, Layout.Alignment);
            }
            else
            {
                NewSlackSize = Elements.CalculateSlackGrow(NewSize, OldSize, 1);
            }

            if (NewSlackSize == NewSize)
            {
                // No slack, return as-is
                return NewMaxElements;
            }

            // Calculate elements that fit in slacked space
            SizeType SlackNumElements = GetMaxElementsForAvailableSpace(NewSlackSize, static_cast<u32>(NewHashCount), NewMaxElements, Layout);
            if (SlackNumElements <= NewMaxElements)
            {
                // At slack limit, alignment changes may cause space loss
                return NewMaxElements;
            }

            sizet SlackHashCount = GetHashCount(SlackNumElements);
            if (SlackHashCount > NewHashCount)
            {
                // Too much space needed, cut hash count in half
                SlackNumElements = static_cast<SizeType>(SlackHashCount - 1);
                SlackHashCount /= 2;
            }

            OLO_CORE_ASSERT(SlackNumElements >= NewMaxElements, "Slack calculation error");
            OLO_CORE_ASSERT(GetTotalMemoryRequiredInBytes(SlackNumElements, static_cast<u32>(SlackHashCount), Layout) <= NewSlackSize, 
                           "Slack size calculation error");

            return SlackNumElements;
        }

        /**
         * @brief Resize allocation for new max element count
         */
        void ResizeAllocation(i32 NewMaxElements, const FCompactSetLayout& Layout)
        {
            (void)ResizeAllocationPreserveData(NewMaxElements, Layout, false);
        }

        /**
         * @brief Resize allocation, optionally preserving existing data
         * @return true if rehash is required
         */
        [[nodiscard]] bool ResizeAllocationPreserveData(i32 NewMaxElements, const FCompactSetLayout& Layout, bool bPreserve = true)
        {
            bool bRequiresRehash = false;

            if (NewMaxElements != MaxElements)
            {
                const sizet OldHashCount = MaxElements > 0 ? 
                    *reinterpret_cast<u32*>(Elements.GetAllocation() + GetElementsSizeInBytes(MaxElements, Layout)) : 0;
                const sizet OldSize = MaxElements > 0 ? 
                    GetTotalMemoryRequiredInBytes(MaxElements, static_cast<u32>(OldHashCount), Layout) : 0;

                const sizet NewHashCount = NewMaxElements > 0 ? 
                    CompactHashTable::GetHashCount(NewMaxElements) : 0;
                const sizet NewSize = NewMaxElements > 0 ? 
                    GetTotalMemoryRequiredInBytes(NewMaxElements, static_cast<u32>(NewHashCount), Layout) : 0;

                if (bPreserve && NewMaxElements > MaxElements && OldHashCount == NewHashCount)
                {
                    OLO_CORE_ASSERT(NewSize >= 0 && NewSize <= std::numeric_limits<i32>::max(), 
                        "Invalid size for TSet[{}]: NewMaxElements[{}] ElementSize[{}] HashCount[{}]", 
                        NewSize, NewMaxElements, Layout.Size, NewHashCount);

                    // Preserving and growing with same hash count - copy everything
                    if constexpr (TAllocatorTraits<AllocatorType>::SupportsElementAlignment)
                    {
                        Elements.ResizeAllocation(OldSize, NewSize, 1, Layout.Alignment);
                    }
                    else
                    {
                        Elements.ResizeAllocation(OldSize, NewSize, 1);
                    }

                    const u32 NewTypeShift = CompactHashTable::GetTypeShift(NewMaxElements);

                    // This should always be true since our type size will change on Powerof2 barriers (256, 65536, etc)
                    OLO_CORE_ASSERT(NewTypeShift == CompactHashTable::GetTypeShift(MaxElements), "TypeShift mismatch");

                    const u8* OldHashTable = Elements.GetAllocation() + GetElementsSizeInBytes(MaxElements, Layout);
                    u8* NewHashTable = Elements.GetAllocation() + GetElementsSizeInBytes(NewMaxElements, Layout);

                    const u8* OldHashLocation = OldHashTable + (static_cast<sizet>(MaxElements) << NewTypeShift) + HashCountSize;
                    u8* NewHashLocation = NewHashTable + (static_cast<sizet>(NewMaxElements) << NewTypeShift) + HashCountSize;

                    // Copy hash lookup table first (to free space for other data)
                    FMemory::Memmove(NewHashLocation, OldHashLocation, NewHashCount << NewTypeShift);

                    // Copy hash size + next index table
                    FMemory::Memmove(NewHashTable, OldHashTable, (static_cast<sizet>(NumElements) << NewTypeShift) + HashCountSize);
                }
                else
                {
                    // Not preserving or shrinking - only copy element data
                    if constexpr (TAllocatorTraits<AllocatorType>::SupportsElementAlignment)
                    {
                        Elements.ResizeAllocation(NumElements * Layout.Size, NewSize, 1, Layout.Alignment);
                    }
                    else
                    {
                        Elements.ResizeAllocation(NumElements * Layout.Size, NewSize, 1);
                    }

                    // Update hash count
                    if (NewMaxElements > 0)
                    {
                        u32* NewHashTable = reinterpret_cast<u32*>(
                            Elements.GetAllocation() + GetElementsSizeInBytes(NewMaxElements, Layout));
                        *NewHashTable = static_cast<u32>(NewHashCount);
                        bRequiresRehash = true;
                    }
                }

                MaxElements = NewMaxElements;
            }

            return bRequiresRehash;
        }

        // ====================================================================
        // Member Data
        // ====================================================================

        ElementAllocatorType Elements;
        SizeType NumElements = 0;
        SizeType MaxElements = 0;
    };

} // namespace OloEngine
