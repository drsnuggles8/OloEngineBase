#pragma once

/**
 * @file StableSort.h
 * @brief Stable sorting algorithm wrappers
 *
 * Ported from Unreal Engine's Algo/StableSort.h
 */

#include "OloEngine/Algo/BinarySearch.h"
#include "OloEngine/Algo/Rotate.h"
#include "OloEngine/Templates/IdentityFunctor.h"
#include "OloEngine/Templates/Invoke.h"
#include "OloEngine/Templates/Less.h"
#include "OloEngine/Templates/UnrealTemplate.h"

namespace OloEngine
{
    template<typename T, typename ProjectionType, typename PredicateType>
    void Merge(T* First, i32 Mid, i32 Num, ProjectionType Projection, PredicateType Predicate)
    {
        i32 AStart = 0;
        i32 BStart = Mid;

        while (AStart < BStart && BStart < Num)
        {
            i32 NewAOffset = UpperBoundInternal(First + AStart, BStart - AStart, Invoke(Projection, First[BStart]), Projection, Predicate);
            AStart += NewAOffset;

            if (AStart >= BStart)
            {
                return;
            }

            i32 NewBOffset = LowerBoundInternal(First + BStart, Num - BStart, Invoke(Projection, First[AStart]), Projection, Predicate);
            RotateInternal(First + AStart, NewBOffset + BStart - AStart, BStart - AStart);
            BStart += NewBOffset;
            AStart += NewBOffset + 1;
        }
    }

    inline constexpr i32 MinMergeSubgroupSize = 2;

    /**
     * Sort elements using user defined projection and predicate classes.  The sort is stable, meaning that the ordering of equal items is preserved.
     * This is the internal sorting function used by the Algo::Sort overloads.
     *
     * @param  First       Pointer to the first element to sort.
     * @param  Num         The number of items to sort.
     * @param  Projection  A projection to apply to each element to get the value to sort by.
     * @param  Predicate   A predicate class which compares two projected elements and returns whether one occurs before the other.
     */
    template<typename T, typename ProjectionType, typename PredicateType>
    void StableSortInternal(T* First, i32 Num, ProjectionType Projection, PredicateType Predicate)
    {
        i32 SubgroupStart = 0;

        if constexpr (MinMergeSubgroupSize > 1)
        {
            if constexpr (MinMergeSubgroupSize > 2)
            {
                // First pass with simple bubble-sort.
                do
                {
                    i32 GroupEnd = SubgroupStart + MinMergeSubgroupSize;
                    if (Num < GroupEnd)
                    {
                        GroupEnd = Num;
                    }
                    do
                    {
                        for (i32 It = SubgroupStart; It < GroupEnd - 1; ++It)
                        {
                            if (Invoke(Predicate, Invoke(Projection, First[It + 1]), Invoke(Projection, First[It])))
                            {
                                Swap(First[It], First[It + 1]);
                            }
                        }
                        GroupEnd--;
                    } while (GroupEnd - SubgroupStart > 1);

                    SubgroupStart += MinMergeSubgroupSize;
                } while (SubgroupStart < Num);
            }
            else
            {
                for (i32 Subgroup = 0; Subgroup < Num; Subgroup += 2)
                {
                    if (Subgroup + 1 < Num && Invoke(Predicate, Invoke(Projection, First[Subgroup + 1]), Invoke(Projection, First[Subgroup])))
                    {
                        Swap(First[Subgroup], First[Subgroup + 1]);
                    }
                }
            }
        }

        i32 SubgroupSize = MinMergeSubgroupSize;
        while (SubgroupSize < Num)
        {
            SubgroupStart = 0;
            do
            {
                i32 MergeNum = SubgroupSize << 1;
                if (Num - SubgroupStart < MergeNum)
                {
                    MergeNum = Num - SubgroupStart;
                }

                Merge(First + SubgroupStart, SubgroupSize, MergeNum, Projection, Predicate);
                SubgroupStart += SubgroupSize << 1;
            } while (SubgroupStart < Num);

            SubgroupSize <<= 1;
        }
    }

    namespace Algo
    {
        /**
         * Sort a range of elements using its operator<.  The sort is stable.
         *
         * @param  Range  The range to sort.
         */
        template<typename RangeType>
        OLO_FINLINE void StableSort(RangeType&& Range)
        {
            StableSortInternal(GetData(Range), GetNum(Range), FIdentityFunctor(), TLess<>());
        }

        /**
         * Sort a range of elements using a user-defined predicate class.  The sort is stable.
         *
         * @param  Range      The range to sort.
         * @param  Predicate  A binary predicate object used to specify if one element should precede another.
         */
        template<typename RangeType, typename PredicateType>
        OLO_FINLINE void StableSort(RangeType&& Range, PredicateType Pred)
        {
            StableSortInternal(GetData(Range), GetNum(Range), FIdentityFunctor(), MoveTemp(Pred));
        }

        /**
         * Sort a range of elements by a projection using the projection's operator<.  The sort is stable.
         *
         * @param  Range  The range to sort.
         * @param  Proj   The projection to sort by when applied to the element.
         */
        template<typename RangeType, typename ProjectionType>
        OLO_FINLINE void StableSortBy(RangeType&& Range, ProjectionType Proj)
        {
            StableSortInternal(GetData(Range), GetNum(Range), MoveTemp(Proj), TLess<>());
        }

        /**
         * Sort a range of elements by a projection using a user-defined predicate class.  The sort is stable.
         *
         * @param  Range      The range to sort.
         * @param  Proj       The projection to sort by when applied to the element.
         * @param  Predicate  A binary predicate object, applied to the projection, used to specify if one element should precede another.
         */
        template<typename RangeType, typename ProjectionType, typename PredicateType>
        OLO_FINLINE void StableSortBy(RangeType&& Range, ProjectionType Proj, PredicateType Pred)
        {
            StableSortInternal(GetData(Range), GetNum(Range), MoveTemp(Proj), MoveTemp(Pred));
        }

    } // namespace Algo

} // namespace OloEngine
