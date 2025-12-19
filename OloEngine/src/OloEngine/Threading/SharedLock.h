// SharedLock.h - Shared lock wrappers for reader-writer mutexes
// Ported from UE5.7 UE::TSharedLock, UE::TDynamicSharedLock

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Threading/LockTags.h"

namespace OloEngine
{
    // Forward declaration - FSharedRecursiveMutex has specialized versions
    class FSharedRecursiveMutex;

    // A basic shared mutex ownership wrapper that locks on construction and unlocks on destruction.
    //
    // LockType must contain LockShared() and UnlockShared() functions.
    //
    // Use with mutex types like FSharedMutex and FSharedRecursiveMutex.
    template<typename LockType>
    class TSharedLock final
    {
      public:
        TSharedLock(const TSharedLock&) = delete;
        TSharedLock& operator=(const TSharedLock&) = delete;

        [[nodiscard]] explicit TSharedLock(LockType& Lock)
            : m_Mutex(Lock)
        {
            m_Mutex.LockShared();
        }

        ~TSharedLock()
        {
            m_Mutex.UnlockShared();
        }

      private:
        LockType& m_Mutex;
    };

    // A shared mutex ownership wrapper that allows dynamic locking, unlocking, and deferred locking.
    //
    // LockType must contain LockShared() and UnlockShared() functions.
    //
    // Use with mutex types like FSharedMutex and FSharedRecursiveMutex.
    template<typename LockType>
    class TDynamicSharedLock final
    {
      public:
        TDynamicSharedLock() = default;

        TDynamicSharedLock(const TDynamicSharedLock&) = delete;
        TDynamicSharedLock& operator=(const TDynamicSharedLock&) = delete;

        // Wrap a mutex and lock it in shared mode.
        [[nodiscard]] explicit TDynamicSharedLock(LockType& Lock)
            : m_Mutex(&Lock)
        {
            m_Mutex->LockShared();
            m_bLocked = true;
        }

        // Wrap a mutex without locking it in shared mode.
        [[nodiscard]] explicit TDynamicSharedLock(LockType& Lock, FDeferLock)
            : m_Mutex(&Lock)
        {
        }

        // Move from another lock, transferring any ownership to this lock.
        [[nodiscard]] TDynamicSharedLock(TDynamicSharedLock&& Other)
            : m_Mutex(Other.m_Mutex), m_bLocked(Other.m_bLocked)
        {
            Other.m_Mutex = nullptr;
            Other.m_bLocked = false;
        }

        // Move from another lock, transferring any ownership to this lock, and unlocking the previous mutex if locked.
        TDynamicSharedLock& operator=(TDynamicSharedLock&& Other)
        {
            if (m_bLocked)
            {
                m_Mutex->UnlockShared();
            }
            m_Mutex = Other.m_Mutex;
            m_bLocked = Other.m_bLocked;
            Other.m_Mutex = nullptr;
            Other.m_bLocked = false;
            return *this;
        }

        // Unlock the mutex if locked.
        ~TDynamicSharedLock()
        {
            if (m_bLocked)
            {
                m_Mutex->UnlockShared();
            }
        }

        // Try to lock the associated mutex in shared mode. This lock must have a mutex and must not be locked.
        bool TryLock()
        {
            OLO_CORE_ASSERT(!m_bLocked, "Already locked");
            OLO_CORE_ASSERT(m_Mutex, "No mutex associated");
            m_bLocked = m_Mutex->TryLockShared();
            return m_bLocked;
        }

        // Lock the associated mutex in shared mode. This lock must have a mutex and must not be locked.
        void Lock()
        {
            OLO_CORE_ASSERT(!m_bLocked, "Already locked");
            OLO_CORE_ASSERT(m_Mutex, "No mutex associated");
            m_Mutex->LockShared();
            m_bLocked = true;
        }

        // Unlock the associated mutex in shared mode. This lock must have a mutex and must be locked.
        void Unlock()
        {
            OLO_CORE_ASSERT(m_bLocked, "Not locked");
            m_bLocked = false;
            m_Mutex->UnlockShared();
        }

        // Returns true if this lock has its associated mutex locked.
        bool OwnsLock() const
        {
            return m_bLocked;
        }

        // Returns true if this lock has its associated mutex locked.
        explicit operator bool() const
        {
            return OwnsLock();
        }

      private:
        LockType* m_Mutex = nullptr;
        bool m_bLocked = false;
    };

} // namespace OloEngine
