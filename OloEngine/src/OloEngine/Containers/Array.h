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
#include "OloEngine/Memory/MemoryOps.h"
#include "OloEngine/Misc/IntrusiveUnsetOptionalState.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Templates/TypeHash.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <type_traits>

namespace OloEngine
{
    // Forward declaration
    template <typename T, typename AllocatorType>
    class TArray;

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

    } // namespace Private

    // ============================================================================
    // EAllowShrinking
    // ============================================================================

    /**
     * @enum EAllowShrinking
     * @brief Controls whether operations are allowed to shrink the array allocation
     */
    enum class EAllowShrinking
    {
        No,
        Yes
    };

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

        /** Copy constructor */
        TArray(const TArray& Other)
        {
            CopyToEmpty(Other.GetData(), Other.Num(), 0);
        }

        /**
         * @brief Copy constructor with extra slack
         * @param Other      Array to copy from
         * @param ExtraSlack Additional capacity to reserve beyond copied elements
         */
        TArray(const TArray& Other, SizeType ExtraSlack)
        {
            CopyToEmpty(Other.GetData(), Other.Num(), ExtraSlack);
        }

        /** Move constructor */
        TArray(TArray&& Other) noexcept
        {
            MoveOrCopy(*this, Other);
        }

        /**
         * @brief Move constructor with extra slack
         * @param Other      Array to move from  
         * @param ExtraSlack Additional capacity to reserve
         * 
         * If ExtraSlack is 0, this is equivalent to a normal move.
         * Otherwise, the array is copied and extra space is reserved.
         */
        TArray(TArray&& Other, SizeType ExtraSlack)
        {
            if (ExtraSlack == 0)
            {
                MoveOrCopy(*this, Other);
            }
            else
            {
                CopyToEmpty(Other.GetData(), Other.Num(), ExtraSlack);
                // Clear the source array
                Other.Empty();
            }
        }

        /** 
         * @brief Construct from an array view
         * @param Other The array view to copy from
         */
        template <typename OtherElementType, typename OtherSizeType>
            requires (std::is_convertible_v<OtherElementType*, ElementType*>)
        explicit TArray(TArrayView<OtherElementType, OtherSizeType> Other)
        {
            CopyToEmpty(Other.GetData(), static_cast<SizeType>(Other.Num()), 0);
        }

        /**
         * @brief Constructor from raw pointer and count
         * @param Ptr    Pointer to array of elements to copy
         * @param Count  Number of elements to copy
         */
        TArray(const ElementType* Ptr, SizeType Count)
        {
            OLO_CORE_ASSERT(Ptr != nullptr || Count == 0, "TArray: Null pointer with non-zero count");
            CopyToEmpty(Ptr, Count, 0);
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
            Reserve(static_cast<SizeType>(InitList.size()));
            for (const auto& Item : InitList)
            {
                Add(Item);
            }
            return *this;
        }

        // ====================================================================
        // Intrusive TOptional<TArray> State
        // ====================================================================

        /**
         * @brief Enable intrusive TOptional optimization
         * 
         * When TOptional<TArray> is used, this allows TOptional to use the
         * array's own state (ArrayMax == -1) to represent the "unset" state,
         * eliminating the need for a separate bool and saving memory.
         */
        static constexpr bool bHasIntrusiveUnsetOptionalState = true;
        using IntrusiveUnsetOptionalStateType = TArray;

        /**
         * @brief Constructor for intrusive unset optional state
         * @param Tag  Tag type to distinguish this constructor
         * 
         * Only callable by TOptional. Creates an array in the "unset" state
         * using ArrayMax == -1 as a sentinel value.
         */
        [[nodiscard]] explicit TArray(FIntrusiveUnsetOptionalState Tag)
            : m_ArrayNum(0)
            , m_ArrayMax(-1)
        {
            // Use ArrayMax == -1 as our intrusive state.
            // The destructor still works without change, as it doesn't use ArrayMax.
        }

        /**
         * @brief Check if array is in intrusive unset optional state
         * @param Tag  Tag type for comparison
         * @returns true if array is in the unset state (ArrayMax == -1)
         * 
         * Only used by TOptional to check if the optional is set.
         */
        [[nodiscard]] bool operator==(FIntrusiveUnsetOptionalState Tag) const
        {
            return m_ArrayMax == -1;
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

        /** Get last element */
        [[nodiscard]] OLO_FINLINE ElementType& Last()
        {
            OLO_CORE_ASSERT(m_ArrayNum > 0, "TArray::Last called on empty array");
            return GetData()[m_ArrayNum - 1];
        }

        /** Get last element (const) */
        [[nodiscard]] OLO_FINLINE const ElementType& Last() const
        {
            OLO_CORE_ASSERT(m_ArrayNum > 0, "TArray::Last called on empty array");
            return GetData()[m_ArrayNum - 1];
        }

        /**
         * @brief Get element at index from end
         * @param IndexFromTheEnd  Index from end (0 = last element, 1 = second-to-last, etc.)
         * @returns Reference to the element
         */
        [[nodiscard]] OLO_FINLINE ElementType& Last(SizeType IndexFromTheEnd)
        {
            RangeCheck(m_ArrayNum - IndexFromTheEnd - 1);
            return GetData()[m_ArrayNum - IndexFromTheEnd - 1];
        }

        /**
         * @brief Get element at index from end (const)
         * @param IndexFromTheEnd  Index from end (0 = last element, 1 = second-to-last, etc.)
         * @returns Const reference to the element
         */
        [[nodiscard]] OLO_FINLINE const ElementType& Last(SizeType IndexFromTheEnd) const
        {
            RangeCheck(m_ArrayNum - IndexFromTheEnd - 1);
            return GetData()[m_ArrayNum - IndexFromTheEnd - 1];
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

        /** Get allocated capacity */
        [[nodiscard]] OLO_FINLINE SizeType Max() const
        {
            return m_ArrayMax;
        }

        /**
         * @brief Get the size of each element in bytes
         * @returns sizeof(ElementType)
         */
        [[nodiscard]] static constexpr sizet GetTypeSize()
        {
            return sizeof(ElementType);
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
         * @brief Get the total size in bytes of all elements
         * @returns Num() * sizeof(ElementType)
         */
        [[nodiscard]] OLO_FINLINE sizet NumBytes() const
        {
            return static_cast<sizet>(m_ArrayNum) * sizeof(ElementType);
        }

        /**
         * @brief Debug helper to check that a pointer is within the array bounds
         * @param Addr  Address to check
         * 
         * In debug builds, asserts if Addr points within the current allocation.
         * This catches cases where you're about to Add() an element that's already
         * in the array, which could be invalidated by reallocation.
         */
        OLO_FINLINE void CheckAddress(const ElementType* Addr) const
        {
            OLO_CORE_ASSERT(Addr < GetData() || Addr >= (GetData() + m_ArrayMax),
                "Passed address is from within the array - this will be invalidated if the array reallocates");
        }

        /**
         * @brief Checks if index is in range [0, ArrayNum)
         * @param Index  Index to check
         * 
         * Asserts in debug builds if index is out of range.
         */
        OLO_FINLINE void RangeCheck(SizeType Index) const
        {
            CheckInvariants();
            OLO_CORE_ASSERT((Index >= 0) && (Index < m_ArrayNum),
                "Array index out of bounds: %d from an array of size %d", Index, m_ArrayNum);
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
        // Adding Elements
        // ====================================================================

        /** Add element by copy, return index */
        SizeType Add(const ElementType& Item)
        {
            const SizeType Index = AddUninitialized(1);
            ::new (GetData() + Index) ElementType(Item);
            return Index;
        }

        /** Add element by move, return index */
        SizeType Add(ElementType&& Item)
        {
            const SizeType Index = AddUninitialized(1);
            ::new (GetData() + Index) ElementType(std::move(Item));
            return Index;
        }

        /** Add default-constructed element, return reference */
        ElementType& AddDefaulted()
        {
            const SizeType Index = AddUninitialized(1);
            ElementType* Ptr = GetData() + Index;
            ::new (Ptr) ElementType();
            return *Ptr;
        }

        /** Add N default-constructed elements, return index of first */
        SizeType AddDefaulted(SizeType Count)
        {
            const SizeType Index = AddUninitialized(Count);
            DefaultConstructItems<ElementType>(GetData() + Index, Count);
            return Index;
        }

        /** Add zero-initialized element, return reference */
        ElementType& AddZeroed()
        {
            const SizeType Index = AddUninitialized(1);
            ElementType* Ptr = GetData() + Index;
            FMemory::Memzero(Ptr, sizeof(ElementType));
            return *Ptr;
        }

        /** Add N zero-initialized elements, return index of first */
        SizeType AddZeroed(SizeType Count)
        {
            const SizeType Index = AddUninitialized(Count);
            FMemory::Memzero(GetData() + Index, Count * sizeof(ElementType));
            return Index;
        }

        /** Add uninitialized space for N elements, return index of first */
        SizeType AddUninitialized(SizeType Count = 1)
        {
            OLO_CORE_ASSERT(Count >= 0, "AddUninitialized: Count must be non-negative");

            const SizeType OldNum = m_ArrayNum;
            const SizeType NewNum = OldNum + Count;

            if (NewNum > m_ArrayMax)
            {
                ResizeGrow(NewNum);
            }
            m_ArrayNum = NewNum;

            return OldNum;
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

        /** Emplace element in-place, return reference */
        template <typename... ArgsType>
        ElementType& Emplace(ArgsType&&... Args)
        {
            const SizeType Index = AddUninitialized(1);
            ElementType* Ptr = GetData() + Index;
            ::new (Ptr) ElementType(std::forward<ArgsType>(Args)...);
            return *Ptr;
        }

        /**
         * @brief Add element by copy and return reference to it
         * @param Item  Element to add
         * @returns Reference to the newly added element
         */
        ElementType& Add_GetRef(const ElementType& Item)
        {
            const SizeType Index = AddUninitialized(1);
            ElementType* Ptr = GetData() + Index;
            ::new (Ptr) ElementType(Item);
            return *Ptr;
        }

        /**
         * @brief Add element by move and return reference to it
         * @param Item  Element to add
         * @returns Reference to the newly added element
         */
        ElementType& Add_GetRef(ElementType&& Item)
        {
            const SizeType Index = AddUninitialized(1);
            ElementType* Ptr = GetData() + Index;
            ::new (Ptr) ElementType(std::move(Item));
            return *Ptr;
        }

        /**
         * @brief Add default-constructed element and return reference
         * @returns Reference to the newly added element
         */
        ElementType& AddDefaulted_GetRef()
        {
            const SizeType Index = AddUninitialized(1);
            ElementType* Ptr = GetData() + Index;
            ::new (Ptr) ElementType();
            return *Ptr;
        }

        /**
         * @brief Add zero-initialized element and return reference
         * @returns Reference to the newly added element
         */
        ElementType& AddZeroed_GetRef()
        {
            const SizeType Index = AddUninitialized(1);
            ElementType* Ptr = GetData() + Index;
            FMemory::Memzero(Ptr, sizeof(ElementType));
            return *Ptr;
        }

        /**
         * @brief Add uninitialized element and return reference
         * @returns Reference to the newly added (uninitialized) element
         * 
         * @warning The returned reference points to uninitialized memory.
         *          You must construct an object at this location before use.
         */
        ElementType& AddUninitialized_GetRef()
        {
            const SizeType Index = AddUninitialized(1);
            return GetData()[Index];
        }

        /**
         * @brief Emplace element in-place and return reference
         * @param Args  Arguments forwarded to element constructor
         * @returns Reference to the newly constructed element
         */
        template <typename... ArgsType>
        ElementType& Emplace_GetRef(ArgsType&&... Args)
        {
            const SizeType Index = AddUninitialized(1);
            ElementType* Ptr = GetData() + Index;
            ::new (Ptr) ElementType(std::forward<ArgsType>(Args)...);
            return *Ptr;
        }

        /**
         * @brief Initialize array with Count copies of Element
         * @param Element  The element to copy
         * @param Count    Number of copies to add
         * 
         * Clears the array and fills it with Count copies of Element.
         */
        void Init(const ElementType& Element, SizeType Count)
        {
            Empty(Count);
            for (SizeType i = 0; i < Count; ++i)
            {
                Add(Element);
            }
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

        /** Insert element at index */
        void Insert(const ElementType& Item, SizeType Index)
        {
            InsertUninitialized(Index, 1);
            ::new (GetData() + Index) ElementType(Item);
        }

        /** Insert element at index by move */
        void Insert(ElementType&& Item, SizeType Index)
        {
            InsertUninitialized(Index, 1);
            ::new (GetData() + Index) ElementType(std::move(Item));
        }

        /**
         * @brief Insert element at index and return reference
         * @param Item  Element to insert (copied)
         * @param Index Position to insert at
         * @returns Reference to the inserted element
         */
        ElementType& Insert_GetRef(const ElementType& Item, SizeType Index)
        {
            InsertUninitialized(Index, 1);
            ElementType* Ptr = GetData() + Index;
            ::new (Ptr) ElementType(Item);
            return *Ptr;
        }

        /**
         * @brief Insert element at index and return reference
         * @param Item  Element to insert (moved)
         * @param Index Position to insert at
         * @returns Reference to the inserted element
         */
        ElementType& Insert_GetRef(ElementType&& Item, SizeType Index)
        {
            InsertUninitialized(Index, 1);
            ElementType* Ptr = GetData() + Index;
            ::new (Ptr) ElementType(std::move(Item));
            return *Ptr;
        }

        /** Insert uninitialized space at index */
        void InsertUninitialized(SizeType Index, SizeType Count = 1)
        {
            OLO_CORE_ASSERT(Index >= 0 && Index <= m_ArrayNum, "InsertUninitialized: Index out of bounds");
            OLO_CORE_ASSERT(Count >= 0, "InsertUninitialized: Count must be non-negative");

            const SizeType OldNum = m_ArrayNum;
            const SizeType NewNum = OldNum + Count;

            if (NewNum > m_ArrayMax)
            {
                ResizeGrow(NewNum);
            }

            // Move existing elements to make room
            ElementType* Data = GetData();
            if (Index < OldNum)
            {
                RelocateConstructItems<ElementType>(Data + Index + Count, Data + Index, OldNum - Index);
            }

            m_ArrayNum = NewNum;
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
         * @brief Insert uninitialized element at index and return reference
         * @param Index  Position to insert at
         * @returns Reference to the inserted (uninitialized) element
         * 
         * @warning The returned reference points to uninitialized memory.
         *          You must construct an object at this location before use.
         */
        ElementType& InsertUninitialized_GetRef(SizeType Index)
        {
            InsertUninitialized(Index, 1);
            return GetData()[Index];
        }

        /**
         * @brief Insert default-constructed element at index and return reference
         * @param Index  Position to insert at
         * @returns Reference to the inserted element
         */
        ElementType& InsertDefaulted_GetRef(SizeType Index)
        {
            InsertUninitialized(Index, 1);
            ElementType* Ptr = GetData() + Index;
            ::new (Ptr) ElementType();
            return *Ptr;
        }

        /**
         * @brief Insert zero-initialized element at index and return reference
         * @param Index  Position to insert at
         * @returns Reference to the inserted element
         */
        ElementType& InsertZeroed_GetRef(SizeType Index)
        {
            InsertUninitialized(Index, 1);
            ElementType* Ptr = GetData() + Index;
            FMemory::Memzero(Ptr, sizeof(ElementType));
            return *Ptr;
        }

        /**
         * @brief Construct element in-place at specific index
         * @param Index  Position to insert at
         * @param Args   Arguments forwarded to element constructor
         * @returns Reference to the newly constructed element
         */
        template <typename... ArgsType>
        ElementType& EmplaceAt(SizeType Index, ArgsType&&... Args)
        {
            InsertUninitialized(Index, 1);
            ElementType* Ptr = GetData() + Index;
            ::new (Ptr) ElementType(std::forward<ArgsType>(Args)...);
            return *Ptr;
        }

        /**
         * @brief Construct element in-place at specific index and return reference
         * @param Index  Position to insert at
         * @param Args   Arguments forwarded to element constructor
         * @returns Reference to the newly constructed element
         */
        template <typename... ArgsType>
        ElementType& EmplaceAt_GetRef(SizeType Index, ArgsType&&... Args)
        {
            return EmplaceAt(Index, std::forward<ArgsType>(Args)...);
        }

        // ====================================================================
        // Removing Elements
        // ====================================================================

        /** Remove element at index */
        void RemoveAt(SizeType Index, SizeType Count = 1, EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
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
                    ResizeShrink();
                }
            }
        }

        /** Remove element at index by swapping with last element (faster, changes order) */
        void RemoveAtSwap(SizeType Index, SizeType Count = 1, EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
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
                    ResizeShrink();
                }
            }
        }

        /** Remove last element */
        void Pop(EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
        {
            OLO_CORE_ASSERT(m_ArrayNum > 0, "Pop: Array is empty");
            RemoveAt(m_ArrayNum - 1, 1, AllowShrinking);
        }

        /** Remove all elements */
        void Empty(SizeType ExpectedNumElements = 0)
        {
            DestructItems(GetData(), m_ArrayNum);
            m_ArrayNum = 0;

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
            ResizeAllocation(NewSize);
        }

        /** Set number of elements (destructs extra or default-constructs new) */
        void SetNum(SizeType NewNum, EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
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
        void SetNumUninitialized(SizeType NewNum, EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
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
        void SetNumZeroed(SizeType NewNum, EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
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
         * @brief Set the number of elements without construction/destruction
         * @param NewNum  New number of elements (must be <= current Num())
         * 
         * This is an unsafe internal method that only updates ArrayNum.
         * Does not destruct items, does not deallocate memory.
         * NewNum must be <= current Num() and >= 0.
         */
        void SetNumUnsafeInternal(SizeType NewNum)
        {
            OLO_CORE_ASSERT(NewNum >= 0 && NewNum <= m_ArrayNum, 
                "SetNumUnsafeInternal: NewNum must be in range [0, %d]", m_ArrayNum);
            m_ArrayNum = NewNum;
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

        /**
         * @brief Removes the first occurrence of the specified item, maintaining order
         * @param Item  The item to remove
         * @returns Number of items removed (0 or 1)
         * @see Remove, RemoveAll, RemoveSingleSwap
         */
        SizeType RemoveSingle(const ElementType& Item)
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
         * @brief Removes all instances of Item, maintaining order
         * @param Item  Item to remove from array
         * @returns Number of removed elements
         * @see RemoveAll, RemoveSingle, RemoveSwap
         */
        SizeType Remove(const ElementType& Item)
        {
            return RemoveAll([&Item](const ElementType& Element) { return Element == Item; });
        }

        /**
         * @brief Remove all instances that match the predicate, maintaining order
         * @param Predicate  Predicate returning true for elements to remove
         * @returns Number of removed elements
         * @see Remove, RemoveAllSwap, RemoveSingle
         */
        template <typename Predicate>
        SizeType RemoveAll(Predicate Pred)
        {
            const SizeType OriginalNum = m_ArrayNum;
            if (OriginalNum == 0)
            {
                return 0;
            }

            ElementType* Data = GetData();
            SizeType WriteIndex = 0;
            SizeType ReadIndex = 0;

            // Optimized removal that works with runs of matches/non-matches
            bool bNotMatch = !Pred(Data[ReadIndex]);
            do
            {
                SizeType RunStartIndex = ReadIndex++;
                while (ReadIndex < OriginalNum && bNotMatch == !Pred(Data[ReadIndex]))
                {
                    ++ReadIndex;
                }
                SizeType RunLength = ReadIndex - RunStartIndex;

                if (bNotMatch)
                {
                    // Non-matching run - relocate it
                    if (WriteIndex != RunStartIndex)
                    {
                        RelocateConstructItems<ElementType>(Data + WriteIndex, Data + RunStartIndex, RunLength);
                    }
                    WriteIndex += RunLength;
                }
                else
                {
                    // Matching run - destruct it
                    DestructItems(Data + RunStartIndex, RunLength);
                }
                bNotMatch = !bNotMatch;
            } while (ReadIndex < OriginalNum);

            m_ArrayNum = WriteIndex;
            return OriginalNum - m_ArrayNum;
        }

        /**
         * @brief Removes the first occurrence using swap (O(1), does not preserve order)
         * @param Item  The item to remove
         * @param AllowShrinking  Whether to allow shrinking the allocation
         * @returns Number of items removed (0 or 1)
         * @see RemoveSingle, RemoveSwap, RemoveAllSwap
         */
        SizeType RemoveSingleSwap(const ElementType& Item, EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
        {
            const SizeType Index = Find(Item);
            if (Index != INDEX_NONE)
            {
                RemoveAtSwap(Index, 1, AllowShrinking);
                return 1;
            }
            return 0;
        }

        /**
         * @brief Removes all instances of Item using swap (O(Count), does not preserve order)
         * @param Item  Item to remove from array
         * @param AllowShrinking  Whether to allow shrinking the allocation
         * @returns Number of removed elements
         * @see Remove, RemoveSingleSwap, RemoveAllSwap
         */
        SizeType RemoveSwap(const ElementType& Item, EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
        {
            const SizeType OriginalNum = m_ArrayNum;
            for (SizeType Index = 0; Index < m_ArrayNum; )
            {
                if (GetData()[Index] == Item)
                {
                    RemoveAtSwap(Index, 1, EAllowShrinking::No);
                }
                else
                {
                    ++Index;
                }
            }
            if (AllowShrinking == EAllowShrinking::Yes && m_ArrayNum < OriginalNum)
            {
                ResizeShrink();
            }
            return OriginalNum - m_ArrayNum;
        }

        /**
         * @brief Remove all instances matching predicate using swap (does not preserve order)
         * @param Predicate  Predicate returning true for elements to remove
         * @param AllowShrinking  Whether to allow shrinking the allocation
         * @returns Number of removed elements
         * @see RemoveAll, RemoveSwap, RemoveSingleSwap
         */
        template <typename Predicate>
        SizeType RemoveAllSwap(Predicate Pred, EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
        {
            const SizeType OriginalNum = m_ArrayNum;
            for (SizeType Index = 0; Index < m_ArrayNum; )
            {
                if (Pred(GetData()[Index]))
                {
                    RemoveAtSwap(Index, 1, EAllowShrinking::No);
                }
                else
                {
                    ++Index;
                }
            }
            if (AllowShrinking == EAllowShrinking::Yes && m_ArrayNum < OriginalNum)
            {
                ResizeShrink();
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

        /** Sort using operator< */
        void Sort()
        {
            std::sort(begin(), end());
        }

        /** Sort using custom predicate */
        template <typename Predicate>
        void Sort(Predicate Pred)
        {
            std::sort(begin(), end(), Pred);
        }

        /** Stable sort using operator< */
        void StableSort()
        {
            std::stable_sort(begin(), end());
        }

        /** Stable sort using custom predicate */
        template <typename Predicate>
        void StableSort(Predicate Pred)
        {
            std::stable_sort(begin(), end(), Pred);
        }

        // ====================================================================
        // Heap Operations
        // ====================================================================

        /**
         * Builds a valid heap from the array using operator<.
         * The predicate defines a min-heap: smaller elements are parents of larger ones.
         */
        void Heapify()
        {
            Heapify(TLess<ElementType>());
        }

        /**
         * Builds a valid heap from the array using a custom predicate.
         * @param Predicate Binary predicate - returns true if first argument should precede second
         */
        template <typename Predicate>
        void Heapify(Predicate Pred)
        {
            if (m_ArrayNum <= 1)
            {
                return;
            }

            ElementType* Data = GetData();
            SizeType Index = HeapGetParentIndex(m_ArrayNum - 1);
            for (;;)
            {
                HeapSiftDown(Index, m_ArrayNum, Pred);
                if (Index == 0)
                {
                    return;
                }
                --Index;
            }
        }

        /**
         * Adds a new element to the heap.
         * @param Item The element to add
         * @returns The index of the new element
         */
        SizeType HeapPush(const ElementType& Item)
        {
            return HeapPush(Item, TLess<ElementType>());
        }

        /**
         * Adds a new element to the heap.
         * @param Item The element to add (moved)
         * @returns The index of the new element
         */
        SizeType HeapPush(ElementType&& Item)
        {
            return HeapPush(std::move(Item), TLess<ElementType>());
        }

        /**
         * Adds a new element to the heap using a custom predicate.
         * @param Item The element to add
         * @param Predicate Binary predicate for heap ordering
         * @returns The index of the new element
         */
        template <typename Predicate>
        SizeType HeapPush(const ElementType& Item, Predicate Pred)
        {
            const SizeType Index = Add(Item);
            return HeapSiftUp(0, Index, Pred);
        }

        /**
         * Adds a new element to the heap using a custom predicate.
         * @param Item The element to add (moved)
         * @param Predicate Binary predicate for heap ordering
         * @returns The index of the new element
         */
        template <typename Predicate>
        SizeType HeapPush(ElementType&& Item, Predicate Pred)
        {
            const SizeType Index = Add(std::move(Item));
            return HeapSiftUp(0, Index, Pred);
        }

        /**
         * Removes the top element from the heap.
         * @param OutItem Receives the removed element
         * @param AllowShrinking Whether to allow shrinking the allocation
         */
        void HeapPop(ElementType& OutItem, EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
        {
            HeapPop(OutItem, TLess<ElementType>(), AllowShrinking);
        }

        /**
         * Removes the top element from the heap using a custom predicate.
         * @param OutItem Receives the removed element
         * @param Predicate Binary predicate for heap ordering
         * @param AllowShrinking Whether to allow shrinking the allocation
         */
        template <typename Predicate>
        void HeapPop(ElementType& OutItem, Predicate Pred, EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
        {
            OLO_CORE_ASSERT(m_ArrayNum > 0, "HeapPop: Array is empty");

            OutItem = std::move(GetData()[0]);
            RemoveAtSwap(0, 1, EAllowShrinking::No);

            if (m_ArrayNum > 0)
            {
                HeapSiftDown(0, m_ArrayNum, Pred);
            }

            if (AllowShrinking == EAllowShrinking::Yes)
            {
                ResizeShrink();
            }
        }

        /**
         * Removes the top element from the heap, discarding it.
         * @param AllowShrinking Whether to allow shrinking the allocation
         */
        void HeapPopDiscard(EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
        {
            HeapPopDiscard(TLess<ElementType>(), AllowShrinking);
        }

        /**
         * Removes the top element from the heap using a custom predicate, discarding it.
         * @param Predicate Binary predicate for heap ordering
         * @param AllowShrinking Whether to allow shrinking the allocation
         */
        template <typename Predicate>
        void HeapPopDiscard(Predicate Pred, EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
        {
            OLO_CORE_ASSERT(m_ArrayNum > 0, "HeapPopDiscard: Array is empty");

            RemoveAtSwap(0, 1, EAllowShrinking::No);

            if (m_ArrayNum > 0)
            {
                HeapSiftDown(0, m_ArrayNum, Pred);
            }

            if (AllowShrinking == EAllowShrinking::Yes)
            {
                ResizeShrink();
            }
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
         * Removes an element at a specific index from the heap, maintaining heap property.
         * @param Index The index of the element to remove
         * @param Predicate Binary predicate for heap ordering
         * @param AllowShrinking Whether to allow shrinking the allocation
         */
        template <typename Predicate>
        void HeapRemoveAt(SizeType Index, Predicate Pred, EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
        {
            OLO_CORE_ASSERT(IsValidIndex(Index), "HeapRemoveAt: Invalid index");

            RemoveAtSwap(Index, 1, EAllowShrinking::No);

            if (Index < m_ArrayNum)
            {
                // Sift down, then sift up to restore heap property
                HeapSiftDown(Index, m_ArrayNum, Pred);
                HeapSiftUp(0, Index, Pred);
            }

            if (AllowShrinking == EAllowShrinking::Yes)
            {
                ResizeShrink();
            }
        }

        void HeapRemoveAt(SizeType Index, EAllowShrinking AllowShrinking = EAllowShrinking::Yes)
        {
            HeapRemoveAt(Index, TLess<ElementType>(), AllowShrinking);
        }

        /**
         * Sort the array using heap sort algorithm using operator<.
         */
        void HeapSort()
        {
            HeapSort(TLess<ElementType>());
        }

        /**
         * Sort the array using heap sort algorithm using a custom predicate.
         * @param Predicate Binary predicate for ordering
         */
        template <typename Predicate>
        void HeapSort(Predicate Pred)
        {
            if (m_ArrayNum <= 1)
            {
                return;
            }

            // Use reverse predicate to build a max-heap for ascending sort
            auto ReversePred = [&Pred](const ElementType& A, const ElementType& B) { return Pred(B, A); };

            // Build max-heap
            ElementType* Data = GetData();
            SizeType Index = HeapGetParentIndex(m_ArrayNum - 1);
            for (;;)
            {
                HeapSiftDownInternal(Data, Index, m_ArrayNum, ReversePred);
                if (Index == 0)
                {
                    break;
                }
                --Index;
            }

            // Extract elements from max-heap
            for (SizeType i = m_ArrayNum - 1; i > 0; --i)
            {
                std::swap(Data[0], Data[i]);
                HeapSiftDownInternal(Data, 0, i, ReversePred);
            }
        }

        /**
         * Check if the array satisfies the heap property.
         * @returns true if the array is a valid heap
         */
        [[nodiscard]] bool IsHeap() const
        {
            return IsHeap(TLess<ElementType>());
        }

        /**
         * Check if the array satisfies the heap property using a custom predicate.
         * @param Predicate Binary predicate for heap ordering
         * @returns true if the array is a valid heap
         */
        template <typename Predicate>
        [[nodiscard]] bool IsHeap(Predicate Pred) const
        {
            const ElementType* Data = GetData();
            for (SizeType i = 1; i < m_ArrayNum; ++i)
            {
                const SizeType ParentIndex = HeapGetParentIndex(i);
                if (Pred(Data[i], Data[ParentIndex]))
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief Verify heap property and assert if invalid
         * 
         * Debug helper that asserts if the array does not satisfy heap property.
         * Uses operator< for comparison.
         */
        void VerifyHeap() const
        {
            VerifyHeap(TLess<ElementType>());
        }

        /**
         * @brief Verify heap property with custom predicate and assert if invalid
         * @param Predicate Binary predicate for heap ordering
         */
        template <typename Predicate>
        void VerifyHeap(Predicate Pred) const
        {
            OLO_CORE_ASSERT(IsHeap(Pred), "TArray::VerifyHeap - Heap property violated");
        }

        // ====================================================================
        // Swap
        // ====================================================================

        /** Swap contents with another array */
        void Swap(SizeType FirstIndex, SizeType SecondIndex)
        {
            OLO_CORE_ASSERT(IsValidIndex(FirstIndex) && IsValidIndex(SecondIndex), "Swap: Invalid index");
            if (FirstIndex != SecondIndex)
            {
                std::swap(GetData()[FirstIndex], GetData()[SecondIndex]);
            }
        }

        /**
         * @brief Swap the memory of two elements by index using direct memory swap
         * @param FirstIndexToSwap   Index of first element
         * @param SecondIndexToSwap  Index of second element
         * 
         * Uses bitwise memory swap rather than using move semantics.
         * More efficient for types where move is expensive or not available.
         */
        void SwapMemory(SizeType FirstIndexToSwap, SizeType SecondIndexToSwap)
        {
            RangeCheck(FirstIndexToSwap);
            RangeCheck(SecondIndexToSwap);
            FMemory::Memswap(
                GetData() + FirstIndexToSwap,
                GetData() + SecondIndexToSwap,
                sizeof(ElementType)
            );
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
        }

    public:
        // ====================================================================
        // Serialization Support
        // ====================================================================

        /**
         * Count bytes needed to serialize this array.
         * @param Ar Archive to count for.
         */
        void CountBytes(FArchive& Ar) const
        {
            Ar.CountBytes(m_ArrayNum * sizeof(ElementType), m_ArrayMax * sizeof(ElementType));
        }

        /**
         * Bulk serialize array as a single memory block.
         *
         * IMPORTANT:
         * - T's << operator can NOT perform any fixup operations
         * - T can NOT contain any member variables requiring constructor calls or pointers
         * - sizeof(ElementType) must equal the sum of sizes of its member variables
         * - Can only be called on platforms with same endianness as saved content
         *
         * @param Ar FArchive to bulk serialize this TArray to/from
         * @param bForcePerElementSerialization If true, always serialize per-element
         */
        void BulkSerialize(FArchive& Ar, bool bForcePerElementSerialization = false)
        {
            constexpr i32 ElementSize = sizeof(ElementType);
            i32 SerializedElementSize = ElementSize;
            Ar << SerializedElementSize;

            if (bForcePerElementSerialization
                || (Ar.IsSaving() && !Ar.IsTransacting())
                || Ar.IsByteSwapping())
            {
                Ar << *this;
            }
            else
            {
                CountBytes(Ar);
                if (Ar.IsLoading())
                {
                    if (SerializedElementSize != ElementSize)
                    {
                        Ar.SetError();
                        return;
                    }

                    SizeType NewArrayNum = 0;
                    Ar << NewArrayNum;
                    if (NewArrayNum < 0)
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

        // Friend declarations for serialization
        template<typename T, typename Alloc>
        friend struct TArrayPrivateFriend;
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

} // namespace OloEngine

// ============================================================================
// TArrayPrivateFriend - Serialization Helper
// ============================================================================

namespace OloEngine
{
    /**
     * @struct TArrayPrivateFriend
     * @brief Friend struct for TArray serialization access
     */
    template<typename ElementType, typename AllocatorType>
    struct TArrayPrivateFriend
    {
        /**
         * Serialization operator implementation.
         * @param Ar Archive to serialize the array with
         * @param A Array to serialize
         * @returns The archive
         */
        static FArchive& Serialize(FArchive& Ar, TArray<ElementType, AllocatorType>& A)
        {
            using SizeType = typename TArray<ElementType, AllocatorType>::SizeType;

            A.CountBytes(Ar);

            // For net archives, limit serialization to 16MB to protect against excessive allocation
            constexpr SizeType MaxNetArraySerialize = (16 * 1024 * 1024) / sizeof(ElementType);
            SizeType SerializeNum = Ar.IsLoading() ? 0 : A.m_ArrayNum;

            Ar << SerializeNum;

            if (SerializeNum == 0)
            {
                if (Ar.IsLoading())
                {
                    A.Empty();
                }
                return Ar;
            }

            if (Ar.IsError() || SerializeNum < 0 || (Ar.IsNetArchive() && SerializeNum > MaxNetArraySerialize))
            {
                Ar.SetError();
                return Ar;
            }

            // If we can bulk serialize, do it
            if constexpr (sizeof(ElementType) == 1 || TCanBulkSerialize<ElementType>::Value)
            {
                A.m_ArrayNum = SerializeNum;

                if ((A.m_ArrayNum || A.m_ArrayMax) && Ar.IsLoading())
                {
                    A.ResizeAllocation(A.m_AllocatorInstance.CalculateSlackReserve(SerializeNum, sizeof(ElementType)));
                }

                Ar.Serialize(A.GetData(), A.Num() * sizeof(ElementType));
            }
            else if (Ar.IsLoading())
            {
                A.Empty(SerializeNum);
                for (SizeType i = 0; i < SerializeNum; i++)
                {
                    Ar << A.AddDefaulted_GetRef();
                }
            }
            else
            {
                A.m_ArrayNum = SerializeNum;
                for (SizeType i = 0; i < A.m_ArrayNum; i++)
                {
                    Ar << A[i];
                }
            }

            return Ar;
        }
    };
} // namespace OloEngine

// ============================================================================
// FArchive Serialization Operator
// ============================================================================

/**
 * @brief Serialization operator for TArray
 * @param Ar Archive to serialize with
 * @param A Array to serialize
 * @returns The archive
 */
template<typename ElementType, typename AllocatorType>
OloEngine::FArchive& operator<<(OloEngine::FArchive& Ar, OloEngine::TArray<ElementType, AllocatorType>& A)
{
    return OloEngine::TArrayPrivateFriend<ElementType, AllocatorType>::Serialize(Ar, A);
}

// ============================================================================
// GetTypeHash for TArray
// ============================================================================

/**
 * @brief Calculate hash for TArray
 * @param Array  Array to hash
 * @returns Combined hash of all elements
 * 
 * Requires that elements support GetTypeHash().
 */
template <typename ElementType, typename AllocatorType>
[[nodiscard]] inline u32 GetTypeHash(const OloEngine::TArray<ElementType, AllocatorType>& Array)
{
    u32 Hash = 0;
    for (const auto& V : Array)
    {
        Hash = OloEngine::HashCombineFast(Hash, GetTypeHash(V));
    }
    return Hash;
}

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
