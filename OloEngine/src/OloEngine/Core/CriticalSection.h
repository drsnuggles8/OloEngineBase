#pragma once

/**
 * @file CriticalSection.h
 * @brief Platform-specific recursive mutex with spin-wait optimization
 *
 * Provides FCriticalSection matching UE5.7's implementation:
 * - Recursive (same thread can lock multiple times)
 * - Spin-wait before blocking (reduces context switch overhead)
 * - Uses CRITICAL_SECTION on Windows, pthread_mutex on POSIX
 *
 * Ported from Unreal Engine's HAL/CriticalSection.h and Windows/WindowsCriticalSection.h
 */

#include "OloEngine/Core/Base.h"

#if defined(OLO_PLATFORM_WINDOWS)
#include <Windows.h>
#else
#include <pthread.h>
#endif

namespace OloEngine
{
#if defined(OLO_PLATFORM_WINDOWS)

    /**
     * @class FWindowsCriticalSection
     * @brief Windows CRITICAL_SECTION wrapper with spin-wait optimization
     *
     * Uses InitializeCriticalSectionAndSpinCount with a spin count of 4000,
     * matching UE5.7's default. The spin count reduces context switches for
     * short critical sections by spinning in user mode before blocking.
     */
    class FWindowsCriticalSection
    {
      public:
        /** Spin count before yielding to OS scheduler. UE5.7 uses 4000. */
        static constexpr DWORD SpinCount = 4000;

        FWindowsCriticalSection()
        {
            InitializeCriticalSectionAndSpinCount(&CriticalSection, SpinCount);
        }

        ~FWindowsCriticalSection()
        {
            DeleteCriticalSection(&CriticalSection);
        }

        // Non-copyable, non-movable
        FWindowsCriticalSection(const FWindowsCriticalSection&) = delete;
        FWindowsCriticalSection& operator=(const FWindowsCriticalSection&) = delete;
        FWindowsCriticalSection(FWindowsCriticalSection&&) = delete;
        FWindowsCriticalSection& operator=(FWindowsCriticalSection&&) = delete;

        /**
         * @brief Acquire the lock (blocks if held by another thread)
         * @note Recursive - same thread can call Lock() multiple times
         */
        void Lock()
        {
            EnterCriticalSection(&CriticalSection);
        }

        /**
         * @brief Try to acquire the lock without blocking
         * @return true if lock acquired, false if held by another thread
         */
        bool TryLock()
        {
            return TryEnterCriticalSection(&CriticalSection) != 0;
        }

        /**
         * @brief Release the lock
         * @note Must be called once for each successful Lock() call
         */
        void Unlock()
        {
            LeaveCriticalSection(&CriticalSection);
        }

      private:
        CRITICAL_SECTION CriticalSection;
    };

    /** Default FCriticalSection type for Windows */
    using FCriticalSection = FWindowsCriticalSection;

#else

    /**
     * @class FPThreadsCriticalSection
     * @brief POSIX pthread_mutex_t wrapper with recursive locking
     */
    class FPThreadsCriticalSection
    {
      public:
        FPThreadsCriticalSection()
        {
            pthread_mutexattr_t MutexAttributes;
            pthread_mutexattr_init(&MutexAttributes);
            pthread_mutexattr_settype(&MutexAttributes, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&Mutex, &MutexAttributes);
            pthread_mutexattr_destroy(&MutexAttributes);
        }

        ~FPThreadsCriticalSection()
        {
            pthread_mutex_destroy(&Mutex);
        }

        // Non-copyable, non-movable
        FPThreadsCriticalSection(const FPThreadsCriticalSection&) = delete;
        FPThreadsCriticalSection& operator=(const FPThreadsCriticalSection&) = delete;
        FPThreadsCriticalSection(FPThreadsCriticalSection&&) = delete;
        FPThreadsCriticalSection& operator=(FPThreadsCriticalSection&&) = delete;

        void Lock()
        {
            pthread_mutex_lock(&Mutex);
        }

        bool TryLock()
        {
            return pthread_mutex_trylock(&Mutex) == 0;
        }

        void Unlock()
        {
            pthread_mutex_unlock(&Mutex);
        }

      private:
        pthread_mutex_t Mutex;
    };

    /** Default FCriticalSection type for POSIX */
    using FCriticalSection = FPThreadsCriticalSection;

#endif

    /**
     * @class FScopeLock
     * @brief RAII lock guard for FCriticalSection
     *
     * Locks on construction, unlocks on destruction.
     * Provides exception-safe locking.
     *
     * Usage:
     * @code
     *     FCriticalSection Mutex;
     *     {
     *         FScopeLock Lock(&Mutex);
     *         // Critical section code here
     *     } // Automatically unlocked
     * @endcode
     */
    class FScopeLock
    {
      public:
        /**
         * @brief Construct and immediately lock the critical section
         * @param InSyncObject Pointer to the critical section to lock
         */
        explicit FScopeLock(FCriticalSection* InSyncObject)
            : SyncObject(InSyncObject)
        {
            OLO_CORE_ASSERT(SyncObject != nullptr, "FScopeLock requires a valid FCriticalSection");
            SyncObject->Lock();
        }

        ~FScopeLock()
        {
            SyncObject->Unlock();
        }

        // Non-copyable, non-movable
        FScopeLock(const FScopeLock&) = delete;
        FScopeLock& operator=(const FScopeLock&) = delete;
        FScopeLock(FScopeLock&&) = delete;
        FScopeLock& operator=(FScopeLock&&) = delete;

      private:
        FCriticalSection* SyncObject;
    };

} // namespace OloEngine
