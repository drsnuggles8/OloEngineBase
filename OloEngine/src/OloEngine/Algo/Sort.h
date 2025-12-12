#pragma once

/**
 * @file Sort.h
 * @brief Sorting algorithm wrappers
 * 
 * Ported from Unreal Engine's Algo/Sort.h
 */

#include "OloEngine/Algo/IntroSort.h"

namespace OloEngine
{
    namespace Algo
    {
        /**
         * Sort a range of elements using its operator<.  The sort is unstable.
         *
         * @param  Range  The range to sort.
         */
        template <typename RangeType>
        OLO_FINLINE void Sort(RangeType&& Range)
        {
            IntroSort(Forward<RangeType>(Range));
        }

        /**
         * Sort a range of elements using a user-defined predicate class.  The sort is unstable.
         *
         * @param  Range      The range to sort.
         * @param  Predicate  A binary predicate object used to specify if one element should precede another.
         */
        template <typename RangeType, typename PredicateType>
        OLO_FINLINE void Sort(RangeType&& Range, PredicateType Pred)
        {
            IntroSort(Forward<RangeType>(Range), MoveTemp(Pred));
        }

        /**
         * Sort a range of elements by a projection using the projection's operator<.  The sort is unstable.
         *
         * @param  Range  The range to sort.
         * @param  Proj   The projection to sort by when applied to the element.
         */
        template <typename RangeType, typename ProjectionType>
        OLO_FINLINE void SortBy(RangeType&& Range, ProjectionType Proj)
        {
            IntroSortBy(Forward<RangeType>(Range), MoveTemp(Proj));
        }

        /**
         * Sort a range of elements by a projection using a user-defined predicate class.  The sort is unstable.
         *
         * @param  Range      The range to sort.
         * @param  Proj       The projection to sort by when applied to the element.
         * @param  Predicate  A binary predicate object, applied to the projection, used to specify if one element should precede another.
         */
        template <typename RangeType, typename ProjectionType, typename PredicateType>
        OLO_FINLINE void SortBy(RangeType&& Range, ProjectionType Proj, PredicateType Pred)
        {
            IntroSortBy(Forward<RangeType>(Range), MoveTemp(Proj), MoveTemp(Pred));
        }

    } // namespace Algo

} // namespace OloEngine
