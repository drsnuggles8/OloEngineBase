// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/PlatformTime.h"
#include "OloEngine/Containers/FAAArrayQueue.h"
#include "OloEngine/Task/LowLevelTask.h"
#include "OloEngine/Memory/Platform.h"

#include <atomic>

#if defined(AGGRESSIVE_MEMORY_SAVING) && AGGRESSIVE_MEMORY_SAVING
    #define LOCALQUEUEREGISTRYDEFAULTS_MAX_LOCALQUEUES 1024
    #define LOCALQUEUEREGISTRYDEFAULTS_MAX_ITEMCOUNT 512
#else
    #define LOCALQUEUEREGISTRYDEFAULTS_MAX_LOCALQUEUES 1024
    #define LOCALQUEUEREGISTRYDEFAULTS_MAX_ITEMCOUNT 1024
#endif

namespace OloEngine::LowLevelTasks
{
    namespace LocalQueue_Impl
    {
        // @class TWorkStealingQueueBase2
        // @brief Lock-free work stealing queue base implementation
        // 
        // Each node has one array but we don't search for a vacant entry. 
        // Instead, we use FAA to obtain an index in the array, for enqueueing or dequeuing.
        template<u32 NumItems>
        class TWorkStealingQueueBase2
        {
            enum class ESlotState : uptr
            {
                Free  = 0, // The slot is free and items can be put there
                Taken = 1, // The slot is in the process of being stolen
            };

        protected:
            // @brief Insert an item at the head position (only safe on a single thread, shared with Get)
            OLO_FINLINE bool Put(uptr Item)
            {
                OLO_CORE_ASSERT(Item != uptr(ESlotState::Free), "Cannot put Free sentinel");
                OLO_CORE_ASSERT(Item != uptr(ESlotState::Taken), "Cannot put Taken sentinel");

                u32 Idx = (m_Head + 1) % NumItems;
                uptr Slot = m_ItemSlots[Idx].Value.load(std::memory_order_acquire);

                if (Slot == uptr(ESlotState::Free))
                {
                    m_ItemSlots[Idx].Value.store(Item, std::memory_order_release);
                    m_Head++;
                    OLO_CORE_ASSERT(m_Head % NumItems == Idx, "Head index mismatch");
                    return true;
                }
                return false;
            }

            // @brief Remove an item at the head position in FIFO order (only safe on a single thread, shared with Put)
            OLO_FINLINE bool Get(uptr& Item)
            {
                u32 Idx = m_Head % NumItems;
                uptr Slot = m_ItemSlots[Idx].Value.load(std::memory_order_acquire);

                if (Slot > uptr(ESlotState::Taken) && m_ItemSlots[Idx].Value.compare_exchange_strong(Slot, uptr(ESlotState::Free), std::memory_order_acq_rel))
                {
                    m_Head--;
                    OLO_CORE_ASSERT((m_Head + 1) % NumItems == Idx, "Head index mismatch after Get");
                    Item = Slot;
                    return true;
                }
                return false;
            }

            // @brief Remove an item at the tail position in LIFO order (can be done from any thread including the one that accesses the head)
            OLO_FINLINE bool Steal(uptr& Item)
            {
                do
                {
                    u32 IdxVer = m_Tail.load(std::memory_order_acquire);
                    u32 Idx = IdxVer % NumItems;
                    uptr Slot = m_ItemSlots[Idx].Value.load(std::memory_order_acquire);

                    if (Slot == uptr(ESlotState::Free))
                    {
                        // Once we find a free slot, we need to verify if it's been freed by another steal
                        // so check back the Tail value to make sure it wasn't incremented since we first read the value.
                        // If we don't do this, some threads might not see that other threads
                        // have already stolen the slot, and will wrongly return that no more tasks are available to steal.
                        if (IdxVer != m_Tail.load(std::memory_order_acquire))
                        {
                            continue; // Loop again since tail has changed
                        }
                        return false;
                    }
                    else if (Slot != uptr(ESlotState::Taken) && m_ItemSlots[Idx].Value.compare_exchange_weak(Slot, uptr(ESlotState::Taken), std::memory_order_acq_rel))
                    {
                        if (IdxVer == m_Tail.load(std::memory_order_acquire))
                        {
                            [[maybe_unused]] u32 Prev = m_Tail.fetch_add(1, std::memory_order_release);
                            OLO_CORE_ASSERT(Prev % NumItems == Idx, "Tail index mismatch after Steal");
                            m_ItemSlots[Idx].Value.store(uptr(ESlotState::Free), std::memory_order_release);
                            Item = Slot;
                            return true;
                        }
                        m_ItemSlots[Idx].Value.store(Slot, std::memory_order_release);
                    }
                } while (true);
            }

