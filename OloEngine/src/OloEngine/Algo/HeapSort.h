#pragma once

/**
 * @file HeapSort.h
 * @brief Heap sort algorithm
 * 
 * Ported from Unreal Engine's Algo/HeapSort.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Algo/BinaryHeap.h"
#include "OloEngine/Templates/IdentityFunctor.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include "OloEngine/Templates/UnrealTemplate.h"

namespace OloEngine
{
    namespace Algo
    {
        /**
         * Performs heap sort on a range of elements.
         *
         * @param Range The range to sort.
         * @param Predicate A binary predicate which returns true if the first argument should precede the second.
         */
        template <typename RangeType, typename PredicateType>
        OLO_FINLINE void HeapSort(RangeType&& Range, PredicateType Predicate)
        {
            AlgoImpl::HeapSortInternal(GetData(Range), GetNum(Range), FIdentityFunctor(), Predicate);
        }

        /**
         * Performs heap sort on a range of elements using default comparison.
         *
         * @param Range The range to sort.
         */
        template <typename RangeType>
        OLO_FINLINE void HeapSort(RangeType&& Range)
        {
            using ElementType = std::remove_reference_t<decltype(*GetData(Range))>;
            HeapSort(std::forward<RangeType>(Range), TLess<ElementType>());
        }

    } // namespace Algo

} // namespace OloEngine
