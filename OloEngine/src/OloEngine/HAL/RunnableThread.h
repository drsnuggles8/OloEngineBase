// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

/**
 * @file RunnableThread.h
 * @brief Base class for runnable threads with TLS support
 *
 * Ported from Unreal Engine's HAL/RunnableThread.h.
 *
 * The class is defined here with platform-opaque storage (uptr m_NativeHandle),
 * and all bodies live in HAL/RunnableThread.cpp which includes the appropriate
 * OS headers (Windows.h / pthread.h) privately.
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/PlatformTLS.h"
#include "OloEngine/HAL/Runnable.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/HAL/PlatformMisc.h"
#include "OloEngine/HAL/ManualResetEvent.h"

#include <atomic>
#include <string>

namespace OloEngine
{
    // EThreadCreateFlags is defined in PlatformMisc.h

    /**
     * @class FRunnableThread
     * @brief Base class for system threads with TLS-based access
     *
     * This class provides thread management with a key feature: the ability
     * to get the current thread's FRunnableThread* from any code via TLS.
     * This enables dynamic thread priority changes, querying thread info, etc.
     */
    class FRunnableThread
    {
      public:
        /**
         * @enum ThreadType
         * @brief Type of thread
         */
        enum class ThreadType
        {
            Real,    // Normal OS thread
            Fake,    // Pseudo-thread for single-threaded mode
            Forkable // Thread that can survive process fork
        };

        /**
         * @brief Factory method to create a new thread
         *
         * @param InRunnable The runnable object to execute
         * @param ThreadName Name for the thread (debugging/profiling)
         * @param InStackSize Stack size (0 = default)
         * @param InThreadPri Thread priority
         * @param InThreadAffinityMask CPU affinity mask
         * @param InCreateFlags Creation flags
         * @return Pointer to the created thread, or nullptr on failure
         */
        static FRunnableThread* Create(
            FRunnable* InRunnable,
            const char* ThreadName,
            u32 InStackSize = 0,
            EThreadPriority InThreadPri = EThreadPriority::TPri_Normal,
            u64 InThreadAffinityMask = 0,
            EThreadCreateFlags InCreateFlags = EThreadCreateFlags::None);

        /**
         * @brief Gets the current thread's FRunnableThread
         *
         * This uses TLS to retrieve the FRunnableThread* for the currently
         * executing thread. Returns nullptr if the current thread is not
         * an FRunnableThread (e.g., the main thread before initialization).
         */
        static FRunnableThread* GetRunnableThread();

        /**
         * @brief Destructor
         */
        virtual ~FRunnableThread();

        /**
         * @brief Sets the thread priority
         * @param NewPriority The new priority
         */
        virtual void SetThreadPriority(EThreadPriority NewPriority);

        /**
         * @brief Sets the thread affinity
         * @param Affinity The thread affinity settings
         * @return True if successful
         */
        virtual bool SetThreadAffinity(const FThreadAffinity& Affinity);

        /**
         * @brief Suspends or resumes the thread
         * @param bShouldPause True to suspend, false to resume
         */
        virtual void Suspend(bool bShouldPause = true);

        /**
         * @brief Kills the thread
         * @param bShouldWait True to wait for thread to exit
         * @return True if successful
         */
        virtual bool Kill(bool bShouldWait = true);

        /**
         * @brief Waits for the thread to complete
         */
        virtual void WaitForCompletion();

        /**
         * @brief Gets the thread type
         */
        virtual ThreadType GetThreadType() const
        {
            return ThreadType::Real;
        }

        /**
         * @brief Gets the thread ID
         */
        u32 GetThreadID() const
        {
            return m_ThreadID;
        }

        /**
         * @brief Gets the thread name
         */
        const std::string& GetThreadName() const
        {
            return m_ThreadName;
        }

        /**
         * @brief Gets the current thread priority
         */
        EThreadPriority GetThreadPriority() const
        {
            return m_ThreadPriority;
        }

        /**
         * @brief Check if thread is still running
         */
        bool IsRunning() const
        {
            return m_bIsRunning.load(std::memory_order_acquire);
        }

      protected:
        FRunnableThread();

        /**
         * @brief Internal creation method
         */
        virtual bool CreateInternal(
            FRunnable* InRunnable,
            const char* InThreadName,
            u32 InStackSize,
            EThreadPriority InThreadPri,
            u64 InThreadAffinityMask,
            EThreadCreateFlags InCreateFlags);

        /**
         * @brief Thread entry point (internal, OS-agnostic stage)
         */
        void ThreadEntryPoint();

        /**
         * @brief Store this thread in TLS
         */
        void SetTls();

        /**
         * @brief Clear TLS
         */
        void FreeTls();

      protected:
        std::string m_ThreadName;
        FRunnable* m_Runnable = nullptr;
        u64 m_ThreadAffinityMask = 0;
        EThreadPriority m_ThreadPriority = EThreadPriority::TPri_Normal;
        u32 m_ThreadID = 0;
        std::atomic<bool> m_bIsRunning{ false };
        std::atomic<bool> m_bShouldStop{ false };
        // True iff FRunnable::Init() returned true. Read by CreateInternal after
        // m_InitEvent.Notify() to decide whether thread creation succeeded.
        std::atomic<bool> m_bInitSucceeded{ false };

        // Platform-opaque native thread handle.
        // - On Windows: HANDLE (void*), reinterpret-cast to/from uptr in the .cpp.
        // - On Linux:   pthread_t (unsigned long on glibc), stored directly in uptr.
        uptr m_NativeHandle = 0;
        bool m_HasNativeHandle = false;

        // Synchronization for thread startup - uses our FManualResetEvent
        FManualResetEvent m_InitEvent;

      private:
        // TLS storage for current thread pointer
        static thread_local FRunnableThread* s_CurrentThread;
    };

} // namespace OloEngine
