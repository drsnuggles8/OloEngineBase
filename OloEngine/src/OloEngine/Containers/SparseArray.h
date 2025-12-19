#pragma once

/**
 * @file SparseArray.h
 * @brief Sparse array container with O(1) add/remove using free list
 *
 * Provides a dynamically sized array where element indices aren't necessarily
 * contiguous. Memory is allocated for all elements in the array's index range,
 * but it allows O(1) element removal that doesn't invalidate indices of other
 * elements.
 *
 * Key components:
 * - TSparseArrayElementOrFreeListLink: Union overlapping element data with free list links
 * - FSparseArrayAllocationInfo: Allocation result with index and pointer
 * - TSparseArrayBase: Base class with type-erased operations
 * - TSparseArray: Type-safe sparse array template
 *
 * Used as a foundation for TSet's element storage.
 *
 * Ported from Unreal Engine's Containers/SparseArray.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/ArrayView.h"
#include "OloEngine/Containers/BitArray.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Memory/MemoryOps.h"
#include "OloEngine/Memory/AlignmentTemplates.h"
#include "OloEngine/Algo/Sort.h"
#include "OloEngine/Algo/StableSort.h"
#include <cstdint>
#include <utility>

// Ranged-for iterator checks in debug builds
#if defined(OLO_DIST) || defined(NDEBUG)
#define TSPARSEARRAY_RANGED_FOR_CHECKS 0
#else
#define TSPARSEARRAY_RANGED_FOR_CHECKS 1
#endif

namespace OloEngine
{
    // ============================================================================
    // Forward Declarations
    // ============================================================================

    template<typename InElementType, typename Allocator>
    class TSparseArray;

    // ============================================================================
    // FSparseArrayAllocationInfo
    // ============================================================================

    /**
     * @struct FSparseArrayAllocationInfo
     * @brief The result of a sparse array allocation
     */
    struct FSparseArrayAllocationInfo
    {
        i32 Index;
        void* Pointer;
    };

} // namespace OloEngine

// Forward declarations of placement new operators (must be at global scope)
template<typename T, typename Allocator>
void* operator new(sizet Size, OloEngine::TSparseArray<T, Allocator>& Array);
template<typename T, typename Allocator>
void* operator new(sizet Size, OloEngine::TSparseArray<T, Allocator>& Array, std::int32_t Index);

// Allow placement new with FSparseArrayAllocationInfo (must be at global scope)
inline void* operator new(sizet, const OloEngine::FSparseArrayAllocationInfo& Allocation)
{
    return Allocation.Pointer;
}

inline void operator delete(void*, const OloEngine::FSparseArrayAllocationInfo&) {}

namespace OloEngine
{

    // ============================================================================
    // TSparseArrayElementOrFreeListLink
    // ============================================================================

    /**
     * @struct TSparseArrayElementOrFreeListLink
     * @brief Union type that stores either an element or free list link info
     *
     * For allocated slots, ElementData contains the actual element.
     * For free slots, PrevFreeIndex/NextFreeIndex form a doubly-linked free list.
     */
    template<typename ElementType>
    union TSparseArrayElementOrFreeListLink
    {
        /** Braces are needed to explicitly value-initialize the union */
        TSparseArrayElementOrFreeListLink()
            : PrevFreeIndex(-1), NextFreeIndex(-1)
        {
        }

        /** The element data when this slot is allocated */
        ElementType ElementData;

        /** Free list link data when this slot is not allocated */
        struct
        {
            i32 PrevFreeIndex;
            i32 NextFreeIndex;
        };
    };

    // ============================================================================
    // TSparseArrayBase
    // ============================================================================

    /**
     * @class TSparseArrayBase
     * @brief Type-erased base class for sparse array operations
     *
     * Contains all operations that don't depend on the actual element type,
     * avoiding template instantiation for compatible types.
     */
    template<sizet SizeOfElementType, sizet AlignOfElementType, typename Allocator>
    class TSparseArrayBase
    {
      protected:
        /** Constructor for intrusive optional unset state */
        [[nodiscard]] explicit TSparseArrayBase(FIntrusiveUnsetOptionalState)
            : Data(FIntrusiveUnsetOptionalState{})
        {
        }

      public:
        // ========================================================================
        // Constructors
        // ========================================================================

        [[nodiscard]] constexpr TSparseArrayBase() = default;

        [[nodiscard]] explicit consteval TSparseArrayBase(EConstEval)
            : Data(ConstEval), AllocationFlags(ConstEval)
        {
        }

        // ========================================================================
        // Size / Capacity
        // ========================================================================

        /** Returns the maximum valid index + 1 */
        [[nodiscard]] i32 GetMaxIndex() const
        {
            return Data.Num();
        }

        /** Returns true if the array has no allocated elements */
        [[nodiscard]] bool IsEmpty() const
        {
            return Data.Num() == NumFreeIndices;
        }

        /** Returns the number of allocated elements */
        [[nodiscard]] i32 Num() const
        {
            return Data.Num() - NumFreeIndices;
        }

        /** Returns the number of elements the array can hold before reallocation */
        [[nodiscard]] i32 Max() const
        {
            return Data.Max();
        }

        /** Checks if an index is valid (in bounds and allocated) */
        [[nodiscard]] bool IsValidIndex(i32 Index) const
        {
            return AllocationFlags.IsValidIndex(Index) && AllocationFlags[Index];
        }

        /** Checks if an index is allocated (assumes in bounds) */
        [[nodiscard]] bool IsAllocated(i32 Index) const
        {
            return AllocationFlags[Index];
        }

        // ========================================================================
        // Allocation Operations
        // ========================================================================

        /**
         * @brief Allocate space for an element without constructing it
         * @return Allocation info with index and pointer
         */
        FSparseArrayAllocationInfo AddUninitialized()
        {
            i32 Index;
            FElementOrFreeListLink* DataPtr;

            if (NumFreeIndices > 0)
            {
                // Reuse a free slot from the free list
                Index = FirstFreeIndex;
                DataPtr = reinterpret_cast<FElementOrFreeListLink*>(Data.GetData());

                // Update FirstFreeIndex
                FirstFreeIndex = DataPtr[Index].NextFreeIndex;

                // Update the next free element's prev link
                if (FirstFreeIndex >= 0)
                {
                    DataPtr[FirstFreeIndex].PrevFreeIndex = -1;
                }

                --NumFreeIndices;
            }
            else
            {
                // Add a new element
                Index = Data.AddUninitialized(1);
                AllocationFlags.Add(true);

                // Get data pointer after potential reallocation
                DataPtr = reinterpret_cast<FElementOrFreeListLink*>(Data.GetData());
            }

            // Mark as allocated
            AllocationFlags[Index] = true;

            FSparseArrayAllocationInfo Result;
            Result.Index = Index;
            Result.Pointer = &DataPtr[Index].ElementData;
            return Result;
        }

