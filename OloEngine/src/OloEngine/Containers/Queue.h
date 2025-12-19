// Queue.h - Generic queue container
// Ported from UE5.7 Containers/Queue.h

#pragma once

/**
 * @file Queue.h
 * @brief Template for unbounded lock-free queues with various concurrency modes
 *
 * This template implements an unbounded non-intrusive queue using a lock-free linked
 * list that stores copies of the queued items. The template can operate in three modes:
 * Multiple-producers single-consumer (MPSC), Single-producer single-consumer (SPSC),
 * and Single-threaded.
 *
 * @note Consider using TSpscQueue or TMpscQueue for higher-performance specialized
 *       queue implementations.
 *
 * Ported from Unreal Engine's Containers/Queue.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Templates/UnrealTemplate.h"
#include "OloEngine/HAL/PlatformMisc.h"

#include <atomic>
#include <type_traits>

namespace OloEngine
{
    /**
     * @enum EQueueMode
     * @brief Enumerates concurrent queue modes
     */
    enum class EQueueMode
    {
        /** Multiple-producers, single-consumer queue. */
        Mpsc,

        /** Single-producer, single-consumer queue. */
        Spsc,

        /** Single-threaded - no guarantees of concurrent safety. */
        SingleThreaded,
    };

    /**
     * @class TQueue
     * @brief Template for unbounded lock-free queues
     *
     * The queue is thread-safe in both MPSC and SPSC modes. The Dequeue() method ensures
     * thread-safety by writing it in a way that does not depend on possible instruction
     * reordering on the CPU. The Enqueue() method uses an atomic exchange in
     * multiple-producers scenarios.
     *
     * The queue is not thread-safe in single-threaded mode, as the name suggests.
     *
     * @tparam T The type of items stored in the queue.
     * @tparam Mode The queue mode (single-producer, single-consumer by default).
     */
    template<typename T, EQueueMode Mode = EQueueMode::Spsc>
    class TQueue
    {
      public:
        using FElementType = T;

        /** Non-copyable, non-movable */
        TQueue(const TQueue&) = delete;
        TQueue& operator=(const TQueue&) = delete;
        TQueue(TQueue&&) = delete;
        TQueue& operator=(TQueue&&) = delete;

        /** Default constructor. */
        [[nodiscard]] TQueue()
        {
            m_Head = m_Tail = new TNode();
        }

        /** Destructor. */
        ~TQueue()
        {
            while (m_Tail != nullptr)
            {
                TNode* Node = m_Tail;
                m_Tail = m_Tail->NextNode;
                delete Node;
            }
        }

        /**
         * @brief Removes and returns the item from the tail of the queue.
         *
         * @param OutItem Will hold the returned value.
         * @return true if a value was returned, false if the queue was empty.
         * @note To be called only from consumer thread.
         * @see Empty, Enqueue, IsEmpty, Peek, Pop
         */
        bool Dequeue(FElementType& OutItem)
        {
            TNode* Popped = m_Tail->NextNode;

            if (Popped == nullptr)
            {
                return false;
            }

            OutItem = MoveTemp(Popped->Item);

            TNode* OldTail = m_Tail;
            m_Tail = Popped;
            m_Tail->Item = FElementType();
            delete OldTail;

            return true;
        }

        /**
         * @brief Empty the queue, discarding all items.
         *
         * @note To be called only from consumer thread.
         * @see Dequeue, IsEmpty, Peek, Pop
         */
        void Empty()
        {
            while (Pop())
                ;
        }

        /**
         * @brief Adds an item to the head of the queue (copy version).
         *
         * @param Item The item to add.
         * @return true if the item was added, false otherwise.
         * @note To be called only from producer thread(s).
         * @see Dequeue, Pop
         */
        bool Enqueue(const FElementType& Item)
        {
            TNode* NewNode = new TNode(Item);

            if (NewNode == nullptr)
            {
                return false;
            }

            TNode* OldHead;

            if constexpr (Mode == EQueueMode::Mpsc)
            {
                // Multiple producers: use atomic exchange
                OldHead = m_Head.exchange(NewNode, std::memory_order_acq_rel);

                // Store the pointer to the new node
                TNode* Expected = nullptr;
                while (!OldHead->NextNode.compare_exchange_weak(Expected, NewNode,
                                                                std::memory_order_release, std::memory_order_relaxed))
                {
                    Expected = nullptr;
                }
            }
            else
            {
                // Single producer
                OldHead = m_Head.load(std::memory_order_relaxed);
                m_Head.store(NewNode, std::memory_order_relaxed);

                if constexpr (Mode == EQueueMode::Spsc)
                {
                    FPlatformMisc::MemoryBarrier();
                }

                OldHead->NextNode.store(NewNode, std::memory_order_release);
            }

            return true;
        }

        /**
         * @brief Adds an item to the head of the queue (move version).
         *
         * @param Item The item to add.
         * @return true if the item was added, false otherwise.
         * @note To be called only from producer thread(s).
         * @see Dequeue, Pop
         */
        bool Enqueue(FElementType&& Item)
        {
            TNode* NewNode = new TNode(MoveTemp(Item));

            if (NewNode == nullptr)
            {
                return false;
            }

            TNode* OldHead;

            if constexpr (Mode == EQueueMode::Mpsc)
            {
                // Multiple producers: use atomic exchange
                OldHead = m_Head.exchange(NewNode, std::memory_order_acq_rel);

                // Store the pointer to the new node
                TNode* Expected = nullptr;
                while (!OldHead->NextNode.compare_exchange_weak(Expected, NewNode,
                                                                std::memory_order_release, std::memory_order_relaxed))
                {
                    Expected = nullptr;
                }
            }
            else
            {
                // Single producer
                OldHead = m_Head.load(std::memory_order_relaxed);
                m_Head.store(NewNode, std::memory_order_relaxed);

                if constexpr (Mode == EQueueMode::Spsc)
                {
                    FPlatformMisc::MemoryBarrier();
                }

                OldHead->NextNode.store(NewNode, std::memory_order_release);
            }

            return true;
        }

        /**
         * @brief Checks whether the queue is empty.
         *
         * @return true if the queue is empty, false otherwise.
         * @note To be called only from consumer thread.
         * @see Dequeue, Empty, Peek, Pop
         */
        [[nodiscard]] bool IsEmpty() const
        {
            return (m_Tail->NextNode.load(std::memory_order_acquire) == nullptr);
        }

        /**
         * @brief Peeks at the queue's tail item without removing it.
         *
         * @param OutItem Will hold the peeked at item.
         * @return true if an item was returned, false if the queue was empty.
         * @note To be called only from consumer thread.
         * @see Dequeue, Empty, IsEmpty, Pop
         */
        bool Peek(FElementType& OutItem) const
        {
            TNode* Next = m_Tail->NextNode.load(std::memory_order_acquire);
            if (Next == nullptr)
            {
                return false;
            }

            OutItem = Next->Item;
            return true;
        }

        /**
         * @brief Peek at the queue's tail item without removing it.
         *
         * This version of Peek allows peeking at a queue of items that do not allow
         * copying, such as TUniquePtr.
         *
         * @return Pointer to the item, or nullptr if queue is empty
         */
        [[nodiscard]] FElementType* Peek()
        {
            TNode* Next = m_Tail->NextNode.load(std::memory_order_acquire);
            if (Next == nullptr)
            {
                return nullptr;
            }

            return &Next->Item;
        }

        [[nodiscard]] OLO_FINLINE const FElementType* Peek() const
        {
            return const_cast<TQueue*>(this)->Peek();
        }

        /**
         * @brief Removes the item from the tail of the queue.
         *
         * @return true if a value was removed, false if the queue was empty.
         * @note To be called only from consumer thread.
         * @see Dequeue, Empty, Enqueue, IsEmpty, Peek
         */
        bool Pop()
        {
            TNode* Popped = m_Tail->NextNode.load(std::memory_order_acquire);

            if (Popped == nullptr)
            {
                return false;
            }

            TNode* OldTail = m_Tail;
            m_Tail = Popped;
            m_Tail->Item = FElementType();
            delete OldTail;

            return true;
        }

      private:
        /** Structure for the internal linked list. */
        struct TNode
        {
            /** Holds a pointer to the next node in the list. */
            std::atomic<TNode*> NextNode{ nullptr };

            /** Holds the node's item. */
            FElementType Item;

            /** Default constructor. */
            [[nodiscard]] TNode() = default;

            /** Creates and initializes a new node. */
            [[nodiscard]] explicit TNode(const FElementType& InItem)
                : Item(InItem)
            {
            }

            /** Creates and initializes a new node. */
            [[nodiscard]] explicit TNode(FElementType&& InItem)
                : Item(MoveTemp(InItem))
            {
            }
        };

        /** Holds a pointer to the head of the list. */
        OLO_ALIGN(16)
        std::atomic<TNode*> m_Head OLO_GCC_ALIGN(16);

        /** Holds a pointer to the tail of the list. */
        TNode* m_Tail;
    };

} // namespace OloEngine
