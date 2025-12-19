// SharedRecursiveMutex.h - Shared mutex that supports recursive locking
// Ported from UE5.7 UE::FSharedRecursiveMutex

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Threading/SharedLock.h"
#include <atomic>

namespace OloEngine
{
    // Forward declaration
    class FSharedRecursiveMutex;

    namespace Private
    {
        // Link node for tracking shared locks per thread
        struct FSharedRecursiveMutexLink
        {
            [[nodiscard]] static bool Owns(const FSharedRecursiveMutex* Mutex);
            void Push(const FSharedRecursiveMutex* Mutex);
            void Pop();

            const FSharedRecursiveMutex* OwnedMutex = nullptr;
            FSharedRecursiveMutexLink* Next = nullptr;
        };

    } // namespace Private

    // An eight-byte shared mutex that is not fair and supports recursive locking.
    //
    // Prefer FRecursiveMutex when shared locking is not required.
    // Prefer FSharedMutex when recursive locking is not required.
    // All non-recursive shared locks will wait when any thread is waiting to take an exclusive lock.
    // An exclusive lock and a shared lock may not be simultaneously held by the same thread.
    class FSharedRecursiveMutex final
    {
      public:
        constexpr FSharedRecursiveMutex() = default;

        FSharedRecursiveMutex(const FSharedRecursiveMutex&) = delete;
        FSharedRecursiveMutex& operator=(const FSharedRecursiveMutex&) = delete;

        [[nodiscard]] bool IsLocked() const
        {
            return !!(m_State.load(std::memory_order_relaxed) & LockCountMask);
        }

        [[nodiscard]] bool TryLock();
        void Lock();
        void Unlock();

        [[nodiscard]] bool IsLockShared() const
        {
            return !!(m_State.load(std::memory_order_relaxed) & SharedLockCountMask);
        }

        // Use TSharedLock or TDynamicSharedLock to acquire a shared lock.
        [[nodiscard]] bool TryLockShared(Private::FSharedRecursiveMutexLink& Link);
        void LockShared(Private::FSharedRecursiveMutexLink& Link);
        void UnlockShared(Private::FSharedRecursiveMutexLink& Link);

      private:
        void LockSlow(u32 CurrentState, u32 CurrentThreadId);
        void LockSharedSlow(Private::FSharedRecursiveMutexLink& Link);
        void WakeWaitingThreads(u32 CurrentState);

        const void* GetLockAddress() const;
        const void* GetSharedLockAddress() const;

        static constexpr u32 MayHaveWaitingLockFlag = 1 << 0;
        static constexpr u32 MayHaveWaitingSharedLockFlag = 1 << 1;
        static constexpr u32 LockCountShift = 2;
        static constexpr u32 LockCountMask = 0x0000'0ffc;
        static constexpr u32 SharedLockCountShift = 12;
        static constexpr u32 SharedLockCountMask = 0xffff'f000;

        std::atomic<u32> m_State{ 0 };
        std::atomic<u32> m_ThreadId{ 0 };
    };

    // Specialized TSharedLock for FSharedRecursiveMutex
    template<>
    class TSharedLock<FSharedRecursiveMutex> final
    {
      public:
        TSharedLock(const TSharedLock&) = delete;
        TSharedLock& operator=(const TSharedLock&) = delete;

        [[nodiscard]] explicit TSharedLock(FSharedRecursiveMutex& Lock)
            : m_Mutex(Lock)
        {
            m_Mutex.LockShared(m_Link);
        }

        ~TSharedLock()
        {
            m_Mutex.UnlockShared(m_Link);
        }

      private:
        FSharedRecursiveMutex& m_Mutex;
        Private::FSharedRecursiveMutexLink m_Link;
    };

    // Specialized TDynamicSharedLock for FSharedRecursiveMutex
    template<>
    class TDynamicSharedLock<FSharedRecursiveMutex> final
    {
      public:
        TDynamicSharedLock() = default;

        TDynamicSharedLock(const TDynamicSharedLock&) = delete;
        TDynamicSharedLock& operator=(const TDynamicSharedLock&) = delete;

        [[nodiscard]] explicit TDynamicSharedLock(FSharedRecursiveMutex& Lock)
            : m_Mutex(&Lock)
        {
            m_Mutex->LockShared(m_Link);
            m_bLocked = true;
        }

        [[nodiscard]] explicit TDynamicSharedLock(FSharedRecursiveMutex& Lock, FDeferLock)
            : m_Mutex(&Lock)
        {
        }

        [[nodiscard]] TDynamicSharedLock(TDynamicSharedLock&& Other)
            : m_Mutex(Other.m_Mutex), m_bLocked(Other.m_bLocked)
        {
            if (m_bLocked)
            {
                m_Mutex->LockShared(m_Link);
                m_Mutex->UnlockShared(Other.m_Link);
            }
            Other.m_Mutex = nullptr;
            Other.m_bLocked = false;
        }

        TDynamicSharedLock& operator=(TDynamicSharedLock&& Other)
        {
            if (m_bLocked)
            {
                m_Mutex->UnlockShared(m_Link);
            }
            m_Mutex = Other.m_Mutex;
            m_bLocked = Other.m_bLocked;
            if (m_bLocked)
            {
                m_Mutex->LockShared(m_Link);
                m_Mutex->UnlockShared(Other.m_Link);
            }
            Other.m_Mutex = nullptr;
            Other.m_bLocked = false;
            return *this;
        }

        ~TDynamicSharedLock()
        {
            if (m_bLocked)
            {
                m_Mutex->UnlockShared(m_Link);
            }
        }

        [[nodiscard]] bool TryLock()
        {
            OLO_CORE_ASSERT(!m_bLocked, "Already locked");
            OLO_CORE_ASSERT(m_Mutex, "No mutex");
            m_bLocked = m_Mutex->TryLockShared(m_Link);
            return m_bLocked;
        }

        void Lock()
        {
            OLO_CORE_ASSERT(!m_bLocked, "Already locked");
            OLO_CORE_ASSERT(m_Mutex, "No mutex");
            m_Mutex->LockShared(m_Link);
            m_bLocked = true;
        }

        void Unlock()
        {
            OLO_CORE_ASSERT(m_bLocked, "Not locked");
            m_bLocked = false;
            m_Mutex->UnlockShared(m_Link);
        }

        [[nodiscard]] bool OwnsLock() const
        {
            return m_bLocked;
        }

        explicit operator bool() const
        {
            return OwnsLock();
        }

      private:
        FSharedRecursiveMutex* m_Mutex = nullptr;
        Private::FSharedRecursiveMutexLink m_Link;
        bool m_bLocked = false;
    };

} // namespace OloEngine
