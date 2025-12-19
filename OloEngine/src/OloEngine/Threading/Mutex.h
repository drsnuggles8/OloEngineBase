// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Threading/LockTags.h"
#include <atomic>

namespace OloEngine
{
    // A one-byte mutex that is not fair and does not support recursive locking.
    class FMutex final
    {
    public:
        constexpr FMutex() = default;

        // Construct in a locked state. Avoids an expensive compare-and-swap at creation time.
        OLO_FINLINE explicit FMutex(FAcquireLock)
            : m_State(IsLockedFlag)
        {
        }

        FMutex(const FMutex&) = delete;
        FMutex& operator=(const FMutex&) = delete;

        [[nodiscard]] OLO_FINLINE bool IsLocked() const
        {
            return !!(m_State.load(std::memory_order_relaxed) & IsLockedFlag);
        }

        [[nodiscard]] OLO_FINLINE bool TryLock()
        {
            u8 Expected = m_State.load(std::memory_order_relaxed);
            return !(Expected & IsLockedFlag) &&
                m_State.compare_exchange_strong(Expected, Expected | IsLockedFlag, std::memory_order_acquire, std::memory_order_relaxed);
        }

        OLO_FINLINE void Lock()
        {
            // Fast path: expect unlocked state (State == 0), atomically set to locked
            u8 Expected = 0;
            if (OLO_LIKELY(m_State.compare_exchange_weak(Expected, IsLockedFlag, std::memory_order_acquire, std::memory_order_relaxed)))
            {
                return;
            }
            LockSlow();
        }

        OLO_FINLINE void Unlock()
        {
            // Unlock immediately to allow other threads to acquire the lock while this thread looks for a thread to wake.
            const u8 LastState = m_State.fetch_sub(IsLockedFlag, std::memory_order_release);
            if (OLO_LIKELY(!(LastState & MayHaveWaitingLockFlag)))
            {
                return;
            }
            WakeWaitingThread();
        }

        // Try to wake a waiting thread. Returns true if a thread was woken.
        [[nodiscard]] bool TryWakeWaitingThread();

    private:
        void LockSlow();
        void WakeWaitingThread();

        // FParams for TIntrusiveMutex delegation - defined in Mutex.cpp
        struct FParams;

    public:
        // Public for intrusive mutex access
        static constexpr u8 IsLockedFlag = 1 << 0;
        static constexpr u8 MayHaveWaitingLockFlag = 1 << 1;

    private:
        friend void LockSlowCanWait(FMutex*);  // Allow internal lambda access

        std::atomic<u8> m_State = 0;
    };
} // namespace OloEngine
