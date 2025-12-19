// SharedMutex.h - Shared (reader-writer) mutex
// Ported from UE5.7 Async/SharedMutex.h

#pragma once

// @file SharedMutex.h
// @brief A four-byte shared mutex (reader-writer lock)
// 
// This mutex allows multiple readers to hold the lock simultaneously,
// but only one writer can hold the lock at a time. Writers have priority
// over new readers when waiting.
// 
// Ported from Unreal Engine's Async/SharedMutex.h

#include "OloEngine/Core/Base.h"
#include "OloEngine/Threading/IntrusiveMutex.h"
#include "OloEngine/Threading/SharedLock.h"
#include "OloEngine/HAL/ParkingLot.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/Task/Oversubscription.h"

#include <atomic>

namespace OloEngine
{
    // @class FSharedMutex
    // @brief A four-byte shared mutex that is not fair and does not support recursive locking.
    //
    // Prefer FMutex when shared locking is not required.
    // All new shared locks will wait when any thread is waiting to take an exclusive lock.
    // An exclusive lock and a shared lock may not be simultaneously held by the same thread.
    // 
    // State bits layout:
    // - Bit 0: IsLockedFlag - set when exclusively locked
    // - Bit 1: MayHaveWaitingLockFlag - set when threads are waiting for exclusive lock
    // - Bit 2: MayHaveWaitingSharedLockFlag - set when threads are waiting for shared lock
    // - Bits 3-31: SharedLockCount - number of shared locks held
    class FSharedMutex final
    {
    public:
        constexpr FSharedMutex() = default;

        FSharedMutex(const FSharedMutex&) = delete;
        FSharedMutex& operator=(const FSharedMutex&) = delete;

        // ============================================================================
        // Exclusive (Write) Lock Operations
        // ============================================================================

        // @brief Check if the mutex is exclusively locked
        [[nodiscard]] inline bool IsLocked() const
        {
            return !!(m_State.load(std::memory_order_relaxed) & IsLockedFlag);
        }

        // @brief Try to acquire an exclusive lock without blocking
        // @return true if lock was acquired
        [[nodiscard]] inline bool TryLock()
        {
            u32 Expected = m_State.load(std::memory_order_relaxed);
            return !(Expected & (IsLockedFlag | SharedLockCountMask)) &&
                m_State.compare_exchange_strong(Expected, Expected | IsLockedFlag,
                    std::memory_order_acquire, std::memory_order_relaxed);
        }

        // @brief Acquire an exclusive lock, blocking until available
        inline void Lock()
        {
            u32 Expected = 0;
            if (OLO_LIKELY(m_State.compare_exchange_weak(Expected, IsLockedFlag, 
                std::memory_order_acquire, std::memory_order_relaxed)))
            {
                return;
            }
            LockSlow();
        }

        // @brief Release an exclusive lock
        inline void Unlock()
        {
            // Unlock immediately to allow other threads to acquire the lock while this thread 
            // looks for a thread to wake.
            u32 LastState = m_State.fetch_sub(IsLockedFlag, std::memory_order_release);
            OLO_CORE_ASSERT(LastState & IsLockedFlag, "FSharedMutex::Unlock called when not locked");
            if (OLO_LIKELY(!(LastState & (MayHaveWaitingLockFlag | MayHaveWaitingSharedLockFlag))))
            {
                return;
            }
            WakeWaitingThreads(LastState);
        }

        // ============================================================================
        // Shared (Read) Lock Operations
        // ============================================================================

        // @brief Check if the mutex has any shared locks
        [[nodiscard]] inline bool IsLockedShared() const
        {
            return !!(m_State.load(std::memory_order_relaxed) & SharedLockCountMask);
        }

        // @brief Try to acquire a shared lock without blocking
        // @return true if lock was acquired
        [[nodiscard]] inline bool TryLockShared()
        {
            u32 Expected = m_State.load(std::memory_order_relaxed);
            while (OLO_LIKELY(!(Expected & (IsLockedFlag | MayHaveWaitingLockFlag))))
            {
                if (OLO_LIKELY(m_State.compare_exchange_weak(Expected, Expected + (1 << SharedLockCountShift),
                        std::memory_order_acquire, std::memory_order_relaxed)))
                {
                    return true;
                }
            }
            return false;
        }

        // @brief Acquire a shared lock, blocking until available
        inline void LockShared()
        {
            u32 Expected = m_State.load(std::memory_order_relaxed);
            if (OLO_LIKELY(!(Expected & (IsLockedFlag | MayHaveWaitingLockFlag)) &&
                m_State.compare_exchange_weak(Expected, Expected + (1 << SharedLockCountShift),
                    std::memory_order_acquire, std::memory_order_relaxed)))
            {
                return;
            }
            LockSharedSlow();
        }