        /**
         * @brief Allocate at the lowest available free index
         * @param LowestFreeIndexSearchStart  Hint for where to start searching
         * @return Allocation info with index and pointer
         */
        FSparseArrayAllocationInfo AddUninitializedAtLowestFreeIndex(i32& LowestFreeIndexSearchStart)
        {
            i32 Index;
            FElementOrFreeListLink* DataPtr;

            if (NumFreeIndices > 0)
            {
                // Find the lowest free index starting from the hint
                Index = AllocationFlags.FindAndSetFirstZeroBit(LowestFreeIndexSearchStart);
                LowestFreeIndexSearchStart = Index + 1;

                DataPtr = reinterpret_cast<FElementOrFreeListLink*>(Data.GetData());

                // Update FirstFreeIndex if needed
                if (FirstFreeIndex == Index)
                {
                    FirstFreeIndex = DataPtr[Index].NextFreeIndex;
                }

                // Link prev and next free nodes together
                if (DataPtr[Index].NextFreeIndex >= 0)
                {
                    DataPtr[DataPtr[Index].NextFreeIndex].PrevFreeIndex = DataPtr[Index].PrevFreeIndex;
                }

                if (DataPtr[Index].PrevFreeIndex >= 0)
                {
                    DataPtr[DataPtr[Index].PrevFreeIndex].NextFreeIndex = DataPtr[Index].NextFreeIndex;
                }

                --NumFreeIndices;
            }
            else
            {
                // Add a new element
                Index = Data.AddUninitialized(1);
                AllocationFlags.Add(true);

                DataPtr = reinterpret_cast<FElementOrFreeListLink*>(Data.GetData());
            }

            FSparseArrayAllocationInfo Result;
            Result.Index = Index;
            Result.Pointer = &DataPtr[Index].ElementData;
            return Result;
        }

        /**
         * @brief Mark an existing index as allocated
         * @param Index  The index to mark as allocated
         * @return Allocation info with the index and pointer
         */
        FSparseArrayAllocationInfo AllocateIndex(i32 Index)
        {
            OLO_CORE_ASSERT(Index >= 0 && Index < Data.Num(), "Index out of bounds");
            OLO_CORE_ASSERT(!AllocationFlags[Index], "Index already allocated");

            AllocationFlags[Index] = true;

            FSparseArrayAllocationInfo Result;
            Result.Index = Index;
            Result.Pointer = &reinterpret_cast<FElementOrFreeListLink*>(Data.GetData())[Index].ElementData;
            return Result;
        }

        /**
         * @brief Allocate space at a specific index
         * @param Index  The index at which to allocate
         * @return Allocation info with index and pointer
         */
        FSparseArrayAllocationInfo InsertUninitialized(i32 Index)
        {
            FElementOrFreeListLink* DataPtr;

            // Enlarge the array to include the given index
            if (Index >= Data.Num())
            {
                Data.AddUninitialized(Index + 1 - Data.Num());

                DataPtr = reinterpret_cast<FElementOrFreeListLink*>(Data.GetData());

                while (AllocationFlags.Num() < Data.Num())
                {
                    const i32 FreeIndex = AllocationFlags.Num();
                    DataPtr[FreeIndex].PrevFreeIndex = -1;
                    DataPtr[FreeIndex].NextFreeIndex = FirstFreeIndex;
                    if (NumFreeIndices)
                    {
                        DataPtr[FirstFreeIndex].PrevFreeIndex = FreeIndex;
                    }
                    FirstFreeIndex = FreeIndex;
                    AllocationFlags.Add(false);
                    ++NumFreeIndices;
                }
            }
            else
            {
                DataPtr = reinterpret_cast<FElementOrFreeListLink*>(Data.GetData());
            }

            // Verify that the specified index is free
            OLO_CORE_ASSERT(!AllocationFlags[Index], "Index already allocated");

            // Remove the index from the free list
            --NumFreeIndices;
            const i32 PrevFreeIndex = DataPtr[Index].PrevFreeIndex;
            const i32 NextFreeIndex = DataPtr[Index].NextFreeIndex;
            if (PrevFreeIndex != -1)
            {
                DataPtr[PrevFreeIndex].NextFreeIndex = NextFreeIndex;
            }
            else
            {
                FirstFreeIndex = NextFreeIndex;
            }
            if (NextFreeIndex != -1)
            {
                DataPtr[NextFreeIndex].PrevFreeIndex = PrevFreeIndex;
            }

            return AllocateIndex(Index);
        }

        /**
         * @brief Remove elements without destructing them
         * @param Index  First index to remove
         * @param Count  Number of elements to remove
         */
        void RemoveAtUninitialized(i32 Index, i32 Count = 1)
        {
            FElementOrFreeListLink* DataPtr = reinterpret_cast<FElementOrFreeListLink*>(Data.GetData());

            for (; Count; --Count)
            {
                OLO_CORE_ASSERT(AllocationFlags[Index], "Cannot remove unallocated element");

                // Mark the element as free and add it to the free list
                if (NumFreeIndices)
                {
                    DataPtr[FirstFreeIndex].PrevFreeIndex = Index;
                }
                DataPtr[Index].PrevFreeIndex = -1;
                DataPtr[Index].NextFreeIndex = NumFreeIndices > 0 ? FirstFreeIndex : INDEX_NONE;
                FirstFreeIndex = Index;
                ++NumFreeIndices;
                AllocationFlags[Index] = false;

                ++Index;
            }
        }

