// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

/**
 * @file RunnableThread.h
 * @brief Base class for runnable threads with TLS support
 *
 * Ported from Unreal Engine's HAL/RunnableThread.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/PlatformTLS.h"
#include "OloEngine/HAL/Runnable.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/HAL/PlatformMisc.h"
#include "OloEngine/HAL/ManualResetEvent.h"

#include <atomic>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace OloEngine
{
    /**
     * @enum EThreadCreateFlags
     * @brief Flags for thread creation
     */
    enum class EThreadCreateFlags : i8
    {
        None = 0,
        SMTExclusive = (1 << 0), // Request exclusive access to SMT core
    };

    OLO_FINLINE EThreadCreateFlags operator|(EThreadCreateFlags A, EThreadCreateFlags B)
    {
        return static_cast<EThreadCreateFlags>(static_cast<i8>(A) | static_cast<i8>(B));
    }

    OLO_FINLINE EThreadCreateFlags operator&(EThreadCreateFlags A, EThreadCreateFlags B)
    {
        return static_cast<EThreadCreateFlags>(static_cast<i8>(A) & static_cast<i8>(B));
    }

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
         * @brief Thread entry point (internal)
         */
        void ThreadEntryPoint();

#ifdef _WIN32
        /**
         * @brief Windows-specific thread entry point (called from CreateThread)
         *
         * Unlike ThreadEntryPoint(), this doesn't need to duplicate handle
         * since we already have m_NativeHandle from CreateThread.
         */
        void ThreadEntryPointWindows();
#elif defined(__linux__) || defined(__APPLE__)
        /**
         * @brief POSIX-specific thread entry point (called from pthread_create)
         *
         * Sets up TLS, runs the runnable, and cleans up on exit.
         */
        void ThreadEntryPointPosix();
#endif

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

        // Native thread handle for platform-specific operations
#ifdef _WIN32
        HANDLE m_NativeHandle = nullptr;
#elif defined(__linux__) || defined(__APPLE__)
        pthread_t m_PosixThread{};
        bool m_bHasPosixThread = false;
#endif

        // Synchronization for thread startup - uses our FManualResetEvent
        FManualResetEvent m_InitEvent;

      private:
        // TLS storage for current thread pointer
        static thread_local FRunnableThread* s_CurrentThread;
    };

    //=============================================================================
    // IMPLEMENTATION
    //=============================================================================

    // Static TLS storage
    inline thread_local FRunnableThread* FRunnableThread::s_CurrentThread = nullptr;

    inline FRunnableThread::FRunnableThread() = default;

    inline FRunnableThread::~FRunnableThread()
    {
        // Ensure thread is stopped before destruction
        Kill(true);

#ifdef _WIN32
        // Close the duplicated native handle
        if (m_NativeHandle != nullptr)
        {
            ::CloseHandle(m_NativeHandle);
            m_NativeHandle = nullptr;
        }
#endif
    }

    inline FRunnableThread* FRunnableThread::Create(
        FRunnable* InRunnable,
        const char* ThreadName,
        u32 InStackSize,
        EThreadPriority InThreadPri,
        u64 InThreadAffinityMask,
        EThreadCreateFlags InCreateFlags)
    {
        FRunnableThread* NewThread = new FRunnableThread();
        if (NewThread->CreateInternal(InRunnable, ThreadName, InStackSize, InThreadPri, InThreadAffinityMask, InCreateFlags))
        {
            return NewThread;
        }
        delete NewThread;
        return nullptr;
    }

    inline FRunnableThread* FRunnableThread::GetRunnableThread()
    {
        return s_CurrentThread;
    }

    inline bool FRunnableThread::CreateInternal(
        FRunnable* InRunnable,
        const char* InThreadName,
        u32 InStackSize,
        EThreadPriority InThreadPri,
        u64 InThreadAffinityMask,
        EThreadCreateFlags InCreateFlags)
    {
        (void)InCreateFlags; // SMT exclusive not implemented

        m_Runnable = InRunnable;
        m_ThreadName = InThreadName ? InThreadName : "UnnamedThread";
        m_ThreadPriority = InThreadPri;
        m_ThreadAffinityMask = InThreadAffinityMask;

        // Reset the init event
        m_InitEvent.Reset();

#ifdef _WIN32
        // Use CreateThread on Windows to support stack size configuration
        // Stack size of 0 means use default (1MB on Windows)
        m_NativeHandle = ::CreateThread(
            nullptr,     // Default security attributes
            InStackSize, // Stack size (0 = default)
            [](LPVOID Param) -> DWORD
            {
                FRunnableThread* Thread = static_cast<FRunnableThread*>(Param);
                Thread->ThreadEntryPointWindows();
                return 0;
            },
            this,   // Thread parameter
            0,      // Creation flags (start immediately)
            nullptr // Thread ID (we get it inside the thread)
        );

        if (m_NativeHandle == nullptr)
        {
            return false;
        }

        // Wait for thread to initialize
        m_InitEvent.Wait();

        return true;
#elif defined(__linux__) || defined(__APPLE__)
        // Use pthread on POSIX platforms
        pthread_attr_t Attr;
        pthread_attr_init(&Attr);

        // Set stack size if specified
        if (InStackSize > 0)
        {
            pthread_attr_setstacksize(&Attr, InStackSize);
        }

        // Create the thread
        int Result = pthread_create(&m_PosixThread, &Attr, [](void* Param) -> void*
                                    {
                FRunnableThread* Thread = static_cast<FRunnableThread*>(Param);
                Thread->ThreadEntryPointPosix();
                return nullptr; }, this);

        pthread_attr_destroy(&Attr);

        if (Result != 0)
        {
            return false;
        }

        m_bHasPosixThread = true;

        // Wait for thread to initialize
        m_InitEvent.Wait();

        return true;
#else
        // Fallback - not supported
        (void)InStackSize;
        return false;
#endif
    }

    inline void FRunnableThread::ThreadEntryPoint()
    {
        // This is the common thread entry logic - extracted for reuse
        // Store thread ID using platform API (not std::hash)
        m_ThreadID = FPlatformTLS::GetCurrentThreadId();

        // Set up TLS
        SetTls();

        // Set thread name
        FPlatformProcess::SetThreadName(m_ThreadName.c_str());

        // Set thread priority
        FPlatformProcess::SetThreadPriority(m_ThreadPriority);

        // Set thread affinity (with default group 0)
        if (m_ThreadAffinityMask != 0)
        {
            FPlatformProcess::SetThreadGroupAffinity(m_ThreadAffinityMask, 0);
        }

        // Signal that we're initialized
        m_InitEvent.Notify();

        m_bIsRunning.store(true, std::memory_order_release);

        // Call runnable's Init
        if (m_Runnable && m_Runnable->Init())
        {
            // Run the main work
            m_Runnable->Run();
        }

        // Call runnable's Exit
        if (m_Runnable)
        {
            m_Runnable->Exit();
        }

        m_bIsRunning.store(false, std::memory_order_release);

        // Clear TLS
        FreeTls();
    }

