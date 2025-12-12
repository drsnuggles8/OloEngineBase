#pragma once

/**
 * @file FAAArrayQueue.h
 * @brief Fetch-And-Add Array Queue - A lock-free MPMC queue
 * 
 * Copyright (c) 2014-2016, Pedro Ramalhete, Andreia Correia
 * All rights reserved.
 * BSD 3-Clause License
 * 
 * Each node has one array but we don't search for a vacant entry. Instead, we
 * use FAA to obtain an index in the array, for enqueueing or dequeuing.
 * 
 * Features:
 * - Lock-free for both enqueue and dequeue
 * - Multi-producer, multi-consumer (MPMC)
 * - Uses hazard pointers for safe memory reclamation
 * - Linearizable consistency
 * 
 * Each entry in the array may contain one of three possible values:
 * - A valid item that has been enqueued
 * - nullptr, which means no item has yet been enqueued in that position
 * - taken, a special value that means there was an item but it has been dequeued
 * 
 * Algorithm:
 * - Enqueue: FAA + CAS(null,item)
 * - Dequeue: FAA + CAS(item,taken)
 * 
 * Uncontended enqueue: 1 FAA + 1 CAS + 1 HP
 * Uncontended dequeue: 1 FAA + 1 CAS + 1 HP
 * 
 * Based on Michael-Scott queue algorithm with FAA optimization.
 * 
 * @see http://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf
 * @see http://web.cecs.pdx.edu/~walpole/class/cs510/papers/11.pdf
 * 
 * Ported from Unreal Engine's Experimental/Containers/FAAArrayQueue.h
 * 
 * @author Pedro Ramalhete
 * @author Andreia Correia
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/HazardPointer.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Templates/UnrealTemplate.h"

#include <atomic>
#include <thread>

namespace OloEngine
{
    /**
     * @class FAAArrayQueue
     * @brief Lock-free multi-producer/multi-consumer unbounded queue
     * 
     * @tparam T Element type (must be a pointer type or convertible to/from pointer)
     */
    template<typename T>
    class FAAArrayQueue 
    {
        static constexpr i64 BUFFER_SIZE = 1024;

    private:
        struct FNode 
        {
            std::atomic<i32> DeqIdx;
            std::atomic<T*> Items[BUFFER_SIZE];
            std::atomic<i32> EnqIdx;
            std::atomic<FNode*> Next;

            // Start with the first entry pre-filled and EnqIdx at 1
            FNode(T* Item) 
                : DeqIdx{0}
                , EnqIdx{1}
                , Next{nullptr} 
            {
                Items[0].store(Item, std::memory_order_relaxed);
                for (i64 i = 1; i < BUFFER_SIZE; i++) 
                {
                    Items[i].store(nullptr, std::memory_order_relaxed);
                }
            }

            bool CasNext(FNode* Cmp, FNode* Val) 
            {
                return Next.compare_exchange_strong(Cmp, Val);
            }
        };

        bool CasTail(FNode* Cmp, FNode* Val) 
        {
            return m_Tail.compare_exchange_strong(Cmp, Val);
        }

        bool CasHead(FNode* Cmp, FNode* Val) 
        {
            return m_Head.compare_exchange_strong(Cmp, Val);
        }

        // Pointers to head and tail of the list
        alignas(OLO_PLATFORM_CACHE_LINE_SIZE * 2) std::atomic<FNode*> m_Head;
        alignas(OLO_PLATFORM_CACHE_LINE_SIZE * 2) std::atomic<FNode*> m_Tail;

        FHazardPointerCollection m_Hazards;

        inline static T* GetTakenPointer()
        {
            return reinterpret_cast<T*>(~uptr(0));
        }

    public:
        /**
         * @brief Construct an empty queue
         */
        FAAArrayQueue()
        {
            FNode* SentinelNode = new FNode(nullptr);
            SentinelNode->EnqIdx.store(0, std::memory_order_relaxed);
            m_Head.store(SentinelNode, std::memory_order_relaxed);
            m_Tail.store(SentinelNode, std::memory_order_relaxed);
        }

        /**
         * @brief Destructor - drains the queue and deletes the last node
         */
        ~FAAArrayQueue() 
        {
            while (Dequeue() != nullptr); // Drain the queue
            delete m_Head.load();          // Delete the last node
        }

        // Non-copyable
        FAAArrayQueue(const FAAArrayQueue&) = delete;
        FAAArrayQueue& operator=(const FAAArrayQueue&) = delete;

        /**
         * @class EnqueueHazard
         * @brief Cached hazard pointer for enqueue operations
         * 
         * Keeping a hazard pointer across multiple enqueue operations avoids
         * the overhead of acquiring/releasing hazard slots repeatedly.
         */
        class EnqueueHazard : private THazardPointer<FNode, true>
        {
            friend class FAAArrayQueue<T>;
            inline EnqueueHazard(std::atomic<FNode*>& Hazard, FHazardPointerCollection& Collection) 
                : THazardPointer<FNode, true>(Hazard, Collection)
            {}

        public:
            inline EnqueueHazard() = default;
            inline EnqueueHazard(EnqueueHazard&& Hazard) 
                : THazardPointer<FNode, true>(MoveTemp(static_cast<THazardPointer<FNode, true>&>(Hazard)))
            {}

            inline EnqueueHazard& operator=(EnqueueHazard&& Other)
            {
                static_cast<THazardPointer<FNode, true>&>(*this) = MoveTemp(static_cast<THazardPointer<FNode, true>&>(Other));
                return *this;
            }
        };

    private:
        template<typename HazardType>
        void EnqueueInternal(T* Item, HazardType& Hazard) 
        {
            OLO_CORE_ASSERT(Item != nullptr, "Cannot enqueue null item");
            while (true) 
            {
                FNode* LocalTail = Hazard.Get();
                const i32 Idx = LocalTail->EnqIdx.fetch_add(1);
                if (Idx > BUFFER_SIZE - 1) 
                { 
                    // This node is full
                    if (LocalTail != m_Tail.load())
                    {
                        continue;
                    }
                    FNode* LocalNext = LocalTail->Next.load();
                    if (LocalNext == nullptr) 
                    {
                        FNode* NewNode = new FNode(Item);
                        if (LocalTail->CasNext(nullptr, NewNode)) 
                        {
                            CasTail(LocalTail, NewNode);
                            Hazard.Retire();
                            return;
                        }
                        delete NewNode;
                    } 
                    else 
                    {
                        CasTail(LocalTail, LocalNext);
                    }
                    continue;
                }
                T* ItemNull = nullptr;
                if (LocalTail->Items[Idx].compare_exchange_strong(ItemNull, Item)) 
                {
                    Hazard.Retire();
                    return;
                }
            }
        }

    public:
        /**
         * @brief Get a cached tail hazard pointer for repeated enqueue operations
         * @return EnqueueHazard that can be reused across multiple enqueues
         */
        inline EnqueueHazard GetTailHazard()
        {
            return {m_Tail, m_Hazards};
        }

        /**
         * @brief Enqueue an item using a cached hazard pointer
         * @param Item Item to enqueue (must not be nullptr)
         * @param Hazard Cached hazard pointer from GetTailHazard()
         */
        inline void Enqueue(T* Item, EnqueueHazard& Hazard) 
        {
            EnqueueInternal(Item, Hazard);
        }

        /**
         * @brief Enqueue an item (creates temporary hazard pointer)
         * @param Item Item to enqueue (must not be nullptr)
         */
        inline void Enqueue(T* Item)
        {
            THazardPointer<FNode> Hazard(m_Tail, m_Hazards);
            EnqueueInternal(Item, Hazard);
        }

        /**
         * @class DequeueHazard
         * @brief Cached hazard pointer for dequeue operations
         * 
         * Keeping a hazard pointer across multiple dequeue operations avoids
         * the overhead of acquiring/releasing hazard slots repeatedly.
         */
        class DequeueHazard : private THazardPointer<FNode, true>
        {
            friend class FAAArrayQueue<T>;
            inline DequeueHazard(std::atomic<FNode*>& Hazard, FHazardPointerCollection& Collection) 
                : THazardPointer<FNode, true>(Hazard, Collection)
            {}

        public:
            inline DequeueHazard() = default;
            inline DequeueHazard(DequeueHazard&& Hazard) 
                : THazardPointer<FNode, true>(MoveTemp(static_cast<THazardPointer<FNode, true>&>(Hazard)))
            {}

            inline DequeueHazard& operator=(DequeueHazard&& Other)
            {
                static_cast<THazardPointer<FNode, true>&>(*this) = MoveTemp(static_cast<THazardPointer<FNode, true>&>(Other));
                return *this;
            }
        };

    private:
        template<typename HazardType>
        T* DequeueInternal(HazardType& Hazard) 
        {
            while (true) 
            {
                FNode* LocalHead = Hazard.Get();
                if (LocalHead->DeqIdx.load() >= LocalHead->EnqIdx.load() && LocalHead->Next.load() == nullptr) 
                {
                    break;
                }
                const i32 Idx = LocalHead->DeqIdx.fetch_add(1);
                if (Idx > BUFFER_SIZE - 1) 
                { 
                    // This node has been drained, check if there is another one
                    FNode* LocalNext = LocalHead->Next.load();
                    if (LocalNext == nullptr)
                    {
                        break;  // No more nodes in the queue
                    }
                    if (CasHead(LocalHead, LocalNext))
                    {
                        Hazard.Retire();
                        m_Hazards.Delete(LocalHead);
                    }
                    continue;
                }

                // When there are more consumers than producers we can end up stealing
                // empty slots that producers have reserved but not yet had time to write into.
                // This leads to a lot of retries on the producers side so help this case
                // by spinning just a little bit when we know the DeqIdx we got is valid
                // and is likely to be written to very soon.
                if (LocalHead->Items[Idx].load() == nullptr && Idx <= LocalHead->EnqIdx.load())
                {
                    for (i32 Try = 0; LocalHead->Items[Idx].load() == nullptr && Try < 10; ++Try)
                    {
                        std::this_thread::yield();
                    }
                }

                T* Item = LocalHead->Items[Idx].exchange(GetTakenPointer());
                if (Item == nullptr)
                {
                    continue;
                }
                Hazard.Retire();
                return Item;
            }
            Hazard.Retire();
            return nullptr;
        }

    public:
        /**
         * @brief Dequeue an item using a cached hazard pointer
         * @param Hazard Cached hazard pointer from GetHeadHazard()
         * @return Dequeued item, or nullptr if queue is empty
         */
        inline T* Dequeue(DequeueHazard& Hazard) 
        {
            return DequeueInternal(Hazard);
        }

        /**
         * @brief Get a cached head hazard pointer for repeated dequeue operations
         * @return DequeueHazard that can be reused across multiple dequeues
         */
        inline DequeueHazard GetHeadHazard()
        {
            return {m_Head, m_Hazards};
        }

        /**
         * @brief Dequeue an item (creates temporary hazard pointer)
         * @return Dequeued item, or nullptr if queue is empty
         */
        inline T* Dequeue()
        {
            THazardPointer<FNode> Hazard(m_Head, m_Hazards);
            return DequeueInternal(Hazard);
        }
    };

} // namespace OloEngine
