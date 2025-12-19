// RecursiveMutex.cpp - Recursive mutex with parking lot implementation
// Ported 1:1 from UE5.7 FRecursiveMutex

#include "OloEngine/Threading/RecursiveMutex.h"
#include "OloEngine/Core/Assert.h"
#include "OloEngine/Core/PlatformTLS.h"
#include "OloEngine/HAL/ParkingLot.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/Task/Oversubscription.h"

namespace OloEngine
{
    bool FRecursiveMutex::TryLock()
    {
        const u32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();
        u32 CurrentState = m_State.load(std::memory_order_relaxed);

        // Try to acquire the lock if it was unlocked, even if there are waiting threads.
        // Acquiring the lock despite the waiting threads means that this lock is not FIFO and thus not fair.
        if (OLO_LIKELY(!(CurrentState & LockCountMask)))
        {
            if (OLO_LIKELY(m_State.compare_exchange_strong(CurrentState, CurrentState | (1 << LockCountShift),
                                                           std::memory_order_acquire, std::memory_order_relaxed)))
            {
                OLO_CORE_CHECK_SLOW(m_ThreadId.load(std::memory_order_relaxed) == 0, "ThreadId should be 0 when uncontended lock is acquired");
                m_ThreadId.store(CurrentThreadId, std::memory_order_relaxed);
                return true;
            }
        }

        // Lock recursively if this is the thread that holds the lock.
        if (m_ThreadId.load(std::memory_order_relaxed) == CurrentThreadId)
        {
            m_State.fetch_add(1 << LockCountShift, std::memory_order_relaxed);
            return true;
        }

        return false;
    }

    void FRecursiveMutex::Lock()
    {
        const u32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();
        u32 CurrentState = m_State.load(std::memory_order_relaxed);

        // Try to acquire the lock if it was unlocked, even if there are waiting threads.
        // Acquiring the lock despite the waiting threads means that this lock is not FIFO and thus not fair.
        if (OLO_LIKELY(!(CurrentState & LockCountMask)))
        {
            if (OLO_LIKELY(m_State.compare_exchange_weak(CurrentState, CurrentState | (1 << LockCountShift),
                                                         std::memory_order_acquire, std::memory_order_relaxed)))
            {
                OLO_CORE_CHECK_SLOW(m_ThreadId.load(std::memory_order_relaxed) == 0, "ThreadId should be 0 when uncontended lock is acquired");
                m_ThreadId.store(CurrentThreadId, std::memory_order_relaxed);
                return;
            }
        }

        // Lock recursively if this is the thread that holds the lock.
        if (m_ThreadId.load(std::memory_order_relaxed) == CurrentThreadId)
        {
            m_State.fetch_add(1 << LockCountShift, std::memory_order_relaxed);
            return;
        }

        LockSlow(CurrentState, CurrentThreadId);
    }

    void FRecursiveMutex::Unlock()
    {
        u32 CurrentState = m_State.load(std::memory_order_relaxed);
        OLO_CORE_CHECK_SLOW(CurrentState & LockCountMask, "FRecursiveMutex::Unlock called without matching Lock");
        OLO_CORE_CHECK_SLOW(m_ThreadId.load(std::memory_order_relaxed) == FPlatformTLS::GetCurrentThreadId(),
                            "FRecursiveMutex::Unlock called from wrong thread");

        if (OLO_LIKELY((CurrentState & LockCountMask) == (1 << LockCountShift)))
        {
            // Remove the association with this thread before unlocking.
            m_ThreadId.store(0, std::memory_order_relaxed);

            // Unlock immediately to allow other threads to acquire the lock while this thread looks for a thread to wake.
            const u32 LastState = m_State.fetch_sub(1 << LockCountShift, std::memory_order_release);

            // Wake one exclusive waiter if there are waiting threads.
            if (OLO_UNLIKELY(LastState & MayHaveWaitingLockFlag))
            {
                WakeWaitingThread();
            }
        }
        else
        {
            // This is recursively locked. Decrement the lock count.
            m_State.fetch_sub(1 << LockCountShift, std::memory_order_relaxed);
        }
    }

    OLO_NOINLINE void FRecursiveMutex::LockSlow(u32 CurrentState, u32 InThreadId)
    {
        constexpr i32 SpinLimit = 40;
        i32 SpinCount = 0;

        for (;;)
        {
            // Try to acquire the lock if it was unlocked, even if there are waiting threads.
            // Acquiring the lock despite the waiting threads means that this lock is not FIFO and thus not fair.
            if (OLO_LIKELY(!(CurrentState & LockCountMask)))
            {
                if (OLO_LIKELY(m_State.compare_exchange_weak(CurrentState, CurrentState | (1 << LockCountShift),
                                                             std::memory_order_acquire, std::memory_order_relaxed)))
                {
                    OLO_CORE_CHECK_SLOW(m_ThreadId.load(std::memory_order_relaxed) == 0, "ThreadId should be 0 when lock is acquired");
                    m_ThreadId.store(InThreadId, std::memory_order_relaxed);
                    return;
                }
                continue;
            }

            // Spin up to the spin limit while there are no waiting threads.
            if (OLO_LIKELY(!(CurrentState & MayHaveWaitingLockFlag) && SpinCount < SpinLimit))
            {
                FPlatformProcess::Yield();
                ++SpinCount;
                CurrentState = m_State.load(std::memory_order_relaxed);
                continue;
            }

            // Store that there are waiting threads. Restart if the state has changed since it was loaded.
            if (OLO_LIKELY(!(CurrentState & MayHaveWaitingLockFlag)))
            {
                if (OLO_UNLIKELY(!m_State.compare_exchange_weak(CurrentState, CurrentState | MayHaveWaitingLockFlag,
                                                                std::memory_order_relaxed)))
                {
                    continue;
                }
                CurrentState |= MayHaveWaitingLockFlag;
            }

            // Do not enter oversubscription during a wait on a mutex since the wait is generally too short
            // for it to matter and it can worsen performance a lot for heavily contended locks.
            LowLevelTasks::Private::FOversubscriptionAllowedScope _(false);

            // Wait if the state has not changed. Either way, loop back and try to acquire the lock after trying to wait.
            struct FWaitContext
            {
                FRecursiveMutex* Mutex;
                u32 ExpectedState;
            };
            FWaitContext WaitCtx{ this, CurrentState };

            ParkingLot::Wait(
                &m_State,
                [](void* Context) -> bool
                {
                    FWaitContext* Ctx = static_cast<FWaitContext*>(Context);
                    return Ctx->Mutex->m_State.load(std::memory_order_relaxed) == Ctx->ExpectedState;
                },
                &WaitCtx,
                nullptr,
                nullptr);
            CurrentState = m_State.load(std::memory_order_relaxed);
        }
    }

    OLO_NOINLINE void FRecursiveMutex::WakeWaitingThread()
    {
        // Use callback to properly check if there are still waiting threads
        ParkingLot::WakeOne(&m_State, [](void* Context, ParkingLot::FWakeState WakeState) -> u64
                            {
			FRecursiveMutex* Self = static_cast<FRecursiveMutex*>(Context);
			if (!WakeState.bHasWaitingThreads)
			{
				Self->m_State.fetch_and(~MayHaveWaitingLockFlag, std::memory_order_relaxed);
			}
			return 0; }, this);
    }

} // namespace OloEngine
