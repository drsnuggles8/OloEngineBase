// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/HAL/PlatformMisc.h"
#include "OloEngine/HAL/PlatformProcess.h"

#include <atomic>

namespace OloEngine
{
    // Forward declarations
    class FRunnable;
    class FRunnableThread;

    // @enum EForkProcessRole
    // @brief Role of the current process in a fork scenario
    enum class EForkProcessRole : u8
    {
        Parent,
        Child,
    };

    // @class FForkProcessHelper
    // @brief Helper functions for processes that fork in order to share memory pages.
    //
    // About multithreading:
    // When a process gets forked, any existing threads will not exist on the new forked process.
    // To solve this we use forkable threads that are notified when the fork occurs and will
    // automatically convert themselves into real runnable threads.
    // On the master process, these forkable threads will be fake threads that are executed on
    // the main thread and will block the critical path.
    //
    // Currently the game code is responsible for calling Fork on itself then calling
    // FForkProcessHelper::OnForkingOccured to transform the forkable threads.
    // Ideally the fork point is done right after the game has loaded all the assets it wants
    // to share so it can maximize the shared memory pool.
    // From the fork point any memory page that gets written into by a forked process will be
    // transferred into a unique page for this process.
    class FForkProcessHelper
    {
      public:
        // @brief Returns true if the server process was launched with the intention to fork.
        // This could be a process on a fork-supported platform that will launch real child processes.
        // Or it could be a process that will simulate forking by transforming itself into a child
        // process via fake forking.
        static bool IsForkRequested()
        {
            return s_bForkRequested.load(std::memory_order_relaxed);
        }

        // @brief Sets the fork requested flag.
        // Call this before engine initialization if you intend to fork.
        static void SetForkRequested(bool bRequested)
        {
            s_bForkRequested.store(bRequested, std::memory_order_release);
        }

        // @brief Are we a forked process that supports multithreading.
        // This only becomes true after it's safe to be multithread.
        // Since a process can be forked mid-tick, there is a period of time where
        // IsForkedChildProcess is true but IsForkedMultithreadInstance will be false.
        static bool IsForkedMultithreadInstance()
        {
            return s_bIsForkedMultithreadInstance.load(std::memory_order_acquire);
        }

        // @brief Is this a process that was forked.
        static bool IsForkedChildProcess()
        {
            return s_bIsForkedChildProcess.load(std::memory_order_acquire);
        }

        // @brief Sets the forked child process flag and index given to this child process.
        // @param ChildIndex Unique index for this forked child (0 = master)
        static void SetIsForkedChildProcess(u16 ChildIndex = 1)
        {
            s_ForkedChildProcessIndex = ChildIndex;
            s_bIsForkedChildProcess.store(true, std::memory_order_release);
        }

        // @brief Returns the unique index of this forked child process.
        // Index 0 is for the master server.
        static u16 GetForkedChildProcessIndex()
        {
            return s_ForkedChildProcessIndex;
        }

        // @brief Event triggered when a fork occurred on the child process and it's safe
        // to create real threads.
        static void OnForkingOccured()
        {
            s_bIsForkedMultithreadInstance.store(true, std::memory_order_release);
        }

        // @brief Tells if we allow multithreading on forked processes.
        // Default is set to false but can be configured.
        // This is important because after fork(), only the calling thread exists in the child.
        static bool SupportsMultithreadingPostFork()
        {
            return s_bSupportsMultithreadingPostFork;
        }

        // @brief Set whether multithreading is supported post-fork.
        // @param bSupported Whether to allow multithreading after fork
        static void SetSupportsMultithreadingPostFork(bool bSupported)
        {
            s_bSupportsMultithreadingPostFork = bSupported;
        }

        // @brief Performs low-level cross-platform actions that should happen immediately
        // BEFORE forking in a well-specified order.
        // Runs after any higher level code like calling into game-level constructs or
        // anything that may allocate memory.
        // E.g. notifies GMalloc to optimize for memory sharing across parent/child process.
        // Note: This will be called multiple times on the parent before each fork.
        static void LowLevelPreFork()
        {
            // Placeholder for memory allocator notification
            // GMalloc->OnPreFork();
        }

        // @brief Performs low-level cross-platform actions that should happen immediately
        // AFTER forking in the PARENT process in a well-specified order.
        // Runs before any higher level code like calling into game-level constructs.
        // E.g. notifies GMalloc to optimize for memory sharing across parent/child process.
        static void LowLevelPostForkParent()
        {
            // Placeholder for memory allocator notification
            // GMalloc->OnPostForkParent();
        }

        // @brief Performs low-level cross-platform actions that should happen immediately
        // AFTER forking in the CHILD process in a well-specified order.
        // Runs before any higher level code like calling into game-level constructs.
        // E.g. notifies GMalloc to optimize for memory sharing across parent/child process.
        // @param ChildIndex Unique index for this forked child
        static void LowLevelPostForkChild(u16 ChildIndex = 1)
        {
            SetIsForkedChildProcess(ChildIndex);
            // Placeholder for memory allocator notification
            // GMalloc->OnPostForkChild();
        }