        /**
         * @brief Reserve space for elements
         * @param ExpectedNumElements  Total number of elements to reserve space for
         */
        void Reserve(i32 ExpectedNumElements)
        {
            if (ExpectedNumElements > Data.Num())
            {
                const i32 ElementsToAdd = ExpectedNumElements - Data.Num();

                Data.Reserve(ExpectedNumElements);
                i32 ElementIndex = Data.AddUninitialized(ElementsToAdd);

                FElementOrFreeListLink* DataPtr = reinterpret_cast<FElementOrFreeListLink*>(Data.GetData());

                // Mark the new elements as free
                for (i32 FreeIndex = ExpectedNumElements - 1; FreeIndex >= ElementIndex; --FreeIndex)
                {
                    if (NumFreeIndices)
                    {
                        DataPtr[FirstFreeIndex].PrevFreeIndex = FreeIndex;
                    }
                    DataPtr[FreeIndex].PrevFreeIndex = -1;
                    DataPtr[FreeIndex].NextFreeIndex = NumFreeIndices > 0 ? FirstFreeIndex : INDEX_NONE;
                    FirstFreeIndex = FreeIndex;
                    ++NumFreeIndices;
                }

                if (ElementsToAdd == ExpectedNumElements)
                {
                    AllocationFlags.Init(false, ElementsToAdd);
                }
                else
                {
                    AllocationFlags.Add(false, ElementsToAdd);
                }
            }
        }

        /** Shrinks the array's storage to avoid slack */
        void Shrink()
        {
            // Determine the highest allocated index
            i32 MaxAllocatedIndex = AllocationFlags.FindLast(true);

            const i32 FirstIndexToRemove = MaxAllocatedIndex + 1;
            if (FirstIndexToRemove < Data.Num())
            {
                if (NumFreeIndices > 0)
                {
                    FElementOrFreeListLink* DataPtr = reinterpret_cast<FElementOrFreeListLink*>(Data.GetData());

                    // Remove free list elements that will be truncated
                    i32 FreeIndex = FirstFreeIndex;
                    while (FreeIndex != INDEX_NONE)
                    {
                        if (FreeIndex >= FirstIndexToRemove)
                        {
                            const i32 PrevFreeIndex = DataPtr[FreeIndex].PrevFreeIndex;
                            const i32 NextFreeIndex = DataPtr[FreeIndex].NextFreeIndex;
                            if (NextFreeIndex != -1)
                            {
                                DataPtr[NextFreeIndex].PrevFreeIndex = PrevFreeIndex;
                            }
                            if (PrevFreeIndex != -1)
                            {
                                DataPtr[PrevFreeIndex].NextFreeIndex = NextFreeIndex;
                            }
                            else
                            {
                                FirstFreeIndex = NextFreeIndex;
                            }
                            --NumFreeIndices;

                            FreeIndex = NextFreeIndex;
                        }
                        else
                        {
                            FreeIndex = DataPtr[FreeIndex].NextFreeIndex;
                        }
                    }
                }

                // Truncate unallocated elements
                Data.RemoveAt(FirstIndexToRemove, Data.Num() - FirstIndexToRemove, EAllowShrinking::No);
                AllocationFlags.RemoveAt(FirstIndexToRemove, AllocationFlags.Num() - FirstIndexToRemove);
            }

            Data.Shrink();
        }

        /**
         * @brief Sort the free list for deterministic allocation order
         *
         * Makes subsequent allocations occur at the lowest available position.
         */
        void SortFreeList()
        {
            FElementOrFreeListLink* DataPtr = reinterpret_cast<FElementOrFreeListLink*>(Data.GetData());
            i32 CurrentHeadIndex = INDEX_NONE;
            i32 NumFreeIndicesProcessed = 0;

            // Reverse iteration to build list from low to high indices
            for (i32 Index = Data.Num() - 1; NumFreeIndicesProcessed < NumFreeIndices; --Index)
            {
                if (!IsValidIndex(Index))
                {
                    DataPtr[Index].PrevFreeIndex = INDEX_NONE;
                    DataPtr[Index].NextFreeIndex = INDEX_NONE;

                    if (CurrentHeadIndex != INDEX_NONE)
                    {
                        DataPtr[CurrentHeadIndex].PrevFreeIndex = Index;
                        DataPtr[Index].NextFreeIndex = CurrentHeadIndex;
                    }

                    CurrentHeadIndex = Index;
                    ++NumFreeIndicesProcessed;
                }
            }

            FirstFreeIndex = CurrentHeadIndex;
        }

        /** Compact elements into a contiguous range (may change order) */
        bool Compact()
        {
            i32 NumFree = NumFreeIndices;
            if (NumFree == 0)
            {
                return false;
            }

            bool bResult = false;

            FElementOrFreeListLink* DataPtr = reinterpret_cast<FElementOrFreeListLink*>(Data.GetData());

            i32 EndIndex = Data.Num();
            i32 TargetIndex = EndIndex - NumFree;
            i32 FreeIndex = FirstFreeIndex;
            while (FreeIndex != -1)
            {
                i32 NextFreeIndex = DataPtr[FreeIndex].NextFreeIndex;
                if (FreeIndex < TargetIndex)
                {
                    // We need an element here - find one from the end
                    do
                    {
                        --EndIndex;
                    } while (!AllocationFlags[EndIndex]);

                    RelocateConstructItems<FElementOrFreeListLink>(DataPtr + FreeIndex, DataPtr + EndIndex, 1);
                    AllocationFlags[FreeIndex] = true;

                    bResult = true;
                }

                FreeIndex = NextFreeIndex;
            }

            Data.RemoveAt(TargetIndex, NumFree, EAllowShrinking::No);
            AllocationFlags.RemoveAt(TargetIndex, NumFree);

            NumFreeIndices = 0;
            FirstFreeIndex = -1;

            Data.Shrink();

            return bResult;
        }

      protected:
        // Move helper for derived classes
        template<typename ArrayType>
        static void Move(ArrayType& To, ArrayType& From)
        {
            To.Data = MoveTemp(From.Data);
            To.AllocationFlags = MoveTemp(From.AllocationFlags);
            To.FirstFreeIndex = From.FirstFreeIndex;
            To.NumFreeIndices = From.NumFreeIndices;

            From.FirstFreeIndex = -1;
            From.NumFreeIndices = 0;
        }

        // ========================================================================
        // Type Definitions
        // ========================================================================

        /**
         * The element type stored is only indirectly related to the element type requested,
         * to avoid instantiating TArray redundantly for compatible types.
         */
        using FElementOrFreeListLink = TSparseArrayElementOrFreeListLink<
            TAlignedBytes<SizeOfElementType, AlignOfElementType>>;

