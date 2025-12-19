// SharedRecursiveMutex.cpp - Implementation of FSharedRecursiveMutex
// Ported from UE5.7 UE::FSharedRecursiveMutex

#include "OloEngine/Threading/SharedRecursiveMutex.h"
#include "OloEngine/HAL/ParkingLot.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/Core/PlatformTLS.h"
#include "OloEngine/Task/Oversubscription.h"

namespace OloEngine
{

    namespace Private
    {
        struct FSharedRecursiveMutexStack
        {
            constexpr FSharedRecursiveMutexStack() = default;

            ~FSharedRecursiveMutexStack()
            {
                OLO_CORE_ASSERT(!Top, "Thread destroyed while holding a shared lock on FSharedRecursiveMutex");
            }

            FSharedRecursiveMutexLink* Top = nullptr;
        };

        static thread_local FSharedRecursiveMutexStack ThreadLocalSharedLocks;

        bool FSharedRecursiveMutexLink::Owns(const FSharedRecursiveMutex* Mutex)
        {
            for (FSharedRecursiveMutexLink* It = ThreadLocalSharedLocks.Top; It; It = It->Next)
            {
                if (It->OwnedMutex == Mutex)
                {
                    return true;
                }
            }
            return false;
        }

        void FSharedRecursiveMutexLink::Push(const FSharedRecursiveMutex* Mutex)
        {
            OLO_CORE_ASSERT(!OwnedMutex && !Next, "Link already in use");
            OwnedMutex = Mutex;
            Next = ThreadLocalSharedLocks.Top;
            ThreadLocalSharedLocks.Top = this;
        }

        void FSharedRecursiveMutexLink::Pop()
        {
            OLO_CORE_ASSERT(OwnedMutex, "Link not in use");
            for (FSharedRecursiveMutexLink** Link = &ThreadLocalSharedLocks.Top; *Link; Link = &(*Link)->Next)
            {
                if ((*Link) == this)
                {
                    *Link = Next;
                    OwnedMutex = nullptr;
                    Next = nullptr;
                    return;
                }
            }
        }

    } // namespace Private

    const void* FSharedRecursiveMutex::GetLockAddress() const
    {
        return &m_State;
    }

    const void* FSharedRecursiveMutex::GetSharedLockAddress() const
    {
        // Shared locks need a distinct address from exclusive locks
        return reinterpret_cast<const u8*>(&m_State) + 1;
    }

    bool FSharedRecursiveMutex::TryLock()
    {
        const u32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();
        u32 CurrentState = m_State.load(std::memory_order_relaxed);

        // Try to acquire the lock if it was unlocked
        if (OLO_LIKELY(!(CurrentState & (LockCountMask | SharedLockCountMask))))
        {
            if (OLO_LIKELY(m_State.compare_exchange_strong(CurrentState, CurrentState | (1 << LockCountShift), std::memory_order_acquire, std::memory_order_relaxed)))
            {
                m_ThreadId.store(CurrentThreadId, std::memory_order_relaxed);
                return true;
            }
        }

        // Lock recursively if this is the thread that holds the lock
        if (m_ThreadId.load(std::memory_order_relaxed) == CurrentThreadId)
        {
            OLO_CORE_ASSERT((CurrentState & LockCountMask) != LockCountMask, "Lock count overflow");
            m_State.fetch_add(1 << LockCountShift, std::memory_order_relaxed);
            return true;
        }

        return false;
    }

    void FSharedRecursiveMutex::Lock()
    {
        const u32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();
        u32 CurrentState = m_State.load(std::memory_order_relaxed);

        // Try to acquire the lock if it was unlocked
        if (OLO_LIKELY(!(CurrentState & (LockCountMask | SharedLockCountMask))))
        {
            if (OLO_LIKELY(m_State.compare_exchange_weak(CurrentState, CurrentState | (1 << LockCountShift), std::memory_order_acquire, std::memory_order_relaxed)))
            {
                m_ThreadId.store(CurrentThreadId, std::memory_order_relaxed);
                return;
            }
        }

        // Lock recursively if this is the thread that holds the lock
        if (m_ThreadId.load(std::memory_order_relaxed) == CurrentThreadId)
        {
            OLO_CORE_ASSERT((CurrentState & LockCountMask) != LockCountMask, "Lock count overflow");
            m_State.fetch_add(1 << LockCountShift, std::memory_order_relaxed);
            return;
        }

        LockSlow(CurrentState, CurrentThreadId);
    }

