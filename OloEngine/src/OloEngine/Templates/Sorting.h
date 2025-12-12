#pragma once

/**
 * @file Sorting.h
 * @brief Helper class for dereferencing pointer types in Sort functions
 * 
 * Provides TDereferenceWrapper which automatically dereferences raw pointers
 * when sorting, so predicates receive references to objects rather than pointers.
 * 
 * Ported from Unreal Engine's Templates/Sorting.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include <algorithm>

namespace OloEngine
{
    /**
     * Helper class for dereferencing pointer types in Sort function.
     * 
     * For non-pointer types, it passes through unchanged.
     * For pointer types, it dereferences before passing to the predicate.
     */
    template <typename T, typename PredicateType>
    struct TDereferenceWrapper
    {
        const PredicateType& Predicate;

        TDereferenceWrapper(const PredicateType& InPredicate)
            : Predicate(InPredicate)
        {
        }

        /** Pass through for non-pointer types */
        OLO_FINLINE bool operator()(T& A, T& B)
        {
            return Predicate(A, B);
        }

        OLO_FINLINE bool operator()(const T& A, const T& B) const
        {
            return Predicate(A, B);
        }
    };

    /**
     * Partially specialized version for pointer types.
     * Dereferences pointers before passing to the predicate.
     */
    template <typename T, typename PredicateType>
    struct TDereferenceWrapper<T*, PredicateType>
    {
        const PredicateType& Predicate;

        TDereferenceWrapper(const PredicateType& InPredicate)
            : Predicate(InPredicate)
        {
        }

        /** Dereference pointers */
        OLO_FINLINE bool operator()(T* A, T* B) const
        {
            return Predicate(*A, *B);
        }
    };

    /**
     * Wraps a range into a container like interface to satisfy the GetData and GetNum global functions.
     * We're not using TArrayView since it calls ::Sort creating a circular dependency.
     */
    template <typename T>
    struct TArrayRange
    {
        TArrayRange(T* InPtr, i32 InSize)
            : Begin(InPtr)
            , Size(InSize)
        {
        }

        T* GetData() const { return Begin; }
        i32 Num() const { return Size; }

    private:
        T* Begin;
        i32 Size;
    };

    template <typename T>
    struct TIsContiguousContainer<TArrayRange<T>>
    {
        static constexpr bool Value = true;
    };

} // namespace OloEngine