#ifdef _WIN32
    inline void FRunnableThread::ThreadEntryPointWindows()
    {
        // Store thread ID using platform API
        m_ThreadID = FPlatformTLS::GetCurrentThreadId();

        // m_NativeHandle is already set by CreateThread

        // Common thread initialization
        ThreadEntryPoint();
    }
#endif

#if defined(__linux__) || defined(__APPLE__)
    inline void FRunnableThread::ThreadEntryPointPosix()
    {
        // Store thread ID using platform API
        m_ThreadID = FPlatformTLS::GetCurrentThreadId();

        // Common thread initialization
        ThreadEntryPoint();
    }
#endif

    inline void FRunnableThread::SetTls()
    {
        s_CurrentThread = this;
    }

    inline void FRunnableThread::FreeTls()
    {
        s_CurrentThread = nullptr;
    }

    inline void FRunnableThread::SetThreadPriority(EThreadPriority NewPriority)
    {
        m_ThreadPriority = NewPriority;

        // If called from within the thread, apply immediately
        if (s_CurrentThread == this)
        {
            FPlatformProcess::SetThreadPriority(NewPriority);
        }
        // Otherwise, priority will be applied next time the thread checks
    }

    inline bool FRunnableThread::SetThreadAffinity(const FThreadAffinity& Affinity)
    {
        m_ThreadAffinityMask = Affinity.ThreadAffinityMask;

        // If called from within the thread, apply immediately
        if (s_CurrentThread == this)
        {
            FPlatformProcess::SetThreadGroupAffinity(Affinity.ThreadAffinityMask, Affinity.ProcessorGroup);
            return true;
        }
        return false;
    }

    inline void FRunnableThread::Suspend(bool bShouldPause)
    {
#ifdef _WIN32
        // Use Windows SuspendThread/ResumeThread APIs
        if (m_NativeHandle != nullptr)
        {
            if (bShouldPause)
            {
                ::SuspendThread(m_NativeHandle);
            }
            else
            {
                ::ResumeThread(m_NativeHandle);
            }
        }
#else
        // Note: POSIX doesn't have a standard suspend/resume mechanism
        // pthread_kill with SIGSTOP/SIGCONT could work but is not portable
        (void)bShouldPause;
#endif
    }

    inline bool FRunnableThread::Kill(bool bShouldWait)
    {
        m_bShouldStop.store(true, std::memory_order_release);

        if (m_Runnable)
        {
            m_Runnable->Stop();
        }

        if (bShouldWait)
        {
            WaitForCompletion();
        }

        return true;
    }

    inline void FRunnableThread::WaitForCompletion()
    {
#ifdef _WIN32
        // On Windows, use native handle
        if (m_NativeHandle != nullptr)
        {
            ::WaitForSingleObject(m_NativeHandle, INFINITE);
        }
#elif defined(__linux__) || defined(__APPLE__)
        // On POSIX, use pthread_join
        if (m_bHasPosixThread)
        {
            pthread_join(m_PosixThread, nullptr);
            m_bHasPosixThread = false;
        }
#endif
    }

} // namespace OloEngine
