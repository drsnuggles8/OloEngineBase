#pragma once

/**
 * @file Rotate.h
 * @brief Array rotation algorithm
 *
 * Ported from Unreal Engine's Algo/Rotate.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/UnrealTemplate.h"

namespace OloEngine
{
    template<typename T>
    i32 RotateInternal(T* First, i32 Num, i32 Count)
    {
        if (Count == 0)
        {
            return Num;
        }

        if (Count >= Num)
        {
            return 0;
        }

        T* Iter = First;
        T* Mid = First + Count;
        T* End = First + Num;

        T* OldMid = Mid;
        for (;;)
        {
            Swap(*Iter++, *Mid++);
            if (Mid == End)
            {
                if (Iter == OldMid)
                {
                    return Num - Count;
                }

                Mid = OldMid;
            }
            else if (Iter == OldMid)
            {
                OldMid = Mid;
            }
        }
    }

    namespace Algo
    {
        /**
         * Rotates a given amount of elements from the front of the range to the end of the range.
         *
         * @param  Range  The range to rotate.
         * @param  Count  The number of elements to rotate from the front of the range.
         *
         * @return The new index of the element that was previously at the start of the range.
         */
        template<typename RangeType>
        OLO_FINLINE i32 Rotate(RangeType&& Range, i32 Count)
        {
            return RotateInternal(GetData(Range), GetNum(Range), Count);
        }

    } // namespace Algo

} // namespace OloEngine