        using DataType = TArray<FElementOrFreeListLink, typename Allocator::ElementAllocator>;
        DataType Data;

        using AllocationBitArrayType = TBitArray<typename Allocator::BitArrayAllocator>;
        AllocationBitArrayType AllocationFlags;

        /** Head of the free list (-1 if empty) */
        i32 FirstFreeIndex = -1;

        /** Number of free slots */
        i32 NumFreeIndices = 0;
    };

    // ============================================================================
    // TSparseArray
    // ============================================================================

    /**
     * @class TSparseArray
     * @brief A dynamically sized array where element indices aren't necessarily contiguous
     *
     * Memory is allocated for all elements in the array's index range, but removed
     * elements leave holes that can be reused. This allows O(1) removal without
     * invalidating indices of other elements.
     *
     * @tparam InElementType  The type of elements stored
     * @tparam Allocator      The allocator policy (default: FDefaultSparseArrayAllocator)
     */
    template<typename InElementType, typename Allocator = FDefaultSparseArrayAllocator>
    class TSparseArray : public TSparseArrayBase<sizeof(InElementType), alignof(InElementType), Allocator>
    {
        using SuperType = TSparseArrayBase<sizeof(InElementType), alignof(InElementType), Allocator>;
        using ElementType = InElementType;

      public:
        using SuperType::AllocationFlags;
        using SuperType::Data;
        using SuperType::FirstFreeIndex;
        using SuperType::NumFreeIndices;
        using typename SuperType::AllocationBitArrayType;
        using typename SuperType::DataType;
        using typename SuperType::FElementOrFreeListLink;

        // ========================================================================
        // Intrusive TOptional<TSparseArray> State
        // ========================================================================

        /** Enables intrusive optional state for this type */
        constexpr static bool bHasIntrusiveUnsetOptionalState = true;
        using IntrusiveUnsetOptionalStateType = TSparseArray;

        /** Constructor for intrusive optional unset state */
        [[nodiscard]] explicit TSparseArray(FIntrusiveUnsetOptionalState Tag)
            : SuperType(Tag)
        {
        }

        /** Comparison with intrusive optional unset state */
        [[nodiscard]] bool operator==(FIntrusiveUnsetOptionalState Tag) const
        {
            return Data == Tag;
        }

        // ========================================================================
        // Constructors / Destructor
        // ========================================================================

        /** Default constructor */
        [[nodiscard]] constexpr TSparseArray() = default;

        /** Explicitly consteval constructor for compile-time constant arrays */
        [[nodiscard]] explicit consteval TSparseArray(EConstEval)
            : SuperType(ConstEval)
        {
        }

        /** Move constructor */
        [[nodiscard]] TSparseArray(TSparseArray&& Other)
        {
            SuperType::Move(*this, Other);
        }

        /** Copy constructor */
        [[nodiscard]] TSparseArray(const TSparseArray& Other)
        {
            *this = Other;
        }

        /** Initializer list constructor */
        [[nodiscard]] TSparseArray(std::initializer_list<ElementType> InitList)
        {
            for (const auto& Element : InitList)
            {
                Add(Element);
            }
        }

        /** Destructor */
        ~TSparseArray()
        {
            Empty();
        }

        // ========================================================================
        // Assignment Operators
        // ========================================================================

        /** Move assignment */
        TSparseArray& operator=(TSparseArray&& Other)
        {
            if (this != &Other)
            {
                Empty();
                SuperType::Move(*this, Other);
            }
            return *this;
        }

        /** Copy assignment */
        TSparseArray& operator=(const TSparseArray& Other)
        {
            if (this != &Other)
            {
                i32 SrcMax = Other.GetMaxIndex();

                // Reallocate the array
                Empty(SrcMax);
                Data.AddUninitialized(SrcMax);

                // Copy the other array's element allocation state
                FirstFreeIndex = Other.FirstFreeIndex;
                NumFreeIndices = Other.NumFreeIndices;
                AllocationFlags = Other.AllocationFlags;

                FElementOrFreeListLink* DestData = reinterpret_cast<FElementOrFreeListLink*>(Data.GetData());
                const FElementOrFreeListLink* SrcData = reinterpret_cast<const FElementOrFreeListLink*>(Other.Data.GetData());

                if constexpr (!std::is_trivially_copy_constructible_v<ElementType>)
                {
                    for (i32 Index = 0; Index < SrcMax; ++Index)
                    {
                        FElementOrFreeListLink& DestElement = DestData[Index];
                        const FElementOrFreeListLink& SrcElement = SrcData[Index];
                        if (Other.IsAllocated(Index))
                        {
                            ::new (reinterpret_cast<u8*>(&DestElement.ElementData)) ElementType(
                                *reinterpret_cast<const ElementType*>(&SrcElement.ElementData));
                        }
                        else
                        {
                            DestElement.PrevFreeIndex = SrcElement.PrevFreeIndex;
                            DestElement.NextFreeIndex = SrcElement.NextFreeIndex;
                        }
                    }
                }
                else
                {
                    if (SrcMax)
                    {
                        std::memcpy(DestData, SrcData, sizeof(FElementOrFreeListLink) * SrcMax);
                    }
                }
            }
            return *this;
        }

        /** Initializer list assignment */
        TSparseArray& operator=(std::initializer_list<ElementType> InitList)
        {
            Empty(static_cast<i32>(InitList.size()));
            for (const auto& Element : InitList)
            {
                Add(Element);
            }
            return *this;
        }

        // ========================================================================
        // Comparison Operators
        // ========================================================================

        [[nodiscard]] bool operator==(const TSparseArray& Other) const
        {
            if (GetMaxIndex() != Other.GetMaxIndex())
            {
                return false;
            }

            for (i32 ElementIndex = 0; ElementIndex < GetMaxIndex(); ++ElementIndex)
            {
                const bool bIsAllocatedA = IsAllocated(ElementIndex);
                const bool bIsAllocatedB = Other.IsAllocated(ElementIndex);
                if (bIsAllocatedA != bIsAllocatedB)
                {
                    return false;
                }
                else if (bIsAllocatedA)
                {
                    if ((*this)[ElementIndex] != Other[ElementIndex])
                    {
                        return false;
                    }
                }
            }

            return true;
        }

        [[nodiscard]] bool operator!=(const TSparseArray& Other) const
        {
            return !(*this == Other);
        }

