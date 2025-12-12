#pragma once

/**
 * @file UnrealTemplate.h
 * @brief Core template utilities for OloEngine
 * 
 * Provides UE-style template utilities:
 * - MoveTemp (std::move with compile-time safety)
 * - MoveTempIfPossible (std::move equivalent)
 * - Forward (std::forward equivalent)
 * - CopyTemp / CopyTempIfNecessary
 * - Swap
 * 
 * Ported from Unreal Engine's Templates/UnrealTemplate.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"  // For TIsContiguousContainer
#include <type_traits>

namespace OloEngine
{
    // ========================================================================
    // GetData / GetNum - Generic container accessors
    // ========================================================================

    namespace Private
    {
        /**
         * @brief Implementation for GetData - containers with .GetData() member
         */
        template <typename T>
        [[nodiscard]] constexpr auto GetDataImpl(T&& Container) -> decltype(Container.GetData())
        {
            return Container.GetData();
        }

        /**
         * @brief Implementation for GetData - C-style arrays
         */
        template <typename T>
            requires (std::is_pointer_v<decltype(+std::declval<T>())>)
        [[nodiscard]] constexpr auto GetDataImpl(T&& Container)
        {
            return +Container;
        }

        /**
         * @brief Implementation for GetNum - containers with .Num() member
         */
        template <typename T>
        [[nodiscard]] constexpr auto GetNumImpl(const T& Container) -> decltype(Container.Num())
        {
            return Container.Num();
        }

        /**
         * @brief Implementation for GetNum - C-style arrays
         */
        template <typename T>
            requires (std::is_pointer_v<decltype(+std::declval<T>())>)
        [[nodiscard]] constexpr auto GetNumImpl(const T& Container) -> sizet
        {
            return sizeof(Container) / sizeof(*Container);
        }
    } // namespace Private

    /**
     * @brief Generically gets the data pointer of a contiguous container
     * @param Container The container to get data from
     * @returns Pointer to the first element
     */
    template <typename T>
        requires (TIsContiguousContainer<T>::Value)
    [[nodiscard]] constexpr auto GetData(T&& Container) -> decltype(Private::GetDataImpl(static_cast<T&&>(Container)))
    {
        return Private::GetDataImpl(static_cast<T&&>(Container));
    }

    /**
     * @brief Gets the data pointer from an initializer list
     * @param List The initializer list
     * @returns Pointer to the first element
     */
    template <typename T>
    [[nodiscard]] constexpr const T* GetData(std::initializer_list<T> List)
    {
        return List.begin();
    }

    /**
     * @brief Generically gets the number of items in a contiguous container
     * @param Container The container to get size from
     * @returns Number of elements
     */
    template <typename T>
        requires (TIsContiguousContainer<T>::Value)
    [[nodiscard]] constexpr auto GetNum(const T& Container) -> decltype(Private::GetNumImpl(Container))
    {
        return Private::GetNumImpl(Container);
    }

    /**
     * @brief Gets the number of items in an initializer list
     * 
     * The return type is i32 for compatibility with other code in the engine.
     * Realistically, an initializer list should not exceed the limits of i32.
     * 
     * @param List The initializer list
     * @returns Number of elements
     */
    template <typename T>
    [[nodiscard]] constexpr i32 GetNum(std::initializer_list<T> List)
    {
        return static_cast<i32>(List.size());
    }
    // ========================================================================
    // TRemovePointer (needed before MoveTemp)
    // ========================================================================

    /**
     * Removes one level of pointer from a type.
     *
     * TRemovePointer<      int32  >::Type == int32
     * TRemovePointer<      int32* >::Type == int32
     * TRemovePointer<      int32**>::Type == int32*
     * TRemovePointer<const int32* >::Type == const int32
     */
    template <typename T> struct TRemovePointer     { using Type = T; };
    template <typename T> struct TRemovePointer<T*> { using Type = T; };

    template <typename T>
    using TRemovePointer_T = typename TRemovePointer<T>::Type;

    // ========================================================================
    // MoveTemp - UE's stricter std::move
    // ========================================================================

    /**
     * MoveTemp will cast a reference to an rvalue reference.
     * This is UE's equivalent of std::move except that it will not compile when passed an rvalue or
     * const object, because we would prefer to be informed when MoveTemp will have no effect.
     */
    template <typename T>
    [[nodiscard]] constexpr std::remove_reference_t<T>&& MoveTemp(T&& Obj) noexcept
    {
        using CastType = std::remove_reference_t<T>;

        // Validate that we're not being passed an rvalue or a const object - the former is redundant, the latter is almost certainly a mistake
        static_assert(std::is_lvalue_reference_v<T>, "MoveTemp called on an rvalue");
        static_assert(!std::is_same_v<CastType&, const CastType&>, "MoveTemp called on a const object");

        return static_cast<CastType&&>(Obj);
    }

    // ========================================================================
    // MoveTempIfPossible - UE's std::move equivalent
    // ========================================================================

    /**
     * MoveTempIfPossible will cast a reference to an rvalue reference.
     * This is UE's equivalent of std::move. It doesn't static assert like MoveTemp, because it is useful in
     * templates or macros where it's not obvious what the argument is, but you want to take advantage of move semantics
     * where you can but not stop compilation.
     */
    template <typename T>
    [[nodiscard]] constexpr std::remove_reference_t<T>&& MoveTempIfPossible(T&& Obj) noexcept
    {
        using CastType = std::remove_reference_t<T>;
        return static_cast<CastType&&>(Obj);
    }

    // ========================================================================
    // CopyTemp - Enforce copy creation
    // ========================================================================

    /**
     * CopyTemp will enforce the creation of a prvalue which can bind to rvalue reference parameters.
     * Unlike MoveTemp, a source lvalue will never be modified. (i.e. a copy will always be made)
     * There is no std:: equivalent, though there is the exposition function std::decay-copy:
     * https://eel.is/c++draft/expos.only.func
     * CopyTemp(<rvalue>) is regarded as an error and will not compile, similarly to how MoveTemp(<rvalue>)
     * does not compile, and CopyTempIfNecessary should be used instead when the nature of the
     * argument is not known in advance.
     */
    template <typename T>
    [[nodiscard]] T CopyTemp(T& Val)
    {
        return const_cast<const T&>(Val);
    }

    template <typename T>
    [[nodiscard]] T CopyTemp(const T& Val)
    {
        return Val;
    }

    // ========================================================================
    // CopyTempIfNecessary - std::decay_copy equivalent
    // ========================================================================

    /**
     * CopyTempIfNecessary will enforce the creation of a prvalue.
     * This is UE's equivalent of the exposition std::decay-copy:
     * https://eel.is/c++draft/expos.only.func
     * It doesn't static assert like CopyTemp, because it is useful in
     * templates or macros where it's not obvious what the argument is, but you want to
     * create a prvalue without stopping compilation.
     */
    template <typename T>
    [[nodiscard]] constexpr std::decay_t<T> CopyTempIfNecessary(T&& Val)
    {
        return static_cast<T&&>(Val);
    }

    // ========================================================================
    // Forward - UE's std::forward equivalent
    // ========================================================================

    /**
     * Forward will cast a reference to an rvalue reference.
     * This is UE's equivalent of std::forward.
     */
    template <typename T>
    [[nodiscard]] constexpr T&& Forward(std::remove_reference_t<T>& Obj) noexcept
    {
        return static_cast<T&&>(Obj);
    }

    template <typename T>
    [[nodiscard]] constexpr T&& Forward(std::remove_reference_t<T>&& Obj) noexcept
    {
        return static_cast<T&&>(Obj);
    }

    // ========================================================================
    // Swap - Bitwise swap for trivially relocatable types
    // ========================================================================

    /**
     * Swap two values. Assumes the types are trivially relocatable.
     */
    template <typename T>
    constexpr void Swap(T& A, T& B)
    {
        // std::is_swappable isn't correct here, because we allow bitwise swapping of types containing e.g. const and reference members,
        // but we don't want to allow swapping of types which are non-movable. We also allow bitwise swapping of arrays, so
        // extents should be removed first.
        static_assert(std::is_move_constructible_v<std::remove_all_extents_t<T>>, "Cannot swap non-movable types");

        T Temp = MoveTempIfPossible(A);
        A = MoveTempIfPossible(B);
        B = MoveTempIfPossible(Temp);
    }

    // ========================================================================
    // Exchange - Atomic-like exchange operation
    // ========================================================================

    /**
     * Exchange will set 'Value' to 'NewValue' and return the old value.
     */
    template <typename T, typename U = T>
    [[nodiscard]] T Exchange(T& Value, U&& NewValue)
    {
        T OldValue = MoveTempIfPossible(Value);
        Value = Forward<U>(NewValue);
        return OldValue;
    }

    // ========================================================================
    // TKeyValuePair - Simple key/value pair helper
    // ========================================================================

    /**
     * Helper class to make it easy to use key/value pairs with a container.
     */
    template <typename KeyType, typename ValueType>
    struct TKeyValuePair
    {
        TKeyValuePair(const KeyType& InKey, const ValueType& InValue)
            : Key(InKey), Value(InValue)
        {
        }

        TKeyValuePair(const KeyType& InKey)
            : Key(InKey)
        {
        }

        TKeyValuePair() = default;

        bool operator==(const TKeyValuePair& Other) const
        {
            return Key == Other.Key;
        }

        bool operator!=(const TKeyValuePair& Other) const
        {
            return Key != Other.Key;
        }

        bool operator<(const TKeyValuePair& Other) const
        {
            return Key < Other.Key;
        }

        OLO_FINLINE bool operator()(const TKeyValuePair& A, const TKeyValuePair& B) const
        {
            return A.Key < B.Key;
        }

        KeyType Key;
        ValueType Value;
    };

    // ========================================================================
    // Misc Template Utilities
    // ========================================================================

    /**
     * FNoncopyable - Base class for non-copyable types
     */
    struct FNoncopyable
    {
        FNoncopyable() = default;
        FNoncopyable(const FNoncopyable&) = delete;
        FNoncopyable& operator=(const FNoncopyable&) = delete;
    };

    /**
     * TGuardValue - RAII helper to save and restore a value
     * 
     * Useful to make sure a value is reverted back to its original value when the current scope exits.
     */
    template <typename RefType, typename AssignedType = RefType>
    struct TGuardValue : private FNoncopyable
    {
        [[nodiscard]] TGuardValue(RefType& ReferenceValue, const AssignedType& NewValue)
            : RefValue(ReferenceValue)
            , OriginalValue(ReferenceValue)
        {
            RefValue = NewValue;
        }

        ~TGuardValue()
        {
            RefValue = OriginalValue;
        }

        /**
         * Provides read-only access to the original value of the data being tracked by this struct
         * @return a const reference to the original data value
         */
        OLO_FINLINE const AssignedType& GetOriginalValue() const
        {
            return OriginalValue;
        }

    private:
        RefType& RefValue;
        AssignedType OriginalValue;
    };

    /**
     * TScopeCounter - Commonly used to make sure a value is incremented, and then decremented anyway the function can terminate.
     * 
     * Usage:
     *     TScopeCounter<int32> BeginProcessing(ProcessingCount); // increments ProcessingCount, and decrements it in the dtor
     */
    template <typename Type>
    struct TScopeCounter : private FNoncopyable
    {
        [[nodiscard]] explicit TScopeCounter(Type& ReferenceValue)
            : RefValue(ReferenceValue)
        {
            ++RefValue;
        }

        ~TScopeCounter()
        {
            --RefValue;
        }

    private:
        Type& RefValue;
    };

} // namespace OloEngine
