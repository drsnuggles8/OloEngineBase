// Copyright (C) 2016 Dmitry Vyukov <dvyukov@google.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

// This implementation is based on EventCount.h
// included in the Eigen library but almost everything has been
// rewritten.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Task/TaskShared.h"
#include "OloEngine/HAL/Event.h"
#include "OloEngine/Templates/FunctionRef.h"

#include <atomic>

namespace OloEngine::LowLevelTasks::Private
{
    enum class EWaitState
    {
        NotSignaled = 0,
        Waiting,
        Signaled,
    };

    // @struct FWaitEvent
    // @brief A node in the waiting queue representing a sleeping thread
    // 
    // The struct is naturally 64 bytes aligned, the extra alignment just
    // re-enforces this assumption and will error if it changes in the future.
    struct alignas(64) FWaitEvent
    {
        std::atomic<u64>     Next{ 0 };
        u64                  Epoch{ 0 };
        std::atomic<EWaitState> State{ EWaitState::NotSignaled };
        FEventRef            Event{ EEventMode::ManualReset };
        bool                 bIsStandby{ false };
    };

    // @class FWaitingQueue
    // @brief A queue that manages sleeping and waking worker threads
    // 
    // This implements a parking lot pattern for efficient thread synchronization.
    // Workers prepare to wait, commit to waiting, and can be woken up by notifications.
    class FWaitingQueue
    {
        u32                            m_ThreadCount{ 0 };    // Normal amount of threads when there is no oversubscription.
        u32                            m_MaxThreadCount{ 0 }; // Max limit that can be reached during oversubscription period.
        TFunction<void()>              m_CreateThread;
        std::atomic<u32>               m_Oversubscription{ 0 };
        std::atomic<u64>               m_State;
        std::atomic<u64>               m_StandbyState;
        TAlignedArray<FWaitEvent>&     m_NodesArray;
        std::atomic<bool>              m_bIsShuttingDown{ false };
        FOversubscriptionLimitReached& m_OversubscriptionLimitReachedEvent;

    public:
        FWaitingQueue(TAlignedArray<FWaitEvent>& InNodesArray, FOversubscriptionLimitReached& InOversubscriptionLimitReachedEvent)
            : m_NodesArray(InNodesArray)
            , m_OversubscriptionLimitReachedEvent(InOversubscriptionLimitReachedEvent)
        {
        }

        FWaitingQueue(const FWaitingQueue&) = delete;
        FWaitingQueue& operator=(const FWaitingQueue&) = delete;

        void Init(u32 InThreadCount, u32 InMaxThreadCount, TFunction<void()> InCreateThread, u32 InActiveThreadCount);
        void StartShutdown();
        void FinishShutdown();

        // First step to execute when no more work is found in the queues.
        void PrepareStandby(FWaitEvent* Node);
        // Second step to execute when no more work is found in the queues.
        bool CommitStandby(FWaitEvent* Node, FOutOfWork& OutOfWork);

        // Immediately goes to sleep if oversubscription period is finished and we're over the allowed thread count.
        void ConditionalStandby(FWaitEvent* Node);

        // First step run by normal workers when no more work is found in the queues.
        void PrepareWait(FWaitEvent* Node);
        // Second step run by normal workers when no more work is found in the queues.
        bool CommitWait(FWaitEvent* Node, FOutOfWork& OutOfWork, i32 SpinCycles, i32 WaitCycles);

        // Step to run by normal workers if they detect new work after they called prepare wait.
        // Returns true if we need to wake up a new worker.
        bool CancelWait(FWaitEvent* Node);

        // Increment oversubscription and notify a thread if we're under the allowed thread count.
        // If dynamic thread creation is allowed, this could spawn a new thread if needed.
        void IncrementOversubscription();

        // Decrement oversubscription only, any active threads will finish their current task and will
        // go to sleep if conditional standby determines we're now over the active thread count.
        void DecrementOversubscription();

        // Is the current waiting queue out of workers
        bool IsOversubscriptionLimitReached() const;

        // Try to wake up the amount of workers passed in the parameters.
        // Return the number that were woken up.
        i32 Notify(i32 Count = 1)
        {
            return NotifyInternal(Count);
        }

    private:
        bool  TryStartNewThread();
        i32   NotifyInternal(i32 Count);
        void  Park(FWaitEvent* Node, FOutOfWork& OutOfWork, i32 SpinCycles, i32 WaitCycles);
        i32   Unpark(FWaitEvent* InNode);
        void  CheckState(u64 State, bool bIsWaiter = false);
        void  CheckStandbyState(u64 State);
    };

} // namespace OloEngine::LowLevelTasks::Private
