// RecursiveWordMutex.h - Pointer-sized recursive mutex without ParkingLot dependency
// Ported from UE5.7 Async/RecursiveWordMutex.h

#pragma once

// @file RecursiveWordMutex.h
// @brief A recursive mutex that is the size of a pointer and does not depend on ParkingLot.
//
// Prefer FRecursiveMutex to FRecursiveWordMutex whenever possible.
// This mutex is not fair and supports recursive locking.
//
// This type is valuable when a mutex must be trivially constructible, trivially
// destructible, or must be functional before or after static initialization.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/PlatformTLS.h"
#include "OloEngine/Threading/WordMutex.h"

#include <atomic>

namespace OloEngine
{

    // @class FRecursiveWordMutex
    // @brief A recursive mutex that is the size of a pointer and does not depend on ParkingLot.
    //
    // Prefer FRecursiveMutex to FRecursiveWordMutex whenever possible.
    // This mutex is not fair and supports recursive locking.
    class FRecursiveWordMutex final
    {
      public:
        constexpr FRecursiveWordMutex() = default;

        FRecursiveWordMutex(const FRecursiveWordMutex&) = delete;
        FRecursiveWordMutex& operator=(const FRecursiveWordMutex&) = delete;

        // @brief Try to acquire the lock without blocking
        // @return true if lock was acquired
        [[nodiscard]] inline bool TryLock()
        {
            const u32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();
            if (m_ThreadId.load(std::memory_order_relaxed) == CurrentThreadId)
            {
                ++m_RecursionCount;
                return true;
            }
            else if (m_Mutex.TryLock())
            {
                m_ThreadId.store(CurrentThreadId, std::memory_order_relaxed);
                return true;
            }
            return false;
        }

        // @brief Acquire the lock, blocking until available
        inline void Lock()
        {
            const u32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();
            if (m_ThreadId.load(std::memory_order_relaxed) == CurrentThreadId)
            {
                ++m_RecursionCount;
            }
            else
            {
                m_Mutex.Lock();
                m_ThreadId.store(CurrentThreadId, std::memory_order_relaxed);
            }
        }

        // @brief Release the lock
        inline void Unlock()
        {
            if (m_RecursionCount > 0)
            {
                --m_RecursionCount;
            }
            else
            {
                m_ThreadId.store(0, std::memory_order_relaxed);
                m_Mutex.Unlock();
            }
        }

        // @brief Check if the mutex is held by the current thread
        [[nodiscard]] inline bool IsLockedByCurrentThread() const
        {
            return m_ThreadId.load(std::memory_order_relaxed) == FPlatformTLS::GetCurrentThreadId();
        }

      private:
        FWordMutex m_Mutex;
        u32 m_RecursionCount = 0;
        std::atomic<u32> m_ThreadId{ 0 };
    };

} // namespace OloEngine
