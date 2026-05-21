// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Threading/LockTags.h"

namespace OloEngine
{
    // A basic mutex ownership wrapper that locks on construction and unlocks on destruction.
    //
    // LockType must contain Lock() and Unlock() functions.
    //
    // Use with mutex types like FMutex and FRecursiveMutex.
    template<typename LockType>
    class TUniqueLock final
    {
      public:
        TUniqueLock(const TUniqueLock&) = delete;
        TUniqueLock& operator=(const TUniqueLock&) = delete;

        [[nodiscard]] OLO_FINLINE explicit TUniqueLock(LockType& Lock)
            : m_Mutex(Lock)
        {
            m_Mutex.Lock();
        }

        OLO_FINLINE ~TUniqueLock()
        {
            m_Mutex.Unlock();
        }

      private:
        LockType& m_Mutex;
    };

    // A mutex ownership wrapper that allows dynamic locking, unlocking, and deferred locking.
    //
    // LockType must contain Lock() and Unlock() functions.
    //
    // Use with mutex types like FMutex and FRecursiveMutex.
    template<typename LockType>
    class TDynamicUniqueLock final
    {
      public:
        TDynamicUniqueLock() = default;

        TDynamicUniqueLock(const TDynamicUniqueLock&) = delete;
        TDynamicUniqueLock& operator=(const TDynamicUniqueLock&) = delete;

        // Wrap a mutex and lock it.
        [[nodiscard]] OLO_FINLINE explicit TDynamicUniqueLock(LockType& Lock)
            : m_Mutex(&Lock)
        {
            m_Mutex->Lock();
            m_Locked = true;
        }

        // Wrap a mutex without locking it.
        [[nodiscard]] OLO_FINLINE explicit TDynamicUniqueLock(LockType& Lock, FDeferLock)
            : m_Mutex(&Lock)
        {
        }

        // Move from another lock, transferring any ownership to this lock.
        [[nodiscard]] OLO_FINLINE TDynamicUniqueLock(TDynamicUniqueLock&& Other)
            : m_Mutex(Other.m_Mutex), m_Locked(Other.m_Locked)
        {
            Other.m_Mutex = nullptr;
            Other.m_Locked = false;
        }

        // Move from another lock, transferring any ownership to this lock, and unlocking the previous mutex if locked.
        OLO_FINLINE TDynamicUniqueLock& operator=(TDynamicUniqueLock&& Other)
        {
            if (m_Locked)
            {
                m_Mutex->Unlock();
            }
            m_Mutex = Other.m_Mutex;
            m_Locked = Other.m_Locked;
            Other.m_Mutex = nullptr;
            Other.m_Locked = false;
            return *this;
        }

        // Unlock the mutex if locked.
        OLO_FINLINE ~TDynamicUniqueLock()
        {
            if (m_Locked)
            {
                m_Mutex->Unlock();
            }
        }

        // Lock the associated mutex. This lock must have a mutex and must not be locked.
        void Lock()
        {
            OLO_CORE_ASSERT(!m_Locked, "Lock is already locked");
            OLO_CORE_ASSERT(m_Mutex != nullptr, "Lock has no associated mutex");
            m_Mutex->Lock();
            m_Locked = true;
        }

        // Unlock the associated mutex. This lock must have a mutex and must be locked.
        void Unlock()
        {
            OLO_CORE_ASSERT(m_Locked, "Lock is not locked");
            m_Locked = false;
            m_Mutex->Unlock();
        }

        // Returns true if this lock has its associated mutex locked.
        OLO_FINLINE bool OwnsLock() const
        {
            return m_Locked;
        }

        // Returns true if this lock has its associated mutex locked.
        OLO_FINLINE explicit operator bool() const
        {
            return OwnsLock();
        }

      private:
        LockType* m_Mutex = nullptr;
        bool m_Locked = false;
    };

    // Type alias for FMutex
    using FUniqueLock = TUniqueLock<class FMutex>;
} // namespace OloEngine
