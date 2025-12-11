#pragma once

/**
 * @file ClosableMpscQueue.h
 * @brief Multi-producer/single-consumer unbounded concurrent queue that can be consumed only once
 * 
 * A lock-free MPSC queue that supports a "close" operation which atomically closes the queue
 * and consumes all items. Once closed, no more items can be enqueued.
 * 
 * Key properties:
 * - Multiple threads can enqueue concurrently
 * - Only a single thread should close/consume
 * - Once closed, the queue cannot be reopened
 * - All items are consumed in FIFO order during close
 * 
 * Ported from Unreal Engine's Containers/ClosableMpscQueue.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/MemoryOps.h"
#include "OloEngine/Templates/UnrealTemplate.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"

#include <atomic>

namespace OloEngine
{
    /**
     * @class TClosableMpscQueue
     * @brief Multi-producer/single-consumer unbounded concurrent queue that can be consumed only once.
     * 
     * This queue uses a lock-free algorithm based on atomic compare-exchange operations.
     * The sentinel node technique avoids special cases for empty queue handling.
     * 
     * Thread safety:
     * - Enqueue: Thread-safe, can be called from multiple threads concurrently
     * - Close: NOT thread-safe with respect to other Close calls; should be called by one thread only
     * - IsClosed: Thread-safe (relaxed read)
     * 
     * @tparam T The element type to store in the queue
     * 
     * Usage example:
     * @code
     *     TClosableMpscQueue<int> Queue;
     *     
     *     // Producer threads
     *     Queue.Enqueue(42);
     *     Queue.Enqueue(123);
     *     
     *     // Consumer thread (single)
     *     Queue.Close([](int Value) {
     *         // Process each value in FIFO order
     *     });
     * @endcode
     */
    template<typename T>
    class TClosableMpscQueue final
    {
    public:
        // Non-copyable
        TClosableMpscQueue(const TClosableMpscQueue&) = delete;
        TClosableMpscQueue& operator=(const TClosableMpscQueue&) = delete;

        [[nodiscard]] TClosableMpscQueue() = default;

        ~TClosableMpscQueue()
        {
            if (m_Head.load(std::memory_order_relaxed) == nullptr)
            {
                return; // Already closed, nothing to clean up
            }

            // Clean up any remaining nodes
            FNode* Tail = m_Sentinel.Next.load(std::memory_order_relaxed);
            while (Tail != nullptr)
            {
                FNode* Next = Tail->Next.load(std::memory_order_relaxed);
                DestructItem(reinterpret_cast<T*>(&Tail->Value));
                delete Tail;
                Tail = Next;
            }
        }

        /**
         * @brief Enqueue an item to the queue
         * 
         * Thread-safe. Can be called from multiple threads concurrently.
         * Uses atomic compare-exchange for lock-free operation.
         * 
         * @tparam ArgTypes Constructor argument types for T
         * @param Args Arguments to forward to T's constructor
         * @return true if the item was successfully enqueued, false if the queue is closed
         */
        template <typename... ArgTypes>
        bool Enqueue(ArgTypes&&... Args)
        {
            FNode* Prev = m_Head.load(std::memory_order_acquire);
            if (Prev == nullptr)
            {
                return false; // Already closed
            }

            FNode* New = new FNode;
            ::new (static_cast<void*>(&New->Value)) T(Forward<ArgTypes>(Args)...);

            // Linearization point: atomically link the new node
            while (!m_Head.compare_exchange_weak(Prev, New, std::memory_order_release) && Prev != nullptr)
            {
                // Retry until success or queue is closed
            }

            if (Prev == nullptr)
            {
                // Queue was closed while we were trying to enqueue
                DestructItem(reinterpret_cast<T*>(&New->Value));
                delete New;
                return false;
            }

            // Link the previous head to the new node
            Prev->Next.store(New, std::memory_order_release);

            return true;
        }

        /**
         * @brief Closes the queue and consumes all items
         * 
         * NOT thread-safe with respect to other Close calls. Should only be called by one thread.
         * After this call, no more items can be enqueued and the queue is permanently closed.
         * 
         * @tparam F Functor type with signature `AnyReturnType (T Value)`
         * @param Consumer Functor that will receive all items in FIFO order
         * @return true if the queue was successfully closed, false if already closed
         */
        template<typename F>
        bool Close(const F& Consumer)
        {
            FNode* Tail = &m_Sentinel;

            // Linearization point: atomically close the queue and capture the head
            // We need to capture the head at the moment of nullifying it because 
            // it may still be unreachable from the tail (producers may be mid-enqueue)
            FNode* const HeadLocal = m_Head.exchange(nullptr, std::memory_order_acq_rel);

            // The queue is closed at this point. The user is free to destroy it.
            // No member variables should be accessed after this point.
            Close_NonMember(HeadLocal, Tail, Consumer);

            return HeadLocal != nullptr;
        }

        /**
         * @brief Check if the queue is closed
         * @return true if the queue has been closed
         */
        [[nodiscard]] bool IsClosed() const
        {
            return m_Head.load(std::memory_order_relaxed) == nullptr;
        }

    private:
        /**
         * @struct FNode
         * @brief Internal node structure for the queue
         */
        struct FNode
        {
            std::atomic<FNode*> Next{ nullptr };
            TTypeCompatibleBytes<T> Value;
        };

        FNode m_Sentinel;
        std::atomic<FNode*> m_Head{ &m_Sentinel };

    private:
        /**
         * @brief Non-member function to consume all items after closing
         * 
         * This is a static function so it doesn't access any member variables,
         * which is important because the queue may be destroyed while this runs.
         * 
         * @param Head The head node at the moment of closing
         * @param Tail The sentinel node (starting point for traversal)
         * @param Consumer The consumer functor
         */
        template<typename F>
        static void Close_NonMember(FNode* Head, FNode* Tail, const F& Consumer)
        {
            if (Head == Tail /* empty */ || Head == nullptr /* already closed */)
            {
                return;
            }

            auto GetNext = [](FNode* Node)
            {
                FNode* Next;
                // Producers may still be updating `Next`, we need to spin until 
                // we detect that the list is fully linked.
                // WARNING: This loop has potential for live-locking if an enqueue 
                // was not completed (e.g., producer thread was running at lower priority)
                do
                {
                    Next = Node->Next.load(std::memory_order_relaxed);
                } while (Next == nullptr);

                return Next;
            };

            // Skip sentinel - do it outside the main loop to avoid unnecessary branching
            Tail = GetNext(Tail);

            auto Consume = [&Consumer](FNode* Node)
            {
                T* ValuePtr = reinterpret_cast<T*>(&Node->Value);
                Consumer(MoveTemp(*ValuePtr));
                DestructItem(ValuePtr);
                delete Node;
            };

            // Consume all nodes from tail to head
            while (Tail != Head)
            {
                FNode* Next = GetNext(Tail);
                Consume(Tail);
                Tail = Next;
            }

            // Consume the final node (head)
            Consume(Head);
        }
    };

} // namespace OloEngine
