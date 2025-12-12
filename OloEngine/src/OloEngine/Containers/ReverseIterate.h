#pragma once

/**
 * @file ReverseIterate.h
 * @brief Reverse iteration utilities for containers
 * 
 * Provides:
 * - TReversePointerIterator: Reverse iterator for pointer-based ranges
 * - ReverseIterate(): Helper to iterate ranges backwards in range-for loops
 * 
 * Ported from Unreal Engine's Misc/ReverseIterate.h
 */

#include "OloEngine/Core/Base.h"
#include <iterator>

/**
 * Pointer-like reverse iterator type.
 * 
 * This iterator type only supports the minimal functionality needed to support
 * C++ ranged-for syntax. For example, it does not provide post-increment ++ nor ==.
 * 
 * Note: Declared at global scope to match UE's API.
 */
template <typename T>
struct TReversePointerIterator
{
    /**
     * Constructor for TReversePointerIterator.
     *
     * @param InPtr A pointer to the location after the element being referenced.
     *
     * @note Like std::reverse_iterator, this points to one past the element being referenced.
     *       Therefore, for an array of size N starting at P, the begin iterator should be
     *       constructed at (P+N) and the end iterator should be constructed at P.
     */
    [[nodiscard]] constexpr explicit TReversePointerIterator(T* InPtr)
        : Ptr(InPtr)
    {
    }

    [[nodiscard]] constexpr inline T& operator*() const
    {
        return *(Ptr - 1);
    }

    constexpr inline TReversePointerIterator& operator++()
    {
        --Ptr;
        return *this;
    }

    [[nodiscard]] constexpr inline bool operator!=(const TReversePointerIterator& Rhs) const
    {
        return Ptr != Rhs.Ptr;
    }

private:
    T* Ptr;
};

namespace OloEngine::Private
{
    template <typename RangeType>
    struct TReverseIterationAdapter
    {
        [[nodiscard]] constexpr explicit TReverseIterationAdapter(RangeType& InRange)
            : Range(InRange)
        {
        }

        TReverseIterationAdapter(TReverseIterationAdapter&&) = delete;
        TReverseIterationAdapter& operator=(TReverseIterationAdapter&&) = delete;
        TReverseIterationAdapter& operator=(const TReverseIterationAdapter&) = delete;
        TReverseIterationAdapter(const TReverseIterationAdapter&) = delete;
        ~TReverseIterationAdapter() = default;

        OLO_FINLINE constexpr auto begin() const
        {
            using std::rbegin;
            return rbegin(Range);
        }

        OLO_FINLINE constexpr auto end() const
        {
            using std::rend;
            return rend(Range);
        }

    private:
        RangeType& Range;
    };

    template <typename ElementType, std::size_t N>
    struct TReverseIterationAdapter<ElementType(&)[N]>
    {
        [[nodiscard]] constexpr explicit TReverseIterationAdapter(ElementType* InArray)
            : Array(InArray)
        {
        }

        TReverseIterationAdapter(TReverseIterationAdapter&&) = delete;
        TReverseIterationAdapter& operator=(TReverseIterationAdapter&&) = delete;
        TReverseIterationAdapter& operator=(const TReverseIterationAdapter&) = delete;
        TReverseIterationAdapter(const TReverseIterationAdapter&) = delete;
        ~TReverseIterationAdapter() = default;

        [[nodiscard]] inline constexpr TReversePointerIterator<ElementType> begin() const
        {
            return TReversePointerIterator<ElementType>(Array + N);
        }

        [[nodiscard]] inline constexpr TReversePointerIterator<ElementType> end() const
        {
            return TReversePointerIterator<ElementType>(Array);
        }

    private:
        ElementType* Array;
    };
} // namespace OloEngine::Private

/**
 * Allows a range to be iterated backwards. The container must not be modified during iteration,
 * but elements may be. The container must outlive the adapter.
 *
 * @param Range The range to iterate over backwards.
 * @return An adapter which allows reverse iteration over the range.
 *
 * Example:
 * @code
 * TArray<int> Array = { 1, 2, 3, 4, 5 };
 *
 * for (int& Element : ReverseIterate(Array))
 * {
 *     // Iterates: 5, 4, 3, 2, 1
 * }
 * @endcode
 */
template <typename RangeType>
[[nodiscard]] inline constexpr OloEngine::Private::TReverseIterationAdapter<RangeType> ReverseIterate(RangeType&& Range)
{
    return OloEngine::Private::TReverseIterationAdapter<RangeType>(Range);
}
