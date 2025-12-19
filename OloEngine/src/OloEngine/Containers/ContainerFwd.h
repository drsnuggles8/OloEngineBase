#pragma once

/**
 * @file ContainerFwd.h
 * @brief Forward declarations and type aliases for container types
 *
 * This file provides forward declarations for container types to avoid
 * circular dependencies and reduce compile times.
 *
 * NOTE: This is a minimal forward declaration file. Unlike UE's ContainersFwd.h,
 * we cannot forward declare TSet/TMap here because they are type aliases in OloEngine,
 * not actual class templates. Include the full headers when you need to use containers.
 *
 * Ported from Unreal Engine's Containers/ContainersFwd.h
 */

#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include <initializer_list>
#include <type_traits>

namespace OloEngine
{
    // ============================================================================
    // TElementType - Traits class to get element type of a container
    // ============================================================================

    namespace Private::ElementType
    {
        // Helper to check if T has ElementType
        template<typename T, typename = void>
        struct HasElementType : std::false_type
        {
        };

        template<typename T>
        struct HasElementType<T, std::void_t<typename T::ElementType>> : std::true_type
        {
        };

        // Helper to check if T has value_type
        template<typename T, typename = void>
        struct HasValueType : std::false_type
        {
        };

        template<typename T>
        struct HasValueType<T, std::void_t<typename T::value_type>> : std::true_type
        {
        };

        /**
         * @brief Primary template - no element type available
         */
        template<typename T, typename = void>
        struct TImpl
        {
        };

        /**
         * @brief Specialization for types that have an ElementType typedef (UE-style)
         */
        template<typename T>
        struct TImpl<T, std::enable_if_t<HasElementType<T>::value>>
        {
            using Type = typename T::ElementType;
        };

        /**
         * @brief Specialization for types that have value_type but not ElementType (STL-style)
         */
        template<typename T>
        struct TImpl<T, std::enable_if_t<!HasElementType<T>::value && HasValueType<T>::value>>
        {
            using Type = typename T::value_type;
        };

    } // namespace Private::ElementType

    /**
     * @brief Traits class which gets the element type of a container
     *
     * Use TElementType<T>::Type or TElementType_T<T> to get the element type
     * of a container. Works with any container that defines an ElementType typedef,
     * a value_type typedef (STL containers), C arrays, and std::initializer_list.
     */
    template<typename T>
    struct TElementType : Private::ElementType::TImpl<T>
    {
    };

    // CV-qualifier and reference stripping specializations
    template<typename T>
    struct TElementType<T&> : TElementType<T>
    {
    };
    template<typename T>
    struct TElementType<T&&> : TElementType<T>
    {
    };
    template<typename T>
    struct TElementType<const T> : TElementType<T>
    {
    };
    template<typename T>
    struct TElementType<volatile T> : TElementType<T>
    {
    };
    template<typename T>
    struct TElementType<const volatile T> : TElementType<T>
    {
    };

    /**
     * @brief Specialization for C arrays
     */
    template<typename T, size_t N>
    struct TElementType<T[N]>
    {
        using Type = T;
    };
    template<typename T, size_t N>
    struct TElementType<const T[N]>
    {
        using Type = T;
    };
    template<typename T, size_t N>
    struct TElementType<volatile T[N]>
    {
        using Type = T;
    };
    template<typename T, size_t N>
    struct TElementType<const volatile T[N]>
    {
        using Type = T;
    };

    /**
     * @brief Specialization for initializer lists
     */
    template<typename T>
    struct TElementType<std::initializer_list<T>>
    {
        using Type = std::remove_cv_t<T>;
    };

    /**
     * @brief Helper alias template for TElementType
     */
    template<typename T>
    using TElementType_T = typename TElementType<T>::Type;

    // ============================================================================
    // Type Aliases for 64-bit Arrays
    // ============================================================================
    //
    // These are the only safe forward declarations since they reference
    // types that have their defaults already defined.

    // Forward declaration of TArray
    // Note: Default argument is defined in ArrayView.h which is the primary forward declaration point
    template<typename T, typename Allocator>
    class TArray;

    template<typename T>
    using TArray64 = TArray<T, FDefaultAllocator64>;

    // Forward declaration of TArrayView (actual class with defaults in ArrayView.h)
    template<typename T, typename SizeType>
    class TArrayView;

    template<typename T>
    using TArrayView64 = TArrayView<T, i64>;

    template<typename T, typename SizeType>
    using TConstArrayView = TArrayView<const T, SizeType>;

    template<typename T>
    using TConstArrayView64 = TConstArrayView<T, i64>;

    // ============================================================================
    // Notes on other containers:
    // ============================================================================
    //
    // TSet, TMap, TMultiMap are TYPE ALIASES, not classes, in OloEngine.
    // They alias to either TCompactSet/TCompactMap or TSparseSet/TSparseMap
    // based on OLO_USE_COMPACT_SET_AS_DEFAULT.
    //
    // Therefore, they cannot be forward declared here - include the full headers:
    // - "OloEngine/Containers/Set.h" for TSet
    // - "OloEngine/Containers/Map.h" for TMap and TMultiMap
    // - "OloEngine/Containers/BitArray.h" for TBitArray
    // - "OloEngine/Containers/SparseArray.h" for TSparseArray
    //
    // The underlying implementations (TCompactSet, TSparseSet, etc.) could be
    // forward declared, but doing so with default template arguments creates
    // "redefinition of default argument" errors when the full header is included.

} // namespace OloEngine