        private:
            struct FAlignedElement
            {
                alignas(OLO_PLATFORM_CACHE_LINE_SIZE * 2) std::atomic<uptr> Value = {};
            };

            alignas(OLO_PLATFORM_CACHE_LINE_SIZE * 2) u32 m_Head{~0u};
            alignas(OLO_PLATFORM_CACHE_LINE_SIZE * 2) std::atomic_uint m_Tail{0};
            alignas(OLO_PLATFORM_CACHE_LINE_SIZE * 2) FAlignedElement m_ItemSlots[NumItems] = {};
        };

        // @class TWorkStealingQueue2
        // @brief Typed wrapper for the work stealing queue
        template<typename Type, u32 NumItems>
        class TWorkStealingQueue2 final : protected TWorkStealingQueueBase2<NumItems>
        {
            using PointerType = Type*;

        public:
            OLO_FINLINE bool Put(PointerType Item)
            {
                return TWorkStealingQueueBase2<NumItems>::Put(reinterpret_cast<uptr>(Item));
            }

            OLO_FINLINE bool Get(PointerType& Item)
            {
                return TWorkStealingQueueBase2<NumItems>::Get(reinterpret_cast<uptr&>(Item));
            }

            OLO_FINLINE bool Steal(PointerType& Item)
            {
                return TWorkStealingQueueBase2<NumItems>::Steal(reinterpret_cast<uptr&>(Item));
            }
        };

    } // namespace LocalQueue_Impl

    namespace Private
    {
        // @enum ELocalQueueType
        // @brief Type of local queue (foreground or background worker)
        enum class ELocalQueueType
        {
            EBackground,
            EForeground,
        };

        // @class TLocalQueueRegistry
        // @brief A collection of lock-free queues for work distribution
        // 
        // LocalQueues can only be Enqueued and Dequeued by the current Thread they were installed on. 
        // But Items can be stolen from any Thread.
        // 
        // There is a global OverflowQueue that is used when a LocalQueue goes out of scope to dump all 
        // remaining Items, or when a Thread has no LocalQueue installed or when the LocalQueue is at capacity. 
        // A new LocalQueue registers itself always.
        // 
        // A Dequeue Operation can only be done starting from a LocalQueue, then the GlobalQueue will be checked.
        // Finally Items might get Stolen from other LocalQueues that are registered with the LocalQueueRegistry.
        template<u32 NumLocalItems = LOCALQUEUEREGISTRYDEFAULTS_MAX_ITEMCOUNT, u32 MaxLocalQueues = LOCALQUEUEREGISTRYDEFAULTS_MAX_LOCALQUEUES>
        class TLocalQueueRegistry
        {
            static u32 Rand()
            {
                // Simple PRNG based on PCG, seeded with cycle counter for better randomness
                u32 State = OloEngine::FPlatformTime::Cycles();
                State = State * 747796405u + 2891336453u;
                State = ((State >> ((State >> 28u) + 4u)) ^ State) * 277803737u;
                return (State >> 22u) ^ State;
            }

        public:
            class TLocalQueue;

        private:
            using FLocalQueueType    = LocalQueue_Impl::TWorkStealingQueue2<FTask, NumLocalItems>;
            using FOverflowQueueType = FAAArrayQueue<FTask>;
            using DequeueHazard      = typename FOverflowQueueType::DequeueHazard;

        public:
            // @class TLocalQueue
            // @brief Thread-local work queue with work stealing support
            class TLocalQueue
            {
                template<u32, u32>
                friend class TLocalQueueRegistry;

            public:
                TLocalQueue() = default;

                TLocalQueue(TLocalQueueRegistry& InRegistry, ELocalQueueType InQueueType)
                {
                    Init(InRegistry, InQueueType);
                }

                void Init(TLocalQueueRegistry& InRegistry, ELocalQueueType InQueueType)
                {
                    if (m_bIsInitialized.exchange(true, std::memory_order_relaxed))
                    {
                        OLO_CORE_ASSERT(false, "Trying to initialize local queue more than once");
                    }
                    else
                    {
                        m_Registry = &InRegistry;
                        m_QueueType = InQueueType;

                        // Local queues are never unregistered, everything is shutdown at once.
                        m_Registry->AddLocalQueue(this);
                        for (i32 PriorityIndex = 0; PriorityIndex < i32(ETaskPriority::Count); ++PriorityIndex)
                        {
                            m_DequeueHazards[PriorityIndex] = m_Registry->m_OverflowQueues[PriorityIndex].GetHeadHazard();
                        }
                    }
                }