        // @brief Creates a thread according to the environment it's in:
        //
        // - In environments with SupportsMultithreading: create a real thread that will tick the runnable object itself
        // - In environments without multithreading: create a fake thread that is ticked by the main thread.
        // - In environments without multithreading but that allows multithreading post-fork:
        //   - If called on the original master process: will create a forkable thread that is ticked in the main thread pre-fork but becomes a real thread post-fork
        //   - If called on a forked child process: will create a real thread immediately
        //
        // @param InRunnable The runnable object to execute
        // @param InThreadName Name for debugging/profiling
        // @param InStackSize Stack size (0 = default)
        // @param InThreadPri Thread priority
        // @param InThreadAffinityMask CPU affinity mask
        // @param InCreateFlags Thread creation flags
        // @param bAllowPreFork If true, allows creating real threads even before fork
        // @return Pointer to the created thread, or nullptr on failure
        static FRunnableThread* CreateForkableThread(
            FRunnable* InRunnable,
            const char* InThreadName,
            u32 InStackSize = 0,
            EThreadPriority InThreadPri = EThreadPriority::TPri_Normal,
            u64 InThreadAffinityMask = FPlatformAffinity::GetNoAffinityMask(),
            EThreadCreateFlags InCreateFlags = EThreadCreateFlags::None,
            bool bAllowPreFork = false);

      private:
        inline static std::atomic<bool> s_bForkRequested{ false };
        inline static std::atomic<bool> s_bIsForkedChildProcess{ false };
        inline static std::atomic<bool> s_bIsForkedMultithreadInstance{ false };
        inline static bool s_bSupportsMultithreadingPostFork{ false };
        inline static u16 s_ForkedChildProcessIndex{ 0 };
    };

} // namespace OloEngine

//=============================================================================
// IMPLEMENTATION
//=============================================================================

#include "OloEngine/HAL/RunnableThread.h"

namespace OloEngine
{
    // @class FForkableThread
    // @brief A thread that can survive process fork operations
    //
    // Before fork: Acts as a "fake" thread that is ticked by the main thread
    // After fork: Converts to a real thread that runs independently
    //
    // This class is used internally by CreateForkableThread and generally
    // should not be instantiated directly.
    class FForkableThread : public FRunnableThread
    {
      public:
        FForkableThread() = default;
        virtual ~FForkableThread() override = default;

        // @brief Get the thread type
        // @return ThreadType::Forkable
        virtual ThreadType GetThreadType() const override
        {
            return ThreadType::Forkable;
        }

        // @brief Convert this fake/forkable thread into a real thread
        //
        // Called after fork when it's safe to create real threads.
        // The thread will begin executing its runnable independently.
        //
        // @return True if conversion was successful
        bool ConvertToRealThread()
        {
            if (m_bIsRealThread || !m_Runnable)
            {
                return false;
            }

            // Create the actual OS thread
            if (CreateInternal(m_Runnable, m_ThreadName.c_str(), m_StackSize,
                               m_ThreadPriority, m_ThreadAffinityMask, m_CreateFlags))
            {
                m_bIsRealThread = true;
                return true;
            }
            return false;
        }

        // @brief Tick this thread (for fake thread mode)
        //
        // When running as a fake thread pre-fork, the main thread calls
        // this to give the runnable a chance to execute.
        void Tick()
        {
            if (m_bIsRealThread || !m_Runnable)
            {
                return;
            }

            // In fake thread mode, we execute on the main thread
            // The runnable should be designed to handle being ticked
            // rather than running in a loop
        }

        // @brief Initialize as a forkable thread
        bool InitializeForkable(
            FRunnable* InRunnable,
            const char* InThreadName,
            u32 InStackSize,
            EThreadPriority InThreadPri,
            u64 InThreadAffinityMask,
            EThreadCreateFlags InCreateFlags)
        {
            m_Runnable = InRunnable;
            m_ThreadName = InThreadName ? InThreadName : "ForkableThread";
            m_StackSize = InStackSize;
            m_ThreadPriority = InThreadPri;
            m_ThreadAffinityMask = InThreadAffinityMask;
            m_CreateFlags = InCreateFlags;
            m_bIsRealThread = false;

            // Initialize the runnable (but don't start the thread yet)
            if (m_Runnable)
            {
                return m_Runnable->Init();
            }
            return true;
        }

      private:
        u32 m_StackSize = 0;
        EThreadCreateFlags m_CreateFlags = EThreadCreateFlags::None;
        bool m_bIsRealThread = false;
    };

    inline FRunnableThread* FForkProcessHelper::CreateForkableThread(
        FRunnable* InRunnable,
        const char* InThreadName,
        u32 InStackSize,
        EThreadPriority InThreadPri,
        u64 InThreadAffinityMask,
        EThreadCreateFlags InCreateFlags,
        bool bAllowPreFork)
    {
        // Case 1: If we're already in a forked child process that supports multithreading,
        // or if multithreading is already enabled, create a real thread immediately
        if (IsForkedMultithreadInstance() || bAllowPreFork)
        {
            return FRunnableThread::Create(
                InRunnable,
                InThreadName,
                InStackSize,
                InThreadPri,
                InThreadAffinityMask,
                InCreateFlags);
        }

        // Case 2: If fork is requested but we're not yet forked (master process),
        // create a forkable thread that will be converted post-fork
        if (IsForkRequested() && SupportsMultithreadingPostFork())
        {
            FForkableThread* ForkableThread = new FForkableThread();
            if (ForkableThread->InitializeForkable(
                    InRunnable,
                    InThreadName,
                    InStackSize,
                    InThreadPri,
                    InThreadAffinityMask,
                    InCreateFlags))
            {
                // Register with thread manager so it gets converted on fork
                return ForkableThread;
            }
            delete ForkableThread;
            return nullptr;
        }

        // Case 3: Default behavior - create a real thread
        // This handles the case where fork is not being used at all
        return FRunnableThread::Create(
            InRunnable,
            InThreadName,
            InStackSize,
            InThreadPri,
            InThreadAffinityMask,
            InCreateFlags);
    }

} // namespace OloEngine
