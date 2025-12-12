#pragma once

/**
 * @file Array.h
 * @brief Dynamic array container with UE-style allocator support
 * 
 * Provides a dynamic array similar to std::vector but with:
 * - Pluggable allocator policies (heap, inline, stack)
 * - Trivially relocatable optimization (memcpy for moves)
 * - Zero-construct optimization (memset for init)
 * - UE-compatible API and semantics
 * 
 * Ported from Unreal Engine's Containers/Array.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/ArrayView.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Containers/ContainerFwd.h"
#include "OloEngine/Containers/ReverseIterate.h"
#include "OloEngine/Misc/IntrusiveUnsetOptionalState.h"
#include "OloEngine/Memory/MemoryOps.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Templates/TypeHash.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include "OloEngine/Templates/Sorting.h"
#include "OloEngine/Templates/IdentityFunctor.h"
#include "OloEngine/Algo/BinaryHeap.h"
#include "OloEngine/Algo/Sort.h"
#include "OloEngine/Algo/StableSort.h"
#include "OloEngine/Algo/Heapify.h"
#include "OloEngine/Algo/HeapSort.h"
#include "OloEngine/Algo/IsHeap.h"
#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <type_traits>
#include <limits>

namespace OloEngine
{
    // Forward declaration
    template <typename T, typename AllocatorType>
    class TArray;

} // namespace OloEngine

// Forward declarations for placement new operators (must be at global scope)
template <typename T, typename AllocatorType> void* operator new(size_t Size, OloEngine::TArray<T, AllocatorType>& Array);
template <typename T, typename AllocatorType> void* operator new(size_t Size, OloEngine::TArray<T, AllocatorType>& Array, typename OloEngine::TArray<T, AllocatorType>::SizeType Index);

namespace OloEngine
{

    // ============================================================================
    // Array Debug Configuration
    // ============================================================================

    /**
     * @def OLO_ARRAY_RANGED_FOR_CHECKS
     * @brief Controls whether ranged-for iteration detects array resize
     * 
     * When enabled, modifying an array during ranged-for iteration will trigger
     * an assertion failure.
     */
#if !defined(OLO_ARRAY_RANGED_FOR_CHECKS)
    #if OLO_BUILD_SHIPPING
        #define OLO_ARRAY_RANGED_FOR_CHECKS 0
    #else
        #define OLO_ARRAY_RANGED_FOR_CHECKS 1
    #endif
#endif

    // ============================================================================
    // TCheckedPointerIterator - Debug iterator with resize detection
    // ============================================================================

#if OLO_ARRAY_RANGED_FOR_CHECKS
    /**
     * @class TCheckedPointerIterator
     * @brief Pointer-like iterator that detects container resize during iteration
     * 
     * This iterator stores a reference to the container's size and checks on
     * each iteration step that the size hasn't changed. This catches common
     * bugs where the container is modified during ranged-for iteration.
     * 
     * @tparam ElementType The element type
     * @tparam SizeType    The container's size type
     * @tparam bReverse    Whether to iterate in reverse
     */
    template <typename ElementType, typename SizeType, bool bReverse = false>
    struct TCheckedPointerIterator
    {
        // This iterator type only supports the minimal functionality needed to support
        // C++ ranged-for syntax. For example, it does not provide post-increment ++ nor ==.
        // We do add an operator-- to help with some implementations

        [[nodiscard]] explicit TCheckedPointerIterator(const SizeType& InNum, ElementType* InPtr)
            : Ptr(InPtr)
            , CurrentNum(InNum)
            , InitialNum(InNum)
        {
        }

        [[nodiscard]] OLO_FINLINE ElementType* operator->() const
        {
            if constexpr (bReverse)
            {
                return Ptr - 1;
            }
            else
            {
                return Ptr;
            }
        }

        [[nodiscard]] OLO_FINLINE ElementType& operator*() const
        {
            if constexpr (bReverse)
            {
                return *(Ptr - 1);
            }
            else
            {
                return *Ptr;
            }
        }

        OLO_FINLINE TCheckedPointerIterator& operator++()
        {
            if constexpr (bReverse)
            {
                --Ptr;
            }
            else
            {
                ++Ptr;
            }
            return *this;
        }

        OLO_FINLINE TCheckedPointerIterator& operator--()
        {
            if constexpr (bReverse)
            {
                ++Ptr;
            }
            else
            {
                --Ptr;
            }
            return *this;
        }

        [[nodiscard]] OLO_FINLINE bool operator!=(const TCheckedPointerIterator& Rhs) const
        {
            // We only need to do the check in this operator, because no other operator will be
            // called until after this one returns.
            //
            // Also, we should only need to check one side of this comparison - if the other iterator isn't
            // even from the same array then the compiler has generated bad code.
            OLO_CORE_ASSERT(CurrentNum == InitialNum, "Array has changed during ranged-for iteration!");
            return Ptr != Rhs.Ptr;
        }

        [[nodiscard]] OLO_FINLINE bool operator==(const TCheckedPointerIterator& Rhs) const
        {
            return !(*this != Rhs);
        }

    private:
        ElementType*    Ptr;
        const SizeType& CurrentNum;
        SizeType        InitialNum;
    };
