#pragma once

/**
 * @file Heapify.h
 * @brief Heapify algorithm
 * 
 * Ported from Unreal Engine's Algo/Heapify.h
 */

#include "OloEngine/Algo/BinaryHeap.h"
#include "OloEngine/Templates/IdentityFunctor.h"
#include "OloEngine/Templates/Invoke.h"
#include "OloEngine/Templates/Less.h"
#include "OloEngine/Templates/UnrealTemplate.h"

namespace OloEngine
{
    namespace Algo
    {
        /**
         * Builds an implicit min-heap from a range of elements. Assumes < operator is defined
         * for the element type.
         *
         * @param Range	The range to heapify.
         */
        template <typename RangeType>
        OLO_FINLINE void Heapify(RangeType&& Range)
        {
            HeapifyInternal(GetData(Range), GetNum(Range), FIdentityFunctor(), TLess<>());
        }

        /** 
         * Builds an implicit min-heap from a range of elements.
         *
         * @param Range		The range to heapify.
         * @param Predicate	A binary predicate object used to specify if one element should precede another.
         */
        template <typename RangeType, typename PredicateType>
        OLO_FINLINE void Heapify(RangeType&& Range, PredicateType Predicate)
        {
            HeapifyInternal(GetData(Range), GetNum(Range), FIdentityFunctor(), Predicate);
        }
        
        /** 
         * Builds an implicit min-heap from a range of elements. Assumes < operator is defined
         * for the projected element type.
         *
         * @param Range			The range to heapify.
         * @param Projection	The projection to apply to the elements.
         */
        template <typename RangeType, typename ProjectionType>
        OLO_FINLINE void HeapifyBy(RangeType&& Range, ProjectionType Projection)
        {
            HeapifyInternal(GetData(Range), GetNum(Range), Projection, TLess<>());
        }

        /** 
         * Builds an implicit min-heap from a range of elements.
         *
         * @param Range			The range to heapify.
         * @param Projection	The projection to apply to the elements.
         * @param Predicate		A binary predicate object used to specify if one element should precede another.
         */
        template <typename RangeType, typename ProjectionType, typename PredicateType>
        OLO_FINLINE void HeapifyBy(RangeType&& Range, ProjectionType Projection, PredicateType Predicate)
        {
            HeapifyInternal(GetData(Range), GetNum(Range), Projection, Predicate);
        }

    } // namespace Algo

} // namespace OloEngine
