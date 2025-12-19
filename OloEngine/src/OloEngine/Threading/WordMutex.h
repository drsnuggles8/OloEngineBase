// WordMutex.h - Pointer-sized mutex without ParkingLot dependency
// Ported from UE5.7 Async/WordMutex.h

#pragma once

// @file WordMutex.h
// @brief A mutex that is the size of a pointer and does not depend on ParkingLot.
//
// This mutex uses an intrusive linked list of waiting threads instead of the
// global ParkingLot hash table. It's useful when you need a mutex that has
// minimal dependencies and a predictable size.
//
// Ported from Unreal Engine's Async/WordMutex.h

#include "OloEngine/Core/Base.h"
#include "OloEngine/HAL/ManualResetEvent.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/Task/Oversubscription.h"

#include <atomic>

namespace OloEngine
{
    // @class FWordMutex
    // @brief A mutex that is the size of a pointer and does not depend on ParkingLot.
    //
    // Prefer FMutex to FWordMutex whenever possible. FMutex is typically more
    // efficient due to ParkingLot optimizations.
    //
    // This mutex is not fair and does not support recursive locking.
    //
    // State layout:
    // - Bit 0: IsLockedFlag - set when the mutex is locked
    // - Bit 1: IsQueueLockedFlag - set when a thread is traversing the wait queue
    // - Bits 2+: QueueMask - pointer to the tail of the intrusive wait queue
    class FWordMutex final
    {
      public:
        constexpr FWordMutex() = default;

        FWordMutex(const FWordMutex&) = delete;
        FWordMutex& operator=(const FWordMutex&) = delete;

        // @brief Try to acquire the lock without blocking
        // @return true if lock was acquired
        [[nodiscard]] inline bool TryLock()
        {
            uptr Expected = 0;
            return m_State.compare_exchange_strong(Expected, IsLockedFlag,
                                                   std::memory_order_acquire, std::memory_order_relaxed);
        }

        // @brief Acquire the lock, blocking until available
        inline void Lock()
        {
            uptr Expected = 0;
            if (OLO_LIKELY(m_State.compare_exchange_weak(Expected, IsLockedFlag,
                                                         std::memory_order_acquire, std::memory_order_relaxed)))
            {
                return;
            }

            LockSlow();
        }

        // @brief Release the lock
        inline void Unlock()
        {
            // Unlock immediately to allow other threads to acquire the lock
            // while this thread looks for a thread to wake.
            uptr CurrentState = m_State.fetch_sub(IsLockedFlag, std::memory_order_release);
            OLO_CORE_ASSERT(CurrentState & IsLockedFlag, "FWordMutex::Unlock called when not locked");

            // An empty queue indicates that there are no threads to wake.
            const bool bQueueEmpty = !(CurrentState & QueueMask);
            // A locked queue indicates that another thread is looking for a thread to wake.
            const bool bQueueLocked = (CurrentState & IsQueueLockedFlag);

            if (OLO_LIKELY(bQueueEmpty || bQueueLocked))
            {
                return;
            }

            UnlockSlow(CurrentState);
        }

      private:
        // @brief Node for the intrusive wait queue
        //
        // This is a doubly-linked list where:
        // - Prev: points from tail toward head (set when enqueuing)
        // - Next: points from head toward tail (set when dequeuing)
        struct FQueueNode
        {
            // Points to the next node in the tail-to-head direction. Only null for the current tail.
            FQueueNode* Prev = nullptr;
            // Points to the next node in the head-to-tail direction. The tail points to the head.
            // Null until UnlockSlow() has traversed from the tail to fill in next pointers.
            FQueueNode* Next = nullptr;

            FPlatformManualResetEvent Event;
        };

