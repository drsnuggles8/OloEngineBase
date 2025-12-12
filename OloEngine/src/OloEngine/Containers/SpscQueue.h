#pragma once

/**
 * @file SpscQueue.h
 * @brief Fast single-producer/single-consumer unbounded concurrent queue
 * 
 * Based on http://www.1024cores.net/home/lock-free-algorithms/queues/unbounded-spsc-queue
 * 
 * Features:
 * - Lock-free
 * - Single producer, single consumer
 * - Unbounded (dynamically allocates nodes)
 * - Doesn't free memory until destruction but recycles consumed items
 * - FIFO ordering
 * 
 * Ported from Unreal Engine's Containers/SpscQueue.h
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
     * @class TSpscQueue
     * @brief Fast single-producer/single-consumer unbounded concurrent queue
     * 
     * @tparam T Element type
     * @tparam AllocatorType Memory allocator (defaults to FMemory)
     */
    template<typename T, typename AllocatorType = FMemory>
    class TSpscQueue final
    {
    public:
        using ElementType = T;

        // Non-copyable
        TSpscQueue(const TSpscQueue&) = delete;
        TSpscQueue& operator=(const TSpscQueue&) = delete;

        /**
         * @brief Construct an empty queue
         */
        [[nodiscard]] TSpscQueue()
        {
            FNode* Node = ::new(AllocatorType::Malloc(sizeof(FNode), alignof(FNode))) FNode;
            m_Tail.store(Node, std::memory_order_relaxed);
            m_Head = m_First = m_TailCopy = Node;
            m_NumElems = 0;
        }

        /**
         * @brief Destructor - destroys all elements and frees all nodes
         */
        ~TSpscQueue()
        {
            FNode* Node = m_First;
            FNode* LocalTail = m_Tail.load(std::memory_order_relaxed);

            // Delete all nodes which are the sentinel or unoccupied
            bool bContinue = false;
            do
            {
                FNode* Next = Node->Next.load(std::memory_order_relaxed);
                bContinue = Node != LocalTail;
                AllocatorType::Free(Node);
                Node = Next;
            } while (bContinue);

            // Delete all nodes which are occupied, destroying the element first
            while (Node != nullptr)
            {
                FNode* Next = Node->Next.load(std::memory_order_relaxed);
                DestructItem(reinterpret_cast<ElementType*>(&Node->Value));
                AllocatorType::Free(Node);
                Node = Next;
            }
        }

        /**
         * @brief Enqueue an item (single producer only - NOT thread-safe for multiple producers)
         * @tparam ArgTypes Constructor argument types
         * @param Args Arguments forwarded to element constructor
         */
        template <typename... ArgTypes>
        void Enqueue(ArgTypes&&... Args)
        {
            FNode* Node = AllocNode();
            ::new(static_cast<void*>(&Node->Value)) ElementType(Forward<ArgTypes>(Args)...);

            m_Head->Next.store(Node, std::memory_order_release);
            m_Head = Node;

            m_NumElems++;
        }

        /**
         * @brief Dequeue an item (single consumer only - NOT thread-safe for multiple consumers)
         * @return Optional containing the dequeued element, or empty if queue is empty
         */
        std::optional<ElementType> Dequeue()
        {
            FNode* LocalTail = m_Tail.load(std::memory_order_relaxed);
            FNode* LocalTailNext = LocalTail->Next.load(std::memory_order_acquire);
            if (LocalTailNext == nullptr)
            {
                return {};
            }

            ElementType* TailNextValue = reinterpret_cast<ElementType*>(&LocalTailNext->Value);
            std::optional<ElementType> Value{ MoveTemp(*TailNextValue) };
            DestructItem(TailNextValue);

            m_Tail.store(LocalTailNext, std::memory_order_release);

            if (Value.has_value())
            {
                m_NumElems--;
            }
            return Value;
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
         * @brief Check if the queue is empty
         * @return true if queue is empty
         */
        [[nodiscard]] bool IsEmpty() const
        {
            return !m_NumElems;
        }

        /**
         * @brief Get the number of elements in the queue
         * @return Number of elements
         */
        [[nodiscard]] i32 Num() const
        {
            return m_NumElems;
        }

        /**
         * @brief Peek at the front element without removing it (single consumer only)
         * @return Pointer to the front element, or nullptr if queue is empty
         * 
         * @note There's no overload with std::optional as it doesn't support references
         */
        [[nodiscard]] ElementType* Peek() const
        {
            FNode* LocalTail = m_Tail.load(std::memory_order_relaxed);
            FNode* LocalTailNext = LocalTail->Next.load(std::memory_order_acquire);

            if (LocalTailNext == nullptr)
            {
                return nullptr;
            }

            return reinterpret_cast<ElementType*>(&LocalTailNext->Value);
        }

    private:
        struct FNode
        {
            std::atomic<FNode*> Next{ nullptr };
            TTypeCompatibleBytes<ElementType> Value;
        };

    public:
        /**
         * @class FIterator
         * @brief Allows the single consumer to iterate the contents of the queue without popping
         * 
         * The single producer may continue to insert items in the queue while the consumer is iterating.
         * These new items may or may not be seen by the consumer, since the consumer might have finished
         * iterating before reaching those new elements.
         */
        struct FIterator
        {
            typename TSpscQueue::FNode* Current;

            FIterator(const TSpscQueue& Queue)
                : Current(Queue.m_Tail.load(std::memory_order_relaxed))
            {
                ++(*this);
            }

            FIterator& operator++ ()
            {
                Current = Current->Next.load(std::memory_order_acquire);
                return *this;
            }

            bool operator== (std::nullptr_t) const
            {
                return Current == nullptr;
            }

            const ElementType& operator* () const
            {
                return *reinterpret_cast<const ElementType*>(&Current->Value);
            }
        };

        FIterator begin() const
        {
            return FIterator(*this);
        }

        std::nullptr_t end() const
        {
            return nullptr;
        }

    private:
        FNode* AllocNode()
        {
            // First tries to allocate node from internal node cache,
            // if attempt fails, allocates node via allocator

            auto AllocFromCache = [this]()
            {
                FNode* Node = m_First;
                m_First = m_First->Next;
                Node->Next.store(nullptr, std::memory_order_relaxed);
                return Node;
            };

            if (m_First != m_TailCopy)
            {
                return AllocFromCache();
            }

            m_TailCopy = m_Tail.load(std::memory_order_acquire);
            if (m_First != m_TailCopy)
            {
                return AllocFromCache();
            }

            return ::new(AllocatorType::Malloc(sizeof(FNode), alignof(FNode))) FNode();
        }

    private:
        // Consumer part
        // Accessed mainly by consumer, infrequently by producer
        std::atomic<FNode*> m_Tail; // tail of the queue

        // Producer part
        // Accessed only by producer
        FNode* m_Head; // head of the queue

        FNode* m_First; // last unused node (tail of node cache)
        FNode* m_TailCopy; // helper (points somewhere between First and Tail)

        std::atomic<i32> m_NumElems;
    };

} // namespace OloEngine