        // ========================================================================
        // Element Access
        // ========================================================================

        using SuperType::GetMaxIndex;
        using SuperType::IsAllocated;
        using SuperType::IsEmpty;
        using SuperType::IsValidIndex;
        using SuperType::Max;
        using SuperType::Num;

        /** Access element by index */
        [[nodiscard]] ElementType& operator[](i32 Index)
        {
            OLO_CORE_ASSERT(IsAllocated(Index), "Accessing unallocated sparse array element");
            return *reinterpret_cast<ElementType*>(
                &reinterpret_cast<FElementOrFreeListLink*>(Data.GetData())[Index].ElementData);
        }

        /** Access element by index (const) */
        [[nodiscard]] const ElementType& operator[](i32 Index) const
        {
            OLO_CORE_ASSERT(IsAllocated(Index), "Accessing unallocated sparse array element");
            return *reinterpret_cast<const ElementType*>(
                &reinterpret_cast<const FElementOrFreeListLink*>(Data.GetData())[Index].ElementData);
        }

        // ========================================================================
        // Add / Emplace
        // ========================================================================

        using SuperType::AddUninitialized;
        using SuperType::AddUninitializedAtLowestFreeIndex;
        using SuperType::AllocateIndex;
        using SuperType::InsertUninitialized;

        /** Add an element (copy) */
        i32 Add(const ElementType& Element)
        {
            FSparseArrayAllocationInfo Allocation = AddUninitialized();
            new (Allocation) ElementType(Element);
            return Allocation.Index;
        }

        /** Add an element (move) */
        i32 Add(ElementType&& Element)
        {
            FSparseArrayAllocationInfo Allocation = AddUninitialized();
            new (Allocation) ElementType(MoveTemp(Element));
            return Allocation.Index;
        }

        /** Construct an element in place */
        template<typename... ArgsType>
        i32 Emplace(ArgsType&&... Args)
        {
            FSparseArrayAllocationInfo Allocation = AddUninitialized();
            new (Allocation) ElementType(Forward<ArgsType>(Args)...);
            return Allocation.Index;
        }

        /** Construct at the lowest free index */
        template<typename... ArgsType>
        i32 EmplaceAtLowestFreeIndex(i32& LowestFreeIndexSearchStart, ArgsType&&... Args)
        {
            FSparseArrayAllocationInfo Allocation = AddUninitializedAtLowestFreeIndex(LowestFreeIndexSearchStart);
            new (Allocation) ElementType(Forward<ArgsType>(Args)...);
            return Allocation.Index;
        }

        /** Construct at a specific index */
        template<typename... ArgsType>
        i32 EmplaceAt(i32 Index, ArgsType&&... Args)
        {
            FSparseArrayAllocationInfo Allocation;
            if (!AllocationFlags.IsValidIndex(Index) || !AllocationFlags[Index])
            {
                Allocation = InsertUninitialized(Index);
            }
            else
            {
                FElementOrFreeListLink* Elem = reinterpret_cast<FElementOrFreeListLink*>(Data.GetData()) + Index;
                reinterpret_cast<ElementType&>(Elem->ElementData).~ElementType();

                Allocation.Index = Index;
                Allocation.Pointer = &Elem->ElementData;
            }

            new (Allocation) ElementType(Forward<ArgsType>(Args)...);
            return Allocation.Index;
        }

        /** Insert an element at a specific index */
        void Insert(i32 Index, const ElementType& Element)
        {
            new (InsertUninitialized(Index)) ElementType(Element);
        }

        // ========================================================================
        // Remove
        // ========================================================================

        using SuperType::RemoveAtUninitialized;

        /** Remove elements from the array */
        void RemoveAt(i32 Index, i32 Count = 1)
        {
            if constexpr (!std::is_trivially_destructible_v<ElementType>)
            {
                for (i32 i = Index; i < Index + Count; ++i)
                {
                    if (IsAllocated(i))
                    {
                        (*this)[i].~ElementType();
                    }
                }
            }

            RemoveAtUninitialized(Index, Count);
        }

        /**
         * @brief Remove all elements
         * @param ExpectedNumElements  Expected number of elements to be added
         */
        void Empty(i32 ExpectedNumElements = 0)
        {
            // Destruct allocated elements
            if constexpr (!std::is_trivially_destructible_v<ElementType>)
            {
                for (TIterator It(*this); It; ++It)
                {
                    (*It).~ElementType();
                }
            }

            // Free the allocated elements
            Data.Empty(ExpectedNumElements);
            FirstFreeIndex = -1;
            NumFreeIndices = 0;
            AllocationFlags.Empty(ExpectedNumElements);
        }

        /** Empty the array but keep allocated memory as slack */
        void Reset()
        {
            // Destruct allocated elements
            if constexpr (!std::is_trivially_destructible_v<ElementType>)
            {
                for (TIterator It(*this); It; ++It)
                {
                    (*It).~ElementType();
                }
            }

            Data.Reset();
            FirstFreeIndex = -1;
            NumFreeIndices = 0;
            AllocationFlags.Reset();
        }

        // ========================================================================
        // Other Operations
        // ========================================================================

        using SuperType::Compact;
        using SuperType::Reserve;
        using SuperType::Shrink;
        using SuperType::SortFreeList;

        /** Compact elements while preserving iteration order */
        bool CompactStable()
        {
            if (NumFreeIndices == 0)
            {
                return false;
            }

            // Copy existing elements to a new array
            TSparseArray<ElementType, Allocator> CompactedArray;
            CompactedArray.Empty(Num());
            for (TIterator It(*this); It; ++It)
            {
                new (CompactedArray.AddUninitialized()) ElementType(MoveTemp(*It));
            }

            // Replace this array with the compacted array
            std::swap(*this, CompactedArray);

            return true;
        }

        /** Sort elements using a predicate */
        template<typename PredicateClass>
        void Sort(const PredicateClass& Predicate)
        {
            if (Num() > 0)
            {
                // Compact the elements array so all the elements are contiguous
                Compact();

                // Sort the elements according to the provided comparison class
                Algo::Sort(TArrayView<FElementOrFreeListLink>(Data.GetData(), Num()), FElementCompareClass<PredicateClass>(Predicate));
            }
        }

        /** Sort elements using operator< */
        void Sort()
        {
            Sort(TLess<ElementType>());
        }

