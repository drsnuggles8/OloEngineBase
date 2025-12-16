// Copyright (C) 2016 Dmitry Vyukov <dvyukov@google.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

// This implementation is based on EventCount.h
// included in the Eigen library but almost everything has been
// rewritten.
// Ported to OloEngine

#include "OloEngine/Task/WaitingQueue.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Task/Oversubscription.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/Debug/TaskTrace.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine::LowLevelTasks::Private
{
    namespace WaitingQueueImpl
    {
        // State_ layout:
        // - low kWaiterBits is a stack of waiters committed wait
        //   (indexes in NodesArray are used as stack elements,
        //   kStackMask means empty stack).
        // - next kWaiterBits is count of waiters in prewait state.
        // - next kWaiterBits is count of pending signals.
        // - remaining bits are ABA counter for the stack.
        //   (stored in Waiter node and incremented on push).
        static constexpr u64 WaiterBits = 14;
        static constexpr u64 StackMask = (1ull << WaiterBits) - 1;
        static constexpr u64 WaiterShift = WaiterBits;
        static constexpr u64 WaiterMask = ((1ull << WaiterBits) - 1) << WaiterShift;
        static constexpr u64 WaiterInc = 1ull << WaiterShift;
        static constexpr u64 SignalShift = 2 * WaiterBits;
        static constexpr u64 SignalMask = ((1ull << WaiterBits) - 1) << SignalShift;
        static constexpr u64 SignalInc = 1ull << SignalShift;
        static constexpr u64 EpochShift = 3 * WaiterBits;
        static constexpr u64 EpochBits = 64 - EpochShift;
        static constexpr u64 EpochMask = ((1ull << EpochBits) - 1) << EpochShift;
        static constexpr u64 EpochInc = 1ull << EpochShift;

        // Get the active thread count out of the standby state.
        u64 GetActiveThreadCount(u64 StandbyState)
        {
            // The StandbyState stores the active thread count in the waiter bits.
            return (StandbyState & WaiterMask) >> WaiterShift;
        }

        void EnterWait(FWaitEvent* Node)
        {
            // Flush any open profiler scope before going to sleep so that anything that happened
            // before appears in Tracy right away. If we don't do this, the thread buffer will be
            // held to this thread until we wake up and fill it, so it might cause events to appear
            // as missing in Tracy, especially in case we never wake up again (i.e. deadlock / crash).
            // This matches UE5.7's TRACE_CPUPROFILER_EVENT_FLUSH() call.
            TRACE_CPUPROFILER_EVENT_FLUSH();
            
            // Disallow oversubscription for this wait
            Private::FOversubscriptionAllowedScope OversubscriptionScope(false);

            // Let the memory manager know we're inactive so it can do whatever it wants with our
            // thread-local memory cache if we have any.
            FMemory::MarkTLSCachesAsUnusedOnCurrentThread();

            Node->Event->Wait();

            // Let the memory manager know we're active again and need our
            // thread-local memory cache back if we have any.
            FMemory::MarkTLSCachesAsUsedOnCurrentThread();
        }
    }

    void FWaitingQueue::Init(u32 InThreadCount, u32 InMaxThreadCount, TFunction<void()> InCreateThread, u32 InActiveThreadCount)
    {
        using namespace WaitingQueueImpl;

        m_ThreadCount = InThreadCount;
        m_MaxThreadCount = InMaxThreadCount;
        m_CreateThread = MoveTemp(InCreateThread);
        m_Oversubscription = 0;
        m_bIsShuttingDown = false;
        m_State = StackMask;

        // Store the external thread creations in the waiter bits which
        // represent the number of currently active threads.
        m_StandbyState = StackMask | ((u64(InActiveThreadCount) << WaiterBits) & WaiterMask);

        OLO_CORE_ASSERT(m_NodesArray.Num() < (1ull << WaiterBits) - 1, "Too many nodes in array");
    }

    void FWaitingQueue::FinishShutdown()
    {
        using namespace WaitingQueueImpl;

        OLO_CORE_ASSERT((m_State & (StackMask | WaiterMask)) == StackMask, "State should be empty");
        OLO_CORE_ASSERT((m_StandbyState & StackMask) == StackMask, "StandbyState should be empty");
    }

    void FWaitingQueue::PrepareWait(FWaitEvent* Node)
    {
        using namespace WaitingQueueImpl;

        m_State.fetch_add(WaiterInc, std::memory_order_relaxed);
    }

    bool FWaitingQueue::IsOversubscriptionLimitReached() const
    {
        return m_Oversubscription.load(std::memory_order_relaxed) >= m_MaxThreadCount;
    }

    void FWaitingQueue::CheckState(u64 InState, bool bInIsWaiter)
    {
        using namespace WaitingQueueImpl;

        static_assert(EpochBits >= 20, "Not enough bits to prevent ABA problem");
#ifdef OLO_DEBUG
        const u64 Waiters = (InState & WaiterMask) >> WaiterShift;
        const u64 Signals = (InState & SignalMask) >> SignalShift;
        OLO_CORE_ASSERT(Waiters >= Signals, "Waiters must be >= Signals");
        OLO_CORE_ASSERT(Waiters < (1 << WaiterBits) - 1, "Too many waiters");
        OLO_CORE_ASSERT(!bInIsWaiter || Waiters > 0, "Must have waiters if bInIsWaiter");
        (void)Waiters;
        (void)Signals;
#else
        (void)InState;
        (void)bInIsWaiter;
#endif
    }

    void FWaitingQueue::CheckStandbyState(u64 InState)
    {
        using namespace WaitingQueueImpl;

#ifdef OLO_DEBUG
        const u64 Index = (InState & StackMask);
        const u64 ActiveThreadCount = (InState & WaiterMask) >> WaiterShift;
        const u64 Signals = (InState & SignalMask) >> SignalShift;
        OLO_CORE_ASSERT(Signals == 0, "Signals unused in this mode");
        OLO_CORE_ASSERT(ActiveThreadCount <= static_cast<u64>(m_NodesArray.Num()), "ActiveThreadCount too high");
        OLO_CORE_ASSERT(Index == StackMask || Index < static_cast<u64>(m_NodesArray.Num()), "Invalid index");
        (void)Index;
        (void)ActiveThreadCount;
        (void)Signals;
#else
        (void)InState;
#endif
    }

    bool FWaitingQueue::CommitWait(FWaitEvent* Node, FOutOfWork& OutOfWork, i32 SpinCycles, i32 WaitCycles)
    {
        using namespace WaitingQueueImpl;

        OLO_CORE_ASSERT((Node->Epoch & ~EpochMask) == 0, "Epoch should fit in EpochMask");
        Node->State.store(EWaitState::NotSignaled, std::memory_order_relaxed);

        u64 LocalState = m_State.load(std::memory_order_relaxed);

        CheckState(LocalState, true);
        u64 NewState;
        if ((LocalState & SignalMask) != 0)
        {
            // Consume the signal and return immediately.
            NewState = LocalState - WaiterInc - SignalInc + EpochInc;
        }
        else
        {
            // Remove this thread from pre-wait counter and add to the waiter stack.
            NewState = ((LocalState & (WaiterMask | EpochMask)) - WaiterInc + EpochInc) | static_cast<u64>(Node - &m_NodesArray[0]);
            Node->Next.store(LocalState & StackMask, std::memory_order_relaxed);
        }
        CheckState(NewState);
        if (m_State.compare_exchange_weak(LocalState, NewState, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            if ((LocalState & SignalMask) == 0)
            {
                // Fallthrough to park
            }
            else
            {
                OutOfWork.Stop();
                return true;
            }
        }
        else
        {
            // Avoid too much contention on commit as it's not healthy. 
            // Prefer going back validating if anything has come up in the task queues
            // in between commit retries.
            return false;
        }

        Park(Node, OutOfWork, SpinCycles, WaitCycles);
        return true;
    }

    bool FWaitingQueue::CancelWait(FWaitEvent* Node)
    {
        using namespace WaitingQueueImpl;

        u64 LocalState = m_State.load(std::memory_order_relaxed);
        for (;;)
        {
            bool bConsumedSignal = false;
            CheckState(LocalState, true);
            u64 NewState = LocalState - WaiterInc;

            // When we consume a signal, the caller will have to try to wake up an additional
            // worker otherwise we could end up missing a wakeup and end up into a deadlock.
            // The more signal we consume, the more spurious wakeups we're going to have so
            // only consume a signal when both waiters and signals are equal so we get the
            // minimal amount of consumed signals possible.
            if (((LocalState & WaiterMask) >> WaiterShift) == ((LocalState & SignalMask) >> SignalShift))
            {
                NewState -= SignalInc;
                bConsumedSignal = true;
            }
            else
            {
                bConsumedSignal = false;
            }

            CheckState(NewState);
            if (m_State.compare_exchange_weak(LocalState, NewState, std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                if (bConsumedSignal)
                {
                    // Since we consumed the event, but we don't know if we're cancelling because of the task
                    // this other thread is waking us for or another task entirely. Tell the caller to wake another thread.
                    return true;
                }
                return false;
            }
        }
    }

    void FWaitingQueue::StartShutdown()
    {
        using namespace WaitingQueueImpl;

        m_bIsShuttingDown = true;

        // Wake up all workers.
        NotifyInternal(m_NodesArray.Num());

        // Notification above doesn't trigger standby threads
        // during shutdown so trigger them here.
        u64 LocalState = m_StandbyState;
        while ((LocalState & StackMask) != StackMask)
        {
            FWaitEvent* Node = &m_NodesArray[LocalState & StackMask];
            Node->Event->Trigger();
            LocalState = Node->Next;
        }
        m_StandbyState = StackMask;
    }

    void FWaitingQueue::PrepareStandby(FWaitEvent* Node)
    {
        // We store the whole state before going back checking the queue so that we can't possibly
        // miss an event in-between PrepareStandby and CommitStandby.
        Node->Epoch = m_StandbyState;
    }

    void FWaitingQueue::ConditionalStandby(FWaitEvent* Node)
    {
        using namespace WaitingQueueImpl;

        if (m_bIsShuttingDown.load(std::memory_order_relaxed))
        {
            return;
        }

        u64 LocalState = m_StandbyState;
        while (GetActiveThreadCount(LocalState) > m_ThreadCount + m_Oversubscription.load(std::memory_order_relaxed))
        {
            CheckStandbyState(LocalState);
            // We store the active thread count in the waiters slot, so decrement it by 1.
            const u64 Waiters  = (LocalState & WaiterMask) - WaiterInc;
            const u64 NewEpoch = (LocalState & EpochMask) + EpochInc;
            const u64 NewState = static_cast<u64>(Node - &m_NodesArray[0]) | NewEpoch | Waiters;

            Node->Next.store(LocalState & StackMask);
            Node->Event->Reset();

            CheckStandbyState(NewState);
            if (m_StandbyState.compare_exchange_weak(LocalState, NewState))
            {
                EnterWait(Node);
            }
        }
    }

    bool FWaitingQueue::CommitStandby(FWaitEvent* Node, FOutOfWork& OutOfWork)
    {
        using namespace WaitingQueueImpl;

        u64 LocalState = Node->Epoch;
        CheckStandbyState(LocalState);
        // We store the active thread count in the waiters slot, so decrement it by 1.
        const u64 Waiters = (LocalState & WaiterMask) - WaiterInc;
        const u64 Epoch = (LocalState & EpochMask) + EpochInc;
        const u64 NewState = static_cast<u64>(Node - &m_NodesArray[0]) | Epoch | Waiters;

        Node->Next.store(LocalState & StackMask);
        Node->Event->Reset();

        CheckStandbyState(NewState);
        if (m_StandbyState.compare_exchange_strong(LocalState, NewState))
        {
            // Fallthrough to wait
        }
        else
        {
            // Update the value before we go back checking if new tasks have been queued.
            Node->Epoch = LocalState;
            return false;
        }

        OutOfWork.Stop();
        EnterWait(Node);
        return true;
    }

    void FWaitingQueue::IncrementOversubscription()
    {
        if (++m_Oversubscription >= m_MaxThreadCount)
        {
            m_OversubscriptionLimitReachedEvent.Broadcast();
        }

        // This is important that StandbyState is invalidated after Oversubscription is increased so we
        // can detect stale decisions and reevaluate oversubscription.
        // Notify -> TryStartNewThread takes care of updating StandbyState for us, but only
        // when standby threads are actually needed.

        Notify();
    }

    void FWaitingQueue::DecrementOversubscription()
    {
        --m_Oversubscription;
    }

    bool FWaitingQueue::TryStartNewThread()
    {
        using namespace WaitingQueueImpl;

        // Invalidate the current state by adding an Epoch right away so compare-exchange for other threads can detect
        // oversubscription has changed which happens in IncrementOversubscription before calling this function.
        //
        // Important to always read the StandbyState before the Oversubscription value so that we capture the current epoch to validate
        // Oversubscription didn't change while we were doing the CAS.
        u64 LocalState = m_StandbyState.fetch_add(EpochInc, std::memory_order_seq_cst) + EpochInc;
        while (GetActiveThreadCount(LocalState) < m_MaxThreadCount && GetActiveThreadCount(LocalState) < m_ThreadCount + m_Oversubscription.load(std::memory_order_relaxed))
        {
            CheckStandbyState(LocalState);

            // We store the active thread count in the waiters slot, so increment it by 1.
            const u64 NewEpoch = (LocalState & EpochMask) + EpochInc;
            u64 NewState = NewEpoch | (LocalState & WaiterMask) + WaiterInc;
            if ((LocalState & StackMask) != StackMask)
            {
                FWaitEvent* Node = &m_NodesArray[LocalState & StackMask];
                u64 Next = Node->Next.load(std::memory_order_relaxed);
                NewState |= Next & StackMask;
            }
            else
            {
                NewState |= LocalState & StackMask;
            }

            CheckStandbyState(NewState);
            if (m_StandbyState.compare_exchange_weak(LocalState, NewState, std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                if ((LocalState & StackMask) != StackMask)
                {
                    // We got an existing node, wake it from standby
                    FWaitEvent* Node = &m_NodesArray[LocalState & StackMask];
                    Node->Event->Trigger();
                    return true;
                }
                else if (m_bIsShuttingDown.load(std::memory_order_relaxed) == false)
                {
                    m_CreateThread();
                    return true;
                }
                else
                {
                    m_StandbyState -= WaiterInc;
                    return false;
                }
            }
        }

        return false;
    }

    i32 FWaitingQueue::NotifyInternal(i32 Count)
    {
        using namespace WaitingQueueImpl;

        i32 Notifications = 0;
        while (Count > Notifications)
        {
            u64 LocalState = m_State.load(std::memory_order_relaxed);
            for (;;)
            {
                CheckState(LocalState);
                const u64 Waiters  = (LocalState & WaiterMask) >> WaiterShift;
                const u64 Signals  = (LocalState & SignalMask) >> SignalShift;
                const u64 NewEpoch = (LocalState & EpochMask) + EpochInc;
                const bool bNotifyAll = Count >= m_NodesArray.Num();

                u64 NewState;
                if ((LocalState & StackMask) == StackMask && Waiters == Signals)
                {
                    // No more waiters, go through the CAS to provide proper ordering
                    // with other threads entering PrepareWait.
                    NewState = LocalState + EpochInc;
                }
                else if (bNotifyAll)
                {
                    // Empty wait stack and set signal to number of pre-wait threads.
                    NewState = (LocalState & WaiterMask) | (Waiters << SignalShift) | StackMask | NewEpoch;
                }
                else if (Signals < Waiters)
                {
                    // There is a thread in pre-wait state, unblock it.
                    NewState = LocalState + SignalInc + EpochInc;
                }
                else
                {
                    // Pop a waiter from list and unpark it.
                    FWaitEvent* Node = &m_NodesArray[LocalState & StackMask];
                    u64 Next = Node->Next.load(std::memory_order_relaxed);
                    NewState = (LocalState & (WaiterMask | SignalMask)) | (Next & StackMask) | NewEpoch;
                }
                CheckState(NewState);
                if (m_State.compare_exchange_weak(LocalState, NewState, std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    if (!bNotifyAll && (Signals < Waiters))
                    {
                        Notifications++;
                        break;  // unblocked pre-wait thread
                    }

                    if ((LocalState & StackMask) == StackMask)
                    {
                        if (TryStartNewThread())
                        {
                            Notifications++;
                            break;
                        }
                        return Notifications;
                    }

                    FWaitEvent* Node = &m_NodesArray[LocalState & StackMask];
                    if (!bNotifyAll)
                    {
                        Node->Next.store(StackMask, std::memory_order_relaxed);
                        Notifications += Unpark(Node);
                        break;
                    }
                    else
                    {
                        Notifications += static_cast<i32>(Waiters);
                        return Unpark(Node) + Notifications;
                    }
                }
            }
        }

        return Notifications;
    }

    void FWaitingQueue::Park(FWaitEvent* Node, FOutOfWork& OutOfWork, i32 SpinCycles, i32 WaitCycles)
    {
        using namespace WaitingQueueImpl;

        // Spinning for a very short while helps reduce signaling cost
        // since we're giving the other threads a final chance to wake us with an 
        // atomic only instead of a more costly kernel call.
        for (i32 Spin = 0; Spin < SpinCycles; ++Spin)
        {
            if (Node->State.load(std::memory_order_relaxed) == EWaitState::NotSignaled)
            {
                // Yield CPU cycles using platform-optimized pause instructions
                FPlatformProcess::YieldCycles(WaitCycles);
            }
            else
            {
                OutOfWork.Stop();
                return;
            }
        }

        Node->Event->Reset();
        EWaitState Target = EWaitState::NotSignaled;
        if (Node->State.compare_exchange_strong(Target, EWaitState::Waiting, std::memory_order_relaxed, std::memory_order_relaxed))
        {
            // Fall through to the wait function
        }
        else
        {
            OutOfWork.Stop();
            return;
        }

        OutOfWork.Stop();
        EnterWait(Node);
    }

    i32 FWaitingQueue::Unpark(FWaitEvent* InNode)
    {
        using namespace WaitingQueueImpl;

        i32 UnparkedCount = 0;
        for (FWaitEvent* Node = InNode; Node != nullptr; )
        {
            u64 NextNode = Node->Next.load(std::memory_order_relaxed) & StackMask;
            FWaitEvent* Next = NextNode == StackMask ? nullptr : &m_NodesArray[static_cast<sizet>(NextNode)];

            UnparkedCount++;

            // Signaling can be very costly on some platforms. So only trigger
            // the event if the other thread was in the waiting state.
            if (Node->State.exchange(EWaitState::Signaled, std::memory_order_relaxed) == EWaitState::Waiting)
            {
                Node->Event->Trigger();
            }

            Node = Next;
        }

        return UnparkedCount;
    }

} // namespace OloEngine::LowLevelTasks::Private
