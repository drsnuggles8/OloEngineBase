// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/HAL/RunnableThread.h"

#ifdef OLO_PLATFORM_WINDOWS
#include "Platform/Windows/WindowsHWrapper.h"
#elif defined(OLO_PLATFORM_LINUX)
#include <cerrno>
#include <climits>
#include <pthread.h>
#include <signal.h>
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

        // Wait for thread to initialize, but bound the wait with periodic liveness
        // checks so a thread that crashes (or never reaches Notify) can't hang
        // CreateInternal forever. If the thread is no longer alive or a generous
        // overall deadline elapses, treat it as a creation failure.
        {
            constexpr auto PollMs = FMonotonicTimeSpan::FromMilliseconds(100.0);
            // Overall budget: 5 minutes. Long enough to never regress legitimate
            // slow Init() paths, short enough to bound pathological hangs.
            constexpr auto OverallBudgetMs = FMonotonicTimeSpan::FromMilliseconds(5.0 * 60.0 * 1000.0);
            const FMonotonicTimePoint Deadline = FMonotonicTimePoint::Now() + OverallBudgetMs;

            while (!m_InitEvent.WaitFor(PollMs))
            {
                // Liveness check: if the thread exited without ever signalling the event
                // (crash, unhandled exception, early return), bail out instead of blocking.
                if (DWORD ExitCode = 0; ::GetExitCodeThread(Handle, &ExitCode) && ExitCode != STILL_ACTIVE)
                {
                    OLO_CORE_ERROR("FRunnableThread::Create: worker thread '{}' exited (code={}) before signalling init",
                                   m_ThreadName, static_cast<u32>(ExitCode));
                    WaitForCompletion();
                    return false;
                }
                if (FMonotonicTimePoint::Now() >= Deadline)
                {
                    OLO_CORE_ERROR("FRunnableThread::Create: worker thread '{}' did not signal init within the overall deadline",
                                   m_ThreadName);
                    WaitForCompletion();
                    return false;
                }
            }
        }

        // If FRunnable::Init() reported failure, surface it as a Create() failure.
        // The worker will still run Exit() and terminate cleanly; we just refuse
        // to hand back a "successfully created" thread to the caller.
        if (!m_InitSucceeded.load(std::memory_order_acquire))
        {
            WaitForCompletion();
            return false;
        }

        return true;

#elif defined(OLO_PLATFORM_LINUX)
        // Use pthread on Linux
        pthread_attr_t Attr;
        pthread_attr_init(&Attr);

        // Set stack size if specified. Validate against PTHREAD_STACK_MIN and log
        // the rc+InStackSize on failure so callers can diagnose bad sizes instead
        // of silently ending up with an implementation-default stack.
        if (InStackSize > 0)
        {
            sizet StackSize = static_cast<sizet>(InStackSize);
            if (StackSize < static_cast<sizet>(PTHREAD_STACK_MIN))
            {
                OLO_CORE_WARN("FRunnableThread::Create: requested stack size {} < PTHREAD_STACK_MIN ({}); clamping",
                              StackSize, static_cast<sizet>(PTHREAD_STACK_MIN));
                StackSize = static_cast<sizet>(PTHREAD_STACK_MIN);
            }
            const int rc = pthread_attr_setstacksize(&Attr, StackSize);
            if (rc != 0)
            {
                OLO_CORE_ERROR("FRunnableThread::Create: pthread_attr_setstacksize({}) failed rc={}; falling back to default",
                               StackSize, rc);
                // Don't fail the whole create; pthread_create below will use the
                // attribute's current (default) stack size.
            }
        }

        // Create the thread
        pthread_t PosixThread{};
        int Result = pthread_create(
            &PosixThread,
            &Attr,
            [](void* Param) -> void*
            {
                FRunnableThread* Thread = static_cast<FRunnableThread*>(Param);
                Thread->m_ThreadID = FPlatformTLS::GetCurrentThreadId();
                Thread->ThreadEntryPoint();
                return nullptr;
            },
            this);

        pthread_attr_destroy(&Attr);

        if (Result != 0)
        {
            return false;
        }

        m_NativeHandle = static_cast<uptr>(PosixThread);
        m_HasNativeHandle = true;

        // Bound the init wait with a periodic liveness check (see Windows branch).
        {
            constexpr auto PollMs = FMonotonicTimeSpan::FromMilliseconds(100.0);
            constexpr auto OverallBudgetMs = FMonotonicTimeSpan::FromMilliseconds(5.0 * 60.0 * 1000.0);
            const FMonotonicTimePoint Deadline = FMonotonicTimePoint::Now() + OverallBudgetMs;

            while (!m_InitEvent.WaitFor(PollMs))
            {
                // pthread_kill with signal 0 is the portable liveness probe: returns
                // ESRCH if the thread has exited, 0 if still alive.
                const int AliveRc = pthread_kill(PosixThread, 0);
                if (AliveRc == ESRCH)
                {
                    OLO_CORE_ERROR("FRunnableThread::Create: worker thread '{}' exited before signalling init",
                                   m_ThreadName);
                    WaitForCompletion();
                    return false;
                }
                if (FMonotonicTimePoint::Now() >= Deadline)
                {
                    OLO_CORE_ERROR("FRunnableThread::Create: worker thread '{}' did not signal init within the overall deadline",
                                   m_ThreadName);
                    WaitForCompletion();
                    return false;
                }
            }
        }

        if (!m_InitSucceeded.load(std::memory_order_acquire))
        {
            WaitForCompletion();
            return false;
        }

        return true;