        /** Stable sort elements using a predicate (preserves relative order of equal elements) */
        template<typename PredicateClass>
        void StableSort(const PredicateClass& Predicate)
        {
            if (Num() > 0)
            {
                // Compact the elements array so all the elements are contiguous
                CompactStable();

                // Sort the elements according to the provided comparison class
                Algo::StableSort(TArrayView<FElementOrFreeListLink>(Data.GetData(), Num()), FElementCompareClass<PredicateClass>(Predicate));
            }
        }

        /** Stable sort elements using operator< */
        void StableSort()
        {
            StableSort(TLess<ElementType>());
        }

        /** Find an element by predicate */
        template<typename Predicate>
        [[nodiscard]] i32 IndexOfByPredicate(Predicate Pred) const
        {
            for (TConstIterator It(*this); It; ++It)
            {
                if (Pred(*It))
                {
                    return It.GetIndex();
                }
            }
            return INDEX_NONE;
        }

        /** Find any allocated element's index */
        [[nodiscard]] i32 FindArbitraryElementIndex() const
        {
            if (NumFreeIndices == 0)
            {
                return Data.Num() - 1;
            }
            return AllocationFlags.Find(true);
        }

        /** Returns true if the array is compact (no holes) */
        [[nodiscard]] bool IsCompact() const
        {
            return NumFreeIndices == 0;
        }

        /** Returns the amount of memory allocated */
        [[nodiscard]] sizet GetAllocatedSize() const
        {
            return Data.GetAllocatedSize() + AllocationFlags.GetAllocatedSize();
        }

        /** Tracks the container's memory use through an archive */
        void CountBytes(FArchive& Ar) const
        {
            Data.CountBytes(Ar);
            AllocationFlags.CountBytes(Ar);
        }

        /**
         * @brief Convert a pointer to an element into an index
         * @param Ptr  Pointer to an element in the array
         * @return The index of the element
         */
        [[nodiscard]] i32 PointerToIndex(const ElementType* Ptr) const
        {
            OLO_CORE_ASSERT(Data.Num() > 0, "Cannot convert pointer to index in empty array");
            const FElementOrFreeListLink* DataPtr = reinterpret_cast<const FElementOrFreeListLink*>(Data.GetData());
            i32 Index = static_cast<i32>(reinterpret_cast<const FElementOrFreeListLink*>(Ptr) - DataPtr);
            OLO_CORE_ASSERT(Index >= 0 && Index < Data.Num() && AllocationFlags[Index], "Invalid pointer");
            return Index;
        }

        /**
         * @brief Check that the specified address is not part of an element within the container
         * @param Addr  The address to check
         */
        OLO_FINLINE void CheckAddress(const ElementType* Addr) const
        {
            Data.CheckAddress(Addr);
        }

        /** Concatenation operator - append elements from another sparse array */
        TSparseArray& operator+=(const TSparseArray& OtherArray)
        {
            Reserve(Num() + OtherArray.Num());
            for (typename TSparseArray::TConstIterator It(OtherArray); It; ++It)
            {
                Add(*It);
            }
            return *this;
        }

        /** Concatenation operator - append elements from a TArray */
        TSparseArray& operator+=(const TArray<ElementType>& OtherArray)
        {
            Reserve(Num() + OtherArray.Num());
            for (i32 Idx = 0; Idx < OtherArray.Num(); ++Idx)
            {
                Add(OtherArray[Idx]);
            }
            return *this;
        }

        // ========================================================================
        // Iterators
        // ========================================================================

        /** Base iterator class */
        template<bool bConst>
        class TBaseIterator
        {
          public:
            using ArrayType = std::conditional_t<bConst, const TSparseArray, TSparseArray>;
            using IteratedElementType = std::conditional_t<bConst, const ElementType, ElementType>;

          private:
            ArrayType& Array;
            TConstSetBitIterator<typename Allocator::BitArrayAllocator> BitArrayIt;

          public:
            [[nodiscard]] explicit TBaseIterator(ArrayType& InArray, i32 StartIndex = 0)
                : Array(InArray), BitArrayIt(InArray.AllocationFlags, StartIndex)
            {
            }

            OLO_FINLINE TBaseIterator& operator++()
            {
                ++BitArrayIt;
                return *this;
            }

            [[nodiscard]] OLO_FINLINE explicit operator bool() const
            {
                return static_cast<bool>(BitArrayIt);
            }

            [[nodiscard]] OLO_FINLINE bool operator!() const
            {
                return !static_cast<bool>(BitArrayIt);
            }

            [[nodiscard]] OLO_FINLINE i32 GetIndex() const
            {
                return BitArrayIt.GetIndex();
            }

            [[nodiscard]] OLO_FINLINE IteratedElementType& operator*() const
            {
                return Array[GetIndex()];
            }

            [[nodiscard]] OLO_FINLINE IteratedElementType* operator->() const
            {
                return &Array[GetIndex()];
            }

            [[nodiscard]] OLO_FINLINE bool operator==(const TBaseIterator& Other) const
            {
                return BitArrayIt.GetIndex() == Other.BitArrayIt.GetIndex();
            }

            [[nodiscard]] OLO_FINLINE bool operator!=(const TBaseIterator& Other) const
            {
                return !(*this == Other);
            }

            /** Remove current element (mutable iterator only) */
            void RemoveCurrent()
                requires(!bConst)
            {
                Array.RemoveAt(GetIndex());
            }
        };

        using TIterator = TBaseIterator<false>;
        using TConstIterator = TBaseIterator<true>;

#if TSPARSEARRAY_RANGED_FOR_CHECKS
        /** Ranged-for iterator with modification checking */
        class TRangedForIterator : public TIterator
        {
          public:
            [[nodiscard]] TRangedForIterator(TSparseArray& InArray, i32 StartIndex = 0)
                : TIterator(InArray, StartIndex), InitialNum(InArray.Num()), ArrayPtr(&InArray)
            {
            }

            [[nodiscard]] inline bool operator!=(const TRangedForIterator& Rhs) const
            {
                // Check for modification during iteration
                OLO_CORE_ASSERT(ArrayPtr->Num() == InitialNum, "Container has changed during ranged-for iteration!");
                return static_cast<const TIterator&>(*this) != static_cast<const TIterator&>(Rhs);
            }

