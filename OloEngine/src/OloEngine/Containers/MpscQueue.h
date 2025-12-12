#pragma once

/**
 * @file MpscQueue.h
 * @brief Fast multi-producer/single-consumer unbounded concurrent queue
 * 
 * Based on http://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue
 * 
 * Features:
 * - Lock-free for producers
 * - Single consumer (not thread-safe for multiple consumers)
 * - Unbounded (dynamically allocates nodes)
 * - FIFO ordering
 * 
 * Ported from Unreal Engine's Containers/MpscQueue.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Memory/MemoryOps.h"
#include "OloEngine/Templates/UnrealTemplate.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"

#include <atomic>
#include <optional>

namespace OloEngine
{
    /**
     * @class TMpscQueue
     * @brief Fast multi-producer/single-consumer unbounded concurrent queue
     * 
     * @tparam T Element type
     * @tparam AllocatorType Memory allocator (defaults to FMemory)
     */
    template<typename T, typename AllocatorType = FMemory>
    class TMpscQueue final
    {
    public:
        using ElementType = T;

        // Non-copyable
        TMpscQueue(const TMpscQueue&) = delete;
        TMpscQueue& operator=(const TMpscQueue&) = delete;

        /**
         * @brief Construct an empty queue
         */
        TMpscQueue()
        {
            FNode* Sentinel = ::new(AllocatorType::Malloc(sizeof(FNode), alignof(FNode))) FNode;
            m_Head.store(Sentinel, std::memory_order_relaxed);
            m_Tail = Sentinel;
        }

        /**
         * @brief Destructor - drains and frees all nodes
         */
        ~TMpscQueue()
        {
            FNode* Next = m_Tail->Next.load(std::memory_order_relaxed);

            // Sentinel's value is already destroyed
            AllocatorType::Free(m_Tail);

            while (Next != nullptr)
            {
                m_Tail = Next;
                Next = m_Tail->Next.load(std::memory_order_relaxed);

                DestructItem(reinterpret_cast<ElementType*>(&m_Tail->Value));
                AllocatorType::Free(m_Tail);
            }
        }

        /**
         * @brief Enqueue an item (thread-safe for multiple producers)
         * @tparam ArgTypes Constructor argument types
         * @param Args Arguments forwarded to element constructor
         */
        template <typename... ArgTypes>
        void Enqueue(ArgTypes&&... Args)
        {
            FNode* New = ::new(AllocatorType::Malloc(sizeof(FNode), alignof(FNode))) FNode;
            ::new (static_cast<void*>(&New->Value)) ElementType(Forward<ArgTypes>(Args)...);

            FNode* Prev = m_Head.exchange(New, std::memory_order_acq_rel);
            Prev->Next.store(New, std::memory_order_release);
        }

        /**
         * @brief Dequeue an item (single consumer only - NOT thread-safe)
         * @return Optional containing the dequeued element, or empty if queue is empty
         */
        std::optional<ElementType> Dequeue()
        {
            FNode* Next = m_Tail->Next.load(std::memory_order_acquire);

            if (Next == nullptr)
            {
                return {};
            }

            ElementType* ValuePtr = reinterpret_cast<ElementType*>(&Next->Value);
            std::optional<ElementType> Res{ MoveTemp(*ValuePtr) };
            DestructItem(ValuePtr);

            AllocatorType::Free(m_Tail); // current sentinel

            m_Tail = Next; // new sentinel
            return Res;
        }

        /**
         * @brief Dequeue an item into an output parameter
         * @param OutElem Output element (only modified if queue is not empty)
         * @return true if an element was dequeued, false if queue was empty
         */
        bool Dequeue(ElementType& OutElem)
        {
            std::optional<ElementType> LocalElement = Dequeue();
            if (LocalElement.has_value())
            {
                OutElem = MoveTempIfPossible(LocalElement.value());
                return true;
            }

            return false;
        }

        /**
         * @brief Peek at the front element without removing it (single consumer only)
         * @return Pointer to the front element, or nullptr if queue is empty
         * 
         * @note There's no overload with std::optional as it doesn't support references
         */
        [[nodiscard]] ElementType* Peek() const
        {
            FNode* Next = m_Tail->Next.load(std::memory_order_acquire);

            if (Next == nullptr)
            {
                return nullptr;
            }

            return reinterpret_cast<ElementType*>(&Next->Value);
        }

        /**
         * @brief Check if the queue is empty
         * @return true if queue is empty
         */
        [[nodiscard]] bool IsEmpty() const
        {
            return m_Tail->Next.load(std::memory_order_acquire) == nullptr;
        }

    private:
        struct FNode
        {
            std::atomic<FNode*> Next{ nullptr };
            TTypeCompatibleBytes<ElementType> Value;
        };

    private:
        std::atomic<FNode*> m_Head; // accessed only by producers
        /*alignas(OLO_PLATFORM_CACHE_LINE_SIZE)*/ FNode* m_Tail; // accessed only by consumer
    };

} // namespace OloEngine
