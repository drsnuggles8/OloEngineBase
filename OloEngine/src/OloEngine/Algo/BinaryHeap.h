#pragma once

/**
 * @file BinaryHeap.h
 * @brief Binary heap algorithms implementation
 * 
 * Provides the internal heap operations used by TArray's heap methods.
 * 
 * Ported from Unreal Engine's Algo/Impl/BinaryHeap.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/IdentityFunctor.h"
#include "OloEngine/Templates/Projection.h"
#include "OloEngine/Templates/ReversePredicate.h"
#include <type_traits>

namespace OloEngine
{
    namespace AlgoImpl
    {
        /**
         * Gets the index of the left child of node at Index.
         *
         * @param	Index Node for which the left child index is to be returned.
         * @returns	Index of the left child.
         */
        template <typename IndexType>
        OLO_FINLINE IndexType HeapGetLeftChildIndex(IndexType Index)
        {
            return Index * 2 + 1;
        }

        /** 
         * Checks if node located at Index is a leaf or not.
         *
         * @param	Index Node index.
         * @returns	true if node is a leaf, false otherwise.
         */
        template <typename IndexType>
        OLO_FINLINE bool HeapIsLeaf(IndexType Index, IndexType Count)
        {
            return HeapGetLeftChildIndex(Index) >= Count;
        }

        /**
         * Gets the parent index for node at Index.
         *
         * @param	Index node index.
         * @returns	Parent index.
         */
        template <typename IndexType>
        OLO_FINLINE IndexType HeapGetParentIndex(IndexType Index)
        {
            return (Index - 1) / 2;
        }

        /**
         * Fixes a possible violation of order property between node at Index and a child.
         *
         * @param	Heap		Pointer to the first element of a binary heap.
         * @param	Index		Node index.
         * @param	Count		Size of the heap.
         * @param	InProj		The projection to apply to the elements.
         * @param	Predicate	A binary predicate object used to specify if one element should precede another.
         */
        template <typename RangeValueType, typename IndexType, typename ProjectionType, typename PredicateType>
        inline void HeapSiftDown(RangeValueType* Heap, IndexType Index, const IndexType Count, const ProjectionType& InProj, const PredicateType& Predicate)
        {
            auto&& Proj = Projection(InProj);
            while (!HeapIsLeaf(Index, Count))
            {
                const IndexType LeftChildIndex = HeapGetLeftChildIndex(Index);
                const IndexType RightChildIndex = LeftChildIndex + 1;

                IndexType MinChildIndex = LeftChildIndex;
                if (RightChildIndex < Count)
                {
                    MinChildIndex = Predicate(Proj(Heap[LeftChildIndex]), Proj(Heap[RightChildIndex])) ? LeftChildIndex : RightChildIndex;
                }

                if (!Predicate(Proj(Heap[MinChildIndex]), Proj(Heap[Index])))
                {
                    break;
                }

                Swap(Heap[Index], Heap[MinChildIndex]);
                Index = MinChildIndex;
            }
        }

        /**
         * Fixes a possible violation of order property between node at NodeIndex and a parent.
         *
         * @param	Heap		Pointer to the first element of a binary heap.
         * @param	RootIndex	How far to go up?
         * @param	NodeIndex	Node index.
         * @param	InProj		The projection to apply to the elements.
         * @param	Predicate	A binary predicate object used to specify if one element should precede another.
         *
         * @return	The new index of the node that was at NodeIndex
         */
        template <typename RangeValueType, typename IndexType, typename ProjectionType, typename PredicateType>
        inline IndexType HeapSiftUp(RangeValueType* Heap, IndexType RootIndex, IndexType NodeIndex, const ProjectionType& InProj, const PredicateType& Predicate)
        {
            auto&& Proj = Projection(InProj);
            while (NodeIndex > RootIndex)
            {
                IndexType ParentIndex = HeapGetParentIndex(NodeIndex);
                if (!Predicate(Proj(Heap[NodeIndex]), Proj(Heap[ParentIndex])))
                {
                    break;
                }

                Swap(Heap[NodeIndex], Heap[ParentIndex]);
                NodeIndex = ParentIndex;
            }

            return NodeIndex;
        }

        /** 
         * Builds an implicit min-heap from a range of elements.
         * This is the internal function used by Heapify overrides.
         *
         * @param	First		pointer to the first element to heapify
         * @param	Num			the number of items to heapify
         * @param	Proj		The projection to apply to the elements.
         * @param	Predicate	A binary predicate object used to specify if one element should precede another.
         */
        template <typename RangeValueType, typename IndexType, typename ProjectionType, typename PredicateType>
        inline void HeapifyInternal(RangeValueType* First, IndexType Num, ProjectionType Proj, PredicateType Predicate)
        {
            if constexpr (std::is_signed_v<IndexType>)
            {
                OLO_CORE_ASSERT(Num >= 0, "Algo::HeapifyInternal called with negative count");
            }

            if (Num == 0)
            {
                return;
            }

            IndexType Index = HeapGetParentIndex(Num - 1);
            for (;;)
            {
                HeapSiftDown(First, Index, Num, Proj, Predicate);
                if (Index == 0)
                {
                    return;
                }
                --Index;
            }
        }

        /**
         * Performs heap sort on the elements.
         * This is the internal sorting function used by HeapSort overrides.
         *
         * @param	First		pointer to the first element to sort
         * @param	Num			the number of elements to sort
         * @param	Proj		The projection to apply to the elements.
         * @param	Predicate	A binary predicate object used to specify if one element should precede another.
         */
        template <typename RangeValueType, typename IndexType, typename ProjectionType, typename PredicateType>
        void HeapSortInternal(RangeValueType* First, IndexType Num, ProjectionType Proj, PredicateType Predicate)
        {
            if constexpr (std::is_signed_v<IndexType>)
            {
                OLO_CORE_ASSERT(Num >= 0, "Algo::HeapSortInternal called with negative count");
            }

            if (Num == 0)
            {
                return;
            }

            TReversePredicate<PredicateType> ReversePredicateWrapper(Predicate); // Reverse the predicate to build a max-heap instead of a min-heap
            HeapifyInternal(First, Num, Proj, ReversePredicateWrapper);

            for (IndexType Index = Num - 1; Index > 0; Index--)
            {
                Swap(First[0], First[Index]);
                HeapSiftDown(First, (IndexType)0, Index, Proj, ReversePredicateWrapper);
            }
        }

    } // namespace AlgoImpl

} // namespace OloEngine