          private:
            i32 InitialNum;
            TSparseArray* ArrayPtr;
        };

        /** Const ranged-for iterator with modification checking */
        class TRangedForConstIterator : public TConstIterator
        {
          public:
            [[nodiscard]] TRangedForConstIterator(const TSparseArray& InArray, i32 StartIndex = 0)
                : TConstIterator(InArray, StartIndex), InitialNum(InArray.Num()), ArrayPtr(&InArray)
            {
            }

            [[nodiscard]] inline bool operator!=(const TRangedForConstIterator& Rhs) const
            {
                // Check for modification during iteration
                OLO_CORE_ASSERT(ArrayPtr->Num() == InitialNum, "Container has changed during ranged-for iteration!");
                return static_cast<const TConstIterator&>(*this) != static_cast<const TConstIterator&>(Rhs);
            }

          private:
            i32 InitialNum;
            const TSparseArray* ArrayPtr;
        };
#else
        using TRangedForIterator = TIterator;
        using TRangedForConstIterator = TConstIterator;
#endif

        /** Create an iterator */
        [[nodiscard]] TIterator CreateIterator()
        {
            return TIterator(*this);
        }

        /** Create a const iterator */
        [[nodiscard]] TConstIterator CreateConstIterator() const
        {
            return TConstIterator(*this);
        }

        /** Range-based for loop support */
        [[nodiscard]] TRangedForIterator begin()
        {
            return TRangedForIterator(*this);
        }
        [[nodiscard]] TRangedForConstIterator begin() const
        {
            return TRangedForConstIterator(*this);
        }
        [[nodiscard]] TRangedForIterator end()
        {
            return TRangedForIterator(*this, GetMaxIndex());
        }
        [[nodiscard]] TRangedForConstIterator end() const
        {
            return TRangedForConstIterator(*this, GetMaxIndex());
        }

        /** An iterator which only iterates over elements matching a subset bit array */
        template<typename SubsetAllocator = FDefaultAllocator>
        class TConstSubsetIterator
        {
          public:
            [[nodiscard]] TConstSubsetIterator(const TSparseArray& InArray, const TBitArray<SubsetAllocator>& InBitArray)
                : Array(InArray), BitArrayIt(InArray.AllocationFlags, InBitArray)
            {
            }

            inline TConstSubsetIterator& operator++()
            {
                ++BitArrayIt;
                return *this;
            }

            [[nodiscard]] OLO_FINLINE i32 GetIndex() const
            {
                return BitArrayIt.GetIndex();
            }

            /** conversion to "bool" returning true if the iterator is valid. */
            [[nodiscard]] OLO_FINLINE explicit operator bool() const
            {
                return !!BitArrayIt;
            }

            /** inverse of the "bool" operator */
            [[nodiscard]] OLO_FINLINE bool operator!() const
            {
                return !(bool)*this;
            }

            [[nodiscard]] OLO_FINLINE const ElementType& operator*() const
            {
                return Array[GetIndex()];
            }

            [[nodiscard]] OLO_FINLINE const ElementType* operator->() const
            {
                return &Array[GetIndex()];
            }

            [[nodiscard]] OLO_FINLINE const FRelativeBitReference& GetRelativeBitReference() const
            {
                return BitArrayIt;
            }

          private:
            const TSparseArray& Array;
            TConstDualSetBitIterator<typename Allocator::BitArrayAllocator, SubsetAllocator> BitArrayIt;
        };

        // ========================================================================
        // Memory Image Support
        // ========================================================================

        /** Write the sparse array to a memory image for frozen data */
        void WriteMemoryImage(FMemoryImageWriter& Writer) const
        {
            if constexpr (TAllocatorTraits<Allocator>::SupportsFreezeMemoryImage && THasTypeLayout<ElementType>::Value)
            {
                // Write Data
                const i32 NumElements = this->Data.Num();
                if (NumElements > 0)
                {
                    const FTypeLayoutDesc& ElementTypeDesc = StaticGetTypeLayoutDesc<ElementType>();
                    FMemoryImageWriter ArrayWriter = Writer.WritePointer(ElementTypeDesc);
                    for (i32 i = 0; i < NumElements; ++i)
                    {
                        const FElementOrFreeListLink& Elem = reinterpret_cast<const FElementOrFreeListLink*>(this->Data.GetData())[i];
                        const u32 StartOffset = ArrayWriter.WriteAlignment<FElementOrFreeListLink>();
                        if (this->AllocationFlags[i])
                        {
                            ArrayWriter.WriteObject(&Elem.ElementData, ElementTypeDesc);
                        }
                        else
                        {
                            ArrayWriter.WriteBytes(Elem.PrevFreeIndex);
                            ArrayWriter.WriteBytes(Elem.NextFreeIndex);
                        }
                        ArrayWriter.WritePaddingToSize(StartOffset + sizeof(FElementOrFreeListLink));
                    }
                }
                else
                {
                    Writer.WriteNullPointer();
                }
                Writer.WriteBytes(NumElements);
                Writer.WriteBytes(NumElements);

                // Write AllocationFlags
                this->AllocationFlags.WriteMemoryImage(Writer);
                Writer.WriteBytes(this->FirstFreeIndex);
                Writer.WriteBytes(this->NumFreeIndices);
            }
            else
            {
                Writer.WriteBytes(TSparseArray());
            }
        }

        /** Copy from frozen data to unfrozen */
        void CopyUnfrozen(const FMemoryUnfreezeContent& Context, void* Dst) const
        {
            if constexpr (TAllocatorTraits<Allocator>::SupportsFreezeMemoryImage && THasTypeLayout<ElementType>::Value)
            {
                const FTypeLayoutDesc& ElementTypeDesc = StaticGetTypeLayoutDesc<ElementType>();
                TSparseArray* DstObject = reinterpret_cast<TSparseArray*>(Dst);
                {
                    ::new (static_cast<void*>(&DstObject->Data)) DataType();
                    DstObject->Data.SetNumUninitialized(this->Data.Num());
                    for (i32 i = 0; i < this->Data.Num(); ++i)
                    {
                        const FElementOrFreeListLink& Elem = reinterpret_cast<const FElementOrFreeListLink*>(this->Data.GetData())[i];
                        FElementOrFreeListLink& DstElem = reinterpret_cast<FElementOrFreeListLink*>(DstObject->Data.GetData())[i];
                        if (this->AllocationFlags[i])
                        {
                            Context.UnfreezeObject(&Elem.ElementData, ElementTypeDesc, &DstElem.ElementData);
                        }
                        else
                        {
                            DstElem.PrevFreeIndex = Elem.PrevFreeIndex;
                            DstElem.NextFreeIndex = Elem.NextFreeIndex;
                        }
                    }
                }

                ::new (static_cast<void*>(&DstObject->AllocationFlags)) AllocationBitArrayType(this->AllocationFlags);
                DstObject->FirstFreeIndex = this->FirstFreeIndex;
                DstObject->NumFreeIndices = this->NumFreeIndices;
            }
            else
            {
                ::new (Dst) TSparseArray();
            }
        }

