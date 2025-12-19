// RecursiveMutex.h - Recursive mutex with parking lot implementation
// Ported 1:1 from UE5.7 FRecursiveMutex

#pragma once

#include "OloEngine/Core/Base.h"
#include <atomic>

namespace OloEngine
{
    // @class FRecursiveMutex
    // @brief A recursive mutex that uses parking lot for efficient waiting
    //
    // This is an 8-byte recursive mutex that:
    // - Supports recursive locking (same thread can lock multiple times)
    // - Uses parking lot for efficient sleeping when contended
    // - Spins briefly before parking to optimize for low-contention cases
    //
    // Ported from UE5.7 FRecursiveMutex (Async/RecursiveMutex.h)
    //
    // Thread Safety: All methods are thread-safe.
    class FRecursiveMutex final
    {
      public:
        constexpr FRecursiveMutex() = default;

        FRecursiveMutex(const FRecursiveMutex&) = delete;
        FRecursiveMutex& operator=(const FRecursiveMutex&) = delete;
        FRecursiveMutex(FRecursiveMutex&&) = delete;
        FRecursiveMutex& operator=(FRecursiveMutex&&) = delete;

        // @brief Check if the mutex is currently locked by any thread
        // @return true if locked, false otherwise
        [[nodiscard]] bool IsLocked() const
        {
            return (m_State.load(std::memory_order_relaxed) & LockCountMask) != 0;
        }

        // @brief Try to acquire the lock without blocking
        // @return true if lock was acquired, false otherwise
        [[nodiscard]] bool TryLock();

        // @brief Acquire the lock, blocking if necessary
        //
        // If the current thread already owns the lock, increments the lock count.
        // Otherwise blocks until the lock becomes available.
        void Lock();

        // @brief Release the lock
        //
        // Decrements the lock count. If count reaches zero, releases the lock
        // and wakes any waiting thread.
        void Unlock();

      private:
        // @brief Slow path for Lock when fast path fails
        // @param CurrentState The current state of the mutex
        // @param InThreadId The current thread's ID
        void LockSlow(u32 CurrentState, u32 InThreadId);

        // @brief Wake a waiting thread after unlocking
        void WakeWaitingThread();

        // Bit layout of m_State:
        // Bit 0: MayHaveWaitingLockFlag - indicates threads may be waiting
        // Bits 1-31: LockCount - recursive lock count (shifted by 1)
        static constexpr u32 MayHaveWaitingLockFlag = 1 << 0;
        static constexpr u32 LockCountShift = 1;
        static constexpr u32 LockCountMask = 0xffff'fffe;

        std::atomic<u32> m_State{ 0 };
        std::atomic<u32> m_ThreadId{ 0 };
    };

} // namespace OloEngine
