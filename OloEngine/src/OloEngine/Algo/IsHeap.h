#pragma once

/**
 * @file IsHeap.h
 * @brief Heap validation algorithm
 * 
 * Ported from Unreal Engine's Algo/IsHeap.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Algo/BinaryHeap.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include "OloEngine/Templates/UnrealTemplate.h"

namespace OloEngine
{
    namespace Algo
    {
        /**
         * Checks if a range is a valid heap.
         *
         * @param Range The range to check.
         * @param Predicate A binary predicate which returns true if the first argument should precede the second.
         * @return true if the range satisfies the heap property.
         */
        template <typename RangeType, typename PredicateType>
        [[nodiscard]] bool IsHeap(RangeType&& Range, PredicateType Predicate)
        {
            auto* Data = GetData(Range);
            auto Num = GetNum(Range);

            for (decltype(Num) Index = 1; Index < Num; ++Index)
            {
                auto ParentIndex = AlgoImpl::HeapGetParentIndex(Index);
                if (Predicate(Data[Index], Data[ParentIndex]))
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * Checks if a range is a valid heap using default comparison.
         *
         * @param Range The range to check.
         * @return true if the range satisfies the heap property.
         */
        template <typename RangeType>
        [[nodiscard]] bool IsHeap(RangeType&& Range)
        {
            using ElementType = std::remove_reference_t<decltype(*GetData(Range))>;
            return IsHeap(std::forward<RangeType>(Range), TLess<ElementType>());
        }

    } // namespace Algo

} // namespace OloEngine