        /** Append hash for type layout */
        static void AppendHash(const FPlatformTypeLayoutParameters& LayoutParams, FSHA1& Hasher)
        {
            if constexpr (TAllocatorTraits<Allocator>::SupportsFreezeMemoryImage && THasTypeLayout<ElementType>::Value)
            {
                Freeze::AppendHash(StaticGetTypeLayoutDesc<ElementType>(), LayoutParams, Hasher);
            }
        }

      private:
        /** Extracts the element value from the sparse array's element structure for sorting */
        template<typename PredicateClass>
        class FElementCompareClass
        {
            const PredicateClass& Predicate;

          public:
            [[nodiscard]] FElementCompareClass(const PredicateClass& InPredicate)
                : Predicate(InPredicate)
            {
            }

            [[nodiscard]] bool operator()(const FElementOrFreeListLink& A, const FElementOrFreeListLink& B) const
            {
                return Predicate(
                    *reinterpret_cast<const ElementType*>(&A.ElementData),
                    *reinterpret_cast<const ElementType*>(&B.ElementData));
            }
        };
    };

} // namespace OloEngine

// ============================================================================
// TSparseArray operator new implementations
// ============================================================================

/**
 * Placement new operator that allocates a new element in the sparse array
 */
template<typename T, typename Allocator>
void* operator new(sizet Size, OloEngine::TSparseArray<T, Allocator>& Array)
{
    OLO_CORE_ASSERT(Size == sizeof(T));
    const auto Index = Array.AddUninitialized().Index;
    return &Array[Index];
}

/**
 * Placement new operator that allocates an element at a specific index in the sparse array
 */
template<typename T, typename Allocator>
void* operator new(sizet Size, OloEngine::TSparseArray<T, Allocator>& Array, std::int32_t Index)
{
    OLO_CORE_ASSERT(Size == sizeof(T));
    Array.InsertUninitialized(Index);
    return &Array[Index];
}

namespace OloEngine
{

    // ============================================================================
    // Freeze Namespace Functions
    // ============================================================================

    namespace Freeze
    {
        template<typename ElementType, typename Allocator>
        void IntrinsicWriteMemoryImage(FMemoryImageWriter& Writer, const TSparseArray<ElementType, Allocator>& Object, const FTypeLayoutDesc&)
        {
            Object.WriteMemoryImage(Writer);
        }

        template<typename ElementType, typename Allocator>
        u32 IntrinsicUnfrozenCopy(const FMemoryUnfreezeContent& Context, const TSparseArray<ElementType, Allocator>& Object, void* OutDst)
        {
            Object.CopyUnfrozen(Context, OutDst);
            return sizeof(Object);
        }

        template<typename ElementType, typename Allocator>
        u32 IntrinsicAppendHash(const TSparseArray<ElementType, Allocator>*, const FTypeLayoutDesc& TypeDesc, const FPlatformTypeLayoutParameters& LayoutParams, FSHA1& Hasher)
        {
            TSparseArray<ElementType, Allocator>::AppendHash(LayoutParams, Hasher);
            return DefaultAppendHash(TypeDesc, LayoutParams, Hasher);
        }
    } // namespace Freeze

    // ============================================================================
    // Serialization
    // ============================================================================

    /** Serializer */
    template<typename ElementType, typename Allocator>
    FArchive& operator<<(FArchive& Ar, TSparseArray<ElementType, Allocator>& Array)
    {
        Array.CountBytes(Ar);
        if (Ar.IsLoading())
        {
            // Load array
            i32 NewNumElements = 0;
            Ar << NewNumElements;
            Array.Empty(NewNumElements);
            for (i32 ElementIndex = 0; ElementIndex < NewNumElements; ++ElementIndex)
            {
                Ar << *::new (Array.AddUninitialized()) ElementType;
            }
        }
        else
        {
            // Save array
            i32 NewNumElements = Array.Num();
            Ar << NewNumElements;
            for (typename TSparseArray<ElementType, Allocator>::TIterator It(Array); It; ++It)
            {
                Ar << *It;
            }
        }
        return Ar;
    }

    /** Structured archive serializer */
    template<typename ElementType, typename Allocator>
    void operator<<(FStructuredArchive::FSlot Slot, TSparseArray<ElementType, Allocator>& InArray)
    {
        i32 NumElements = InArray.Num();
        FStructuredArchive::FArray Array = Slot.EnterArray(NumElements);
        if (Slot.GetUnderlyingArchive().IsLoading())
        {
            InArray.Empty(NumElements);

            for (i32 Index = 0; Index < NumElements; ++Index)
            {
                if (Slot.GetUnderlyingArchive().IsCriticalError())
                {
                    return;
                }

                FStructuredArchive::FSlot ElementSlot = Array.EnterElement();
                ElementSlot << *::new (InArray.AddUninitialized()) ElementType;
            }
        }
        else
        {
            for (typename TSparseArray<ElementType, Allocator>::TIterator It(InArray); It; ++It)
            {
                FStructuredArchive::FSlot ElementSlot = Array.EnterElement();
                ElementSlot << *It;
            }
        }
    }

    // ============================================================================
    // Hash Function
    // ============================================================================

    template<typename ElementType, typename Allocator>
    [[nodiscard]] u32 GetTypeHash(const TSparseArray<ElementType, Allocator>& Array)
    {
        u32 Hash = 0;
        for (typename TSparseArray<ElementType, Allocator>::TConstIterator It(Array); It; ++It)
        {
            Hash ^= GetTypeHash(*It);
            Hash = (Hash << 5) - Hash; // Hash * 31
        }
        return Hash;
    }

} // namespace OloEngine