        // @brief Release a shared lock
        inline void UnlockShared()
        {
            // Unlock immediately to allow other threads to acquire the lock while this thread 
            // looks for a thread to wake.
            const u32 LastState = m_State.fetch_sub(1 << SharedLockCountShift, std::memory_order_release);
            OLO_CORE_ASSERT(LastState & SharedLockCountMask, "FSharedMutex::UnlockShared called when not shared-locked");
            constexpr u32 WakeState = MayHaveWaitingLockFlag | (1 << SharedLockCountShift);
            if (OLO_LIKELY((LastState & ~MayHaveWaitingSharedLockFlag) != WakeState))
            {
                return;
            }
            WakeWaitingThread();
        }

    private:
        // State bit layout - must be defined BEFORE FParams since FParams references these
        static constexpr u32 IsLockedFlag = 1 << 0;
        static constexpr u32 MayHaveWaitingLockFlag = 1 << 1;
        static constexpr u32 MayHaveWaitingSharedLockFlag = 1 << 2;
        static constexpr u32 SharedLockCountShift = 3;
        static constexpr u32 SharedLockCountMask = 0xffff'fff8;

        // Params for TIntrusiveMutex integration
        struct FParams
        {
            static constexpr u32 IsLockedFlag = FSharedMutex::IsLockedFlag;
            static constexpr u32 IsLockedMask = FSharedMutex::IsLockedFlag | FSharedMutex::SharedLockCountMask;
            static constexpr u32 MayHaveWaitingLockFlag = FSharedMutex::MayHaveWaitingLockFlag;

            inline static const void* GetWaitAddress(const std::atomic<u32>& State)
            {
                return &State;
            }
        };

        inline const void* GetSharedLockAddress() const
        {
            // Shared locks need a distinct address from exclusive locks to allow threads waiting 
            // for exclusive ownership to be woken up without waking any threads waiting for 
            // shared ownership.
            return reinterpret_cast<const u8*>(&m_State) + 1;
        }

        void LockSlow()
        {
            TIntrusiveMutex<FParams>::LockLoop(m_State);
        }

        void LockSharedSlow()
        {
            constexpr i32 SpinLimit = 40;
            i32 SpinCount = 0;
            for (u32 CurrentState = m_State.load(std::memory_order_relaxed);;)
            {
                // Try to acquire the lock if it is unlocked and there are no waiting threads.
                if (OLO_LIKELY(!(CurrentState & (IsLockedFlag | MayHaveWaitingLockFlag))))
                {
                    if (OLO_LIKELY(m_State.compare_exchange_weak(CurrentState, 
                        CurrentState + (1 << SharedLockCountShift), 
                        std::memory_order_acquire, std::memory_order_relaxed)))
                    {
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
                if (OLO_LIKELY(!(CurrentState & MayHaveWaitingSharedLockFlag)))
                {
                    if (OLO_UNLIKELY(!m_State.compare_exchange_weak(CurrentState, 
                        CurrentState | MayHaveWaitingSharedLockFlag, std::memory_order_relaxed)))
                    {
                        continue;
                    }
                    CurrentState |= MayHaveWaitingSharedLockFlag;
                }

                // Do not enter oversubscription during a wait on a mutex since the wait is generally 
                // too short for it to matter and it can worsen performance a lot for heavily contended locks.
                LowLevelTasks::Private::FOversubscriptionAllowedScope _(false);

                // Wait if the state has not changed. Either way, loop back and try to acquire the lock.
                ParkingLot::Wait(GetSharedLockAddress(), [this, CurrentState]() -> bool
                {
                    return m_State.load(std::memory_order_relaxed) == CurrentState;
                });
                CurrentState = m_State.load(std::memory_order_relaxed);
            }
        }

        OLO_NOINLINE void WakeWaitingThread()
        {
            TIntrusiveMutex<FParams>::WakeWaitingThread(m_State);
        }

        OLO_NOINLINE void WakeWaitingThreads(u32 LastState)
        {
            if (LastState & MayHaveWaitingLockFlag)
            {
                // Wake one thread that is waiting to acquire an exclusive lock.
                if (TIntrusiveMutex<FParams>::TryWakeWaitingThread(m_State))
                {
                    return;
                }

                // Reload the state if there were no shared waiters because new
                // ones may have registered themselves since LastState was read.
                if (!(LastState & MayHaveWaitingSharedLockFlag))
                {
                    LastState = m_State.load(std::memory_order_relaxed);
                }
            }

            if (LastState & MayHaveWaitingSharedLockFlag)
            {
                // Wake every thread that is waiting to acquire a shared lock.
                // The awoken threads might race against other exclusive locks.
                if (m_State.fetch_and(~MayHaveWaitingSharedLockFlag, std::memory_order_relaxed) & MayHaveWaitingSharedLockFlag)
                {
                    ParkingLot::WakeAll(GetSharedLockAddress());
                }
            }
        }

        std::atomic<u32> m_State{0};
    };

} // namespace OloEngine