        void LockSlow()
        {
            static_assert((alignof(FQueueNode) & QueueMask) == alignof(FQueueNode),
                          "Alignment of FQueueNode is insufficient to pack flags into the lower bits.");

            constexpr i32 SpinLimit = 40;
            i32 SpinCount = 0;
            for (;;)
            {
                uptr CurrentState = m_State.load(std::memory_order_relaxed);

                // Try to acquire the lock if it was unlocked, even if there is a queue.
                // Acquiring the lock despite the queue means that this lock is not FIFO and thus not fair.
                if (!(CurrentState & IsLockedFlag))
                {
                    if (m_State.compare_exchange_weak(CurrentState, CurrentState | IsLockedFlag,
                                                      std::memory_order_acquire, std::memory_order_relaxed))
                    {
                        return;
                    }
                    continue;
                }

                // Spin up to the spin limit while there is no queue.
                if (!(CurrentState & QueueMask) && SpinCount < SpinLimit)
                {
                    FPlatformProcess::Yield();
                    ++SpinCount;
                    continue;
                }

                // Create the node that will be used to add this thread to the queue.
                FQueueNode Self;
                Self.Event.Reset();

                // The state points to the tail of the queue, and each node points to the previous node.
                if (FQueueNode* Tail = reinterpret_cast<FQueueNode*>(CurrentState & QueueMask))
                {
                    Self.Prev = Tail;
                }
                else
                {
                    Self.Next = &Self;
                }

                // Swap this thread in as the tail, which makes it visible to any other thread
                // that acquires the queue lock.
                if (!m_State.compare_exchange_weak(CurrentState,
                                                   (CurrentState & ~QueueMask) | reinterpret_cast<uptr>(&Self),
                                                   std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    continue;
                }

                // Do not enter oversubscription during a wait on a mutex since the wait is
                // generally too short for it to matter and it can worsen performance a lot
                // for heavily contended locks.
                LowLevelTasks::Private::FOversubscriptionAllowedScope _(false);

                // Wait until another thread wakes this thread, which can happen as soon as
                // the preceding store completes.
                Self.Event.Wait();

                // Loop back and try to acquire the lock.
                SpinCount = 0;
            }
        }

        void UnlockSlow(uptr CurrentState)
        {
            // IsLockedFlag was cleared by Unlock().
            CurrentState &= ~IsLockedFlag;

            for (;;)
            {
                // Try to lock the queue.
                if (m_State.compare_exchange_weak(CurrentState, CurrentState | IsQueueLockedFlag,
                                                  std::memory_order_acquire, std::memory_order_relaxed))
                {
                    CurrentState |= IsQueueLockedFlag;
                    break;
                }

                // A locked queue indicates that another thread is looking for a thread to wake.
                if ((CurrentState & IsQueueLockedFlag) || !(CurrentState & QueueMask))
                {
                    return;
                }
            }

            for (;;)
            {
                // This thread now holds the queue lock. Neither the queue nor State will change
                // while the queue is locked.
                // The state points to the tail of the queue, and each node points to the previous node.
                FQueueNode* Tail = reinterpret_cast<FQueueNode*>(CurrentState & QueueMask);

                // Traverse from the tail to find the head and set next pointers for any nodes
                // added since the last unlock.
                for (FQueueNode* Node = Tail; !Tail->Next;)
                {
                    FQueueNode* Prev = Node->Prev;
                    OLO_CORE_ASSERT(Prev, "FWordMutex queue traversal found null prev pointer");
                    Tail->Next = Prev->Next;
                    Prev->Next = Node;
                    Node = Prev;
                }

                // Another thread may acquire the lock while this thread has been finding a thread
                // to unlock. That case will not be detected on the first iteration of the loop,
                // but only when this thread has failed to unlock the queue at least once.
                // Attempt to unlock the queue here and allow the next unlock to find a thread to wake.
                if (CurrentState & IsLockedFlag)
                {
                    if (m_State.compare_exchange_weak(CurrentState, CurrentState & ~IsQueueLockedFlag,
                                                      std::memory_order_release, std::memory_order_acquire))
                    {
                        return;
                    }
                    continue;
                }

                // The next node from the tail is the head.
                FQueueNode* Head = Tail->Next;

                // Remove the head from the queue and unlock the queue.
                if (FQueueNode* NewHead = Head->Next; NewHead == Head)
                {
                    // Unlock and clear the queue. Failure needs to restart the loop, because
                    // newly-added nodes will have a pointer to the node being removed.
                    if (!m_State.compare_exchange_strong(CurrentState, CurrentState & IsLockedFlag,
                                                         std::memory_order_release, std::memory_order_acquire))
                    {
                        continue;
                    }
                }
                else
                {
                    // Clear pointers to the head node being removed.
                    OLO_CORE_ASSERT(NewHead, "FWordMutex found null new head");
                    NewHead->Prev = nullptr;
                    Tail->Next = NewHead;

                    // Unlock the queue regardless of whether new nodes have been added in the meantime.
                    m_State.fetch_and(~IsQueueLockedFlag, std::memory_order_release);
                }

                // Wake the thread that was at the head of the queue.
                Head->Event.Notify();
                break;
            }
        }

        // State bit layout
        static constexpr uptr IsLockedFlag = 1 << 0;
        static constexpr uptr IsQueueLockedFlag = 1 << 1;
        static constexpr uptr QueueMask = ~(IsLockedFlag | IsQueueLockedFlag);

        std::atomic<uptr> m_State{ 0 };
    };

} // namespace OloEngine