                ~TLocalQueue()
                {
                    if (m_bIsInitialized.exchange(false, std::memory_order_relaxed))
                    {
                        for (i32 PriorityIndex = 0; PriorityIndex < i32(ETaskPriority::Count); PriorityIndex++)
                        {
                            while (true)
                            {
                                FTask* Item;
                                if (!m_LocalQueues[PriorityIndex].Get(Item))
                                {
                                    break;
                                }
                                m_Registry->m_OverflowQueues[PriorityIndex].Enqueue(Item);
                            }
                        }
                    }
                }

                // @brief Add an item to the local queue and overflow into the global queue if full
                OLO_FINLINE void Enqueue(FTask* Item, u32 PriorityIndex)
                {
                    OLO_CORE_ASSERT(m_Registry != nullptr, "Registry not initialized");
                    OLO_CORE_ASSERT(PriorityIndex < i32(ETaskPriority::Count), "Priority index out of range");
                    OLO_CORE_ASSERT(Item != nullptr, "Cannot enqueue null item");

                    if (!m_LocalQueues[PriorityIndex].Put(Item))
                    {
                        m_Registry->m_OverflowQueues[PriorityIndex].Enqueue(Item);
                    }
                }

                // @brief Steal from own local queue
                OLO_FINLINE FTask* StealLocal(bool GetBackGroundTasks)
                {
                    const i32 MaxPriority = GetBackGroundTasks ? i32(ETaskPriority::Count) : i32(ETaskPriority::ForegroundCount);

                    for (i32 PriorityIndex = 0; PriorityIndex < MaxPriority; ++PriorityIndex)
                    {
                        FTask* Item;
                        if (m_LocalQueues[PriorityIndex].Steal(Item))
                        {
                            return Item;
                        }
                    }
                    return nullptr;
                }

                // @brief Check both the local and global queue in priority order
                OLO_FINLINE FTask* Dequeue(bool GetBackGroundTasks)
                {
                    const i32 MaxPriority = GetBackGroundTasks ? i32(ETaskPriority::Count) : i32(ETaskPriority::ForegroundCount);

                    for (i32 PriorityIndex = 0; PriorityIndex < MaxPriority; ++PriorityIndex)
                    {
                        FTask* Item;
                        if (m_LocalQueues[PriorityIndex].Get(Item))
                        {
                            return Item;
                        }

                        Item = m_Registry->m_OverflowQueues[PriorityIndex].Dequeue(m_DequeueHazards[PriorityIndex]);
                        if (Item)
                        {
                            return Item;
                        }
                    }
                    return nullptr;
                }

                // @brief Dequeue with work stealing from other queues
                OLO_FINLINE FTask* DequeueSteal(bool GetBackGroundTasks)
                {
                    if (m_CachedRandomIndex == InvalidIndex)
                    {
                        m_CachedRandomIndex = Rand();
                    }

                    FTask* Result = m_Registry->StealItem(m_CachedRandomIndex, m_CachedPriorityIndex, GetBackGroundTasks);
                    if (Result)
                    {
                        return Result;
                    }
                    return nullptr;
                }

            private:
                static constexpr u32   InvalidIndex = ~0u;
                FLocalQueueType        m_LocalQueues[u32(ETaskPriority::Count)];
                DequeueHazard          m_DequeueHazards[u32(ETaskPriority::Count)];
                TLocalQueueRegistry*   m_Registry = nullptr;
                u32                    m_CachedRandomIndex = InvalidIndex;
                u32                    m_CachedPriorityIndex = 0;
                ELocalQueueType        m_QueueType;
                std::atomic<bool>      m_bIsInitialized{ false };

            public:
                // Make movable for std::vector compatibility
                // Note: m_LocalQueues are not moved as they contain non-movable atomics.
                // The queues should be empty when moving, and new ones will be default-initialized.
                // m_DequeueHazards are moved individually since they have move assignment.
                TLocalQueue(TLocalQueue&& Other) noexcept
                    : m_Registry(Other.m_Registry)
                    , m_CachedRandomIndex(Other.m_CachedRandomIndex)
                    , m_CachedPriorityIndex(Other.m_CachedPriorityIndex)
                    , m_QueueType(Other.m_QueueType)
                    , m_bIsInitialized(Other.m_bIsInitialized.load(std::memory_order_relaxed))
                {
                    for (u32 i = 0; i < u32(ETaskPriority::Count); ++i)
                    {
                        m_DequeueHazards[i] = std::move(Other.m_DequeueHazards[i]);
                    }
                    Other.m_Registry = nullptr;
                    Other.m_bIsInitialized.store(false, std::memory_order_relaxed);
                }

