#pragma once

/**
 * @file StableSort.h
 * @brief Stable sorting algorithm wrappers
 * 
 * Ported from Unreal Engine's Algo/StableSort.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include "OloEngine/Templates/UnrealTemplate.h"
#include <algorithm>

namespace OloEngine
{
    namespace Algo
    {
        /**
         * Stable sort a range of elements using the default comparison (operator<).
         * Stable sort preserves the relative order of equal elements.
         *
         * @param Range The range to sort.
         */
        template <typename RangeType>
        OLO_FINLINE void StableSort(RangeType&& Range)
        {
            std::stable_sort(GetData(Range), GetData(Range) + GetNum(Range));
        }

        /**
         * Stable sort a range of elements using a custom predicate.
         * Stable sort preserves the relative order of equal elements.
         *
         * @param Range The range to sort.
         * @param Predicate A binary predicate which returns true if the first argument should precede the second.
         */
        template <typename RangeType, typename PredicateType>
        OLO_FINLINE void StableSort(RangeType&& Range, PredicateType Predicate)
        {
            std::stable_sort(GetData(Range), GetData(Range) + GetNum(Range), Predicate);
        }

        /**
         * Stable sort elements using pointers and count with custom predicate.
         *
         * @param First Pointer to the first element.
         * @param Num Number of elements.
         * @param Predicate A binary predicate.
         */
        template <typename T, typename PredicateType>
        OLO_FINLINE void StableSort(T* First, i32 Num, PredicateType Predicate)
        {
            std::stable_sort(First, First + Num, Predicate);
        }

    } // namespace Algo

} // namespace OloEngine
