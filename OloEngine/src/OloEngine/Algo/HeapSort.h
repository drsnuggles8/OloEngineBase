#pragma once

/**
 * @file HeapSort.h
 * @brief Heap sort algorithm
 * 
 * Ported from Unreal Engine's Algo/HeapSort.h
 */

#include "OloEngine/Algo/BinaryHeap.h"
#include "OloEngine/Templates/IdentityFunctor.h"
#include "OloEngine/Templates/Less.h"
#include "OloEngine/Templates/UnrealTemplate.h"

namespace OloEngine
{
    namespace Algo
    {
        /**
         * Performs heap sort on the elements. Assumes < operator is defined for the element type.
         *
         * @param Range		The range to sort.
         */
        template <typename RangeType>
        OLO_FINLINE void HeapSort(RangeType&& Range)
        {
            HeapSortInternal(GetData(Range), GetNum(Range), FIdentityFunctor(), TLess<>());
        }

        /**
         * Performs heap sort on the elements.
         *
         * @param Range		The range to sort.
         * @param Predicate	A binary predicate object used to specify if one element should precede another.
         */
        template <typename RangeType, typename PredicateType>
        OLO_FINLINE void HeapSort(RangeType&& Range, PredicateType Predicate)
        {
            HeapSortInternal(GetData(Range), GetNum(Range), FIdentityFunctor(), MoveTemp(Predicate));
        }

        /**
         * Performs heap sort on the elements. Assumes < operator is defined for the projected element type.
         *
         * @param Range		The range to sort.
         * @param Projection	The projection to sort by when applied to the element.
         */
        template <typename RangeType, typename ProjectionType>
        OLO_FINLINE void HeapSortBy(RangeType&& Range, ProjectionType Projection)
        {
            HeapSortInternal(GetData(Range), GetNum(Range), MoveTemp(Projection), TLess<>());
        }

        /**
         * Performs heap sort on the elements.
         *
         * @param Range		The range to sort.
         * @param Projection	The projection to sort by when applied to the element.
         * @param Predicate	A binary predicate object, applied to the projection, used to specify if one element should precede another.
         */
        template <typename RangeType, typename ProjectionType, typename PredicateType>
        OLO_FINLINE void HeapSortBy(RangeType&& Range, ProjectionType Projection, PredicateType Predicate)
        {
            HeapSortInternal(GetData(Range), GetNum(Range), MoveTemp(Projection), MoveTemp(Predicate));
        }

    } // namespace Algo

} // namespace OloEngine
