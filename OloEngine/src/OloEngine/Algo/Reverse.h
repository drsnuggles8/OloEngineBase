#pragma once

/**
 * @file Reverse.h
 * @brief Reverse algorithm for arrays and containers
 *
 * Ported from Unreal Engine's Algo/Reverse.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/UnrealTemplate.h" // for Swap

namespace OloEngine
{
    namespace AlgoImpl
    {
        template <typename T>
        void Reverse(T* Array, i32 ArraySize)
        {
            for (i32 i = 0, i2 = ArraySize - 1; i < ArraySize / 2 /*rounding down*/; ++i, --i2)
            {
                Swap(Array[i], Array[i2]);
            }
        }
    }

    namespace Algo
    {
        /**
         * Reverses a range
         *
         * @param  Array  The array to reverse.
         */
        template <typename T, i32 ArraySize>
        void Reverse(T (&Array)[ArraySize])
        {
            return AlgoImpl::Reverse((T*)Array, ArraySize);
        }

        /**
         * Reverses a range
         *
         * @param  Array      A pointer to the array to reverse
         * @param  ArraySize  The number of elements in the array.
         */
        template <typename T>
        void Reverse(T* Array, i32 ArraySize)
        {
            return AlgoImpl::Reverse(Array, ArraySize);
        }

        /**
         * Reverses a range
         *
         * @param  Container  The container to reverse
         */
        template <typename ContainerType>
        void Reverse(ContainerType&& Container)
        {
            return AlgoImpl::Reverse(Container.GetData(), Container.Num());
        }
    }

} // namespace OloEngine

