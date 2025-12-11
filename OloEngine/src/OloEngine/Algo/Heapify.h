#pragma once

/**
 * @file Heapify.h
 * @brief Heapify algorithm
 * 
 * Ported from Unreal Engine's Algo/Heapify.h
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
         * Builds an implicit min-heap from a range of elements.
         *
         * @param Range The range to heapify.
         * @param Predicate A binary predicate which returns true if the first argument should precede the second.
         */
        template <typename RangeType, typename PredicateType>
        OLO_FINLINE void Heapify(RangeType&& Range, PredicateType Predicate)
        {
            AlgoImpl::HeapifyInternal(GetData(Range), GetNum(Range), FIdentityFunctor(), Predicate);
        }

        /**
         * Builds an implicit min-heap from a range of elements using default comparison.
         *
         * @param Range The range to heapify.
         */
        template <typename RangeType>
        OLO_FINLINE void Heapify(RangeType&& Range)
        {
            using ElementType = std::remove_reference_t<decltype(*GetData(Range))>;
            Heapify(std::forward<RangeType>(Range), TLess<ElementType>());
        }

    } // namespace Algo

} // namespace OloEngine