#else
        // Fallback - not supported
        (void)InStackSize;
        return false;
#endif
    }

    void FRunnableThread::ThreadEntryPoint()
    {
        // This is the common thread entry logic - extracted for reuse.
        // m_ThreadID is already set by the platform-specific thread lambda
        // (before this function is invoked) so no duplicate write here.

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

        // Run the runnable's Init() BEFORE signalling m_InitEvent so that
        // CreateInternal observes the real success/failure result and can fail
        // thread creation if Init returned false.
        const bool initOk = (m_Runnable == nullptr) || m_Runnable->Init();
        m_InitSucceeded.store(initOk, std::memory_order_release);

        // Publish the running flag BEFORE Notify() so any thread woken from the
        // init event observes m_IsRunning == true (only if init succeeded).
        if (initOk)
        {
            m_IsRunning.store(true, std::memory_order_release);
        }
        m_InitEvent.Notify();

        if (initOk && m_Runnable)
        {
            // Run the main work
            m_Runnable->Run();
        }

        // Call runnable's Exit (always, even if Init failed, to give the runnable
        // a chance to release anything allocated before Init's failure point).
        if (m_Runnable)
        {
            m_Runnable->Exit();
        }

        m_IsRunning.store(false, std::memory_order_release);

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

        // If called from within the target thread, apply immediately via the OS.
        if (s_CurrentThread == this)
        {
            FPlatformProcess::SetThreadPriority(NewPriority);
        }
        // If called from another thread, only m_ThreadPriority is updated — there is
        // NO background polling that would automatically re-apply it to the target
        // thread. The target thread must call SetThreadPriority on itself (e.g. from
        // its runnable's Run loop) if the new value should take effect while the
        // thread is already executing. Alternatively callers can implement explicit
        // cross-thread signaling if automatic re-apply is required.
    }

    /**
     * Update this thread's cached affinity mask, and if invoked from the target
     * thread itself also apply it via FPlatformProcess::SetThreadGroupAffinity.
     *
     * Returns true if the mask was successfully cached on the FRunnableThread
     * object. When called from the target thread the OS-level affinity is also
     * applied immediately. When called from a different thread the mask is only
     * cached — the target thread does not automatically re-read
     * m_ThreadAffinityMask, so callers must either invoke this from the target
     * thread or arrange their own re-apply signalling. In either case the value
     * is successfully stored, so we return true.
     */
    bool FRunnableThread::SetThreadAffinity(const FThreadAffinity& Affinity)
    {
        m_ThreadAffinityMask = Affinity.ThreadAffinityMask;

        // Actual OS-level application only happens when we are the target thread.
        if (s_CurrentThread == this)
        {
            FPlatformProcess::SetThreadGroupAffinity(Affinity.ThreadAffinityMask, Affinity.ProcessorGroup);
        }
        return true;
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
        m_ShouldStop.store(true, std::memory_order_release);

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
        // On Windows, use native handle. Close after waiting so the destructor
        // doesn't attempt to close a stale handle (mirrors Linux pthread_join).
        if (m_HasNativeHandle && m_NativeHandle != 0)
        {
            HANDLE Handle = reinterpret_cast<HANDLE>(m_NativeHandle);
            ::WaitForSingleObject(Handle, INFINITE);
            ::CloseHandle(Handle);
            m_NativeHandle = 0;
            m_HasNativeHandle = false;
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
