#pragma once

/**
 * @file ArrayView.h
 * @brief Non-owning view into a contiguous array of elements
 * 
 * TArrayView provides a non-owning view into a contiguous sequence of elements,
 * similar to std::span but with UE-style API. Key features:
 * 
 * - Zero-copy view into arrays, pointers, or any contiguous container
 * - Constexpr support for compile-time array manipulation
 * - Compatible with ranged-for loops
 * - Supports slicing and subviews
 * - Search and sort operations on the view
 * 
 * Ported from Unreal Engine's Containers/ArrayView.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Containers/ReverseIterate.h"
#include "OloEngine/Memory/MemoryOps.h"
#include "OloEngine/Misc/IntrusiveUnsetOptionalState.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include "OloEngine/Templates/Sorting.h"
#include "OloEngine/Algo/Sort.h"
#include "OloEngine/Algo/StableSort.h"
#include <algorithm>
#include <compare>
#include <initializer_list>
#include <type_traits>
#include <functional>

namespace OloEngine
{
    // Forward declarations
    template <typename T, typename AllocatorType = FDefaultAllocator>
    class TArray;

    template <typename InElementType, typename InSizeType>
    class TArrayView;

    // ============================================================================
    // TArrayView Type Traits
    // ============================================================================

    namespace ArrayView::Private
    {
        /**
         * @brief Helper to detect if a type has GetData() and GetNum() functions
         */
        template <typename T>
        constexpr auto GetDataHelper(T& Container) -> decltype(GetData(Container))
        {
            return GetData(Container);
        }

        template <typename T>
        constexpr auto GetDataHelper(T& Container) -> decltype(Container.GetData())
        {
            return Container.GetData();
        }

        template <typename T, sizet N>
        constexpr T* GetDataHelper(T (&Array)[N])
        {
            return Array;
        }
    }

    /**
     * @brief Type trait to detect TArrayView types
     */
    template <typename T>
    inline constexpr bool TIsTArrayView_V = false;

    template <typename InElementType, typename InSizeType>
    inline constexpr bool TIsTArrayView_V<               TArrayView<InElementType, InSizeType>> = true;
    template <typename InElementType, typename InSizeType>
    inline constexpr bool TIsTArrayView_V<      volatile TArrayView<InElementType, InSizeType>> = true;
    template <typename InElementType, typename InSizeType>
    inline constexpr bool TIsTArrayView_V<const          TArrayView<InElementType, InSizeType>> = true;
    template <typename InElementType, typename InSizeType>
    inline constexpr bool TIsTArrayView_V<const volatile TArrayView<InElementType, InSizeType>> = true;

    template <typename T>
    struct TIsTArrayView
    {
        static constexpr bool Value = TIsTArrayView_V<T>;
        static constexpr bool value = TIsTArrayView_V<T>;  // STL compatibility
    };

    /**
     * @brief Type trait to detect if an element type is compatible
     * 
     * Compatible means the pointer types are convertible - e.g., T* to const T*
     */
    template <typename From, typename To>
    struct TIsCompatibleElementType
    {
        static constexpr bool Value = std::is_convertible_v<From*, To*>;
    };

    template <typename From, typename To>
    inline constexpr bool TIsCompatibleElementType_V = TIsCompatibleElementType<From, To>::Value;

    /**
     * @brief Type trait to check if a range type is compatible with TArrayView
     * 
     * A range is compatible if its data pointer can be converted to the element type pointer
     */
    template <typename RangeType, typename ElementType, typename = void>
    struct TIsCompatibleRangeType
    {
        static constexpr bool Value = false;
    };

    template <typename RangeType, typename ElementType>
    struct TIsCompatibleRangeType<
        RangeType,
        ElementType,
        std::void_t<
            decltype(GetData(std::declval<RangeType&>())),
            decltype(GetNum(std::declval<RangeType&>()))
        >
    >
    {
        static constexpr bool Value = 
            TIsCompatibleElementType_V<
                std::remove_pointer_t<decltype(GetData(std::declval<RangeType&>()))>,
                ElementType
            >;
    };

    template <typename RangeType, typename ElementType>
    inline constexpr bool TIsCompatibleRangeType_V = TIsCompatibleRangeType<RangeType, ElementType>::Value;

    /**
     * @brief Type trait to check if a range can be reinterpreted as an array view
     */
    template <typename RangeType, typename ElementType, typename = void>
    struct TIsReinterpretableRangeType
    {
        static constexpr bool Value = false;
    };

    template <typename RangeType, typename ElementType>
    struct TIsReinterpretableRangeType<
        RangeType,
        ElementType,
        std::void_t<
            decltype(GetData(std::declval<RangeType&>())),
            decltype(GetNum(std::declval<RangeType&>()))
        >
    >
    {
    private:
        using RangeElementType = std::remove_pointer_t<decltype(GetData(std::declval<RangeType&>()))>;
    public:
        static constexpr bool Value = 
            sizeof(RangeElementType) == sizeof(ElementType) &&
            alignof(RangeElementType) == alignof(ElementType);
    };

    template <typename RangeType, typename ElementType>
    inline constexpr bool TIsReinterpretableRangeType_V = TIsReinterpretableRangeType<RangeType, ElementType>::Value;

    // ============================================================================
    // TArrayView
    // ============================================================================

    /**
     * @class TArrayView
     * @brief A non-owning view into a contiguous array of elements
     * 
     * TArrayView is similar to std::span - it provides a lightweight view into
     * a contiguous sequence of elements without owning the underlying storage.
     * 
     * Key characteristics:
     * - Does not own the memory it references
     * - Lightweight: just a pointer and size
     * - Supports const and non-const elements
     * - Constexpr friendly
     * - Compatible with ranged-for loops
     * 
     * @tparam InElementType The element type (can be const for read-only view)
     * @tparam InSizeType    The size type (default: i32)
     */
    template <typename InElementType, typename InSizeType = i32>
    class TArrayView
    {
    public:
        using ElementType = InElementType;
        using SizeType = InSizeType;

        static_assert(std::is_signed_v<SizeType>, "TArrayView only supports signed index types");

    private:
        // Traits for checking compatible types
        template <typename OtherRangeType>
        using TIsCompatibleRange = std::bool_constant<
            TIsCompatibleRangeType_V<OtherRangeType, ElementType> &&
            !TIsTArrayView_V<std::remove_cv_t<std::remove_reference_t<OtherRangeType>>>
        >;

        template <typename OtherRangeType>
        using TIsCompatibleArrayViewRange = std::bool_constant<
            TIsCompatibleRangeType_V<OtherRangeType, ElementType> &&
            TIsTArrayView_V<std::remove_cv_t<std::remove_reference_t<OtherRangeType>>>
        >;

    public:
        // ====================================================================
        // Constructors
        // ====================================================================

        // Defaulted object behavior - we want compiler-generated functions rather than going through the generic range constructor.
        [[nodiscard]] constexpr TArrayView(const TArrayView&) = default;
        TArrayView& operator=(const TArrayView&) = default;
        ~TArrayView() = default;

        /** Default constructor - creates an empty view */
        [[nodiscard]] constexpr TArrayView()
            : DataPtr(nullptr)
            , ArrayNum(0)
        {
        }

        /**
         * @brief Construct from a pointer and count
         * @param InData Pointer to the first element
         * @param InCount Number of elements
         */
        template <typename OtherElementType>
            requires (TIsCompatibleElementType_V<OtherElementType, ElementType>)
        [[nodiscard]] constexpr TArrayView(OtherElementType* InData OLO_LIFETIMEBOUND, SizeType InCount)
            : DataPtr(InData)
            , ArrayNum(InCount)
        {
            OLO_CORE_ASSERT(ArrayNum >= 0, "TArrayView count must be non-negative");
        }

        /**
         * @brief Construct from a compatible range (non-TArrayView)
         * @param Other The range to view
         */
        template <typename OtherRangeType>
            requires (TIsCompatibleRange<OtherRangeType>::value)
        constexpr TArrayView(OtherRangeType&& Other OLO_LIFETIMEBOUND)
            : DataPtr(GetData(Other))
            , ArrayNum(static_cast<SizeType>(GetNum(Other)))
        {
        }

        /**
         * @brief Construct from another TArrayView with compatible element type
         * @param Other The array view to copy
         */
        template <typename OtherElementType, typename OtherSizeType>
            requires (TIsCompatibleElementType_V<OtherElementType, ElementType>)
        constexpr TArrayView(const TArrayView<OtherElementType, OtherSizeType>& Other)
            : DataPtr(Other.GetData())
            , ArrayNum(static_cast<SizeType>(Other.Num()))
        {
        }

        /**
         * @brief Construct from an initializer list (const elements only)
         * @param List The initializer list
         */
        [[nodiscard]] constexpr TArrayView(std::initializer_list<std::remove_const_t<ElementType>> List OLO_LIFETIMEBOUND)
            requires (std::is_const_v<ElementType>)
            : DataPtr(List.begin())
            , ArrayNum(static_cast<SizeType>(List.size()))
        {
        }

        ///////////////////////////////////////////////////
        // Start - intrusive TOptional<TArrayView> state //
        ///////////////////////////////////////////////////
        static constexpr bool bHasIntrusiveUnsetOptionalState = true;
        using IntrusiveUnsetOptionalStateType = TArrayView;

        [[nodiscard]] explicit constexpr TArrayView(FIntrusiveUnsetOptionalState)
            : DataPtr(nullptr)
            , ArrayNum(-1)
        {
        }
        [[nodiscard]] constexpr bool operator==(FIntrusiveUnsetOptionalState) const
        {
            return ArrayNum == -1;
        }
        /////////////////////////////////////////////////
        // End - intrusive TOptional<TArrayView> state //
        /////////////////////////////////////////////////

        // ====================================================================
        // Assignment
        // ====================================================================

        /**
         * @brief Assign from a compatible range
         * @param Other The range to view
         */
        template <typename OtherRangeType>
            requires (TIsCompatibleRange<OtherRangeType>::value)
        constexpr TArrayView& operator=(OtherRangeType&& Other)
        {
            DataPtr = GetData(Other);
            ArrayNum = static_cast<SizeType>(GetNum(Other));
            return *this;
        }

        // ====================================================================
        // Accessors
        // ====================================================================

        /**
         * @brief Get pointer to the first element
         * @returns Pointer to the first element, or nullptr if empty
         */
        [[nodiscard]] OLO_FINLINE constexpr ElementType* GetData() const
        {
            return DataPtr;
        }

        /**
         * @brief Helper function returning the size of the inner type.
         * @returns Size in bytes of array type.
         */
        [[nodiscard]] OLO_FINLINE static constexpr sizet GetTypeSize()
        {
            return sizeof(ElementType);
        }

        /**
         * @brief Helper function returning the alignment of the inner type.
         * @returns Alignment in bytes of array type.
         */
        [[nodiscard]] OLO_FINLINE static constexpr sizet GetTypeAlignment()
        {
            return alignof(ElementType);
        }

        /**
         * @brief Checks array invariants: if array size is greater than or equal to zero.
         */
        OLO_FINLINE constexpr void CheckInvariants() const
        {
            OLO_CORE_ASSERT(ArrayNum >= 0, "TArrayView invariant violated");
        }

        /**
         * @brief Checks if index is in array range.
         * @param Index Index to check.
         */
        constexpr void RangeCheck(SizeType Index) const
        {
            CheckInvariants();
            OLO_CORE_ASSERT((Index >= 0) & (Index < ArrayNum),
                "Array index out of bounds: {} from an array of size {}", Index, ArrayNum);
        }

        /**
         * @brief Checks if a slice range [Index, Index+InNum) is in array range.
         * Length is 0 is allowed on empty arrays; Index must be 0 in that case.
         * 
         * @param Index Starting index of the slice.
         * @param InNum Length of the slice.
         */
        constexpr void SliceRangeCheck(SizeType Index, SizeType InNum) const
        {
            OLO_CORE_ASSERT(Index >= 0, "Invalid index ({})", Index);
            OLO_CORE_ASSERT(InNum >= 0, "Invalid count ({})", InNum);
            OLO_CORE_ASSERT(Index + InNum <= ArrayNum, 
                "Range (index: {}, count: {}) lies outside the view of {} elements", Index, InNum, ArrayNum);
        }

        /**
         * @brief Get the number of elements
         * @returns Number of elements in the view
         */
        [[nodiscard]] OLO_FINLINE constexpr SizeType Num() const
        {
            return ArrayNum;
        }

        /**
         * @brief Get the number of bytes used
         * @returns Number of bytes used by all elements
         */
        [[nodiscard]] OLO_FINLINE constexpr sizet NumBytes() const
        {
            return static_cast<sizet>(ArrayNum) * sizeof(ElementType);
        }

        /**
         * @brief Check if the view is empty
         * @returns True if the view has no elements
         */
        [[nodiscard]] constexpr bool IsEmpty() const
        {
            return ArrayNum == 0;
        }

        /**
         * @brief Check if the index is valid
         * @returns True if the index is in bounds
         */
        [[nodiscard]] OLO_FINLINE constexpr bool IsValidIndex(SizeType Index) const
        {
            return (Index >= 0) && (Index < ArrayNum);
        }

        /**
         * @brief Get the total size in bytes (alias for NumBytes)
         * @returns Total size of all elements in bytes
         */
        [[nodiscard]] constexpr sizet GetAllocatedSize() const
        {
            return NumBytes();
        }

        // ====================================================================
        // Element Access
        // ====================================================================

        /**
         * @brief Access element by index with bounds checking
         * @param Index Index of the element
         * @returns Reference to the element
         */
        [[nodiscard]] constexpr ElementType& operator[](SizeType Index) const
        {
            RangeCheck(Index);
            return GetData()[Index];
        }

        /**
         * @brief Access the last element
         * @param IndexFromTheEnd Index from the end (0 = last element)
         * @returns Reference to the element
         */
        [[nodiscard]] constexpr ElementType& Last(SizeType IndexFromTheEnd = 0) const
        {
            RangeCheck(ArrayNum - IndexFromTheEnd - 1);
            return GetData()[ArrayNum - IndexFromTheEnd - 1];
        }

        // ====================================================================
        // Slicing Operations
        // ====================================================================

        /**
         * @brief Get a strict slice of the view
         * 
         * Unlike Mid(), this function has a narrow contract - slicing outside
         * the bounds is illegal and will assert.
         * 
         * @param Index Starting index
         * @param InNum Number of elements in the slice
         * @returns A new view containing the slice
         */
        [[nodiscard]] constexpr TArrayView Slice(SizeType Index, SizeType InNum) const
        {
            SliceRangeCheck(Index, InNum);
            return TArrayView(DataPtr + Index, InNum);
        }

        /**
         * @brief Get the leftmost N elements
         * @param Count Number of elements to take
         * @returns A new view containing the left portion
         */
        [[nodiscard]] constexpr TArrayView Left(SizeType Count) const
        {
            return TArrayView(DataPtr, std::clamp(Count, SizeType(0), ArrayNum));
        }

        /**
         * @brief Get the view with N elements removed from the right
         * @param Count Number of elements to remove from the right
         * @returns A new view with the right portion chopped
         */
        [[nodiscard]] constexpr TArrayView LeftChop(SizeType Count) const
        {
            return TArrayView(DataPtr, std::clamp(ArrayNum - Count, SizeType(0), ArrayNum));
        }

        /**
         * @brief Get the rightmost N elements
         * @param Count Number of elements to take
         * @returns A new view containing the right portion
         */
        [[nodiscard]] constexpr TArrayView Right(SizeType Count) const
        {
            const SizeType OutLen = std::clamp(Count, SizeType(0), ArrayNum);
            return TArrayView(DataPtr + ArrayNum - OutLen, OutLen);
        }

        /**
         * @brief Get the view with N elements removed from the left
         * @param Count Number of elements to remove from the left
         * @returns A new view with the left portion chopped
         */
        [[nodiscard]] constexpr TArrayView RightChop(SizeType Count) const
        {
            const SizeType OutLen = std::clamp(ArrayNum - Count, SizeType(0), ArrayNum);
            return TArrayView(DataPtr + ArrayNum - OutLen, OutLen);
        }

        /**
         * @brief Get a middle portion of the view (wide contract)
         * 
         * This function has a wide contract - it will clamp indices to valid ranges.
         * 
         * @param Index Starting index
         * @param Count Maximum number of elements
         * @returns A new view containing the middle portion
         */
        [[nodiscard]] constexpr TArrayView Mid(SizeType Index, SizeType Count = TNumericLimits<SizeType>::Max()) const
        {
            ElementType* const CurrentStart = GetData();
            const SizeType CurrentLength = Num();

            // Clamp minimum index at the start of the range, adjusting the length down if necessary
            const SizeType NegativeIndexOffset = (Index < 0) ? Index : 0;
            Count += NegativeIndexOffset;
            Index -= NegativeIndexOffset;

            // Clamp maximum index at the end of the range
            Index = (Index > CurrentLength) ? CurrentLength : Index;

            // Clamp count between 0 and the distance to the end of the range
            Count = std::clamp(Count, SizeType(0), CurrentLength - Index);

            return TArrayView(CurrentStart + Index, Count);
        }

        // ====================================================================
        // In-place Slicing Operations
        // ====================================================================

        /** Modify the view to be the left N elements */
        constexpr void LeftInline(SizeType Count)
        {
            *this = Left(Count);
        }

        /** Modify the view to chop N elements from the right */
        constexpr void LeftChopInline(SizeType Count)
        {
            *this = LeftChop(Count);
        }

        /** Modify the view to be the right N elements */
        constexpr void RightInline(SizeType Count)
        {
            *this = Right(Count);
        }

        /** Modify the view to chop N elements from the left */
        constexpr void RightChopInline(SizeType Count)
        {
            *this = RightChop(Count);
        }

        /** Modify the view to be a middle portion */
        constexpr void MidInline(SizeType Position, SizeType Count = TNumericLimits<SizeType>::Max())
        {
            *this = Mid(Position, Count);
        }

        // ====================================================================
        // Search Operations
        // ====================================================================

        /**
         * @brief Find an element and return its index via output parameter
         * @param Item Item to search for
         * @param Index Output parameter for the found index
         * @returns True if found
         */
        [[nodiscard]] constexpr bool Find(const ElementType& Item, SizeType& Index) const
        {
            Index = this->Find(Item);
            return Index != static_cast<SizeType>(INDEX_NONE);
        }

        /**
         * @brief Find an element and return its index
         * @param Item Item to search for
         * @returns Index of the item, or INDEX_NONE if not found
         */
        [[nodiscard]] constexpr SizeType Find(const ElementType& Item) const
        {
            const ElementType* OLO_RESTRICT Start = GetData();
            for (const ElementType* OLO_RESTRICT Data = Start, *OLO_RESTRICT DataEnd = Data + ArrayNum; Data != DataEnd; ++Data)
            {
                if (*Data == Item)
                {
                    return static_cast<SizeType>(Data - Start);
                }
            }
            return static_cast<SizeType>(INDEX_NONE);
        }

        /**
         * @brief Find an element from the end and return its index via output parameter
         * @param Item Item to search for
         * @param Index Output parameter for the found index
         * @returns True if found
         */
        [[nodiscard]] constexpr bool FindLast(const ElementType& Item, SizeType& Index) const
        {
            Index = this->FindLast(Item);
            return Index != static_cast<SizeType>(INDEX_NONE);
        }

        /**
         * @brief Find an element from the end and return its index
         * @param Item Item to search for
         * @returns Index of the item, or INDEX_NONE if not found
         */
        [[nodiscard]] constexpr SizeType FindLast(const ElementType& Item) const
        {
            for (const ElementType* OLO_RESTRICT Start = GetData(), *OLO_RESTRICT Data = Start + ArrayNum; Data != Start; )
            {
                --Data;
                if (*Data == Item)
                {
                    return static_cast<SizeType>(Data - Start);
                }
            }
            return static_cast<SizeType>(INDEX_NONE);
        }

        /**
         * @brief Find element by predicate, searching backwards from a start index
         * @param Pred Predicate function
         * @param StartIndex Index to start searching from
         * @returns Index of the found element, or INDEX_NONE
         */
        template <typename Predicate>
        [[nodiscard]] constexpr SizeType FindLastByPredicate(Predicate Pred, SizeType StartIndex) const
        {
            OLO_CORE_ASSERT(StartIndex >= 0 && StartIndex <= this->Num(), "Invalid StartIndex for FindLastByPredicate");
            for (const ElementType* OLO_RESTRICT Start = GetData(), *OLO_RESTRICT Data = Start + StartIndex; Data != Start; )
            {
                --Data;
                if (std::invoke(Pred, *Data))
                {
                    return static_cast<SizeType>(Data - Start);
                }
            }
            return static_cast<SizeType>(INDEX_NONE);
        }

        /**
         * @brief Find element by predicate, searching backwards from the end
         * @param Pred Predicate function
         * @returns Index of the found element, or INDEX_NONE
         */
        template <typename Predicate>
        [[nodiscard]] OLO_FINLINE constexpr SizeType FindLastByPredicate(Predicate Pred) const
        {
            return FindLastByPredicate(Pred, ArrayNum);
        }

        /**
         * @brief Find an item by key
         * @param Key Key to search for
         * @returns Index of the found element, or INDEX_NONE
         */
        template <typename KeyType>
        [[nodiscard]] constexpr SizeType IndexOfByKey(const KeyType& Key) const
        {
            const ElementType* OLO_RESTRICT Start = GetData();
            for (const ElementType* OLO_RESTRICT Data = Start, *OLO_RESTRICT DataEnd = Start + ArrayNum; Data != DataEnd; ++Data)
            {
                if (*Data == Key)
                {
                    return static_cast<SizeType>(Data - Start);
                }
            }
            return static_cast<SizeType>(INDEX_NONE);
        }

        /**
         * @brief Find an item by predicate
         * @param Pred Predicate function
         * @returns Index of the found element, or INDEX_NONE
         */
        template <typename Predicate>
        [[nodiscard]] constexpr SizeType IndexOfByPredicate(Predicate Pred) const
        {
            const ElementType* OLO_RESTRICT Start = GetData();
            for (const ElementType* OLO_RESTRICT Data = Start, *OLO_RESTRICT DataEnd = Start + ArrayNum; Data != DataEnd; ++Data)
            {
                if (std::invoke(Pred, *Data))
                {
                    return static_cast<SizeType>(Data - Start);
                }
            }
            return static_cast<SizeType>(INDEX_NONE);
        }

        /**
         * @brief Find an item by key and return a pointer
         * @param Key Key to search for
         * @returns Pointer to the element, or nullptr
         */
        template <typename KeyType>
        [[nodiscard]] constexpr ElementType* FindByKey(const KeyType& Key) const
        {
            for (ElementType* OLO_RESTRICT Data = GetData(), *OLO_RESTRICT DataEnd = Data + ArrayNum; Data != DataEnd; ++Data)
            {
                if (*Data == Key)
                {
                    return Data;
                }
            }
            return nullptr;
        }

        /**
         * @brief Find an element by predicate and return a pointer
         * @param Pred Predicate function
         * @returns Pointer to the element, or nullptr
         */
        template <typename Predicate>
        [[nodiscard]] constexpr ElementType* FindByPredicate(Predicate Pred) const
        {
            for (ElementType* OLO_RESTRICT Data = GetData(), *OLO_RESTRICT DataEnd = Data + ArrayNum; Data != DataEnd; ++Data)
            {
                if (std::invoke(Pred, *Data))
                {
                    return Data;
                }
            }
            return nullptr;
        }

        /**
         * @brief Filter elements by predicate
         * @param Pred Predicate function
         * @returns New TArray containing matching elements
         */
        template <typename Predicate>
        [[nodiscard]] constexpr TArray<std::remove_const_t<ElementType>> FilterByPredicate(Predicate Pred) const
        {
            TArray<std::remove_const_t<ElementType>> FilterResults;
            for (const ElementType* OLO_RESTRICT Data = GetData(), *OLO_RESTRICT DataEnd = Data + ArrayNum; Data != DataEnd; ++Data)
            {
                if (std::invoke(Pred, *Data))
                {
                    FilterResults.Add(*Data);
                }
            }
            return FilterResults;
        }

        /**
         * @brief Check if the view contains an element
         * @param Item Item to search for
         * @returns True if the item is in the view
         */
        template <typename ComparisonType>
        [[nodiscard]] constexpr bool Contains(const ComparisonType& Item) const
        {
            for (const ElementType* OLO_RESTRICT Data = GetData(), *OLO_RESTRICT DataEnd = Data + ArrayNum; Data != DataEnd; ++Data)
            {
                if (*Data == Item)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Check if any element matches a predicate
         * @param Pred Predicate function
         * @returns True if any element matches
         */
        template <typename Predicate>
        [[nodiscard]] OLO_FINLINE constexpr bool ContainsByPredicate(Predicate Pred) const
        {
            return FindByPredicate(Pred) != nullptr;
        }

        // ====================================================================
        // Iteration Support
        // ====================================================================

        /** Begin iterator for range-based for */
        [[nodiscard]] OLO_FINLINE constexpr ElementType* begin() const { return GetData(); }
        
        /** End iterator for range-based for */
        [[nodiscard]] OLO_FINLINE constexpr ElementType* end() const { return GetData() + Num(); }

        /** Reverse begin iterator */
        [[nodiscard]] OLO_FINLINE constexpr TReversePointerIterator<ElementType> rbegin() const 
        { 
            return TReversePointerIterator<ElementType>(GetData() + Num()); 
        }

        /** Reverse end iterator */
        [[nodiscard]] OLO_FINLINE constexpr TReversePointerIterator<ElementType> rend() const 
        { 
            return TReversePointerIterator<ElementType>(GetData()); 
        }

        // ====================================================================
        // Sorting
        // ====================================================================

        /**
         * @brief Sorts the array assuming < operator is defined for the item type.
         *
         * @note If your array contains raw pointers, they will be automatically dereferenced during sorting.
         *       Therefore, your array will be sorted by the values being pointed to, rather than the pointers' values.
         *       If this is not desirable, please use Algo::Sort(*this) directly instead.
         *       The auto-dereferencing behavior does not occur with smart pointers.
         */
        constexpr void Sort()
        {
            Algo::Sort(*this, TDereferenceWrapper<ElementType, TLess<>>(TLess<>()));
        }

        /**
         * @brief Sorts the array using user define predicate class.
         *
         * @param Predicate Predicate class instance.
         * @note If your array contains raw pointers, they will be automatically dereferenced during sorting.
         *       Therefore, your predicate will be passed references rather than pointers.
         *       If this is not desirable, please use Algo::Sort(*this, Predicate) directly instead.
         *       The auto-dereferencing behavior does not occur with smart pointers.
         */
        template <typename PredicateClass>
        constexpr void Sort(const PredicateClass& Predicate)
        {
            TDereferenceWrapper<ElementType, PredicateClass> PredicateWrapper(Predicate);
            Algo::Sort(*this, PredicateWrapper);
        }

        /**
         * @brief Stable sorts the array assuming < operator is defined for the item type.
         *
         * Stable sort is slower than non-stable algorithm.
         * @note If your array contains raw pointers, they will be automatically dereferenced during sorting.
         *       Therefore, your array will be sorted by the values being pointed to, rather than the pointers' values.
         *       If this is not desirable, please use Algo::StableSort(*this) directly instead.
         *       The auto-dereferencing behavior does not occur with smart pointers.
         */
        constexpr void StableSort()
        {
            Algo::StableSort(*this, TDereferenceWrapper<ElementType, TLess<>>(TLess<>()));
        }

        /**
         * @brief Stable sorts the array using user defined predicate class.
         *
         * Stable sort is slower than non-stable algorithm.
         *
         * @param Predicate Predicate class instance
         * @note If your array contains raw pointers, they will be automatically dereferenced during sorting.
         *       Therefore, your predicate will be passed references rather than pointers.
         *       If this is not desirable, please use Algo::StableSort(*this, Predicate) directly instead.
         *       The auto-dereferencing behavior does not occur with smart pointers.
         */
        template <typename PredicateClass>
        constexpr void StableSort(const PredicateClass& Predicate)
        {
            TDereferenceWrapper<ElementType, PredicateClass> PredicateWrapper(Predicate);
            Algo::StableSort(*this, PredicateWrapper);
        }

        // ====================================================================
        // Comparison
        // ====================================================================

        /**
         * @brief Comparison with TArrayView is deleted to prevent ambiguity
         * 
         * Comparison of array views to each other is not implemented because it
         * is not obvious whether the caller wants an exact match of the data
         * pointer and size, or to compare the objects being pointed to.
         */
        template <typename OtherElementType, typename OtherSizeType>
        bool operator==(TArrayView<OtherElementType, OtherSizeType>) const = delete;

        /**
         * @brief Equality operator
         * @param Rhs Another ranged type to compare.
         * @returns True if this array view's contents and the other ranged type match. False otherwise.
         */
        template <typename RangeType>
            requires (std::is_convertible_v<decltype(ArrayView::Private::GetDataHelper(std::declval<RangeType&>())), const ElementType*>)
        [[nodiscard]] bool operator==(RangeType&& Rhs) const
        {
            auto Count = GetNum(Rhs);
            return Count == ArrayNum && CompareItems(DataPtr, ArrayView::Private::GetDataHelper(Rhs), Count);
        }

    private:
        ElementType* DataPtr;
        SizeType ArrayNum;
    };

    // ============================================================================
    // Deduction Guide
    // ============================================================================

    template <typename T>
    TArrayView(T*, i32) -> TArrayView<T>;

    // ============================================================================
    // Type Traits
    // ============================================================================

    template <typename InElementType>
    struct TIsZeroConstructType<TArrayView<InElementType>>
    {
        static constexpr bool Value = true;
    };

    template <typename T, typename SizeType>
    struct TIsContiguousContainer<TArrayView<T, SizeType>>
    {
        static constexpr bool Value = true;
    };

    // ============================================================================
    // TArrayView Aliases
    // ============================================================================

    /** TArrayView using 64-bit size type */
    template <typename InElementType>
    using TArrayView64 = TArrayView<InElementType, i64>;

    /** Const array view alias */
    template <typename InElementType, typename InSizeType = i32>
    using TConstArrayView = TArrayView<const InElementType, InSizeType>;

    // ============================================================================
    // MakeArrayView Factory Functions
    // ============================================================================

    /**
     * @brief Create a TArrayView from an existing TArrayView
     * @param Other The source view
     * @returns A copy of the view
     */
    template <
        typename OtherRangeType,
        typename CVUnqualifiedOtherRangeType = std::remove_cv_t<std::remove_reference_t<OtherRangeType>>
    >
        requires (TIsContiguousContainer<CVUnqualifiedOtherRangeType>::Value && TIsTArrayView_V<CVUnqualifiedOtherRangeType>)
    [[nodiscard]] constexpr auto MakeArrayView(OtherRangeType&& Other)
    {
        return TArrayView<std::remove_pointer_t<decltype(GetData(std::declval<OtherRangeType&>()))>>(std::forward<OtherRangeType>(Other));
    }

    /**
     * @brief Create a TArrayView from a contiguous container
     * @param Other The source container
     * @returns A view into the container
     */
    template <
        typename OtherRangeType,
        typename CVUnqualifiedOtherRangeType = std::remove_cv_t<std::remove_reference_t<OtherRangeType>>
    >
        requires (TIsContiguousContainer<CVUnqualifiedOtherRangeType>::Value && !TIsTArrayView_V<CVUnqualifiedOtherRangeType>)
    [[nodiscard]] constexpr auto MakeArrayView(OtherRangeType&& Other)
    {
        return TArrayView<std::remove_pointer_t<decltype(GetData(std::declval<OtherRangeType&>()))>>(std::forward<OtherRangeType>(Other));
    }

    /**
     * @brief Create a TArrayView from a pointer and size
     * @param Pointer Pointer to the first element
     * @param Size Number of elements
     * @returns A view into the memory
     */
    template <typename ElementType>
    [[nodiscard]] constexpr TArrayView<ElementType> MakeArrayView(ElementType* Pointer, i32 Size)
    {
        return TArrayView<ElementType>(Pointer, Size);
    }

    /**
     * @brief Create a const TArrayView from an initializer list
     * @param List The initializer list
     * @returns A const view into the list
     */
    template <typename ElementType>
    [[nodiscard]] constexpr TArrayView<const ElementType> MakeArrayView(std::initializer_list<ElementType> List)
    {
        return TArrayView<const ElementType>(List.begin(), static_cast<i32>(List.size()));
    }

    // ============================================================================
    // MakeConstArrayView Factory Functions
    // ============================================================================

    /**
     * @brief Create a const TArrayView from an existing TArrayView
     */
    template <
        typename OtherRangeType,
        typename CVUnqualifiedOtherRangeType = std::remove_cv_t<std::remove_reference_t<OtherRangeType>>
    >
        requires (TIsContiguousContainer<CVUnqualifiedOtherRangeType>::Value && TIsTArrayView_V<CVUnqualifiedOtherRangeType>)
    [[nodiscard]] constexpr auto MakeConstArrayView(OtherRangeType&& Other)
    {
        return TArrayView<const std::remove_pointer_t<decltype(GetData(std::declval<OtherRangeType&>()))>>(std::forward<OtherRangeType>(Other));
    }

    /**
     * @brief Create a const TArrayView from a contiguous container
     */
    template <
        typename OtherRangeType,
        typename CVUnqualifiedOtherRangeType = std::remove_cv_t<std::remove_reference_t<OtherRangeType>>
    >
        requires (TIsContiguousContainer<CVUnqualifiedOtherRangeType>::Value && !TIsTArrayView_V<CVUnqualifiedOtherRangeType>)
    [[nodiscard]] constexpr auto MakeConstArrayView(OtherRangeType&& Other)
    {
        return TArrayView<const std::remove_pointer_t<decltype(GetData(std::declval<OtherRangeType&>()))>>(std::forward<OtherRangeType>(Other));
    }

    /**
     * @brief Create a const TArrayView from a pointer and size
     */
    template <typename ElementType>
    [[nodiscard]] constexpr TArrayView<const ElementType> MakeConstArrayView(const ElementType* Pointer, i32 Size)
    {
        return TArrayView<const ElementType>(Pointer, Size);
    }

    /**
     * @brief Create a const TArrayView from an initializer list
     */
    template <typename ElementType>
    [[nodiscard]] constexpr TArrayView<const ElementType> MakeConstArrayView(std::initializer_list<ElementType> List)
    {
        return TArrayView<const ElementType>(List.begin(), static_cast<i32>(List.size()));
    }

} // namespace OloEngine
