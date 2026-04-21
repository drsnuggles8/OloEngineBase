// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/HAL/RunnableThread.h"

#ifdef OLO_PLATFORM_WINDOWS
#include "Platform/Windows/WindowsHWrapper.h"
#elif defined(OLO_PLATFORM_LINUX)
#include <pthread.h>
#endif

namespace OloEngine
{
    // Static TLS storage
    thread_local FRunnableThread* FRunnableThread::s_CurrentThread = nullptr;

    FRunnableThread::FRunnableThread() = default;

    FRunnableThread::~FRunnableThread()
    {
        // Ensure thread is stopped before destruction
        Kill(true);

#ifdef OLO_PLATFORM_WINDOWS
        // Close the duplicated native handle
        if (m_HasNativeHandle && m_NativeHandle != 0)
        {
            ::CloseHandle(reinterpret_cast<HANDLE>(m_NativeHandle));
            m_NativeHandle = 0;
            m_HasNativeHandle = false;
        }
#endif
    }

    FRunnableThread* FRunnableThread::Create(
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

    FRunnableThread* FRunnableThread::GetRunnableThread()
    {
        return s_CurrentThread;
    }

    bool FRunnableThread::CreateInternal(
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

#ifdef OLO_PLATFORM_WINDOWS
        // Use CreateThread on Windows to support stack size configuration
        // Stack size of 0 means use default (1MB on Windows)
        HANDLE Handle = ::CreateThread(
            nullptr,     // Default security attributes
            InStackSize, // Stack size (0 = default)
            [](LPVOID Param) -> DWORD
            {
                FRunnableThread* Thread = static_cast<FRunnableThread*>(Param);
                // Store thread ID using platform API
                Thread->m_ThreadID = FPlatformTLS::GetCurrentThreadId();
                // Common thread initialization
                Thread->ThreadEntryPoint();
                return 0;
            },
            this,   // Thread parameter
            0,      // Creation flags (start immediately)
            nullptr // Thread ID (we get it inside the thread)
        );

        if (Handle == nullptr)
        {
            return false;
        }

        m_NativeHandle = reinterpret_cast<uptr>(Handle);
        m_HasNativeHandle = true;

        // Wait for thread to initialize
        m_InitEvent.Wait();

        return true;

#elif defined(OLO_PLATFORM_LINUX)
        // Use pthread on Linux
        pthread_attr_t Attr;
        pthread_attr_init(&Attr);

        // Set stack size if specified
        if (InStackSize > 0)
        {
            pthread_attr_setstacksize(&Attr, InStackSize);
        }

        // Create the thread
        pthread_t PosixThread{};
        int Result = pthread_create(&PosixThread, &Attr, [](void* Param) -> void*
                                    {
                FRunnableThread* Thread = static_cast<FRunnableThread*>(Param);
                Thread->m_ThreadID = FPlatformTLS::GetCurrentThreadId();
                Thread->ThreadEntryPoint();
                return nullptr; }, this);

        pthread_attr_destroy(&Attr);

        if (Result != 0)
        {
            return false;
        }

        m_NativeHandle = static_cast<uptr>(PosixThread);
        m_HasNativeHandle = true;

        // Wait for thread to initialize
        m_InitEvent.Wait();

        return true;

#else
        // Fallback - not supported
        (void)InStackSize;
        return false;
#endif
    }

    void FRunnableThread::ThreadEntryPoint()
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

    void FRunnableThread::SetTls()
    {
        s_CurrentThread = this;
    }

    void FRunnableThread::FreeTls()
    {
        s_CurrentThread = nullptr;
    }

    void FRunnableThread::SetThreadPriority(EThreadPriority NewPriority)
    {
        m_ThreadPriority = NewPriority;

        // If called from within the thread, apply immediately
        if (s_CurrentThread == this)
        {
            FPlatformProcess::SetThreadPriority(NewPriority);
        }
        // Otherwise, priority will be applied next time the thread checks
    }

    bool FRunnableThread::SetThreadAffinity(const FThreadAffinity& Affinity)
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

    void FRunnableThread::Suspend(bool bShouldPause)
    {
#ifdef OLO_PLATFORM_WINDOWS
        // Use Windows SuspendThread/ResumeThread APIs
        if (m_HasNativeHandle && m_NativeHandle != 0)
        {
            HANDLE Handle = reinterpret_cast<HANDLE>(m_NativeHandle);
            if (bShouldPause)
            {
                ::SuspendThread(Handle);
            }
            else
            {
                ::ResumeThread(Handle);
            }
        }
#else
        // Note: POSIX doesn't have a standard suspend/resume mechanism
        // pthread_kill with SIGSTOP/SIGCONT could work but is not portable
        (void)bShouldPause;
#endif
    }

    bool FRunnableThread::Kill(bool bShouldWait)
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

    void FRunnableThread::WaitForCompletion()
    {
#ifdef OLO_PLATFORM_WINDOWS
        // On Windows, use native handle
        if (m_HasNativeHandle && m_NativeHandle != 0)
        {
            ::WaitForSingleObject(reinterpret_cast<HANDLE>(m_NativeHandle), INFINITE);
        }
#elif defined(OLO_PLATFORM_LINUX)
        // On POSIX, use pthread_join
        if (m_HasNativeHandle)
        {
            pthread_join(static_cast<pthread_t>(m_NativeHandle), nullptr);
            m_HasNativeHandle = false;
        }
#endif
    }

} // namespace OloEngine