                TLocalQueue& operator=(TLocalQueue&&) = delete;
                TLocalQueue(const TLocalQueue&) = delete;
                TLocalQueue& operator=(const TLocalQueue&) = delete;
            };

            TLocalQueueRegistry() = default;

        private:
            // @brief Add a queue to the Registry. Thread-safe.
            void AddLocalQueue(TLocalQueue* QueueToAdd)
            {
                u32 Index = m_NumLocalQueues.fetch_add(1, std::memory_order_relaxed);
                OLO_CORE_ASSERT(Index < MaxLocalQueues, "Attempting to add more than the maximum allowed number of queues ({})", MaxLocalQueues);

                // std::memory_order_release to make sure values are all written to the TLocalQueue before publishing.
                m_LocalQueues[Index].store(QueueToAdd, std::memory_order_release);
            }

            // @brief StealItem tries to steal an Item from a Registered LocalQueue
            // Thread-safe with AddLocalQueue
            FTask* StealItem(u32& CachedRandomIndex, u32& CachedPriorityIndex, bool GetBackGroundTasks)
            {
                u32 NumQueues   = m_NumLocalQueues.load(std::memory_order_relaxed);
                u32 MaxPriority = GetBackGroundTasks ? i32(ETaskPriority::Count) : i32(ETaskPriority::ForegroundCount);
                CachedRandomIndex = CachedRandomIndex % NumQueues;

                for (u32 Index = 0; Index < m_NumLocalQueues; Index++)
                {
                    // Test for null in case we race on reading NumLocalQueues reserved index before the pointer is set
                    if (TLocalQueue* LocalQueue = m_LocalQueues[Index].load(std::memory_order_acquire))
                    {
                        for (u32 PriorityIndex = 0; PriorityIndex < MaxPriority; PriorityIndex++)
                        {
                            FTask* Item;
                            if (LocalQueue->m_LocalQueues[PriorityIndex].Steal(Item))
                            {
                                return Item;
                            }
                            CachedPriorityIndex = ++CachedPriorityIndex < MaxPriority ? CachedPriorityIndex : 0;
                        }
                        CachedRandomIndex = ++CachedRandomIndex < NumQueues ? CachedRandomIndex : 0;
                    }
                }
                CachedPriorityIndex = 0;
                CachedRandomIndex = TLocalQueue::InvalidIndex;
                return nullptr;
            }

        public:
            // @brief Enqueue an Item directly into the Global OverflowQueue
            void Enqueue(FTask* Item, u32 PriorityIndex)
            {
                OLO_CORE_ASSERT(PriorityIndex < i32(ETaskPriority::Count), "Priority index out of range");
                OLO_CORE_ASSERT(Item != nullptr, "Cannot enqueue null item");

                m_OverflowQueues[PriorityIndex].Enqueue(Item);
            }

            // @brief Grab an Item directly from the Global OverflowQueue
            FTask* DequeueGlobal(bool GetBackGroundTasks = true)
            {
                const i32 MaxPriority = GetBackGroundTasks ? i32(ETaskPriority::Count) : i32(ETaskPriority::ForegroundCount);

                for (i32 PriorityIndex = 0; PriorityIndex < MaxPriority; ++PriorityIndex)
                {
                    if (FTask* Item = m_OverflowQueues[PriorityIndex].Dequeue())
                    {
                        return Item;
                    }
                }
                return nullptr;
            }

            OLO_FINLINE FTask* DequeueSteal(bool GetBackGroundTasks)
            {
                u32 CachedRandomIndex = Rand();
                u32 CachedPriorityIndex = 0;
                FTask* Result = StealItem(CachedRandomIndex, CachedPriorityIndex, GetBackGroundTasks);
                if (Result)
                {
                    return Result;
                }
                return nullptr;
            }

            // @brief Reset the registry. Not thread-safe.
            void Reset()
            {
                u32 NumQueues = m_NumLocalQueues.load(std::memory_order_relaxed);
                for (u32 Index = 0; Index < NumQueues; Index++)
                {
                    m_LocalQueues[Index].store(nullptr, std::memory_order_relaxed);
                }

                m_NumLocalQueues.store(0, std::memory_order_release);
            }

        private:
            FOverflowQueueType        m_OverflowQueues[u32(ETaskPriority::Count)];
            std::atomic<TLocalQueue*> m_LocalQueues[MaxLocalQueues]{nullptr};
            std::atomic<u32>          m_NumLocalQueues{0};
        };

    } // namespace Private

} // namespace OloEngine::LowLevelTasks