    void FSharedRecursiveMutex::LockSlow(u32 CurrentState, const u32 CurrentThreadId)
    {
        constexpr i32 SpinLimit = 40;
        i32 SpinCount = 0;
        for (;;)
        {
            // Try to acquire the lock if it was unlocked
            if (OLO_LIKELY(!(CurrentState & (LockCountMask | SharedLockCountMask))))
            {
                if (OLO_LIKELY(m_State.compare_exchange_weak(CurrentState, CurrentState | (1 << LockCountShift), std::memory_order_acquire, std::memory_order_relaxed)))
                {
                    m_ThreadId.store(CurrentThreadId, std::memory_order_relaxed);
                    return;
                }
                continue;
            }

            // Spin up to the spin limit while there are no waiting threads
            if (OLO_LIKELY(!(CurrentState & MayHaveWaitingLockFlag) && SpinCount < SpinLimit))
            {
                FPlatformProcess::Yield();
                ++SpinCount;
                CurrentState = m_State.load(std::memory_order_relaxed);
                continue;
            }

            // Store that there are waiting threads
            if (OLO_LIKELY(!(CurrentState & MayHaveWaitingLockFlag)))
            {
                if (OLO_UNLIKELY(!m_State.compare_exchange_weak(CurrentState, CurrentState | MayHaveWaitingLockFlag, std::memory_order_relaxed)))
                {
                    continue;
                }
                CurrentState |= MayHaveWaitingLockFlag;
            }

            // Do not enter oversubscription during a wait on a mutex
            LowLevelTasks::Private::FOversubscriptionAllowedScope Scope(false);

            // Wait if the state has not changed
            ParkingLot::Wait(GetLockAddress(), [this, CurrentState]
                             { return m_State.load(std::memory_order_relaxed) == CurrentState; }, nullptr);
            CurrentState = m_State.load(std::memory_order_relaxed);
        }
    }

    void FSharedRecursiveMutex::Unlock()
    {
        u32 CurrentState = m_State.load(std::memory_order_relaxed);
        OLO_CORE_ASSERT(CurrentState & LockCountMask, "Not locked");
        OLO_CORE_ASSERT(m_ThreadId.load(std::memory_order_relaxed) == FPlatformTLS::GetCurrentThreadId(), "Wrong thread");

        if (OLO_LIKELY((CurrentState & LockCountMask) == (1 << LockCountShift)))
        {
            // Remove the association with this thread before unlocking
            m_ThreadId.store(0, std::memory_order_relaxed);

            // Unlock immediately
            u32 LastState = m_State.fetch_sub(1 << LockCountShift, std::memory_order_release);

            // Wake one exclusive waiter or every shared waiter if there are waiting threads
            if (OLO_UNLIKELY(LastState & (MayHaveWaitingLockFlag | MayHaveWaitingSharedLockFlag)))
            {
                WakeWaitingThreads(LastState);
            }
        }
        else
        {
            // This is recursively locked. Decrement the lock count.
            m_State.fetch_sub(1 << LockCountShift, std::memory_order_relaxed);
        }
    }

    bool FSharedRecursiveMutex::TryLockShared(Private::FSharedRecursiveMutexLink& Link)
    {
        u32 CurrentState = m_State.load(std::memory_order_relaxed);

        // Recursive shared locks are quick to acquire
        if ((CurrentState & SharedLockCountMask) && Private::FSharedRecursiveMutexLink::Owns(this))
        {
            [[maybe_unused]] u32 LastState = m_State.fetch_add(1 << SharedLockCountShift, std::memory_order_relaxed);
            OLO_CORE_ASSERT((LastState & SharedLockCountMask) != SharedLockCountMask, "Shared lock count overflow");
            Link.Push(this);
            return true;
        }

        // Try to acquire a shared lock if there is no active or waiting exclusive lock
        while (!(CurrentState & (LockCountMask | MayHaveWaitingLockFlag)))
        {
            OLO_CORE_ASSERT((CurrentState & SharedLockCountMask) != SharedLockCountMask, "Shared lock count overflow");
            if (m_State.compare_exchange_weak(CurrentState, CurrentState + (1 << SharedLockCountShift), std::memory_order_acquire, std::memory_order_relaxed))
            {
                Link.Push(this);
                return true;
            }
        }
        return false;
    }

    void FSharedRecursiveMutex::LockShared(Private::FSharedRecursiveMutexLink& Link)
    {
        u32 CurrentState = m_State.load(std::memory_order_relaxed);

        // Recursive shared locks are quick to acquire
        if ((CurrentState & SharedLockCountMask) && Private::FSharedRecursiveMutexLink::Owns(this))
        {
            [[maybe_unused]] u32 LastState = m_State.fetch_add(1 << SharedLockCountShift, std::memory_order_relaxed);
            OLO_CORE_ASSERT((LastState & SharedLockCountMask) != SharedLockCountMask, "Shared lock count overflow");
            Link.Push(this);
            return;
        }

        // Try to acquire a shared lock if there is no active or waiting exclusive lock
        if (!(CurrentState & (LockCountMask | MayHaveWaitingLockFlag)))
        {
            OLO_CORE_ASSERT((CurrentState & SharedLockCountMask) != SharedLockCountMask, "Shared lock count overflow");
            if (m_State.compare_exchange_weak(CurrentState, CurrentState + (1 << SharedLockCountShift), std::memory_order_acquire, std::memory_order_relaxed))
            {
                Link.Push(this);
                return;
            }
        }
        LockSharedSlow(Link);
    }

