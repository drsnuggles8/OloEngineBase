// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Memory/MemoryOps.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include "OloEngine/Templates/UnrealTemplate.h"
#include <atomic>
#include <type_traits>

namespace OloEngine
{
    /**
     * Result of TConsumeAllMpmcQueue::ProduceItem operation
     */
    enum class EConsumeAllMpmcQueueResult
    {
        HadItems,
        WasEmpty,
    };

    /**
     * Multi-producer/multi-consumer unbounded concurrent queue (implemented as a Stack) that is atomically consumed
     * and is reset to its default empty state. Validated and run through atomic race detector.
     * 
     * This queue is optimized for the consume-all pattern where all items are atomically removed and processed
     * at once, rather than individual pop operations.
     * 
     * @tparam T The element type to store
     * @tparam AllocatorType Allocator class that provides Malloc/Free static methods. Defaults to FMemory.
     */
    template<typename T, typename AllocatorType = FMemory>
    class TConsumeAllMpmcQueue final
    {
    public:
        TConsumeAllMpmcQueue(const TConsumeAllMpmcQueue&) = delete;
        TConsumeAllMpmcQueue& operator=(const TConsumeAllMpmcQueue&) = delete;

        [[nodiscard]] TConsumeAllMpmcQueue() = default;

        ~TConsumeAllMpmcQueue()
        {
            static_assert(std::is_trivially_destructible_v<FNode>);
            if (m_Head.load(std::memory_order_acquire) != nullptr)
            {
                ConsumeAllLifo([](T&&) {});
            }
        }

        /**
         * Push an Item to the Queue.
         * @param Args Arguments to forward to T's constructor
         * @returns EConsumeAllMpmcQueueResult::WasEmpty if the Queue was empty before,
         *          or EConsumeAllMpmcQueueResult::HadItems if there were already items in it.
         */
        template <typename... ArgTypes>
        EConsumeAllMpmcQueueResult ProduceItem(ArgTypes&&... Args)
        {
            FNode* New = ::new(AllocatorType::Malloc(sizeof(FNode), alignof(FNode))) FNode;
            ::new (static_cast<void*>(&New->Item)) T(Forward<ArgTypes>(Args)...);

            // Atomically append to the top of the Queue
            FNode* Prev = m_Head.load(std::memory_order_relaxed);
            do
            {
                New->Next.store(Prev, std::memory_order_relaxed);
            } while (!m_Head.compare_exchange_weak(Prev, New, std::memory_order_acq_rel, std::memory_order_relaxed));
            
            return Prev == nullptr ? EConsumeAllMpmcQueueResult::WasEmpty : EConsumeAllMpmcQueueResult::HadItems;
        }

        /**
         * Take all items off the Queue atomically and consume them in LIFO order
         * @param Consumer Callable that takes T&& as argument
         * @returns EConsumeAllMpmcQueueResult::WasEmpty if queue was empty, HadItems otherwise
         */
        template<typename F>
        EConsumeAllMpmcQueueResult ConsumeAllLifo(const F& Consumer)
        {
            return ConsumeAll<false>(Consumer);
        }

        /**
         * Take all items off the Queue atomically and consume them in FIFO order
         * at the cost of reversing the links once.
         * @param Consumer Callable that takes T&& as argument
         * @returns EConsumeAllMpmcQueueResult::WasEmpty if queue was empty, HadItems otherwise
         */
        template<typename F>
        EConsumeAllMpmcQueueResult ConsumeAllFifo(const F& Consumer)
        {
            return ConsumeAll<true>(Consumer);
        }

        /**
         * Check if the queue is empty
         * @returns true if empty, false otherwise
         */
        [[nodiscard]] bool IsEmpty() const
        {
            return m_Head.load(std::memory_order_relaxed) == nullptr;
        }

    private:
        struct FNode
        {
            std::atomic<FNode*> Next{ nullptr };
            TTypeCompatibleBytes<T> Item;
        };

        std::atomic<FNode*> m_Head{ nullptr };

        template<bool bReverse, typename F>
        inline EConsumeAllMpmcQueueResult ConsumeAll(const F& Consumer)
        {
            // Pop the entire Stack
            FNode* Node = m_Head.exchange(nullptr, std::memory_order_acq_rel);

            if (Node == nullptr)
            {
                return EConsumeAllMpmcQueueResult::WasEmpty;
            }

            if constexpr (bReverse) // Reverse the links to FIFO order if requested
            {
                FNode* Prev = nullptr;
                while (Node)
                {
                    FNode* Tmp = Node;
                    Node = Node->Next.exchange(Prev, std::memory_order_relaxed);
                    Prev = Tmp;
                }
                Node = Prev;
            }

            while (Node) // Consume the nodes of the Queue
            {
                FNode* Next = Node->Next.load(std::memory_order_relaxed);
                T* ValuePtr = Node->Item.GetTypedPtr();
                Consumer(MoveTemp(*ValuePtr));
                DestructItem(ValuePtr);
                AllocatorType::Free(Node);
                Node = Next;
            }

            return EConsumeAllMpmcQueueResult::HadItems;
        }
    };
} // namespace OloEngine
