#pragma once

/**
 * @file Sort.h
 * @brief Sorting algorithm wrappers
 * 
 * Ported from Unreal Engine's Algo/Sort.h
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
         * Sort a range of elements using the default comparison (operator<).
         *
         * @param Range The range to sort.
         */
        template <typename RangeType>
        OLO_FINLINE void Sort(RangeType&& Range)
        {
            std::sort(GetData(Range), GetData(Range) + GetNum(Range));
        }

        /**
         * Sort a range of elements using a custom predicate.
         *
         * @param Range The range to sort.
         * @param Predicate A binary predicate which returns true if the first argument should precede the second.
         */
        template <typename RangeType, typename PredicateType>
        OLO_FINLINE void Sort(RangeType&& Range, PredicateType Predicate)
        {
            std::sort(GetData(Range), GetData(Range) + GetNum(Range), Predicate);
        }

        /**
         * Sort elements using pointers and count with custom predicate.
         *
         * @param First Pointer to the first element.
         * @param Num Number of elements.
         * @param Predicate A binary predicate.
         */
        template <typename T, typename PredicateType>
        OLO_FINLINE void Sort(T* First, i32 Num, PredicateType Predicate)
        {
            std::sort(First, First + Num, Predicate);
        }

    } // namespace Algo

} // namespace OloEngine