#endif

    // ============================================================================
    // TDereferencingIterator - Iterator that dereferences on access
    // ============================================================================

    /**
     * @class TDereferencingIterator
     * @brief Iterator wrapper that automatically dereferences pointer elements
     * 
     * Used for sorting arrays of pointers so that the comparison predicate
     * receives references to the pointed-to objects rather than pointers.
     * 
     * @tparam ElementType  The type of element (after dereferencing)
     * @tparam IteratorType The underlying iterator type
     */
    template <typename ElementType, typename IteratorType>
    struct TDereferencingIterator
    {
        [[nodiscard]] explicit TDereferencingIterator(IteratorType InIter)
            : Iter(InIter)
        {
        }

        [[nodiscard]] OLO_FINLINE ElementType& operator*() const
        {
            return *(ElementType*)*Iter;
        }

        OLO_FINLINE TDereferencingIterator& operator++()
        {
            ++Iter;
            return *this;
        }

        [[nodiscard]] OLO_FINLINE bool operator!=(const TDereferencingIterator& Rhs) const
        {
            return Iter != Rhs.Iter;
        }

    private:
        IteratorType Iter;
    };

    // ============================================================================
    // TIndexedContainerIterator - Index-based iterator
    // ============================================================================

    /**
     * @class TIndexedContainerIterator
     * @brief Generic iterator for indexed containers
     */
    template <typename ContainerType, typename ElementType, typename SizeType>
    class TIndexedContainerIterator
    {
    public:
        [[nodiscard]] TIndexedContainerIterator(ContainerType& InContainer, SizeType StartIndex = 0)
            : m_Container(InContainer)
            , m_Index(StartIndex)
        {
        }

        /** Advances iterator to the next element */
        TIndexedContainerIterator& operator++()
        {
            ++m_Index;
            return *this;
        }

        TIndexedContainerIterator operator++(int)
        {
            TIndexedContainerIterator Tmp(*this);
            ++m_Index;
            return Tmp;
        }

        /** Moves iterator to the previous element */
        TIndexedContainerIterator& operator--()
        {
            --m_Index;
            return *this;
        }

        TIndexedContainerIterator operator--(int)
        {
            TIndexedContainerIterator Tmp(*this);
            --m_Index;
            return Tmp;
        }

        /** Iterator arithmetic support */
        TIndexedContainerIterator& operator+=(SizeType Offset)
        {
            m_Index += Offset;
            return *this;
        }

        [[nodiscard]] TIndexedContainerIterator operator+(SizeType Offset) const
        {
            TIndexedContainerIterator Tmp(*this);
            return Tmp += Offset;
        }

        TIndexedContainerIterator& operator-=(SizeType Offset)
        {
            return *this += -Offset;
        }

        [[nodiscard]] TIndexedContainerIterator operator-(SizeType Offset) const
        {
            TIndexedContainerIterator Tmp(*this);
            return Tmp -= Offset;
        }

        [[nodiscard]] OLO_FINLINE ElementType& operator*() const
        {
            return m_Container[m_Index];
        }

        [[nodiscard]] OLO_FINLINE ElementType* operator->() const
        {
            return &m_Container[m_Index];
        }

        /** Conversion to bool - true if iterator has not reached the last element */
        [[nodiscard]] OLO_FINLINE explicit operator bool() const
        {
            return m_Container.IsValidIndex(m_Index);
        }

        /** Returns index to the current element */
        [[nodiscard]] SizeType GetIndex() const
        {
            return m_Index;
        }

        /** Resets the iterator to the first element */
        void Reset()
        {
            m_Index = 0;
        }

        /** Sets the iterator to one past the last element */
        void SetToEnd()
        {
            m_Index = m_Container.Num();
        }

        /** Removes current element in array */
        void RemoveCurrent()
        {
            m_Container.RemoveAt(m_Index);
            --m_Index;
        }

        /** Removes current element by swapping with end element */
        void RemoveCurrentSwap()
        {
            m_Container.RemoveAtSwap(m_Index);
            --m_Index;
        }

        [[nodiscard]] OLO_FINLINE bool operator==(const TIndexedContainerIterator& Rhs) const
        {
            return &m_Container == &Rhs.m_Container && m_Index == Rhs.m_Index;
        }

        [[nodiscard]] OLO_FINLINE bool operator!=(const TIndexedContainerIterator& Rhs) const
        {
            return !(*this == Rhs);
        }

    private:
        ContainerType& m_Container;
        SizeType m_Index;
    };

    // ============================================================================
    // Private Implementation Helpers
    // ============================================================================

    namespace Private
    {
        /**
         * @brief Simply forwards to an unqualified GetData(), but can be called from within a
         * container or view where GetData() is already a member and so hides any others.
         */
        template <typename T>
        [[nodiscard]] OLO_FINLINE decltype(auto) GetDataHelper(T&& Arg)
        {
            return GetData(std::forward<T>(Arg));
        }

        /**
         * @brief Type trait to check if array elements are compatible for construction
         * 
         * Elements are compatible if they are the same type or if DestType can be constructed from SourceType.
         */
        template <typename DestType, typename SourceType>
        inline constexpr bool TArrayElementsAreCompatible_V = std::disjunction_v<
            std::is_same<DestType, std::decay_t<SourceType>>,
            std::is_constructible<DestType, SourceType>
        >;

        /**
         * @brief Helper functions to detect if a type is TArray or derived from TArray
         * 
         * Uses overload resolution to detect inheritance from TArray.
         */
        template <typename ElementType, typename AllocatorType>
        static char (&ResolveIsTArrayPtr(const volatile TArray<ElementType, AllocatorType>*))[2];
        static char (&ResolveIsTArrayPtr(...))[1];

        /**
         * @brief Type trait to check if T is a TArray or derived from TArray
         * 
         * Unlike TIsTArray_V which only matches exact TArray types, this trait also
         * matches types that inherit from TArray.
         */
        template <typename T>
        inline constexpr bool TIsTArrayOrDerivedFromTArray_V = sizeof(ResolveIsTArrayPtr(static_cast<T*>(nullptr))) == 2;

        /**
         * @brief Check if we can move TArray pointers between two array types
         * 
         * This is true when:
         * - The allocators are the same or move-compatible
         * - The element types are bitwise compatible (no qualifier loss, same underlying type)
         * 
         * @tparam FromArrayType Source array type
         * @tparam ToArrayType Destination array type
         * @return true if pointer movement is safe between the array types
         */
        template <typename FromArrayType, typename ToArrayType>
        [[nodiscard]] constexpr bool CanMoveTArrayPointersBetweenArrayTypes()
        {
            using FromAllocatorType          = typename FromArrayType::AllocatorType;
            using ToAllocatorType            = typename ToArrayType::AllocatorType;
            using FromElementType            = typename FromArrayType::ElementType;
            using ToElementType              = typename ToArrayType::ElementType;
            using UnqualifiedFromElementType = std::remove_cv_t<FromElementType>;
            using UnqualifiedToElementType   = std::remove_cv_t<ToElementType>;

            // Allocators must be equal or move-compatible
            if constexpr (std::is_same_v<FromAllocatorType, ToAllocatorType> || TCanMoveBetweenAllocators<FromAllocatorType, ToAllocatorType>::Value)
            {
                return
                    !TLosesQualifiersFromTo_V<FromElementType, ToElementType> &&
                    (
                        std::is_same_v         <const ToElementType, const FromElementType> ||               // The element type of the container must be the same, or...
                        TIsBitwiseConstructible<UnqualifiedToElementType, UnqualifiedFromElementType>::Value // ... the element type of the source container must be bitwise constructible from the element type in the destination container
                    );
            }
            else
            {
                return false;
            }
        }

        /**
         * @brief Called when an invalid array size is detected
         * 
         * This is called when we detect overflow or underflow in array size calculations.
         * In debug builds it asserts; in release builds it terminates.
         */
        [[noreturn]] inline void OnInvalidArrayNum(unsigned long long NewNum)
        {
            OLO_CORE_ASSERT(false, "Invalid array num: {}", NewNum);
            std::terminate();
        }

    } // namespace Private

    // ============================================================================
    // EAllowShrinking
    // ============================================================================

    /**
     * @enum EAllowShrinking
     * @brief Controls whether operations are allowed to shrink the array allocation
     * 
     * - No: Never shrink
     * - Yes: Always try to shrink
     * - Default: Use allocator's ShrinkByDefault setting (prefer AllowShrinkingByDefault<>() in new code)
     */
    enum class EAllowShrinking : u8
    {
        No,
        Yes,

        Default = Yes // For backwards compatibility when allocator doesn't specify
    };

    namespace Private
    {
        /**
         * @brief Returns EAllowShrinking::Yes or No based on the allocator's ShrinkByDefault setting
         * 
         * This is a helper to determine the appropriate shrinking behavior for a given
         * allocator type at compile time.
         * 
         * @tparam AllocatorType The allocator policy to query
         * @return EAllowShrinking::Yes if allocator shrinks by default, No otherwise
         */
        template <typename AllocatorType>
        consteval EAllowShrinking AllowShrinkingByDefault()
        {
            // Convert the ShrinkByDefault enum into an EAllowShrinking.
            // For backwards compatibility, failure to specify `ShrinkByDefault` means Yes.
            return Detail::ShrinkByDefaultOr<true, AllocatorType>()
                ? EAllowShrinking::Yes
                : EAllowShrinking::No;
        }

        /**
         * @brief Returns a bitmask of allocator capability flags
         * 
         * Used to reduce template instantiation code bloat by encoding allocator
         * capabilities into a single value for conditional branching.
         * 
         * Flags:
         * - Bit 0 (1): TAllocatorTraits<>::SupportsElementAlignment
         * - Bit 1 (2): TAllocatorTraits<>::SupportsSlackTracking
         * 
         * @tparam AllocatorType The allocator policy to query
         * @return Bitmask of capability flags
         */
        template <typename AllocatorType>
        [[nodiscard]] constexpr u32 GetAllocatorFlags()
        {
            u32 Result = 0;
            if constexpr (TAllocatorTraits<AllocatorType>::SupportsElementAlignment)
            {
                Result |= 1;
            }
            if constexpr (TAllocatorTraits<AllocatorType>::SupportsSlackTracking)
            {
                Result |= 2;
            }
            return Result;
        }

        // ====================================================================
        // Optimized Reallocation Functions
        // ====================================================================
        // These functions are templated ONLY on allocator flags (not element type)
        // to minimize code bloat. The flags encode allocator capabilities:
        //   Bit 0 (1): SupportsElementAlignment
        //   Bit 1 (2): SupportsSlackTracking

        /**
         * @brief Helper type to extract SizeType from allocator instance
         */
        template <typename AllocatorInstanceType>
        using TAllocatorSizeType_T = decltype(std::declval<AllocatorInstanceType&>().GetInitialCapacity());

        /**
         * @brief Core implementation for single-element growth reallocation
         * 
         * Called only when we KNOW we are going to do a realloc increasing by 1.
         * In this case, we know that max == num and can simplify things in a
         * very hot location in the code.
         * 
         * @tparam Flags Allocator capability flags from GetAllocatorFlags<>()
         * @tparam AllocatorInstanceType The allocator instance type
         * @param ElementSize Size of each element in bytes
         * @param ElementAlignment Alignment requirement of elements
         * @param AllocatorInstance Reference to the allocator instance
         * @param ArrayMax Reference to the array max capacity (updated)
         * @return The old ArrayMax value (saves a register clobber/reload)
         */
        template <u32 Flags, typename AllocatorInstanceType>
        OLO_FINLINE TAllocatorSizeType_T<AllocatorInstanceType> ReallocGrow1_DoAlloc_Impl(
            u32                                          ElementSize,
            u32                                          ElementAlignment,
            AllocatorInstanceType&                       AllocatorInstance,
            TAllocatorSizeType_T<AllocatorInstanceType>& ArrayMax
        )
        {
            using SizeType  = TAllocatorSizeType_T<AllocatorInstanceType>;
            using USizeType = std::make_unsigned_t<SizeType>;

            const USizeType UOldMax = static_cast<USizeType>(ArrayMax);
            const USizeType UNewNum = UOldMax + 1U;
            const SizeType  OldMax  = static_cast<SizeType>(UOldMax);
            const SizeType  NewNum  = static_cast<SizeType>(UNewNum);

            // This should only happen when we've underflowed or overflowed SizeType
            if (NewNum < OldMax)
            {
                OnInvalidArrayNum(static_cast<unsigned long long>(UNewNum));
            }

            SizeType NewMax;
            if constexpr (!!(Flags & 1)) // SupportsElementAlignment
            {
                NewMax = AllocatorInstance.CalculateSlackGrow(NewNum, OldMax, ElementSize, ElementAlignment);
                AllocatorInstance.ResizeAllocation(UOldMax, NewMax, ElementSize, ElementAlignment);
            }
            else
            {
                NewMax = AllocatorInstance.CalculateSlackGrow(NewNum, OldMax, ElementSize);
                AllocatorInstance.ResizeAllocation(UOldMax, NewMax, ElementSize);
            }
            ArrayMax = NewMax;

            #if OLO_ENABLE_ARRAY_SLACK_TRACKING
            if constexpr (!!(Flags & 2)) // SupportsSlackTracking
            {
                AllocatorInstance.SlackTrackerLogNum(NewNum);
            }
            #endif

            return OldMax;
        }

        /**
         * @brief Single-element growth for small types (size and alignment <= 255)
         * 
         * This version packs size and alignment into a single 16-bit parameter,
         * saving a parameter setup instruction on the function call.
         * 
         * @tparam Flags Allocator capability flags
         * @tparam AllocatorInstanceType The allocator instance type
         * @param ElementSizeAndAlignment Low 8 bits = size, high 8 bits = alignment
         * @param AllocatorInstance Reference to the allocator instance
         * @param ArrayMax Reference to the array max capacity
         * @return The old ArrayMax value
         */
        template <u32 Flags, typename AllocatorInstanceType>
        OLO_NOINLINE TAllocatorSizeType_T<AllocatorInstanceType> ReallocGrow1_DoAlloc_Tiny(
            u16                                          ElementSizeAndAlignment,
            AllocatorInstanceType&                       AllocatorInstance,
            TAllocatorSizeType_T<AllocatorInstanceType>& ArrayMax
        )
        {
            return ReallocGrow1_DoAlloc_Impl<Flags, AllocatorInstanceType>(
                ElementSizeAndAlignment & 0xff,
                ElementSizeAndAlignment >> 8,
                AllocatorInstance,
                ArrayMax
            );
        }

        /**
         * @brief Single-element growth for larger types
         * 
         * @tparam Flags Allocator capability flags
         * @tparam AllocatorInstanceType The allocator instance type
         * @param ElementSize Size of each element
         * @param ElementAlignment Alignment of elements
         * @param AllocatorInstance Reference to the allocator instance
         * @param ArrayMax Reference to the array max capacity
         * @return The old ArrayMax value
         */
        template <u32 Flags, typename AllocatorInstanceType>
        OLO_NOINLINE TAllocatorSizeType_T<AllocatorInstanceType> ReallocGrow1_DoAlloc(
            u32                                          ElementSize,
            u32                                          ElementAlignment,
            AllocatorInstanceType&                       AllocatorInstance,
            TAllocatorSizeType_T<AllocatorInstanceType>& ArrayMax
        )
        {
            return ReallocGrow1_DoAlloc_Impl<Flags, AllocatorInstanceType>(
                ElementSize,
                ElementAlignment,
                AllocatorInstance,
                ArrayMax
            );
        }

        /**
         * @brief Multi-element growth reallocation with amortization
         * 
         * Used for repeated growing operations when reallocations should be
         * amortized over multiple inserts.
         * 
         * @tparam Flags Allocator capability flags
         * @tparam AllocatorInstanceType The allocator instance type
         * @param ElementSize Size of each element
         * @param ElementAlignment Alignment of elements
         * @param Count Number of elements to add
         * @param AllocatorInstance Reference to the allocator instance
         * @param ArrayNum Reference to the array element count (updated)
         * @param ArrayMax Reference to the array max capacity (updated)
         * @return The old ArrayNum value
         */
        template <u32 Flags, typename AllocatorInstanceType>
        OLO_NOINLINE TAllocatorSizeType_T<AllocatorInstanceType> ReallocGrow(
            u32                                          ElementSize,
            u32                                          ElementAlignment,
            TAllocatorSizeType_T<AllocatorInstanceType>  Count,
            AllocatorInstanceType&                       AllocatorInstance,
            TAllocatorSizeType_T<AllocatorInstanceType>& ArrayNum,
            TAllocatorSizeType_T<AllocatorInstanceType>& ArrayMax
        )
        {
            using SizeType  = TAllocatorSizeType_T<AllocatorInstanceType>;
            using USizeType = std::make_unsigned_t<SizeType>;

            const USizeType UCount  = static_cast<USizeType>(Count);
            const USizeType UOldNum = static_cast<USizeType>(ArrayNum);
            const USizeType UOldMax = static_cast<USizeType>(ArrayMax);
            const USizeType UNewNum = UOldNum + UCount;
            const SizeType  OldNum  = static_cast<SizeType>(UOldNum);
            const SizeType  OldMax  = static_cast<SizeType>(UOldMax);
            const SizeType  NewNum  = static_cast<SizeType>(UNewNum);

            OLO_CORE_ASSERT((OldNum >= 0) & (OldMax >= OldNum) & (Count >= 0), "ReallocGrow: invalid state");

            ArrayNum = NewNum;

#if DO_GUARD_SLOW
            if (UNewNum > UOldMax)
#else
            // SECURITY - This check will guard against negative counts too, in case the checkSlow above is compiled out.
            // However, it results in slightly worse code generation.
            if (UCount > UOldMax - UOldNum)
#endif
            {
                // This should only happen when we've underflowed or overflowed SizeType
                if (NewNum < OldNum)
                {
                    OnInvalidArrayNum(static_cast<unsigned long long>(UNewNum));
                }

                SizeType NewMax;
                if constexpr (!!(Flags & 1)) // SupportsElementAlignment
                {
                    NewMax = AllocatorInstance.CalculateSlackGrow(NewNum, OldMax, ElementSize, ElementAlignment);
                    AllocatorInstance.ResizeAllocation(UOldNum, NewMax, ElementSize, ElementAlignment);
                }
                else
                {
                    NewMax = AllocatorInstance.CalculateSlackGrow(NewNum, OldMax, ElementSize);
                    AllocatorInstance.ResizeAllocation(UOldNum, NewMax, ElementSize);
                }
                ArrayMax = NewMax;

                #if OLO_ENABLE_ARRAY_SLACK_TRACKING
                if constexpr (!!(Flags & 2)) // SupportsSlackTracking
                {
                    AllocatorInstance.SlackTrackerLogNum(NewNum);
                }
                #endif
            }

            return OldNum;
        }

        /**
         * @brief Shrink reallocation for removal operations
         * 
         * Used for repeated shrinking operations when reallocations should be
         * amortized over multiple removals.
         * 
         * @tparam Flags Allocator capability flags
         * @tparam AllocatorInstanceType The allocator instance type
         * @param ElementSize Size of each element
         * @param ElementAlignment Alignment of elements
         * @param AllocatorInstance Reference to the allocator instance
         * @param ArrayNum Current number of elements
         * @param ArrayMax Reference to the array max capacity (updated)
         */
        template <u32 Flags, typename AllocatorInstanceType>
        OLO_NOINLINE void ReallocShrink(
            u32                                          ElementSize,
            u32                                          ElementAlignment,
            AllocatorInstanceType&                       AllocatorInstance,
            TAllocatorSizeType_T<AllocatorInstanceType>  ArrayNum,
            TAllocatorSizeType_T<AllocatorInstanceType>& ArrayMax
        )
        {
            using SizeType = TAllocatorSizeType_T<AllocatorInstanceType>;

            SizeType OldArrayMax = ArrayMax;

            if constexpr (!!(Flags & 1)) // SupportsElementAlignment
            {
                SizeType NewArrayMax = AllocatorInstance.CalculateSlackShrink(ArrayNum, OldArrayMax, ElementSize, ElementAlignment);
                if (NewArrayMax != OldArrayMax)
                {
                    ArrayMax = NewArrayMax;
                    AllocatorInstance.ResizeAllocation(ArrayNum, NewArrayMax, ElementSize, ElementAlignment);
                }
            }
            else
            {
                SizeType NewArrayMax = AllocatorInstance.CalculateSlackShrink(ArrayNum, OldArrayMax, ElementSize);
                if (NewArrayMax != OldArrayMax)
                {
                    ArrayMax = NewArrayMax;
                    AllocatorInstance.ResizeAllocation(ArrayNum, NewArrayMax, ElementSize);
                }
            }
        }

        /**
         * @brief Set allocation to a specific size
         * 
         * Precondition: NewMax >= ArrayNum
         * 
         * @tparam Flags Allocator capability flags
         * @tparam AllocatorInstanceType The allocator instance type
         * @param ElementSize Size of each element
         * @param ElementAlignment Alignment of elements
         * @param NewMax Desired allocation capacity
         * @param AllocatorInstance Reference to the allocator instance
         * @param ArrayNum Current number of elements
         * @param ArrayMax Reference to the array max capacity (updated)
         */
        template <u32 Flags, typename AllocatorInstanceType>
        OLO_NOINLINE void ReallocTo(
            u32                                          ElementSize,
            u32                                          ElementAlignment,
            TAllocatorSizeType_T<AllocatorInstanceType>  NewMax,
            AllocatorInstanceType&                       AllocatorInstance,
            TAllocatorSizeType_T<AllocatorInstanceType>  ArrayNum,
            TAllocatorSizeType_T<AllocatorInstanceType>& ArrayMax
        )
        {
            if constexpr (!!(Flags & 1)) // SupportsElementAlignment
            {
                if (NewMax)
                {
                    NewMax = AllocatorInstance.CalculateSlackReserve(NewMax, ElementSize, ElementAlignment);
                }
                if (NewMax != ArrayMax)
                {
                    ArrayMax = NewMax;
                    AllocatorInstance.ResizeAllocation(ArrayNum, NewMax, ElementSize, ElementAlignment);
                }
            }
            else
            {
                if (NewMax)
                {
                    NewMax = AllocatorInstance.CalculateSlackReserve(NewMax, ElementSize);
                }
                if (NewMax != ArrayMax)
                {
                    ArrayMax = NewMax;
                    AllocatorInstance.ResizeAllocation(ArrayNum, NewMax, ElementSize);
                }
            }
        }

        /**
         * @brief Specialized copy allocation
         * 
         * Used for copy operations where we're allocating fresh memory for a copy.
         * 
         * @tparam Flags Allocator capability flags
         * @tparam AllocatorInstanceType The allocator instance type
         * @param ElementSize Size of each element
         * @param ElementAlignment Alignment of elements
         * @param NewMax Desired allocation capacity
         * @param PrevMax Previous allocation capacity
         * @param AllocatorInstance Reference to the allocator instance
         * @param ArrayNum Current number of elements
         * @param ArrayMax Reference to the array max capacity (updated)
         */
        template <u32 Flags, typename AllocatorInstanceType>
        OLO_NOINLINE void ReallocForCopy(
            u32                                          ElementSize,
            u32                                          ElementAlignment,
            TAllocatorSizeType_T<AllocatorInstanceType>  NewMax,
            TAllocatorSizeType_T<AllocatorInstanceType>  PrevMax,
            AllocatorInstanceType&                       AllocatorInstance,
            TAllocatorSizeType_T<AllocatorInstanceType>  ArrayNum,
            TAllocatorSizeType_T<AllocatorInstanceType>& ArrayMax
        )
        {
            if constexpr (!!(Flags & 1)) // SupportsElementAlignment
            {
                if (NewMax)
                {
                    NewMax = AllocatorInstance.CalculateSlackReserve(NewMax, ElementSize, ElementAlignment);
                }
                if (NewMax > PrevMax)
                {
                    AllocatorInstance.ResizeAllocation(0, NewMax, ElementSize, ElementAlignment);
                }
                else
                {
                    NewMax = PrevMax;
                }
            }
            else
            {
                if (NewMax)
                {
                    NewMax = AllocatorInstance.CalculateSlackReserve(NewMax, ElementSize);
                }
                if (NewMax > PrevMax)
                {
                    AllocatorInstance.ResizeAllocation(0, NewMax, ElementSize);
                }
                else
                {
                    NewMax = PrevMax;
                }
            }
            ArrayMax = NewMax;
        }

    } // namespace Private

    // ============================================================================
    // TArray
    // ============================================================================

    /**
     * @class TArray
     * @brief Dynamic array container with pluggable allocator support
     * 
     * A templated dynamic array similar to std::vector but with:
     * - UE-style pluggable allocator policies
     * - Optimizations for trivially relocatable types
     * - Optimizations for zero-constructible types
     * 
     * @tparam InElementType   The element type stored in the array
     * @tparam InAllocatorType The allocator policy to use
     */
    template <typename InElementType, typename InAllocatorType>
    class TArray
    {
    public:
        using ElementType = InElementType;
        using AllocatorType = InAllocatorType;
        using SizeType = typename InAllocatorType::SizeType;
        using USizeType = std::make_unsigned_t<SizeType>;

        using ElementAllocatorType = typename TChooseClass<
            AllocatorType::NeedsElementType,
            typename AllocatorType::template ForElementType<ElementType>,
            typename AllocatorType::ForAnyElementType
        >::Result;

        using Iterator = TIndexedContainerIterator<TArray, ElementType, SizeType>;
        using ConstIterator = TIndexedContainerIterator<const TArray, const ElementType, SizeType>;

    private:
        template <typename OtherElementType, typename OtherAllocatorType>
        friend class TArray;

        ElementAllocatorType m_AllocatorInstance;
        SizeType m_ArrayNum;
        SizeType m_ArrayMax;

    public:
        // ====================================================================
        // Constructors & Destructor
        // ====================================================================

        /** Default constructor */
        [[nodiscard]] OLO_FINLINE constexpr TArray()
            : m_ArrayNum(0)
            , m_ArrayMax(m_AllocatorInstance.GetInitialCapacity())
        {}

        /** Explicitly consteval constructor for compile-time constant arrays. */
        [[nodiscard]] explicit consteval TArray(EConstEval)
            : m_AllocatorInstance(ConstEval)
            , m_ArrayNum(0)
            , m_ArrayMax(m_AllocatorInstance.GetInitialCapacity())
        {}

        /** Constructor with initial size */
        explicit TArray(SizeType InitialSize)
            : m_ArrayNum(0)
            , m_ArrayMax(m_AllocatorInstance.GetInitialCapacity())
        {
            AddUninitialized(InitialSize);
            DefaultConstructItems<ElementType>(GetData(), InitialSize);
        }

        /** Constructor with initial size and default value */
        TArray(SizeType InitialSize, const ElementType& DefaultValue)
            : m_ArrayNum(0)
            , m_ArrayMax(m_AllocatorInstance.GetInitialCapacity())
        {
            Reserve(InitialSize);
            for (SizeType i = 0; i < InitialSize; ++i)
            {
                Add(DefaultValue);
            }
        }

        /** Initializer list constructor */
        TArray(std::initializer_list<ElementType> InitList)
            : m_ArrayNum(0)
            , m_ArrayMax(m_AllocatorInstance.GetInitialCapacity())
        {
            Reserve(static_cast<SizeType>(InitList.size()));
            for (const auto& Item : InitList)
            {
                Add(Item);
            }
        }

        /**
         * Construct from a raw pointer and count.
         *
         * @param Ptr   A pointer to an array of elements to copy.
         * @param Count The number of elements to copy from Ptr.
         */
        OLO_FINLINE TArray(const ElementType* Ptr, SizeType Count)
            : m_ArrayNum(0)
            , m_ArrayMax(m_AllocatorInstance.GetInitialCapacity())
        {
            if (Count < 0)
            {
                // Cast to USizeType first to prevent sign extension on negative sizes
                Private::OnInvalidArrayNum(static_cast<unsigned long long>(static_cast<USizeType>(Count)));
            }

            OLO_CORE_ASSERT(Ptr != nullptr || Count == 0, "TArray: null pointer with non-zero count");

            CopyToEmpty(Ptr, Count, 0);
        }

        /** Constructor from TArrayView */
        template <typename OtherElementType, typename OtherSizeType>
        [[nodiscard]] explicit TArray(const TArrayView<OtherElementType, OtherSizeType>& Other)
            : m_ArrayNum(0)
            , m_ArrayMax(m_AllocatorInstance.GetInitialCapacity())
        {
            CopyToEmpty(Other.GetData(), static_cast<SizeType>(Other.Num()), 0);
        }

        /** Copy constructor */
        TArray(const TArray& Other)
            : m_ArrayNum(0)
            , m_ArrayMax(m_AllocatorInstance.GetInitialCapacity())
        {
            CopyToEmpty(Other.GetData(), Other.Num(), 0);
        }

        /**
         * Copy constructor with extra slack.
         *
         * @param Other The source array to copy.
         * @param ExtraSlack Additional memory to preallocate at the end.
         */
        OLO_FINLINE TArray(const TArray& Other, SizeType ExtraSlack)
            : m_ArrayNum(0)
            , m_ArrayMax(m_AllocatorInstance.GetInitialCapacity())
        {
            CopyToEmptyWithSlack(Other.GetData(), Other.Num(), 0, ExtraSlack);
        }

        /**
         * Copy constructor with changed allocator.
         *
         * @param Other The source array to copy.
         */
        template <typename OtherElementType, typename OtherAllocator>
            requires (std::is_convertible_v<const OtherElementType&, ElementType>)
        OLO_FINLINE explicit TArray(const TArray<OtherElementType, OtherAllocator>& Other)
            : m_ArrayNum(0)
            , m_ArrayMax(m_AllocatorInstance.GetInitialCapacity())
        {
            CopyToEmpty(Other.GetData(), Other.Num(), 0);
        }

        /** Move constructor */
        TArray(TArray&& Other) noexcept
            : m_ArrayNum(0)
            , m_ArrayMax(m_AllocatorInstance.GetInitialCapacity())
        {
            MoveOrCopy(*this, Other);
        }

        /**
         * Move constructor with extra slack.
         *
         * @param Other Array to move from.
         * @param ExtraSlack Additional memory to preallocate at the end.
         */
        template <typename OtherElementType>
            requires (Private::TArrayElementsAreCompatible_V<ElementType, OtherElementType&&>)
        TArray(TArray<OtherElementType, AllocatorType>&& Other, SizeType ExtraSlack)
            : m_ArrayNum(0)
            , m_ArrayMax(m_AllocatorInstance.GetInitialCapacity())
        {
            MoveOrCopyWithSlack(*this, Other, 0, ExtraSlack);
        }

        /** 
         * @brief Construct from an array view
         * @param Other The array view to copy from
         */
        template <typename OtherElementType, typename OtherSizeType>
            requires (std::is_convertible_v<OtherElementType*, ElementType*>)
        explicit TArray(TArrayView<OtherElementType, OtherSizeType> Other)
            : m_ArrayNum(0)
            , m_ArrayMax(m_AllocatorInstance.GetInitialCapacity())
        {
            CopyToEmpty(Other.GetData(), static_cast<SizeType>(Other.Num()), 0);
        }

        /** Destructor */
        ~TArray()
        {
            DestructItems(GetData(), m_ArrayNum);
            // Allocator destructor handles freeing memory
        }

        // ====================================================================
        // Assignment Operators
        // ====================================================================

        /** Copy assignment */
        TArray& operator=(const TArray& Other)
        {
            if (this != &Other)
            {
                DestructItems(GetData(), m_ArrayNum);
                CopyToEmpty(Other.GetData(), Other.Num(), m_ArrayMax);
            }
            return *this;
        }

        /** Move assignment */
        TArray& operator=(TArray&& Other) noexcept
        {
            if (this != &Other)
            {
                DestructItems(GetData(), m_ArrayNum);
                MoveOrCopy(*this, Other);
            }
            return *this;
        }

        /** Initializer list assignment */
        TArray& operator=(std::initializer_list<ElementType> InitList)
        {
            DestructItems(GetData(), m_ArrayNum);
            m_ArrayNum = 0;

            SlackTrackerNumChanged();

            Reserve(static_cast<SizeType>(InitList.size()));
            for (const auto& Item : InitList)
            {
                Add(Item);
            }
            return *this;
        }

        /** Assignment from TArrayView */
        template <typename OtherElementType, typename OtherSizeType>
        TArray& operator=(const TArrayView<OtherElementType, OtherSizeType>& Other)
        {
            DestructItems(GetData(), m_ArrayNum);
            CopyToEmpty(Other.GetData(), static_cast<SizeType>(Other.Num()), m_ArrayMax);
            return *this;
        }

        // ====================================================================
        // Element Access
        // ====================================================================

        /** Access element by index (no bounds check in release) */
        [[nodiscard]] OLO_FINLINE ElementType& operator[](SizeType Index)
        {
            OLO_CORE_ASSERT(IsValidIndex(Index), "TArray index out of bounds: {} (size: {})", Index, m_ArrayNum);
            return GetData()[Index];
        }

        /** Access element by index (const) */
        [[nodiscard]] OLO_FINLINE const ElementType& operator[](SizeType Index) const
        {
            OLO_CORE_ASSERT(IsValidIndex(Index), "TArray index out of bounds: {} (size: {})", Index, m_ArrayNum);
            return GetData()[Index];
        }

        /** Get pointer to first element */
        [[nodiscard]] OLO_FINLINE ElementType* GetData()
        {
            return reinterpret_cast<ElementType*>(m_AllocatorInstance.GetAllocation());
        }

        /** Get pointer to first element (const) */
        [[nodiscard]] OLO_FINLINE const ElementType* GetData() const
        {
            return reinterpret_cast<const ElementType*>(m_AllocatorInstance.GetAllocation());
        }

        /**
         * Helper function returning the size of the inner type.
         *
         * @returns Size in bytes of array type.
         */
        [[nodiscard]] OLO_FINLINE static constexpr u32 GetTypeSize()
        {
            return sizeof(ElementType);
        }

        /** Get first element */
        [[nodiscard]] OLO_FINLINE ElementType& First()
        {
            OLO_CORE_ASSERT(m_ArrayNum > 0, "TArray::First called on empty array");
            return GetData()[0];
        }

        /** Get first element (const) */
        [[nodiscard]] OLO_FINLINE const ElementType& First() const
        {
            OLO_CORE_ASSERT(m_ArrayNum > 0, "TArray::First called on empty array");
            return GetData()[0];
        }

        /**
         * Returns n-th last element from the array.
         *
         * @param IndexFromTheEnd (Optional) Index from the end of array (default = 0).
         * @returns Reference to n-th last element from the array.
         */
        [[nodiscard]] OLO_FINLINE ElementType& Last(SizeType IndexFromTheEnd = 0)
        {
            RangeCheck(m_ArrayNum - IndexFromTheEnd - 1);
            return GetData()[m_ArrayNum - IndexFromTheEnd - 1];
        }

        /**
         * Returns n-th last element from the array (const).
         *
         * @param IndexFromTheEnd (Optional) Index from the end of array (default = 0).
         * @returns Reference to n-th last element from the array.
         */
        [[nodiscard]] OLO_FINLINE const ElementType& Last(SizeType IndexFromTheEnd = 0) const
        {
            return const_cast<TArray*>(this)->Last(IndexFromTheEnd);
        }

        /** Get element at index from end (0 = last element) */
        [[nodiscard]] OLO_FINLINE ElementType& Top()
        {
            return Last();
        }

        /** Get element at index from end (const) */
        [[nodiscard]] OLO_FINLINE const ElementType& Top() const
        {
            return Last();
        }

        // ====================================================================
        // Size & Capacity
        // ====================================================================

        /** Get number of elements */
        [[nodiscard]] OLO_FINLINE SizeType Num() const
        {
            return m_ArrayNum;
        }

        /** Get number of bytes used (excluding slack) */
        [[nodiscard]] OLO_FINLINE sizet NumBytes() const
        {
            return static_cast<sizet>(m_ArrayNum) * sizeof(ElementType);
        }

        /** Get allocated capacity */
        [[nodiscard]] OLO_FINLINE SizeType Max() const
        {
            return m_ArrayMax;
        }

        /** Check if empty */
        [[nodiscard]] OLO_FINLINE bool IsEmpty() const
        {
            return m_ArrayNum == 0;
        }

        /** Check if index is valid */
        [[nodiscard]] OLO_FINLINE bool IsValidIndex(SizeType Index) const
        {
            return Index >= 0 && Index < m_ArrayNum;
        }

        /**
         * @brief Verify internal invariants are valid
         * 
         * Checks that the array's internal state is consistent:
         * - ArrayNum >= 0
         * - ArrayMax >= ArrayNum
         */
        OLO_FINLINE void CheckInvariants() const
        {
            OLO_CORE_ASSERT((m_ArrayNum >= 0) && (m_ArrayMax >= m_ArrayNum),
                "TArray invariant violation: ArrayNum={}, ArrayMax={}", m_ArrayNum, m_ArrayMax);
        }

        /**
         * Checks that the specified address is not part of an element within the container.
         * Used to verify that elements aren't being invalidated by reallocation.
         *
         * @param Addr The address to check.
         */
        OLO_FINLINE void CheckAddress(const ElementType* Addr) const
        {
            OLO_CORE_ASSERT(Addr < GetData() || Addr >= (GetData() + m_ArrayMax),
                "Attempting to use a container element ({}) which already comes from the container being modified ({}, ArrayMax: {}, ArrayNum: {}, SizeofElement: {})!",
                static_cast<const void*>(Addr), static_cast<const void*>(GetData()), m_ArrayMax, m_ArrayNum, sizeof(ElementType));
        }

        /**
         * Checks if index is in array range.
         *
         * @param Index Index to check.
         */
        OLO_FINLINE void RangeCheck(SizeType Index) const
        {
            CheckInvariants();

            // Template property, branch will be optimized out
            if constexpr (AllocatorType::RequireRangeCheck)
            {
                OLO_CORE_ASSERT((Index >= 0) & (Index < m_ArrayNum),
                    "Array index out of bounds: {} into an array of size {}", Index, m_ArrayNum);
            }
        }

        /**
         * Checks if a range of indices are in the array range.
         *
         * @param Index Index of the start of the range to check.
         * @param Count Number of elements in the range.
         */
        OLO_FINLINE void RangeCheck(SizeType Index, SizeType Count) const
        {
            CheckInvariants();

            // Template property, branch will be optimized out
            if constexpr (AllocatorType::RequireRangeCheck)
            {
                OLO_CORE_ASSERT((Count >= 0) & (Index >= 0) & (Index + Count <= m_ArrayNum),
                    "Array range out of bounds: index {} and length {} into an array of size {}", Index, Count, m_ArrayNum);
            }
        }

        /** Get size in bytes */
        [[nodiscard]] sizet GetAllocatedSize() const
        {
            return m_AllocatorInstance.GetAllocatedSize(m_ArrayMax, sizeof(ElementType));
        }

        /** Get amount of slack (unused allocated space) */
        [[nodiscard]] OLO_FINLINE SizeType GetSlack() const
        {
            return m_ArrayMax - m_ArrayNum;
        }

        /**
         * @brief Get access to the allocator instance
         * @returns Reference to the allocator
         */
        [[nodiscard]] OLO_FINLINE ElementAllocatorType& GetAllocatorInstance()
        {
            return m_AllocatorInstance;
        }

        /**
         * @brief Get access to the allocator instance (const)
         * @returns Const reference to the allocator
         */
        [[nodiscard]] OLO_FINLINE const ElementAllocatorType& GetAllocatorInstance() const
        {
            return m_AllocatorInstance;
        }

        // ====================================================================
        // Intrusive TOptional<TArray> State
        // ====================================================================

        /**
         * Static member indicating that TArray supports intrusive unset optional state.
         * This allows TOptional<TArray> to use an empty array with ArrayMax == -1
         * as the "unset" state, avoiding the need for a separate bool flag.
         */
        constexpr static bool bHasIntrusiveUnsetOptionalState = true;

        /** Type alias required for intrusive optional state support */
        using IntrusiveUnsetOptionalStateType = TArray;

        /**
         * Constructor for intrusive unset optional state.
         * Only TOptional can call this constructor.
         * Uses ArrayMax == -1 as the intrusive state so that the destructor
         * still works without change, as it doesn't use ArrayMax.
         */
        [[nodiscard]] explicit TArray(FIntrusiveUnsetOptionalState)
            : m_ArrayNum(0)
            , m_ArrayMax(-1)
        {
        }

        /**
         * Comparison operator for intrusive unset optional state.
         * Only TOptional should call this to check if the array is in the unset state.
         */
        [[nodiscard]] bool operator==(FIntrusiveUnsetOptionalState) const
        {
            return m_ArrayMax == -1;
        }

        // ====================================================================
        // Adding Elements
        // ====================================================================

        /**
         * Adds a new item to the end of the array, possibly reallocating the whole array to fit.
         * Move semantics version.
         *
         * @param Item The item to add
         * @return Index to the new item
         */
        SizeType Add(ElementType&& Item)
        {
            CheckAddress(&Item);
            return Emplace(std::move(Item));
        }

        /**
         * Adds a new item to the end of the array, possibly reallocating the whole array to fit.
         *
         * @param Item The item to add
         * @return Index to the new item
         */
        SizeType Add(const ElementType& Item)
        {
            CheckAddress(&Item);
            return Emplace(Item);
        }

        /**
         * Adds a new item to the end of the array, possibly reallocating the whole array to fit.
         * Move semantics version.
         *
         * @param Item The item to add
         * @return A reference to the newly-inserted element.
         */
        [[nodiscard]] OLO_FINLINE ElementType& Add_GetRef(ElementType&& Item)
        {
            CheckAddress(&Item);
            return Emplace_GetRef(std::move(Item));
        }

        /**
         * Adds a new item to the end of the array, possibly reallocating the whole array to fit.
         *
         * @param Item The item to add
         * @return A reference to the newly-inserted element.
         */
        [[nodiscard]] OLO_FINLINE ElementType& Add_GetRef(const ElementType& Item)
        {
            CheckAddress(&Item);
            return Emplace_GetRef(Item);
        }

        /**
         * Adds new items to the end of the array, possibly reallocating the whole
         * array to fit. The new items will be zeroed.
         *
         * @return Index to the first of the new items.
         */
        SizeType AddZeroed()
        {
            const SizeType Index = AddUninitialized();
            FMemory::Memzero(reinterpret_cast<u8*>(m_AllocatorInstance.GetAllocation()) + Index * sizeof(ElementType), sizeof(ElementType));
            return Index;
        }

        /**
         * Adds new items to the end of the array, possibly reallocating the whole
         * array to fit. The new items will be zeroed.
         *
         * @param Count The number of new items to add.
         * @return Index to the first of the new items.
         */
        SizeType AddZeroed(SizeType Count)
        {
            const SizeType Index = AddUninitialized(Count);
            FMemory::Memzero(reinterpret_cast<u8*>(m_AllocatorInstance.GetAllocation()) + Index * sizeof(ElementType), Count * sizeof(ElementType));
            return Index;
        }

        /**
         * Adds a new item to the end of the array, possibly reallocating the whole
         * array to fit. The new item will be zeroed.
         *
         * @return A reference to the newly-inserted element.
         */
        [[nodiscard]] ElementType& AddZeroed_GetRef()
        {
            const SizeType Index = AddUninitialized();
            ElementType* Ptr = GetData() + Index;
            FMemory::Memzero(Ptr, sizeof(ElementType));
            return *Ptr;
        }

        /**
         * Adds new items to the end of the array, possibly reallocating the whole
         * array to fit. The new items will be default-constructed.
         *
         * @return Index to the first of the new items.
         */
        SizeType AddDefaulted()
        {
            const SizeType Index = AddUninitialized();
            DefaultConstructItems<ElementType>(reinterpret_cast<void*>(reinterpret_cast<u8*>(m_AllocatorInstance.GetAllocation()) + Index * sizeof(ElementType)), 1);
            return Index;
        }

        /**
         * Adds new items to the end of the array, possibly reallocating the whole
         * array to fit. The new items will be default-constructed.
         *
         * @param Count The number of new items to add.
         * @return Index to the first of the new items.
         */
        SizeType AddDefaulted(SizeType Count)
        {
            const SizeType Index = AddUninitialized(Count);
            DefaultConstructItems<ElementType>(reinterpret_cast<void*>(reinterpret_cast<u8*>(m_AllocatorInstance.GetAllocation()) + Index * sizeof(ElementType)), Count);
            return Index;
        }

        /**
         * Add a new item to the end of the array, possibly reallocating the whole
         * array to fit. The new item will be default-constructed.
         *
         * @return A reference to the newly-inserted element.
         */
        [[nodiscard]] ElementType& AddDefaulted_GetRef()
        {
            const SizeType Index = AddUninitialized();
            ElementType* Ptr = GetData() + Index;
            DefaultConstructItems<ElementType>(static_cast<void*>(Ptr), 1);
            return *Ptr;
        }

        /** Add uninitialized space for one element, return index */
        OLO_FINLINE SizeType AddUninitialized()
        {
            // Begin sensitive code!
            // Both branches write the return into m_ArrayNum. This is because the function call
            // clobbers the registers and if we assign as part of the return into something we need,
            // the compiler doesn't have to reload the data into the clobbered register.
            if (m_ArrayNum == m_ArrayMax)
            {
                // When we can pack size and alignment into a single 16-bit load, we save a parameter
                // setup instruction for the function call.
                if constexpr (sizeof(ElementType) <= 255 && alignof(ElementType) <= 255)
                {
                    // Note: realloc functions are templated ONLY on allocator instance so they are
                    // not duplicated in the code for every element type!
                    m_ArrayNum = Private::ReallocGrow1_DoAlloc_Tiny<Private::GetAllocatorFlags<AllocatorType>()>(
                        static_cast<u16>(sizeof(ElementType) | (alignof(ElementType) << 8)),
                        m_AllocatorInstance,
                        m_ArrayMax
                    );
                }
                else
                {
                    m_ArrayNum = Private::ReallocGrow1_DoAlloc<Private::GetAllocatorFlags<AllocatorType>()>(
                        static_cast<u32>(sizeof(ElementType)),
                        static_cast<u32>(alignof(ElementType)),
                        m_AllocatorInstance,
                        m_ArrayMax
                    );
                }
            }
            // End sensitive code!

            SizeType OldArrayNum = m_ArrayNum;
            ++m_ArrayNum;
            return OldArrayNum;
        }

        /** Add uninitialized space for N elements, return index of first */
        OLO_FINLINE SizeType AddUninitialized(SizeType Count)
        {
            OLO_CORE_ASSERT(Count >= 0, "AddUninitialized: Count must be non-negative");

            return Private::ReallocGrow<Private::GetAllocatorFlags<AllocatorType>()>(
                static_cast<u32>(sizeof(ElementType)),
                static_cast<u32>(alignof(ElementType)),
                Count,
                m_AllocatorInstance,
                m_ArrayNum,
                m_ArrayMax
            );
        }

        /** Add unique element (only if not already present), return index */
        SizeType AddUnique(const ElementType& Item)
        {
            const SizeType Index = Find(Item);
            if (Index == INDEX_NONE)
            {
                return Add(Item);
            }
            return Index;
        }

        /**
         * Constructs a new item at the end of the array, possibly reallocating the whole array to fit.
         *
         * @param Args The arguments to forward to the constructor of the new item.
         * @return Index to the new item
         */
        template <typename... ArgsType>
        SizeType Emplace(ArgsType&&... Args)
        {
            const SizeType Index = AddUninitialized(1);
            ::new (static_cast<void*>(GetData() + Index)) ElementType(std::forward<ArgsType>(Args)...);
            return Index;
        }

        /**
         * Constructs a new item at the end of the array, possibly reallocating the whole array to fit.
         *
         * @param Args The arguments to forward to the constructor of the new item.
         * @return A reference to the newly-inserted element.
         */
        template <typename... ArgsType>
        [[nodiscard]] OLO_FINLINE ElementType& Emplace_GetRef(ArgsType&&... Args)
        {
            const SizeType Index = AddUninitialized(1);
            ElementType* Ptr = GetData() + Index;
            ::new (static_cast<void*>(Ptr)) ElementType(std::forward<ArgsType>(Args)...);
            return *Ptr;
        }

        /** Push element (alias for Add) */
        void Push(const ElementType& Item)
        {
            Add(Item);
        }

        void Push(ElementType&& Item)
        {
            Add(std::move(Item));
        }

        // ====================================================================
        // Inserting Elements
        // ====================================================================

        /**
         * Inserts a given element into the array at given location. Move semantics version.
         *
         * @param Item The element to insert.
         * @param Index Tells where to insert the new elements.
         * @returns Location at which the insert was done.
         */
        SizeType Insert(ElementType&& Item, SizeType Index)
        {
            CheckAddress(&Item);

            InsertUninitialized(Index, 1);
            ::new (static_cast<void*>(GetData() + Index)) ElementType(std::move(Item));
            return Index;
        }

        /**
         * Inserts a given element into the array at given location.
         *
         * @param Item The element to insert.
         * @param Index Tells where to insert the new elements.
         * @returns Location at which the insert was done.
         */
        SizeType Insert(const ElementType& Item, SizeType Index)
        {
            CheckAddress(&Item);

            InsertUninitialized(Index, 1);
            ::new (static_cast<void*>(GetData() + Index)) ElementType(Item);
            return Index;
        }

        /**
         * Inserts a given element into the array at given location. Move semantics version.
         *
         * @param Item The element to insert.
         * @param Index Tells where to insert the new element.
         * @return A reference to the newly-inserted element.
         */
        [[nodiscard]] ElementType& Insert_GetRef(ElementType&& Item, SizeType Index)
        {
            CheckAddress(&Item);

            InsertUninitialized(Index, 1);
            ElementType* Ptr = GetData() + Index;
            ::new (static_cast<void*>(Ptr)) ElementType(std::move(Item));
            return *Ptr;
        }

        /**
         * Inserts a given element into the array at given location.
         *
         * @param Item The element to insert.
         * @param Index Tells where to insert the new element.
         * @return A reference to the newly-inserted element.
         */
        [[nodiscard]] ElementType& Insert_GetRef(const ElementType& Item, SizeType Index)
        {
            CheckAddress(&Item);

            InsertUninitialized(Index, 1);
            ElementType* Ptr = GetData() + Index;
            ::new (static_cast<void*>(Ptr)) ElementType(Item);
            return *Ptr;
        }

        /**
         * Constructs a new item at a specified index, possibly reallocating the whole array to fit.
         *
         * @param Index The index to add the item at.
         * @param Args The arguments to forward to the constructor of the new item.
         */
        template <typename... ArgsType>
        OLO_FINLINE void EmplaceAt(SizeType Index, ArgsType&&... Args)
        {
            InsertUninitialized(Index, 1);
            ::new (static_cast<void*>(GetData() + Index)) ElementType(std::forward<ArgsType>(Args)...);
        }

        /**
         * Constructs a new item at a specified index, possibly reallocating the whole array to fit.
         *
         * @param Index The index to add the item at.
         * @param Args The arguments to forward to the constructor of the new item.
         * @return A reference to the newly-inserted element.
         */
        template <typename... ArgsType>
        [[nodiscard]] OLO_FINLINE ElementType& EmplaceAt_GetRef(SizeType Index, ArgsType&&... Args)
        {
            InsertUninitialized(Index, 1);
            ElementType* Ptr = GetData() + Index;
            ::new (static_cast<void*>(Ptr)) ElementType(std::forward<ArgsType>(Args)...);
            return *Ptr;
        }

        /** Insert uninitialized space at index */
        void InsertUninitialized(SizeType Index, SizeType Count = 1)
        {
            OLO_CORE_ASSERT(Index >= 0 && Index <= m_ArrayNum, "InsertUninitialized: Index out of bounds");
            OLO_CORE_ASSERT(Count >= 0, "InsertUninitialized: Count must be non-negative");

            const SizeType OldNum = Private::ReallocGrow<Private::GetAllocatorFlags<AllocatorType>()>(
                static_cast<u32>(sizeof(ElementType)),
                static_cast<u32>(alignof(ElementType)),
                Count,
                m_AllocatorInstance,
                m_ArrayNum,
                m_ArrayMax
            );

            // Move existing elements to make room
            ElementType* Data = GetData();
            if (Index < OldNum)
            {
                RelocateConstructItems<ElementType>(Data + Index + Count, Data + Index, OldNum - Index);
            }
        }

        /** Insert default-constructed elements at index */
        void InsertDefaulted(SizeType Index, SizeType Count = 1)
        {
            InsertUninitialized(Index, Count);
            DefaultConstructItems<ElementType>(GetData() + Index, Count);
        }

        /** Insert zero-initialized elements at index */
        void InsertZeroed(SizeType Index, SizeType Count = 1)
        {
            InsertUninitialized(Index, Count);
            FMemory::Memzero(GetData() + Index, Count * sizeof(ElementType));
        }

        /**
         * Inserts a zeroed element into the array at given location.
         *
         * @param Index Tells where to insert the new element.
         * @return A reference to the newly-inserted element.
         */
        ElementType& InsertZeroed_GetRef(SizeType Index)
        {
            InsertUninitialized(Index, 1);
            ElementType* Ptr = GetData() + Index;
            FMemory::Memzero(Ptr, sizeof(ElementType));
            return *Ptr;
        }

        /**
         * Inserts a default-constructed element into the array at a given location.
         *
         * @param Index Tells where to insert the new element.
         * @return A reference to the newly-inserted element.
         */
        ElementType& InsertDefaulted_GetRef(SizeType Index)
        {
            InsertUninitialized(Index, 1);
            ElementType* Ptr = GetData() + Index;
            DefaultConstructItems<ElementType>(static_cast<void*>(Ptr), 1);
            return *Ptr;
        }

        /**
         * Inserts given elements into the array at given location.
         *
         * @param InitList Array of elements to insert.
         * @param InIndex Tells where to insert the new elements.
         * @returns Location at which the item was inserted.
         */
        SizeType Insert(std::initializer_list<ElementType> InitList, const SizeType InIndex)
        {
            SizeType NumNewElements = static_cast<SizeType>(InitList.size());

            InsertUninitialized(InIndex, NumNewElements);
            ConstructItems<ElementType>(static_cast<void*>(GetData() + InIndex), InitList.begin(), NumNewElements);

            return InIndex;
        }

        /**
         * Inserts given elements into the array at given location.
         *
         * @param Items Array of elements to insert.
         * @param InIndex Tells where to insert the new elements.
         * @returns Location at which the item was inserted.
         */
        template <typename OtherAllocator>
        SizeType Insert(const TArray<ElementType, OtherAllocator>& Items, const SizeType InIndex)
        {
            OLO_CORE_ASSERT(static_cast<const void*>(this) != static_cast<const void*>(&Items), "Insert: Cannot insert array into itself");

            auto NumNewElements = Items.Num();

            InsertUninitialized(InIndex, NumNewElements);
            ConstructItems<ElementType>(static_cast<void*>(GetData() + InIndex), Items.GetData(), NumNewElements);

            return InIndex;
        }

        /**
         * Inserts given elements into the array at given location.
         *
         * @param Items Array of elements to insert.
         * @param InIndex Tells where to insert the new elements.
         * @returns Location at which the item was inserted.
         */
        template <typename OtherAllocator>
        SizeType Insert(TArray<ElementType, OtherAllocator>&& Items, const SizeType InIndex)
        {
            OLO_CORE_ASSERT(static_cast<const void*>(this) != static_cast<const void*>(&Items), "Insert: Cannot insert array into itself");

            auto NumNewElements = Items.Num();

            InsertUninitialized(InIndex, NumNewElements);
            RelocateConstructItems<ElementType>(static_cast<void*>(GetData() + InIndex), Items.GetData(), NumNewElements);
            Items.m_ArrayNum = 0;

            return InIndex;
        }

        /**
         * Inserts a raw array of elements at a particular index in the TArray.
         *
         * @param Ptr A pointer to an array of elements to add.
         * @param Count The number of elements to insert from Ptr.
         * @param Index The index to insert the elements at.
         * @return The index of the first element inserted.
         */
        SizeType Insert(const ElementType* Ptr, SizeType Count, SizeType Index)
        {
            OLO_CORE_ASSERT(Ptr != nullptr, "Insert: null pointer");

            InsertUninitialized(Index, Count);
            ConstructItems<ElementType>(static_cast<void*>(GetData() + Index), Ptr, Count);

            return Index;
        }

        // ====================================================================
        // Removing Elements
        // ====================================================================

        /** Remove element at index */
        void RemoveAt(SizeType Index, SizeType Count = 1, EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            OLO_CORE_ASSERT(Index >= 0 && Index + Count <= m_ArrayNum, "RemoveAt: Index out of bounds");
            OLO_CORE_ASSERT(Count >= 0, "RemoveAt: Count must be non-negative");

            if (Count > 0)
            {
                ElementType* Data = GetData();

                // Destruct removed elements
                DestructItems(Data + Index, Count);

                // Move remaining elements
                const SizeType NumToMove = m_ArrayNum - Index - Count;
                if (NumToMove > 0)
                {
                    RelocateConstructItems<ElementType>(Data + Index, Data + Index + Count, NumToMove);
                }

                m_ArrayNum -= Count;

                if (AllowShrinking == EAllowShrinking::Yes)
                {
                    Private::ReallocShrink<Private::GetAllocatorFlags<AllocatorType>()>(
                        static_cast<u32>(sizeof(ElementType)),
                        static_cast<u32>(alignof(ElementType)),
                        m_AllocatorInstance,
                        m_ArrayNum,
                        m_ArrayMax
                    );
                }
            }
        }

        /** Remove element at index by swapping with last element (faster, changes order) */
        void RemoveAtSwap(SizeType Index, SizeType Count = 1, EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            OLO_CORE_ASSERT(Index >= 0 && Index + Count <= m_ArrayNum, "RemoveAtSwap: Index out of bounds");
            OLO_CORE_ASSERT(Count >= 0, "RemoveAtSwap: Count must be non-negative");

            if (Count > 0)
            {
                ElementType* Data = GetData();

                // Destruct removed elements
                DestructItems(Data + Index, Count);

                // Move elements from the end to fill the gap
                const SizeType NumToMove = std::min(Count, m_ArrayNum - Index - Count);
                if (NumToMove > 0)
                {
                    RelocateConstructItems<ElementType>(Data + Index, Data + m_ArrayNum - NumToMove, NumToMove);
                }

                m_ArrayNum -= Count;

                if (AllowShrinking == EAllowShrinking::Yes)
                {
                    Private::ReallocShrink<Private::GetAllocatorFlags<AllocatorType>()>(
                        static_cast<u32>(sizeof(ElementType)),
                        static_cast<u32>(alignof(ElementType)),
                        m_AllocatorInstance,
                        m_ArrayNum,
                        m_ArrayMax
                    );
                }
            }
        }

        /** 
         * Remove and return the last element
         * @param AllowShrinking Whether to allow shrinking the allocation
         * @return The removed element (moved)
         */
        ElementType Pop(EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            OLO_CORE_ASSERT(m_ArrayNum > 0, "Pop: Array is empty");
            ElementType Result = std::move(GetData()[m_ArrayNum - 1]);
            RemoveAt(m_ArrayNum - 1, 1, AllowShrinking);
            return Result;
        }

        /** Remove all elements */
        void Empty(SizeType ExpectedNumElements = 0)
        {
            DestructItems(GetData(), m_ArrayNum);
            m_ArrayNum = 0;

            SlackTrackerNumChanged();

            if (ExpectedNumElements > m_ArrayMax)
            {
                ResizeAllocation(ExpectedNumElements);
            }
        }

        /** Remove all elements and free memory */
        void Reset(SizeType NewSize = 0)
        {
            DestructItems(GetData(), m_ArrayNum);
            m_ArrayNum = 0;

            SlackTrackerNumChanged();

            ResizeAllocation(NewSize);
        }

        /** Set number of elements (destructs extra or default-constructs new) */
        void SetNum(SizeType NewNum, EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            OLO_CORE_ASSERT(NewNum >= 0, "SetNum: NewNum must be non-negative");

            if (NewNum > m_ArrayNum)
            {
                const SizeType Diff = NewNum - m_ArrayNum;
                AddDefaulted(Diff);
            }
            else if (NewNum < m_ArrayNum)
            {
                RemoveAt(NewNum, m_ArrayNum - NewNum, AllowShrinking);
            }
        }

        /** Set number of elements with uninitialized new elements */
        void SetNumUninitialized(SizeType NewNum, EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            OLO_CORE_ASSERT(NewNum >= 0, "SetNumUninitialized: NewNum must be non-negative");

            if (NewNum > m_ArrayNum)
            {
                AddUninitialized(NewNum - m_ArrayNum);
            }
            else if (NewNum < m_ArrayNum)
            {
                RemoveAt(NewNum, m_ArrayNum - NewNum, AllowShrinking);
            }
        }

        /** Set number of elements with zeroed new elements */
        void SetNumZeroed(SizeType NewNum, EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            OLO_CORE_ASSERT(NewNum >= 0, "SetNumZeroed: NewNum must be non-negative");

            if (NewNum > m_ArrayNum)
            {
                AddZeroed(NewNum - m_ArrayNum);
            }
            else if (NewNum < m_ArrayNum)
            {
                RemoveAt(NewNum, m_ArrayNum - NewNum, AllowShrinking);
            }
        }

        /**
         * Does nothing except setting the new number of elements in the array.
         * Does not destruct items, does not de-allocate memory.
         * @param NewNum New number of elements in the array, must be <= the current number of elements in the array.
         */
        void SetNumUnsafeInternal(SizeType NewNum)
        {
            OLO_CORE_ASSERT(NewNum <= m_ArrayNum && NewNum >= 0, "SetNumUnsafeInternal: NewNum out of bounds");
            m_ArrayNum = NewNum;

            SlackTrackerNumChanged();
        }

        /**
         * Sets the size of the array, filling it with the given element.
         *
         * @param Element The element to fill array with.
         * @param Number The number of elements that the array should be able to contain after allocation.
         */
        void Init(const ElementType& Element, SizeType Number)
        {
            Empty(Number);
            for (SizeType Index = 0; Index < Number; ++Index)
            {
                Add(Element);
            }
        }

        /**
         * Removes the first occurrence of the specified item in the array,
         * maintaining order but not indices.
         *
         * @param Item The item to remove.
         * @returns The number of items removed. For RemoveSingleItem, this is always either 0 or 1.
         */
        SizeType RemoveSingle(const ElementType& Item)
        {
            SizeType Index = Find(Item);
            if (Index == INDEX_NONE)
            {
                return 0;
            }

            auto* RemovePtr = GetData() + Index;

            // Destruct items that match the specified Item.
            DestructItems(RemovePtr, 1);
            RelocateConstructItems<ElementType>(static_cast<void*>(RemovePtr), RemovePtr + 1, m_ArrayNum - (Index + 1));

            // Update the array count
            --m_ArrayNum;

            // Removed one item
            return 1;
        }

        /**
         * Removes the first occurrence of the specified item in the array.
         * This version is much more efficient O(Count) than RemoveSingle O(ArrayNum),
         * but does not preserve the order.
         *
         * @param Item Item to remove from array.
         * @param AllowShrinking By default, arrays with large amounts of slack will automatically shrink.
         * @returns The number of items removed. For RemoveSingleItem, this is always either 0 or 1.
         */
        SizeType RemoveSingleSwap(const ElementType& Item, EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            SizeType Index = Find(Item);
            if (Index == INDEX_NONE)
            {
                return 0;
            }

            RemoveAtSwap(Index, 1, AllowShrinking);

            // Removed one item
            return 1;
        }

        // ====================================================================
        // Capacity Management
        // ====================================================================

        /** Reserve capacity for at least NumElements */
        void Reserve(SizeType NumElements)
        {
            if (NumElements > m_ArrayMax)
            {
                ResizeAllocation(NumElements);
            }
        }

        /** Shrink allocation to fit current size */
        void Shrink()
        {
            if (m_ArrayMax != m_ArrayNum)
            {
                ResizeAllocation(m_ArrayNum);
            }
        }

        // ====================================================================
        // Searching
        // ====================================================================

        /** Find index of element (returns INDEX_NONE if not found) */
        [[nodiscard]] SizeType Find(const ElementType& Item) const
        {
            const ElementType* Data = GetData();
            for (SizeType i = 0; i < m_ArrayNum; ++i)
            {
                if (Data[i] == Item)
                {
                    return i;
                }
            }
            return INDEX_NONE;
        }

        /** Find index of element by predicate */
        template <typename Predicate>
        [[nodiscard]] SizeType FindByPredicate(Predicate Pred) const
        {
            const ElementType* Data = GetData();
            for (SizeType i = 0; i < m_ArrayNum; ++i)
            {
                if (Pred(Data[i]))
                {
                    return i;
                }
            }
            return INDEX_NONE;
        }

        /** Check if element exists */
        [[nodiscard]] bool Contains(const ElementType& Item) const
        {
            return Find(Item) != INDEX_NONE;
        }

        /** Check if element exists by predicate */
        template <typename Predicate>
        [[nodiscard]] bool ContainsByPredicate(Predicate Pred) const
        {
            return FindByPredicate(Pred) != INDEX_NONE;
        }

        /** 
         * Find index of element starting from the end
         * @returns Index of the found element, INDEX_NONE otherwise
         */
        [[nodiscard]] SizeType FindLast(const ElementType& Item) const
        {
            const ElementType* Data = GetData();
            for (SizeType i = m_ArrayNum - 1; i >= 0; --i)
            {
                if (Data[i] == Item)
                {
                    return i;
                }
            }
            return INDEX_NONE;
        }

        /** 
         * Find index of element starting from the end (output parameter version)
         * @returns true if found, false otherwise
         */
        OLO_FINLINE bool FindLast(const ElementType& Item, SizeType& Index) const
        {
            Index = FindLast(Item);
            return Index != INDEX_NONE;
        }

        /**
         * Searches an initial subrange of the array for the last occurrence of an element which matches the specified predicate.
         * @param Pred Predicate taking array element and returns true if element matches search criteria
         * @param Count The number of elements from the front of the array through which to search
         * @returns Index of the found element, INDEX_NONE otherwise
         */
        template <typename Predicate>
        [[nodiscard]] SizeType FindLastByPredicate(Predicate Pred, SizeType Count) const
        {
            OLO_CORE_ASSERT(Count >= 0 && Count <= m_ArrayNum, "FindLastByPredicate: Count out of bounds");
            const ElementType* Data = GetData();
            for (SizeType i = Count - 1; i >= 0; --i)
            {
                if (Pred(Data[i]))
                {
                    return i;
                }
            }
            return INDEX_NONE;
        }

        /**
         * Searches the array for the last occurrence of an element which matches the specified predicate.
         * @param Pred Predicate taking array element and returns true if element matches search criteria
         * @returns Index of the found element, INDEX_NONE otherwise
         */
        template <typename Predicate>
        [[nodiscard]] OLO_FINLINE SizeType FindLastByPredicate(Predicate Pred) const
        {
            return FindLastByPredicate(Pred, m_ArrayNum);
        }

        /**
         * Finds an item by key (assuming the ElementType overloads operator== for the comparison).
         * @param Key The key to search by
         * @returns Index to the first matching element, or INDEX_NONE if none is found
         */
        template <typename KeyType>
        [[nodiscard]] SizeType IndexOfByKey(const KeyType& Key) const
        {
            const ElementType* Data = GetData();
            for (SizeType i = 0; i < m_ArrayNum; ++i)
            {
                if (Data[i] == Key)
                {
                    return i;
                }
            }
            return INDEX_NONE;
        }

        /**
         * Finds an item by predicate.
         * @param Pred The predicate to match
         * @returns Index to the first matching element, or INDEX_NONE if none is found
         */
        template <typename Predicate>
        [[nodiscard]] SizeType IndexOfByPredicate(Predicate Pred) const
        {
            const ElementType* Data = GetData();
            for (SizeType i = 0; i < m_ArrayNum; ++i)
            {
                if (Pred(Data[i]))
                {
                    return i;
                }
            }
            return INDEX_NONE;
        }

        /**
         * Finds an item by key (assuming the ElementType overloads operator== for the comparison).
         * @param Key The key to search by
         * @returns Pointer to the first matching element, or nullptr if none is found
         */
        template <typename KeyType>
        [[nodiscard]] ElementType* FindByKey(const KeyType& Key)
        {
            ElementType* Data = GetData();
            for (SizeType i = 0; i < m_ArrayNum; ++i)
            {
                if (Data[i] == Key)
                {
                    return &Data[i];
                }
            }
            return nullptr;
        }

        template <typename KeyType>
        [[nodiscard]] const ElementType* FindByKey(const KeyType& Key) const
        {
            return const_cast<TArray*>(this)->FindByKey(Key);
        }

        /**
         * Finds an element which matches a predicate functor.
         * @param Pred The functor to apply to each element
         * @returns Pointer to the first element for which the predicate returns true, or nullptr if none is found
         */
        template <typename Predicate>
        [[nodiscard]] ElementType* FindByPredicate(Predicate Pred)
        {
            ElementType* Data = GetData();
            for (SizeType i = 0; i < m_ArrayNum; ++i)
            {
                if (Pred(Data[i]))
                {
                    return &Data[i];
                }
            }
            return nullptr;
        }

        template <typename Predicate>
        [[nodiscard]] const ElementType* FindByPredicate(Predicate Pred) const
        {
            return const_cast<TArray*>(this)->FindByPredicate(Pred);
        }

        /**
         * Filters the elements in the array based on a predicate functor.
         * @param Pred The functor to apply to each element
         * @returns TArray with the same type as this object which contains
         *          the subset of elements for which the functor returns true
         */
        template <typename Predicate>
        [[nodiscard]] TArray<ElementType> FilterByPredicate(Predicate Pred) const
        {
            TArray<ElementType> FilterResults;
            const ElementType* Data = GetData();
            for (SizeType i = 0; i < m_ArrayNum; ++i)
            {
                if (Pred(Data[i]))
                {
                    FilterResults.Add(Data[i]);
                }
            }
            return FilterResults;
        }

        // ====================================================================
        // Removing by Value
        // ====================================================================

        /** Remove first occurrence of element */
        SizeType Remove(const ElementType& Item)
        {
            const SizeType Index = Find(Item);
            if (Index != INDEX_NONE)
            {
                RemoveAt(Index);
                return 1;
            }
            return 0;
        }

        /**
         * Removes as many instances of Item as there are in the array, maintaining
         * order but not indices.
         *
         * @param Item Item to remove from array.
         * @returns Number of removed elements.
         */
        SizeType RemoveAll(const ElementType& Item)
        {
            CheckAddress(&Item);

            // Element is non-const to preserve compatibility with existing code with a non-const operator==() member function
            return RemoveAll([&Item](ElementType& Element) { return Element == Item; });
        }

        /**
         * Remove all instances that match the predicate, maintaining order but not indices.
         * Optimized to work with runs of matches/non-matches.
         *
         * @param Predicate Predicate class instance
         * @returns Number of removed elements.
         */
        template <class PREDICATE_CLASS>
        SizeType RemoveAll(const PREDICATE_CLASS& Predicate)
        {
            const SizeType OriginalNum = m_ArrayNum;
            if (!OriginalNum)
            {
                return 0; // nothing to do, loop assumes one item so need to deal with this edge case here
            }

            ElementType* Data = GetData();

            SizeType WriteIndex = 0;
            SizeType ReadIndex = 0;
            bool bNotMatch = !Predicate(Data[ReadIndex]); // use a ! to guarantee it can't be anything other than zero or one
            do
            {
                SizeType RunStartIndex = ReadIndex++;
                while (ReadIndex < OriginalNum && bNotMatch == !Predicate(Data[ReadIndex]))
                {
                    ReadIndex++;
                }
                SizeType RunLength = ReadIndex - RunStartIndex;
                OLO_CORE_ASSERT(RunLength > 0, "RemoveAll: RunLength must be positive");
                if (bNotMatch)
                {
                    // this was a non-matching run, we need to move it
                    if (WriteIndex != RunStartIndex)
                    {
                        RelocateConstructItems<ElementType>(static_cast<void*>(Data + WriteIndex), Data + RunStartIndex, RunLength);
                    }
                    WriteIndex += RunLength;
                }
                else
                {
                    // this was a matching run, delete it
                    DestructItems(Data + RunStartIndex, RunLength);
                }
                bNotMatch = !bNotMatch;
            } while (ReadIndex < OriginalNum);

            m_ArrayNum = WriteIndex;

            SlackTrackerNumChanged();

            return OriginalNum - m_ArrayNum;
        }

        /** Remove first occurrence by swapping with last */
        SizeType RemoveSwap(const ElementType& Item)
        {
            const SizeType Index = Find(Item);
            if (Index != INDEX_NONE)
            {
                RemoveAtSwap(Index);
                return 1;
            }
            return 0;
        }

        /** Remove all matching predicate (alias for RemoveAll with predicate) */
        template <typename Predicate>
        SizeType RemoveAllByPredicate(Predicate Pred)
        {
            return RemoveAll(Pred);
        }

        /**
         * Remove all instances that match the predicate.
         *
         * This version is much more efficient than RemoveAll O(ArrayNum^2) because it uses
         * RemoveAtSwap internally which is O(Count) instead of RemoveAt which is O(ArrayNum),
         * but does not preserve the order.
         *
         * @param Predicate Predicate to use
         * @param AllowShrinking By default, arrays with large amounts of slack will automatically shrink.
         * @returns Number of elements removed.
         */
        template <class PREDICATE_CLASS>
        SizeType RemoveAllSwap(const PREDICATE_CLASS& Predicate, EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            bool bRemoved = false;
            const SizeType OriginalNum = m_ArrayNum;
            for (SizeType ItemIndex = 0; ItemIndex < Num();)
            {
                if (Predicate((*this)[ItemIndex]))
                {
                    bRemoved = true;
                    RemoveAtSwap(ItemIndex, 1, EAllowShrinking::No);
                }
                else
                {
                    ++ItemIndex;
                }
            }

            if (bRemoved && AllowShrinking == EAllowShrinking::Yes)
            {
                Private::ReallocShrink<Private::GetAllocatorFlags<AllocatorType>()>(
                    static_cast<u32>(sizeof(ElementType)),
                    static_cast<u32>(alignof(ElementType)),
                    m_AllocatorInstance,
                    m_ArrayNum,
                    m_ArrayMax
                );
            }

            return OriginalNum - m_ArrayNum;
        }

        // ====================================================================
        // Append Operations
        // ====================================================================

        /** Append array */
        void Append(const TArray& Source)
        {
            if (Source.Num() > 0)
            {
                const SizeType Index = AddUninitialized(Source.Num());
                ConstructItems<ElementType>(GetData() + Index, Source.GetData(), Source.Num());
            }
        }

        /** Append array by move */
        void Append(TArray&& Source)
        {
            if (Source.Num() > 0)
            {
                const SizeType Index = AddUninitialized(Source.Num());
                RelocateConstructItems<ElementType>(GetData() + Index, Source.GetData(), Source.Num());
                Source.m_ArrayNum = 0;
            }
        }

        /** Append raw array */
        void Append(const ElementType* Ptr, SizeType Count)
        {
            OLO_CORE_ASSERT(Count >= 0, "Append: Count must be non-negative");
            if (Count > 0)
            {
                const SizeType Index = AddUninitialized(Count);
                ConstructItems<ElementType>(GetData() + Index, Ptr, Count);
            }
        }

        /**
         * @brief Appends the elements from a contiguous range to this array
         * 
         * This overload accepts any contiguous container (e.g., std::vector, std::array,
         * TArrayView) that is not a TArray itself. For TArray sources, use the
         * TArray-specific overloads which may be more efficient.
         * 
         * @tparam RangeType A contiguous container type
         * @param Source The range of elements to append
         */
        template <
            typename RangeType,
            typename = std::enable_if_t<
                TIsContiguousContainer<std::remove_reference_t<RangeType>>::Value &&
                !Private::TIsTArrayOrDerivedFromTArray_V<std::remove_reference_t<RangeType>> &&
                Private::TArrayElementsAreCompatible_V<ElementType, TElementType_T<std::remove_reference_t<RangeType>>>
            >
        >
        void Append(RangeType&& Source)
        {
            auto InCount = GetNum(Source);
            OLO_CORE_ASSERT(InCount >= 0, "Append: Invalid range size");

            // Do nothing if the source is empty.
            if (!InCount)
            {
                return;
            }

            SizeType SourceCount = static_cast<SizeType>(InCount);

            // Allocate memory for the new elements.
            const SizeType Pos = AddUninitialized(SourceCount);
            ConstructItems<ElementType>(GetData() + Pos, Private::GetDataHelper(Source), SourceCount);
        }

        /** Append initializer list */
        void Append(std::initializer_list<ElementType> InitList)
        {
            Reserve(m_ArrayNum + static_cast<SizeType>(InitList.size()));
            for (const auto& Item : InitList)
            {
                Add(Item);
            }
        }

        TArray& operator+=(const TArray& Other)
        {
            Append(Other);
            return *this;
        }

        TArray& operator+=(TArray&& Other)
        {
            Append(std::move(Other));
            return *this;
        }

        /**
         * Appends the specified initializer list to this array.
         *
         * @param InitList The initializer list to append.
         */
        TArray& operator+=(std::initializer_list<ElementType> InitList)
        {
            Append(InitList);
            return *this;
        }

        // ====================================================================
        // Comparison
        // ====================================================================

        [[nodiscard]] bool operator==(const TArray& Other) const
        {
            if (m_ArrayNum != Other.m_ArrayNum)
            {
                return false;
            }
            return CompareItems(GetData(), Other.GetData(), m_ArrayNum);
        }

        [[nodiscard]] bool operator!=(const TArray& Other) const
        {
            return !(*this == Other);
        }

        // ====================================================================
        // Serialization
        // ====================================================================

        /**
         * Bulk serialize array as a single memory blob when loading. Uses regular serialization code for saving
         * and doesn't serialize at all otherwise (e.g. transient, garbage collection, ...).
         *
         * Requirements:
         *   - T's << operator needs to serialize ALL member variables in the SAME order they are layed out in memory.
         *   - T's << operator can NOT perform any fixup operations.
         *   - T can NOT contain any member variables requiring constructor calls or pointers
         *   - sizeof(ElementType) must be equal to the sum of sizes of it's member variables.
         *   - Code can not rely on serialization of T if neither IsLoading() nor IsSaving() is true.
         *   - Can only be called on platforms that either have the same endianness as the one the content was saved with
         *     or had the endian conversion occur in a cooking process.
         *
         * @param Ar FArchive to bulk serialize this TArray to/from
         * @param bForcePerElementSerialization If true, always use per-element serialization
         */
        void BulkSerialize(FArchive& Ar, bool bForcePerElementSerialization = false)
        {
            constexpr i32 ElementSize = sizeof(ElementType);
            // Serialize element size to detect mismatch across platforms.
            i32 SerializedElementSize = ElementSize;
            Ar << SerializedElementSize;

            if (bForcePerElementSerialization
                || (Ar.IsSaving()           // if we are saving, we always do the ordinary serialize as a way to make sure it matches up with bulk serialization
                && !Ar.IsTransacting())     // but transacting is performance critical, so we skip that
                || Ar.IsByteSwapping()      // if we are byteswapping, we need to do that per-element
                )
            {
                Ar << *this;
            }
            else
            {
                CountBytes(Ar);
                if (Ar.IsLoading())
                {
                    // Basic sanity checking to ensure that sizes match.
                    if (SerializedElementSize != ElementSize)
                    {
                        Ar.SetError();
                        return;
                    }

                    // Serialize the number of elements, block allocate the right amount of memory and deserialize
                    // the data as a giant memory blob in a single call to Serialize.
                    SizeType NewArrayNum = 0;
                    Ar << NewArrayNum;
                    if (NewArrayNum < 0 || (std::numeric_limits<SizeType>::max() / static_cast<SizeType>(ElementSize) < NewArrayNum))
                    {
                        Ar.SetError();
                        return;
                    }
                    Empty(NewArrayNum);
                    AddUninitialized(NewArrayNum);
                    Ar.Serialize(GetData(), static_cast<i64>(NewArrayNum) * static_cast<i64>(ElementSize));
                }
                else if (Ar.IsSaving())
                {
                    SizeType ArrayCount = Num();
                    Ar << ArrayCount;
                    Ar.Serialize(GetData(), static_cast<i64>(ArrayCount) * static_cast<i64>(ElementSize));
                }
            }
        }

        /**
         * Count bytes needed to serialize this array.
         *
         * @param Ar Archive to count for.
         */
        void CountBytes(FArchive& Ar) const
        {
            Ar.CountBytes(m_ArrayNum * sizeof(ElementType), m_ArrayMax * sizeof(ElementType));
        }

        // ====================================================================
        // Iterators
        // ====================================================================

        [[nodiscard]] Iterator CreateIterator()
        {
            return Iterator(*this);
        }

        [[nodiscard]] ConstIterator CreateConstIterator() const
        {
            return ConstIterator(*this);
        }

        // ====================================================================
        // Range-for iterator types
        // ====================================================================
    private:
#if OLO_ARRAY_RANGED_FOR_CHECKS
        using RangedForIteratorType             = TCheckedPointerIterator<      ElementType, SizeType, false>;
        using RangedForConstIteratorType        = TCheckedPointerIterator<const ElementType, SizeType, false>;
        using RangedForReverseIteratorType      = TCheckedPointerIterator<      ElementType, SizeType, true>;
        using RangedForConstReverseIteratorType = TCheckedPointerIterator<const ElementType, SizeType, true>;
#else
        using RangedForIteratorType             =                               ElementType*;
        using RangedForConstIteratorType        =                         const ElementType*;
        using RangedForReverseIteratorType      = TReversePointerIterator<      ElementType>;
        using RangedForConstReverseIteratorType = TReversePointerIterator<const ElementType>;
#endif

    public:
        // Range-for support
#if OLO_ARRAY_RANGED_FOR_CHECKS
        [[nodiscard]] OLO_FINLINE RangedForIteratorType             begin ()       { return RangedForIteratorType            (m_ArrayNum, GetData()); }
        [[nodiscard]] OLO_FINLINE RangedForConstIteratorType        begin () const { return RangedForConstIteratorType       (m_ArrayNum, GetData()); }
        [[nodiscard]] OLO_FINLINE RangedForIteratorType             end   ()       { return RangedForIteratorType            (m_ArrayNum, GetData() + Num()); }
        [[nodiscard]] OLO_FINLINE RangedForConstIteratorType        end   () const { return RangedForConstIteratorType       (m_ArrayNum, GetData() + Num()); }
        [[nodiscard]] OLO_FINLINE RangedForReverseIteratorType      rbegin()       { return RangedForReverseIteratorType     (m_ArrayNum, GetData() + Num()); }
        [[nodiscard]] OLO_FINLINE RangedForConstReverseIteratorType rbegin() const { return RangedForConstReverseIteratorType(m_ArrayNum, GetData() + Num()); }
        [[nodiscard]] OLO_FINLINE RangedForReverseIteratorType      rend  ()       { return RangedForReverseIteratorType     (m_ArrayNum, GetData()); }
        [[nodiscard]] OLO_FINLINE RangedForConstReverseIteratorType rend  () const { return RangedForConstReverseIteratorType(m_ArrayNum, GetData()); }
#else
        [[nodiscard]] OLO_FINLINE RangedForIteratorType             begin ()       { return                                   GetData(); }
        [[nodiscard]] OLO_FINLINE RangedForConstIteratorType        begin () const { return                                   GetData(); }
        [[nodiscard]] OLO_FINLINE RangedForIteratorType             end   ()       { return                                   GetData() + Num(); }
        [[nodiscard]] OLO_FINLINE RangedForConstIteratorType        end   () const { return                                   GetData() + Num(); }
        [[nodiscard]] OLO_FINLINE RangedForReverseIteratorType      rbegin()       { return RangedForReverseIteratorType     (GetData() + Num()); }
        [[nodiscard]] OLO_FINLINE RangedForConstReverseIteratorType rbegin() const { return RangedForConstReverseIteratorType(GetData() + Num()); }
        [[nodiscard]] OLO_FINLINE RangedForReverseIteratorType      rend  ()       { return RangedForReverseIteratorType     (GetData()); }
        [[nodiscard]] OLO_FINLINE RangedForConstReverseIteratorType rend  () const { return RangedForConstReverseIteratorType(GetData()); }
#endif

        // ====================================================================
        // Sorting
        // ====================================================================

        /**
         * Sorts the array assuming < operator is defined for the item type.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during sorting.
         *        Therefore, your array will be sorted by the values being pointed to, rather than the pointers' values.
         *        If this is not desirable, please use Algo::Sort(MyArray) directly instead.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        void Sort()
        {
            Algo::Sort(*this, TDereferenceWrapper<ElementType, TLess<>>(TLess<>()));
        }

        /**
         * Sorts the array using user defined predicate class.
         *
         * @param Predicate Predicate class instance.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during sorting.
         *        Therefore, your predicate will be passed references rather than pointers.
         *        If this is not desirable, please use Algo::Sort(MyArray, Predicate) directly instead.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        template <class PREDICATE_CLASS>
        void Sort(const PREDICATE_CLASS& Predicate)
        {
            TDereferenceWrapper<ElementType, PREDICATE_CLASS> PredicateWrapper(Predicate);
            Algo::Sort(*this, PredicateWrapper);
        }

        /**
         * Stable sorts the array assuming < operator is defined for the item type.
         *
         * Stable sort is slower than non-stable algorithm.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during sorting.
         *        Therefore, your array will be sorted by the values being pointed to, rather than the pointers' values.
         *        If this is not desirable, please use Algo::StableSort(MyArray) directly instead.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        void StableSort()
        {
            Algo::StableSort(*this, TDereferenceWrapper<ElementType, TLess<>>(TLess<>()));
        }

        /**
         * Stable sorts the array using user defined predicate class.
         *
         * Stable sort is slower than non-stable algorithm.
         *
         * @param Predicate Predicate class instance
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during sorting.
         *        Therefore, your predicate will be passed references rather than pointers.
         *        If this is not desirable, please use Algo::StableSort(MyArray, Predicate) directly instead.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        template <class PREDICATE_CLASS>
        void StableSort(const PREDICATE_CLASS& Predicate)
        {
            TDereferenceWrapper<ElementType, PREDICATE_CLASS> PredicateWrapper(Predicate);
            Algo::StableSort(*this, PredicateWrapper);
        }

        // ====================================================================
        // Heap Operations
        // ====================================================================

        /**
         * Builds an implicit heap from the array.
         *
         * @param Predicate Predicate class instance.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your predicate will be passed references rather than pointers.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        template <class PREDICATE_CLASS>
        OLO_FINLINE void Heapify(const PREDICATE_CLASS& Predicate)
        {
            TDereferenceWrapper<ElementType, PREDICATE_CLASS> PredicateWrapper(Predicate);
            Algo::Heapify(*this, PredicateWrapper);
        }

        /**
         * Builds an implicit heap from the array. Assumes < operator is defined
         * for the template type.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your array will be heapified by the values being pointed to, rather than the pointers' values.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        void Heapify()
        {
            Heapify(TLess<ElementType>());
        }

        /**
         * Adds a new element to the heap.
         *
         * @param InItem Item to be added.
         * @param Predicate Predicate class instance.
         * @return The index of the new element.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your predicate will be passed references rather than pointers.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        template <class PREDICATE_CLASS>
        SizeType HeapPush(ElementType&& InItem, const PREDICATE_CLASS& Predicate)
        {
            // Add at the end, then sift up
            Add(std::move(InItem));
            TDereferenceWrapper<ElementType, PREDICATE_CLASS> PredicateWrapper(Predicate);
            SizeType Result = HeapSiftUp(GetData(), static_cast<SizeType>(0), Num() - 1, FIdentityFunctor(), PredicateWrapper);

            return Result;
        }

        /**
         * Adds a new element to the heap.
         *
         * @param InItem Item to be added.
         * @param Predicate Predicate class instance.
         * @return The index of the new element.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your predicate will be passed references rather than pointers.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        template <class PREDICATE_CLASS>
        SizeType HeapPush(const ElementType& InItem, const PREDICATE_CLASS& Predicate)
        {
            // Add at the end, then sift up
            Add(InItem);
            TDereferenceWrapper<ElementType, PREDICATE_CLASS> PredicateWrapper(Predicate);
            SizeType Result = HeapSiftUp(GetData(), static_cast<SizeType>(0), Num() - 1, FIdentityFunctor(), PredicateWrapper);

            return Result;
        }

        /**
         * Adds a new element to the heap. Assumes < operator is defined for the
         * template type.
         *
         * @param InItem Item to be added.
         * @return The index of the new element.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your array will be heapified by the values being pointed to, rather than the pointers' values.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        SizeType HeapPush(ElementType&& InItem)
        {
            return HeapPush(std::move(InItem), TLess<ElementType>());
        }

        /**
         * Adds a new element to the heap. Assumes < operator is defined for the
         * template type.
         *
         * @param InItem Item to be added.
         * @return The index of the new element.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your array will be heapified by the values being pointed to, rather than the pointers' values.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        SizeType HeapPush(const ElementType& InItem)
        {
            return HeapPush(InItem, TLess<ElementType>());
        }

        /**
         * Removes the top element from the heap.
         *
         * @param OutItem The removed item.
         * @param Predicate Predicate class instance.
         * @param AllowShrinking By default, arrays with large amounts of slack will automatically shrink.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your predicate will be passed references rather than pointers.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        template <class PREDICATE_CLASS>
        void HeapPop(ElementType& OutItem, const PREDICATE_CLASS& Predicate, EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            OutItem = std::move((*this)[0]);
            RemoveAtSwap(0, 1, AllowShrinking);

            TDereferenceWrapper<ElementType, PREDICATE_CLASS> PredicateWrapper(Predicate);
            HeapSiftDown(GetData(), static_cast<SizeType>(0), Num(), FIdentityFunctor(), PredicateWrapper);
        }

        /**
         * Removes the top element from the heap. Assumes < operator is defined for
         * the template type.
         *
         * @param OutItem The removed item.
         * @param AllowShrinking By default, arrays with large amounts of slack will automatically shrink.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your array will be heapified by the values being pointed to, rather than the pointers' values.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        void HeapPop(ElementType& OutItem, EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            HeapPop(OutItem, TLess<ElementType>(), AllowShrinking);
        }

        /**
         * Removes the top element from the heap.
         *
         * @param Predicate Predicate class instance.
         * @param AllowShrinking By default, arrays with large amounts of slack will automatically shrink.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your predicate will be passed references rather than pointers.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        template <class PREDICATE_CLASS>
        void HeapPopDiscard(const PREDICATE_CLASS& Predicate, EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            RemoveAtSwap(0, 1, AllowShrinking);
            TDereferenceWrapper<ElementType, PREDICATE_CLASS> PredicateWrapper(Predicate);
            HeapSiftDown(GetData(), static_cast<SizeType>(0), Num(), FIdentityFunctor(), PredicateWrapper);
        }

        /**
         * Removes the top element from the heap. Assumes < operator is defined for the template type.
         *
         * @param AllowShrinking By default, arrays with large amounts of slack will automatically shrink.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your array will be heapified by the values being pointed to, rather than the pointers' values.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        void HeapPopDiscard(EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            HeapPopDiscard(TLess<ElementType>(), AllowShrinking);
        }

        /**
         * Returns the top element of the heap without removing it.
         * @returns Reference to the top element
         */
        [[nodiscard]] const ElementType& HeapTop() const
        {
            OLO_CORE_ASSERT(m_ArrayNum > 0, "HeapTop: Array is empty");
            return GetData()[0];
        }

        [[nodiscard]] ElementType& HeapTop()
        {
            OLO_CORE_ASSERT(m_ArrayNum > 0, "HeapTop: Array is empty");
            return GetData()[0];
        }

        /**
         * Verifies the heap.
         *
         * @param Predicate Predicate class instance.
         */
        template <class PREDICATE_CLASS>
        void VerifyHeap(const PREDICATE_CLASS& Predicate)
        {
            OLO_CORE_ASSERT(Algo::IsHeap(*this, Predicate), "VerifyHeap: Heap property violated");
        }

        /**
         * Removes an element from the heap.
         *
         * @param Index Position at which to remove item.
         * @param Predicate Predicate class instance.
         * @param AllowShrinking By default, arrays with large amounts of slack will automatically shrink.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your predicate will be passed references rather than pointers.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        template <class PREDICATE_CLASS>
        void HeapRemoveAt(SizeType Index, const PREDICATE_CLASS& Predicate, EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            RemoveAtSwap(Index, 1, AllowShrinking);

            TDereferenceWrapper<ElementType, PREDICATE_CLASS> PredicateWrapper(Predicate);
            HeapSiftDown(GetData(), Index, Num(), FIdentityFunctor(), PredicateWrapper);
            
            // Only sift up if the array is not empty
            if (Num() > 0)
            {
                HeapSiftUp(GetData(), static_cast<SizeType>(0), std::min(Index, Num() - 1), FIdentityFunctor(), PredicateWrapper);
            }
        }

        /**
         * Removes an element from the heap. Assumes < operator is defined for the template type.
         *
         * @param Index Position at which to remove item.
         * @param AllowShrinking By default, arrays with large amounts of slack will automatically shrink.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your array will be heapified by the values being pointed to, rather than the pointers' values.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        void HeapRemoveAt(SizeType Index, EAllowShrinking AllowShrinking = Private::AllowShrinkingByDefault<AllocatorType>())
        {
            HeapRemoveAt(Index, TLess<ElementType>(), AllowShrinking);
        }

        /**
         * Performs heap sort on the array.
         *
         * @param Predicate Predicate class instance.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your predicate will be passed references rather than pointers.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        template <class PREDICATE_CLASS>
        void HeapSort(const PREDICATE_CLASS& Predicate)
        {
            TDereferenceWrapper<ElementType, PREDICATE_CLASS> PredicateWrapper(Predicate);
            Algo::HeapSort(*this, PredicateWrapper);
        }

        /**
         * Performs heap sort on the array. Assumes < operator is defined for the
         * template type.
         *
         * @note: If your array contains raw pointers, they will be automatically dereferenced during heapification.
         *        Therefore, your array will be heapified by the values being pointed to, rather than the pointers' values.
         *        The auto-dereferencing behavior does not occur with smart pointers.
         */
        void HeapSort()
        {
            HeapSort(TLess<ElementType>());
        }

        /**
         * Check if the array satisfies the heap property using a custom predicate.
         * @param Predicate Binary predicate for heap ordering
         * @returns true if the array is a valid heap
         */
        template <typename Predicate>
        [[nodiscard]] bool IsHeap(Predicate Pred) const
        {
            return Algo::IsHeap(*this, Pred);
        }

        /**
         * Check if the array satisfies the heap property.
         * @returns true if the array is a valid heap
         */
        [[nodiscard]] bool IsHeap() const
        {
            return IsHeap(TLess<ElementType>());
        }

        // ====================================================================
        // Swap
        // ====================================================================

        /**
         * Element-wise array element swap. No bounds checking.
         *
         * @param FirstIndexToSwap Position of the first element to swap.
         * @param SecondIndexToSwap Position of the second element to swap.
         */
        OLO_FINLINE void SwapMemory(SizeType FirstIndexToSwap, SizeType SecondIndexToSwap)
        {
            ::Swap(
                *reinterpret_cast<ElementType*>(reinterpret_cast<u8*>(m_AllocatorInstance.GetAllocation()) + (sizeof(ElementType) * FirstIndexToSwap)),
                *reinterpret_cast<ElementType*>(reinterpret_cast<u8*>(m_AllocatorInstance.GetAllocation()) + (sizeof(ElementType) * SecondIndexToSwap))
            );
        }

        /**
         * Element-wise array element swap.
         * This version is doing more sanity checks than SwapMemory.
         *
         * @param FirstIndexToSwap Position of the first element to swap.
         * @param SecondIndexToSwap Position of the second element to swap.
         */
        OLO_FINLINE void Swap(SizeType FirstIndexToSwap, SizeType SecondIndexToSwap)
        {
            OLO_CORE_ASSERT((FirstIndexToSwap >= 0) && (SecondIndexToSwap >= 0), "Swap: indices must be non-negative");
            OLO_CORE_ASSERT((m_ArrayNum > FirstIndexToSwap) && (m_ArrayNum > SecondIndexToSwap), "Swap: indices out of bounds");
            if (FirstIndexToSwap != SecondIndexToSwap)
            {
                SwapMemory(FirstIndexToSwap, SecondIndexToSwap);
            }
        }

    private:
        // ====================================================================
        // Heap Internal Helpers
        // ====================================================================

        /** Gets the index of the left child of node at Index */
        [[nodiscard]] static constexpr SizeType HeapGetLeftChildIndex(SizeType Index)
        {
            return Index * 2 + 1;
        }

        /** Checks if node located at Index is a leaf */
        [[nodiscard]] static constexpr bool HeapIsLeaf(SizeType Index, SizeType Count)
        {
            return HeapGetLeftChildIndex(Index) >= Count;
        }

        /** Gets the parent index for node at Index */
        [[nodiscard]] static constexpr SizeType HeapGetParentIndex(SizeType Index)
        {
            return (Index - 1) / 2;
        }

        /** Sift down to restore heap property (member function version) */
        template <typename Predicate>
        void HeapSiftDown(SizeType Index, SizeType Count, Predicate Pred)
        {
            HeapSiftDownInternal(GetData(), Index, Count, Pred);
        }

        /** Sift down to restore heap property (static internal version) */
        template <typename Predicate>
        static void HeapSiftDownInternal(ElementType* Data, SizeType Index, SizeType Count, Predicate Pred)
        {
            while (!HeapIsLeaf(Index, Count))
            {
                const SizeType LeftChildIndex = HeapGetLeftChildIndex(Index);
                const SizeType RightChildIndex = LeftChildIndex + 1;

                SizeType MinChildIndex = LeftChildIndex;
                if (RightChildIndex < Count)
                {
                    if (Pred(Data[RightChildIndex], Data[LeftChildIndex]))
                    {
                        MinChildIndex = RightChildIndex;
                    }
                }

                if (!Pred(Data[MinChildIndex], Data[Index]))
                {
                    break;
                }

                std::swap(Data[Index], Data[MinChildIndex]);
                Index = MinChildIndex;
            }
        }

        /** Sift up to restore heap property, returns new index */
        template <typename Predicate>
        SizeType HeapSiftUp(SizeType RootIndex, SizeType NodeIndex, Predicate Pred)
        {
            ElementType* Data = GetData();
            while (NodeIndex > RootIndex)
            {
                const SizeType ParentIndex = HeapGetParentIndex(NodeIndex);
                if (!Pred(Data[NodeIndex], Data[ParentIndex]))
                {
                    break;
                }

                std::swap(Data[NodeIndex], Data[ParentIndex]);
                NodeIndex = ParentIndex;
            }
            return NodeIndex;
        }

        // ====================================================================
        // Internal Helpers
        // ====================================================================

        /**
         * Notify slack tracking system that ArrayNum has changed.
         * Should be called whenever m_ArrayNum is modified outside of
         * the ReallocGrow/ReallocShrink paths.
         */
        OLO_FINLINE void SlackTrackerNumChanged()
        {
#if OLO_ENABLE_ARRAY_SLACK_TRACKING
            if constexpr (TAllocatorTraits<AllocatorType>::SupportsSlackTracking)
            {
                m_AllocatorInstance.SlackTrackerLogNum(m_ArrayNum);
            }
#endif
        }

        /** Resize allocation to fit at least NewMax elements */
        void ResizeAllocation(SizeType NewMax)
        {
            if (NewMax != m_ArrayMax)
            {
                m_AllocatorInstance.ResizeAllocation(m_ArrayNum, NewMax, sizeof(ElementType));
                m_ArrayMax = NewMax;
            }
        }

        /** Resize allocation for growth */
        void ResizeGrow(SizeType NewNum)
        {
            const SizeType NewMax = m_AllocatorInstance.CalculateSlackGrow(NewNum, m_ArrayMax, sizeof(ElementType));
            m_AllocatorInstance.ResizeAllocation(m_ArrayNum, NewMax, sizeof(ElementType));
            m_ArrayMax = NewMax;
        }

        /** Resize allocation for shrinking */
        void ResizeShrink()
        {
            const SizeType NewMax = m_AllocatorInstance.CalculateSlackShrink(m_ArrayNum, m_ArrayMax, sizeof(ElementType));
            if (NewMax != m_ArrayMax)
            {
                m_AllocatorInstance.ResizeAllocation(m_ArrayNum, NewMax, sizeof(ElementType));
                m_ArrayMax = NewMax;
            }
        }

        /** Copy from raw pointer to empty array */
        void CopyToEmpty(const ElementType* Source, SizeType Count, SizeType ExtraSlack)
        {
            OLO_CORE_ASSERT(Count >= 0, "CopyToEmpty: Count must be non-negative");
            m_ArrayNum = 0;

            if (Count > 0 || ExtraSlack > 0)
            {
                const SizeType NewMax = Count + ExtraSlack;
                ResizeAllocation(m_AllocatorInstance.CalculateSlackReserve(NewMax, sizeof(ElementType)));
                ConstructItems<ElementType>(GetData(), Source, Count);
                m_ArrayNum = Count;
            }

            SlackTrackerNumChanged();
        }

        /**
         * Copy from raw pointer to empty array with extra slack.
         *
         * @param OtherData Pointer to source data.
         * @param OtherNum Number of elements to copy.
         * @param PrevMax Previous allocation max (used for efficient reallocation).
         * @param ExtraSlack Additional memory to allocate at the end.
         */
        template <typename OtherElementType, typename OtherSizeType>
        void CopyToEmptyWithSlack(const OtherElementType* OtherData, OtherSizeType OtherNum, SizeType PrevMax, SizeType ExtraSlack)
        {
            SizeType NewNum = static_cast<SizeType>(OtherNum);
            OLO_CORE_ASSERT(static_cast<OtherSizeType>(NewNum) == OtherNum, "CopyToEmptyWithSlack: Invalid number of elements");

            m_ArrayNum = NewNum;
            if (OtherNum || ExtraSlack || PrevMax)
            {
                USizeType NewMax = static_cast<USizeType>(NewNum) + static_cast<USizeType>(ExtraSlack);

                // This should only happen when we've underflowed or overflowed SizeType
                if (static_cast<SizeType>(NewMax) < NewNum)
                {
                    Private::OnInvalidArrayNum(static_cast<unsigned long long>(NewMax));
                }

                ResizeAllocation(m_AllocatorInstance.CalculateSlackReserve(NewNum + ExtraSlack, sizeof(ElementType)));
                ConstructItems<ElementType>(GetData(), OtherData, OtherNum);
            }
            else
            {
                m_ArrayMax = m_AllocatorInstance.GetInitialCapacity();
            }

            SlackTrackerNumChanged();
        }

        /** Move or copy helper */
        template <typename ArrayType>
        static void MoveOrCopy(ArrayType& ToArray, ArrayType& FromArray)
        {
            // Move the allocator state
            ToArray.m_AllocatorInstance.MoveToEmpty(FromArray.m_AllocatorInstance);

            ToArray.m_ArrayNum = FromArray.m_ArrayNum;
            ToArray.m_ArrayMax = FromArray.m_ArrayMax;
            FromArray.m_ArrayNum = 0;
            FromArray.m_ArrayMax = 0;

            // Notify slack tracking for both arrays
            FromArray.SlackTrackerNumChanged();
            ToArray.SlackTrackerNumChanged();
        }

        /**
         * Move or copy with extra slack helper.
         *
         * @param ToArray Array to move into.
         * @param FromArray Array to move from.
         * @param PrevMax The previous allocated size.
         * @param ExtraSlack Tells how much extra memory should be preallocated
         *                   at the end of the array in the number of elements.
         */
        template <typename FromArrayType, typename ToArrayType>
        static OLO_FINLINE void MoveOrCopyWithSlack(ToArrayType& ToArray, FromArrayType& FromArray, SizeType PrevMax, SizeType ExtraSlack)
        {
            if constexpr (Private::CanMoveTArrayPointersBetweenArrayTypes<FromArrayType, ToArrayType>())
            {
                // Move
                MoveOrCopy(ToArray, FromArray);

                USizeType LocalArrayNum = static_cast<USizeType>(ToArray.m_ArrayNum);
                USizeType NewMax        = static_cast<USizeType>(LocalArrayNum) + static_cast<USizeType>(ExtraSlack);

                // This should only happen when we've underflowed or overflowed SizeType
                if (static_cast<SizeType>(NewMax) < static_cast<SizeType>(LocalArrayNum))
                {
                    Private::OnInvalidArrayNum(static_cast<unsigned long long>(ExtraSlack));
                }

                ToArray.Reserve(NewMax);
            }
            else
            {
                // Copy
                ToArray.CopyToEmptyWithSlack(FromArray.GetData(), FromArray.Num(), PrevMax, ExtraSlack);
            }
        }
    };

    // ============================================================================
    // Type Aliases
    // ============================================================================

    /** Array with inline storage for small sizes */
    template <typename ElementType, u32 NumInlineElements>
    using TInlineArray = TArray<ElementType, TInlineAllocator<NumInlineElements>>;

    // ============================================================================
    // TArray Type Traits
    // ============================================================================

    /** TArray is a contiguous container */
    template <typename T, typename AllocatorType>
    struct TIsContiguousContainer<TArray<T, AllocatorType>>
    {
        static constexpr bool Value = true;
    };

    /**
     * @brief TArray can be zero-constructed
     *
     * An empty TArray (ArrayNum=0, ArrayMax=0, no allocation) is a valid state,
     * and zero-initialization produces this state for most allocators.
     */
    template <typename T, typename AllocatorType>
    struct TIsZeroConstructType<TArray<T, AllocatorType>>
    {
        static constexpr bool Value = true;
        enum { value = true };
    };

    /**
     * @brief Type trait to detect TArray types
     * 
     * Use TIsTArray<T>::Value or TIsTArray_V<T> to check if T is a TArray.
     */
    template <typename T> 
    inline constexpr bool TIsTArray_V = false;

    template <typename InElementType, typename InAllocatorType> 
    inline constexpr bool TIsTArray_V<               TArray<InElementType, InAllocatorType>> = true;
    template <typename InElementType, typename InAllocatorType> 
    inline constexpr bool TIsTArray_V<const          TArray<InElementType, InAllocatorType>> = true;
    template <typename InElementType, typename InAllocatorType> 
    inline constexpr bool TIsTArray_V<      volatile TArray<InElementType, InAllocatorType>> = true;
    template <typename InElementType, typename InAllocatorType> 
    inline constexpr bool TIsTArray_V<const volatile TArray<InElementType, InAllocatorType>> = true;

    template <typename T>
    struct TIsTArray
    {
        static constexpr bool Value = TIsTArray_V<T>;
    };

    // ============================================================================
    // TArray Serialization
    // ============================================================================

    /**
     * @brief Serializes a TArray to/from an archive
     * 
     * Uses bulk serialization for types that support it (TCanBulkSerialize),
     * otherwise serializes each element individually.
     *
     * @param Ar The archive to serialize to/from
     * @param A The array to serialize
     * @return Reference to the archive
     */
    template<typename ElementType, typename AllocatorType>
    FArchive& operator<<(FArchive& Ar, TArray<ElementType, AllocatorType>& A)
    {
        using SizeType = typename TArray<ElementType, AllocatorType>::SizeType;

        // Serialize the number of elements
        SizeType SerializeNum = A.Num();
        Ar << SerializeNum;

        if (SerializeNum == 0)
        {
            // if we are loading, then we have to reset the size to 0, in case it isn't currently 0
            if (Ar.IsLoading())
            {
                A.Empty();
            }
            return Ar;
        }

        if (Ar.IsError() || SerializeNum < 0)
        {
            Ar.SetError();
            return Ar;
        }

        // if we don't need to perform per-item serialization, just read it in bulk
        if constexpr (sizeof(ElementType) == 1 || TCanBulkSerialize_V<ElementType>)
        {
            // Serialize simple bytes which require no construction or destruction.
            if ((SerializeNum || A.Max()) && Ar.IsLoading())
            {
                A.Empty(SerializeNum);
                A.AddUninitialized(SerializeNum);
            }

            Ar.Serialize(A.GetData(), A.Num() * sizeof(ElementType));
        }
        else if (Ar.IsLoading())
        {
            // Required for resetting ArrayNum
            A.Empty(SerializeNum);

            for (SizeType i = 0; i < SerializeNum; i++)
            {
                Ar << A.AddDefaulted_GetRef();
            }
        }
        else
        {
            for (SizeType i = 0; i < A.Num(); i++)
            {
                Ar << A[i];
            }
        }

        return Ar;
    }

    // ============================================================================
    // TArray Hash Function
    // ============================================================================

    /** Returns a unique hash by combining those of each array element. */
    template<typename InElementType, typename InAllocatorType>
    [[nodiscard]] u32 GetTypeHash(const TArray<InElementType, InAllocatorType>& A)
    {
        u32 Hash = 0;
        for (const InElementType& V : A)
        {
            Hash = HashCombineFast(Hash, GetTypeHash(V));
        }
        return Hash;
    }

} // namespace OloEngine

// ============================================================================
// Placement new for TArray
// ============================================================================

template <typename T, typename AllocatorType>
inline void* operator new(size_t Size, OloEngine::TArray<T, AllocatorType>& Array)
{
    static_assert(sizeof(T) == Size, "Size mismatch in TArray placement new");
    const auto Index = Array.AddUninitialized(1);
    return Array.GetData() + Index;
}

template <typename T, typename AllocatorType>
inline void* operator new(size_t Size, OloEngine::TArray<T, AllocatorType>& Array, typename OloEngine::TArray<T, AllocatorType>::SizeType Index)
{
    static_assert(sizeof(T) == Size, "Size mismatch in TArray placement new");
    Array.InsertUninitialized(Index, 1);
    return Array.GetData() + Index;
}