    void FSharedRecursiveMutex::LockSharedSlow(Private::FSharedRecursiveMutexLink& Link)
    {
        constexpr i32 SpinLimit = 40;
        i32 SpinCount = 0;
        for (u32 CurrentState = m_State.load(std::memory_order_relaxed);;)
        {
            // Try to acquire the lock if it is unlocked and there are no waiting threads
            if (OLO_LIKELY(!(CurrentState & (LockCountMask | MayHaveWaitingLockFlag))))
            {
                OLO_CORE_ASSERT((CurrentState & SharedLockCountMask) != SharedLockCountMask, "Shared lock count overflow");
                if (OLO_LIKELY(m_State.compare_exchange_weak(CurrentState, CurrentState + (1 << SharedLockCountShift), std::memory_order_acquire, std::memory_order_relaxed)))
                {
                    Link.Push(this);
                    return;
                }
                continue;
            }

            // Spin up to the spin limit while there are no waiting threads
            if (OLO_LIKELY(!(CurrentState & MayHaveWaitingLockFlag) && SpinCount < SpinLimit))
            {
                FPlatformProcess::Yield();
                ++SpinCount;
                CurrentState = m_State.load(std::memory_order_relaxed);
                continue;
            }

            // Store that there are waiting threads
            if (OLO_LIKELY(!(CurrentState & MayHaveWaitingSharedLockFlag)))
            {
                if (OLO_UNLIKELY(!m_State.compare_exchange_weak(CurrentState, CurrentState | MayHaveWaitingSharedLockFlag, std::memory_order_relaxed)))
                {
                    continue;
                }
                CurrentState |= MayHaveWaitingSharedLockFlag;
            }

            // Do not enter oversubscription during a wait on a mutex
            LowLevelTasks::Private::FOversubscriptionAllowedScope Scope(false);

            // Wait if the state has not changed
            ParkingLot::Wait(GetSharedLockAddress(), [this, CurrentState]
                             { return m_State.load(std::memory_order_relaxed) == CurrentState; }, nullptr);
            CurrentState = m_State.load(std::memory_order_relaxed);
        }
    }

    void FSharedRecursiveMutex::UnlockShared(Private::FSharedRecursiveMutexLink& Link)
    {
        Link.Pop();
        const u32 LastState = m_State.fetch_sub(1 << SharedLockCountShift, std::memory_order_release);
        OLO_CORE_ASSERT(LastState & SharedLockCountMask, "Not shared locked");

        constexpr u32 WakeState = MayHaveWaitingLockFlag | (1 << SharedLockCountShift);
        if (OLO_UNLIKELY((LastState & ~MayHaveWaitingSharedLockFlag) == WakeState))
        {
            // The last shared lock was released and there is a waiting exclusive lock
            ParkingLot::WakeOne(GetLockAddress(), [this](ParkingLot::FWakeState WakeState) -> u64
                                {
			if (!WakeState.bDidWake)
			{
				m_State.fetch_and(~MayHaveWaitingLockFlag, std::memory_order_relaxed);
			}
			return 0; });
        }
    }

    void FSharedRecursiveMutex::WakeWaitingThreads(u32 LastState)
    {
        if (LastState & MayHaveWaitingLockFlag)
        {
            // Wake one thread that is waiting to acquire an exclusive lock
            bool bDidWake = false;
            ParkingLot::WakeOne(GetLockAddress(), [this, &bDidWake](ParkingLot::FWakeState WakeState) -> u64
                                {
			if (!WakeState.bDidWake)
			{
				m_State.fetch_and(~MayHaveWaitingLockFlag, std::memory_order_relaxed);
			}
			bDidWake = WakeState.bDidWake;
			return 0; });
            if (bDidWake)
            {
                return;
            }

            // Reload the state if there were no shared waiters
            if (!(LastState & MayHaveWaitingSharedLockFlag))
            {
                LastState = m_State.load(std::memory_order_relaxed);
            }
        }

        if (LastState & MayHaveWaitingSharedLockFlag)
        {
            // Wake every thread that is waiting to acquire a shared lock
            if (m_State.fetch_and(~MayHaveWaitingSharedLockFlag, std::memory_order_relaxed) & MayHaveWaitingSharedLockFlag)
            {
                ParkingLot::WakeAll(GetSharedLockAddress());
            }
        }
    }

} // namespace OloEngine
