#pragma once

/**
 * @file ContainersFwd.h
 * @brief Forward declarations for containers
 * 
 * Forward declares container types and their default allocator types.
 * Include this header when you need to forward declare containers in headers.
 * 
 * Ported from Unreal Engine's Containers/ContainersFwd.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"

namespace OloEngine
{
    /// @cond DOXYGEN_WARNINGS

    // ============================================================================
    // Allocator Forward Declarations
    // ============================================================================

    template <i32 IndexSize>
    class TSizedDefaultAllocator;

    template <i32 IndexSize>
    class TSizedNonshrinkingAllocator;

    template <typename ElementType, bool bInAllowDuplicateKeys = false>
    struct DefaultKeyFuncs;

    /** Default allocator using 32-bit index type */
    using FDefaultAllocator = TSizedDefaultAllocator<32>;

    /** Non-shrinking allocator using 32-bit index type */
    using FNonshrinkingAllocator = TSizedNonshrinkingAllocator<32>;

    /** Default allocator using 64-bit index type */
    using FDefaultAllocator64 = TSizedDefaultAllocator<64>;

    /** Default set allocator */
    class FDefaultSetAllocator;

    /** Default compact set allocator */
    class FDefaultCompactSetAllocator;

    /** Default sparse set allocator */
    class FDefaultSparseSetAllocator;

    // ============================================================================
    // String Forward Declarations
    // ============================================================================

    class FString;
    class FAnsiString;
    class FUtf8String;

    // FWideString is an alias for FString.
    //
    // This is so ANSICHAR/UTF8CHAR/WIDECHAR can be matched with FAnsiString/FUtf8String/FWideString when overloaded or specialized.
    //
    // FWideString should be the 'real' string class and FString should be the alias, but can't be for legacy reasons.
    // Forward declarations of FString expect it to be a class and changing it would affect ABIs and bloat PDBs.
    using FWideString = FString;

    template<> struct TIsContiguousContainer<FString>     { static constexpr bool Value = true; };
    template<> struct TIsContiguousContainer<FAnsiString> { static constexpr bool Value = true; };
    template<> struct TIsContiguousContainer<FUtf8String> { static constexpr bool Value = true; };

    namespace Private
    {
        template <typename CharType>
        struct TCharTypeToStringType;

        template <>
        struct TCharTypeToStringType<wchar_t>
        {
            using Type = FWideString;
        };

        template <>
        struct TCharTypeToStringType<char>
        {
            using Type = FAnsiString;
        };

        template <>
        struct TCharTypeToStringType<char8_t>
        {
            using Type = FUtf8String;
        };
    }

    template <typename CharType>
    using TString = typename Private::TCharTypeToStringType<CharType>::Type;

    // ============================================================================
    // Type Traits Forward Declarations
    // ============================================================================

    template <typename T = void>
    struct TLess;

    template <typename>
    struct TTypeTraits;

    // ============================================================================
    // Container Forward Declarations
    // ============================================================================

    /** Dynamic array container */
    template <typename T, typename Allocator = FDefaultAllocator>
    class TArray;

    /** Array with 64-bit index type */
    template <typename T>
    using TArray64 = TArray<T, FDefaultAllocator64>;

    /** Transactional array */
    template <typename T>
    class TTransArray;

    /** Non-owning view into an array */
    template <typename T, typename SizeType = i32>
    class TArrayView;

    /** Non-owning view into an array (64-bit size type) */
    template <typename T>
    using TArrayView64 = TArrayView<T, i64>;

    /** Const array view alias */
    template <typename T, typename SizeType = i32>
    using TConstArrayView = TArrayView<const T, SizeType>;

    /** Const array view (64-bit size type) */
    template <typename T>
    using TConstArrayView64 = TConstArrayView<T, i64>;

    // ============================================================================
    // Map Forward Declarations
    // ============================================================================

    template <typename InKeyType, typename InValueType, bool bInAllowDuplicateKeys>
    struct TDefaultMapHashableKeyFuncs;

    /** Sparse map using sparse array storage */
    template <typename InKeyType,
              typename InValueType,
              typename SetAllocator = FDefaultSparseSetAllocator,
              typename KeyFuncs = TDefaultMapHashableKeyFuncs<InKeyType, InValueType, false>>
    class TSparseMap;

    /** Compact map using compact set storage */
    template <typename InKeyType,
              typename InValueType,
              typename SetAllocator = FDefaultCompactSetAllocator,
              typename KeyFuncs = TDefaultMapHashableKeyFuncs<InKeyType, InValueType, false>>
    class TCompactMap;

    /** Hash map container */
    template <typename InKeyType, 
              typename InValueType,
              typename SetAllocator = FDefaultSetAllocator,
              typename KeyFuncs = TDefaultMapHashableKeyFuncs<InKeyType, InValueType, false>>
    class TMap;

    /** Sparse multimap allowing multiple values per key */
    template <typename KeyType,
              typename ValueType,
              typename SetAllocator = FDefaultSparseSetAllocator,
              typename KeyFuncs = TDefaultMapHashableKeyFuncs<KeyType, ValueType, true>>
    class TSparseMultiMap;

    /** Compact multimap allowing multiple values per key */
    template <typename KeyType,
              typename ValueType,
              typename SetAllocator = FDefaultCompactSetAllocator,
              typename KeyFuncs = TDefaultMapHashableKeyFuncs<KeyType, ValueType, true>>
    class TCompactMultiMap;

    /** Hash multimap allowing multiple values per key */
    template <typename KeyType, 
              typename ValueType,
              typename SetAllocator = FDefaultSetAllocator,
              typename KeyFuncs = TDefaultMapHashableKeyFuncs<KeyType, ValueType, true>>
    class TMultiMap;

    // ============================================================================
    // Set Forward Declarations
    // ============================================================================

    /** Sparse set using sparse array storage */
    template <typename InElementType,
              typename KeyFuncs = DefaultKeyFuncs<InElementType>,
              typename Allocator = FDefaultSparseSetAllocator>
    class TSparseSet;

    /** Compact set using dense storage */
    template <typename InElementType,
              typename KeyFuncs = DefaultKeyFuncs<InElementType>,
              typename Allocator = FDefaultCompactSetAllocator>
    class TCompactSet;

    /** Hash set container */
    template <typename InElementType,
              typename KeyFuncs = DefaultKeyFuncs<InElementType>,
              typename Allocator = FDefaultSetAllocator>
    class TSet;

    // ============================================================================
    // Sorted Container Forward Declarations
    // ============================================================================

    /** Sorted map (binary search tree based) */
    template <typename InKeyType,
              typename InValueType,
              typename ArrayAllocator = FDefaultAllocator,
              typename SortPredicate = TLess<typename TTypeTraits<InKeyType>::ConstPointerType>>
    class TSortedMap;

    /** Sorted set (binary search tree based) */
    template <typename InElementType,
              typename ArrayAllocator = FDefaultAllocator,
              typename SortPredicate = TLess<InElementType>>
    class TSortedSet;

    // ============================================================================
    // View Forward Declarations
    // ============================================================================

    /** Strided view into contiguous memory */
    template <typename InElementType, typename InSizeType = i32>
    class TStridedView;

    /** Const strided view alias */
    template <typename T, typename SizeType = i32>
    using TConstStridedView = TStridedView<const T, SizeType>;

    /// @endcond

} // namespace OloEngine
