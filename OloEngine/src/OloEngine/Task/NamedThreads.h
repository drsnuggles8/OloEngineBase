// @file NamedThreads.h
// @brief Named thread dispatch system (OloEngine custom implementation)
//
// =============================================================================
// ARCHITECTURE NOTES - OloEngine Named Thread System
// =============================================================================
//
// This is OloEngine's custom named thread dispatch system, inspired by but
// intentionally DECOUPLED from UE5.7's TaskGraph approach.
//
// BACKGROUND - HOW UE5.7 HANDLES NAMED THREADS:
// ---------------------------------------------
// In UE5.7, tasks destined for named threads (GameThread, RenderThread, etc.)
// are still routed through the legacy FTaskGraphInterface system. When you
// create a task with an EExtendedTaskPriority like GameThreadNormalPri:
//
//   1. FTaskBase::Schedule() checks IsNamedThreadTask()
//   2. If true, it calls FTaskGraphInterface::Get().QueueTask(TaskGraphTask)
//   3. TaskGraph maintains separate queues for each named thread
//   4. Named threads call FTaskGraphInterface::ProcessThreadUntilIdle()
//
// This creates a tight coupling between the "new" task system (UE::Tasks)
// and the "legacy" TaskGraph (FTaskGraphInterface), which UE maintains for
// backwards compatibility.
//
// OUR APPROACH - STANDALONE NAMED THREAD DISPATCH:
// ------------------------------------------------
// We've implemented a fully standalone named thread system that:
//
//   1. Does NOT depend on TaskGraph or any legacy dispatch mechanism
//   2. Uses a simple FNamedThreadManager singleton with per-thread queues
//   3. Supports the same priority/queue semantics (Main/Local, High/Normal)
//   4. Can be integrated with the main task scheduler if needed, but doesn't
//      require it
//
// KEY COMPONENTS:
// ---------------
// - ENamedThread: Enum identifying special threads (GameThread, RenderThread, RHIThread)
// - FNamedThreadTask: Wrapper around a callable with priority/debug info
// - FNamedThreadQueue: Per-thread queue with Main/Local and High/Normal priority
// - FNamedThreadManager: Singleton managing all named thread queues
//
// QUEUE STRUCTURE (matching UE semantics):
// ----------------------------------------
// Each named thread has 4 logical queues with priority ordering:
//   1. Main High Priority    - High-pri tasks from any thread
//   2. Local High Priority   - High-pri tasks from the owning thread only
//   3. Main Normal Priority  - Normal-pri tasks from any thread
//   4. Local Normal Priority - Normal-pri tasks from the owning thread only
//
// The "Local" queues are for tasks that should only be processed by the
// thread that owns them (e.g., continuation tasks that must run on GT).
//
// USAGE PATTERN:
// --------------
// // At startup (once per named thread):
// FNamedThreadManager::Get().AttachToThread(ENamedThread::GameThread);
//
// // To enqueue work from anywhere:
// EnqueueGameThreadTask([](){ DoSomething(); });
//
// // On the game thread's tick:
// FNamedThreadManager::Get().ProcessTasks(true); // true = include local queue
//
// DIFFERENCES FROM UE5.7:
// -----------------------
// | Aspect                  | UE5.7                      | OloEngine             |
// |-------------------------|----------------------------|-----------------------|
// | Dispatch mechanism      | Via FTaskGraphInterface    | Direct queue enqueue  |
// | Task wrapper            | FTaskGraphTask             | FNamedThreadTask      |
// | Queue storage           | In TaskGraph               | In FNamedThreadManager|
// | Legacy compatibility    | Full TaskGraph support     | None (clean design)   |
// | Complexity              | High (cross-system)        | Low (standalone)      |
//
// @todo FUTURE ENHANCEMENTS:
// - [ ] Review and optimize lock contention (consider lock-free queues)
// - [ ] Add task stealing from named threads when worker pool is idle
// - [ ] Integrate with Tracy for per-queue profiling
// - [ ] Consider adding WaitForNamedThreadTask() for cross-thread sync
// - [ ] Evaluate whether we need the full Main/Local queue distinction
//       or if a simpler High/Normal split suffices
// - [ ] Add metrics/stats collection for queue depths and processing times
// - [ ] Consider thread affinity hints for better cache behavior
//
// @note This system was designed to be simple and correct first. Once we have
//       real-world usage patterns, we can optimize as needed.
//
// @author OloEngine Team
// @date December 2024
// =============================================================================

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/TaskTag.h"
#include "OloEngine/Task/ExtendedTaskPriority.h"
#include "OloEngine/HAL/EventCount.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/Deque.h"
#include "OloEngine/Templates/Function.h"

#include <atomic>
#include <thread>

namespace OloEngine::Tasks
{
    // Forward declarations
    namespace Private
    {
        class FTaskBase;
    }

    // @enum ENamedThread
    // @brief Named thread identifiers for task dispatch
    enum class ENamedThread : i32
    {
        GameThread = 0,
        RenderThread = 1,
        RHIThread = 2,
        AudioThread = 3,

        Count,
        Invalid = -1
    };

    // @brief Convert EExtendedTaskPriority to ENamedThread
    // @return ENamedThread::Invalid if not a named thread priority
    inline ENamedThread GetNamedThread(EExtendedTaskPriority Priority)
    {
        switch (Priority)
        {
            case EExtendedTaskPriority::GameThreadNormalPri:
            case EExtendedTaskPriority::GameThreadHiPri:
            case EExtendedTaskPriority::GameThreadNormalPriLocalQueue:
            case EExtendedTaskPriority::GameThreadHiPriLocalQueue:
                return ENamedThread::GameThread;

            case EExtendedTaskPriority::RenderThreadNormalPri:
            case EExtendedTaskPriority::RenderThreadHiPri:
            case EExtendedTaskPriority::RenderThreadNormalPriLocalQueue:
            case EExtendedTaskPriority::RenderThreadHiPriLocalQueue:
                return ENamedThread::RenderThread;

            case EExtendedTaskPriority::RHIThreadNormalPri:
            case EExtendedTaskPriority::RHIThreadHiPri:
            case EExtendedTaskPriority::RHIThreadNormalPriLocalQueue:
            case EExtendedTaskPriority::RHIThreadHiPriLocalQueue:
                return ENamedThread::RHIThread;

            default:
                return ENamedThread::Invalid;
        }
    }

    // @brief Check if priority is high priority variant
    inline bool IsHighPriority(EExtendedTaskPriority Priority)
    {
        switch (Priority)
        {
            case EExtendedTaskPriority::GameThreadHiPri:
            case EExtendedTaskPriority::GameThreadHiPriLocalQueue:
            case EExtendedTaskPriority::RenderThreadHiPri:
            case EExtendedTaskPriority::RenderThreadHiPriLocalQueue:
            case EExtendedTaskPriority::RHIThreadHiPri:
            case EExtendedTaskPriority::RHIThreadHiPriLocalQueue:
                return true;
            default:
                return false;
        }
    }

    // @brief Check if priority uses local queue
    inline bool IsLocalQueue(EExtendedTaskPriority Priority)
    {
        switch (Priority)
        {
            case EExtendedTaskPriority::GameThreadNormalPriLocalQueue:
            case EExtendedTaskPriority::GameThreadHiPriLocalQueue:
            case EExtendedTaskPriority::RenderThreadNormalPriLocalQueue:
            case EExtendedTaskPriority::RenderThreadHiPriLocalQueue:
            case EExtendedTaskPriority::RHIThreadNormalPriLocalQueue:
            case EExtendedTaskPriority::RHIThreadHiPriLocalQueue:
                return true;
            default:
                return false;
        }
    }

    // @class FNamedThreadTask
    // @brief Wrapper for a task to be executed on a named thread
    class FNamedThreadTask
    {
      public:
        using FTaskFunction = TFunction<void()>;

        FNamedThreadTask() = default;

        FNamedThreadTask(FTaskFunction InTask, EExtendedTaskPriority InPriority, const char* InDebugName = nullptr)
            : m_Task(MoveTemp(InTask)), m_Priority(InPriority), m_DebugName(InDebugName)
        {
        }

        void Execute()
        {
            if (m_Task)
            {
                m_Task();
            }
        }

        EExtendedTaskPriority GetPriority() const
        {
            return m_Priority;
        }
        const char* GetDebugName() const
        {
            return m_DebugName;
        }
        bool IsValid() const
        {
            return static_cast<bool>(m_Task);
        }

      private:
        FTaskFunction m_Task;
        EExtendedTaskPriority m_Priority = EExtendedTaskPriority::None;
        const char* m_DebugName = nullptr;
    };

    // @class FNamedThreadQueue
    // @brief Task queue for a single named thread
    //
    // Supports two queues per thread:
    // - Main queue: Tasks that can be processed from other threads
    // - Local queue: Tasks that should only be processed by the owning thread
    //
    // Each queue has high and normal priority variants.
    class FNamedThreadQueue
    {
      public:
        FNamedThreadQueue() = default;
        ~FNamedThreadQueue() = default;

        // Non-copyable
        FNamedThreadQueue(const FNamedThreadQueue&) = delete;
        FNamedThreadQueue& operator=(const FNamedThreadQueue&) = delete;

        // @brief Enqueue a task
        // @param Task The task to enqueue
        void Enqueue(FNamedThreadTask Task)
        {
            bool bHighPri = IsHighPriority(Task.GetPriority());
            bool bLocal = IsLocalQueue(Task.GetPriority());

            {
                TUniqueLock<FMutex> Lock(m_Mutex);
                if (bLocal)
                {
                    if (bHighPri)
                    {
                        m_LocalHighPriQueue.PushLast(MoveTemp(Task));
                    }
                    else
                    {
                        m_LocalNormalPriQueue.PushLast(MoveTemp(Task));
                    }
                }
                else
                {
                    if (bHighPri)
                    {
                        m_MainHighPriQueue.PushLast(MoveTemp(Task));
                    }
                    else
                    {
                        m_MainNormalPriQueue.PushLast(MoveTemp(Task));
                    }
                }
            }

            m_TaskAvailable.Notify();
        }

        // @brief Try to dequeue and execute a task
        // @param bIncludeLocalQueue Whether to process local queue tasks
        // @return true if a task was executed
        bool TryExecuteOne(bool bIncludeLocalQueue = false)
        {
            FNamedThreadTask Task;
            {
                TUniqueLock<FMutex> Lock(m_Mutex);

                // Priority order: MainHigh > LocalHigh (if included) > MainNormal > LocalNormal (if included)
                if (!m_MainHighPriQueue.IsEmpty())
                {
                    m_MainHighPriQueue.TryPopFirst(Task);
                }
                else if (bIncludeLocalQueue && !m_LocalHighPriQueue.IsEmpty())
                {
                    m_LocalHighPriQueue.TryPopFirst(Task);
                }
                else if (!m_MainNormalPriQueue.IsEmpty())
                {
                    m_MainNormalPriQueue.TryPopFirst(Task);
                }
                else if (bIncludeLocalQueue && !m_LocalNormalPriQueue.IsEmpty())
                {
                    m_LocalNormalPriQueue.TryPopFirst(Task);
                }
            }

            if (Task.IsValid())
            {
                Task.Execute();
                return true;
            }
            return false;
        }

        // @brief Process all available tasks
        // @param bIncludeLocalQueue Whether to process local queue tasks
        // @return Number of tasks processed
        u32 ProcessAll(bool bIncludeLocalQueue = false)
        {
            u32 Count = 0;
            while (TryExecuteOne(bIncludeLocalQueue))
            {
                ++Count;
            }
            return Count;
        }

        // @brief Process tasks until a condition is met or idle
        // @param ShouldStop Predicate that returns true when processing should stop
        // @param bIncludeLocalQueue Whether to process local queue tasks
        template<typename Predicate>
        void ProcessUntil(Predicate&& ShouldStop, bool bIncludeLocalQueue = false)
        {
            while (!ShouldStop())
            {
                if (!TryExecuteOne(bIncludeLocalQueue))
                {
                    // No tasks available, wait for more
                    FEventCountToken Token = m_TaskAvailable.PrepareWait();
                    if (!HasPendingTasks(bIncludeLocalQueue) && !ShouldStop())
                    {
                        m_TaskAvailable.Wait(Token);
                    }
                }
            }
        }

        // @brief Process tasks until idle, then return
        // @param bIncludeLocalQueue Whether to process local queue tasks
        void ProcessUntilIdle(bool bIncludeLocalQueue = false)
        {
            while (TryExecuteOne(bIncludeLocalQueue))
            {
                // Keep processing
            }
        }

        // @brief Check if there are pending tasks
        bool HasPendingTasks(bool bIncludeLocalQueue = false) const
        {
            TUniqueLock<FMutex> Lock(m_Mutex);
            if (!m_MainHighPriQueue.IsEmpty() || !m_MainNormalPriQueue.IsEmpty())
            {
                return true;
            }
            if (bIncludeLocalQueue)
            {
                return !m_LocalHighPriQueue.IsEmpty() || !m_LocalNormalPriQueue.IsEmpty();
            }
            return false;
        }

        // @brief Request the thread to return from ProcessUntil
        void RequestReturn()
        {
            m_bReturnRequested.store(true, std::memory_order_release);
            m_TaskAvailable.Notify();
        }

        // @brief Clear the return request flag
        void ClearReturnRequest()
        {
            m_bReturnRequested.store(false, std::memory_order_release);
        }

        // @brief Check if return has been requested
        bool IsReturnRequested() const
        {
            return m_bReturnRequested.load(std::memory_order_acquire);
        }

      private:
        mutable FMutex m_Mutex;
        TDeque<FNamedThreadTask> m_MainHighPriQueue;
        TDeque<FNamedThreadTask> m_MainNormalPriQueue;
        TDeque<FNamedThreadTask> m_LocalHighPriQueue;
        TDeque<FNamedThreadTask> m_LocalNormalPriQueue;
        FEventCount m_TaskAvailable;
        std::atomic<bool> m_bReturnRequested{ false };
    };

    // @class FNamedThreadManager
    // @brief Singleton manager for named thread task dispatch
    //
    // This is OloEngine's custom implementation that replaces UE's approach of
    // routing named thread tasks through FTaskGraphInterface. See file header
    // for detailed architecture notes.
    //
    // @note Unlike UE5.7 which couples to TaskGraph, we use direct queue dispatch.
    //
    // Usage:
    // 1. On the main thread at startup: AttachToThread(ENamedThread::GameThread)
    // 2. On render thread (if any): AttachToThread(ENamedThread::RenderThread)
    // 3. To queue a task: EnqueueTask(ENamedThread::GameThread, []{ ... })
    // 4. On each named thread's tick: ProcessTasks(thread)
    class FNamedThreadManager
    {
      public:
        static FNamedThreadManager& Get()
        {
            static FNamedThreadManager Instance;
            return Instance;
        }

        // @brief Attach the current thread as a named thread
        // @param Thread The named thread type
        //
        // Call this once per named thread at startup.
        void AttachToThread(ENamedThread Thread)
        {
            OLO_CORE_ASSERT(Thread != ENamedThread::Invalid && Thread < ENamedThread::Count,
                            "Invalid named thread");

            u32 Index = static_cast<u32>(Thread);
            m_ThreadIds[Index].store(OLO::FTaskTagScope::GetCurrentTag() != OLO::ETaskTag::ENone
                                         ? static_cast<u32>(OLO::FTaskTagScope::GetCurrentTag())
                                         : GetCurrentThreadIdInternal(),
                                     std::memory_order_release);

            s_CurrentNamedThread = Thread;
        }

        // @brief Detach the current thread from named thread role
        void DetachFromThread(ENamedThread Thread)
        {
            OLO_CORE_ASSERT(Thread != ENamedThread::Invalid && Thread < ENamedThread::Count,
                            "Invalid named thread");

            u32 Index = static_cast<u32>(Thread);
            m_ThreadIds[Index].store(0, std::memory_order_release);

            if (s_CurrentNamedThread == Thread)
            {
                s_CurrentNamedThread = ENamedThread::Invalid;
            }
        }

        // @brief Get the current thread if it's a named thread
        // @return ENamedThread::Invalid if not a named thread
        ENamedThread GetCurrentThreadIfKnown() const
        {
            return s_CurrentNamedThread;
        }

        // @brief Check if we're currently on a named thread
        bool IsOnNamedThread() const
        {
            return s_CurrentNamedThread != ENamedThread::Invalid;
        }

        // @brief Enqueue a task to a named thread
        void EnqueueTask(ENamedThread Thread, FNamedThreadTask Task)
        {
            OLO_CORE_ASSERT(Thread != ENamedThread::Invalid && Thread < ENamedThread::Count,
                            "Invalid named thread");

            m_Queues[static_cast<u32>(Thread)].Enqueue(MoveTemp(Task));
        }

        // @brief Enqueue a task using extended priority
        void EnqueueTask(EExtendedTaskPriority Priority, FNamedThreadTask::FTaskFunction Task, const char* DebugName = nullptr)
        {
            ENamedThread Thread = GetNamedThread(Priority);
            OLO_CORE_ASSERT(Thread != ENamedThread::Invalid, "Priority is not for a named thread");

            EnqueueTask(Thread, FNamedThreadTask(MoveTemp(Task), Priority, DebugName));
        }

        // @brief Process tasks on the current named thread
        // @param bIncludeLocalQueue Whether to process local queue tasks
        // @return Number of tasks processed
        u32 ProcessTasks(bool bIncludeLocalQueue = true)
        {
            ENamedThread Thread = s_CurrentNamedThread;
            if (Thread == ENamedThread::Invalid)
            {
                return 0;
            }
            return m_Queues[static_cast<u32>(Thread)].ProcessAll(bIncludeLocalQueue);
        }

        // @brief Process tasks on a specific named thread
        // @note Should only be called from that thread
        u32 ProcessTasks(ENamedThread Thread, bool bIncludeLocalQueue = true)
        {
            OLO_CORE_ASSERT(Thread != ENamedThread::Invalid && Thread < ENamedThread::Count,
                            "Invalid named thread");
            return m_Queues[static_cast<u32>(Thread)].ProcessAll(bIncludeLocalQueue);
        }

        // @brief Process tasks until a return is requested
        void ProcessUntilRequestReturn(ENamedThread Thread)
        {
            OLO_CORE_ASSERT(Thread != ENamedThread::Invalid && Thread < ENamedThread::Count,
                            "Invalid named thread");

            FNamedThreadQueue& Queue = m_Queues[static_cast<u32>(Thread)];
            Queue.ClearReturnRequest();
            Queue.ProcessUntil([&Queue]()
                               { return Queue.IsReturnRequested(); }, true);
        }

        // @brief Request a named thread to return from ProcessUntilRequestReturn
        void RequestReturn(ENamedThread Thread)
        {
            OLO_CORE_ASSERT(Thread != ENamedThread::Invalid && Thread < ENamedThread::Count,
                            "Invalid named thread");
            m_Queues[static_cast<u32>(Thread)].RequestReturn();
        }

        // @brief Check if a named thread is currently processing tasks
        //
        // Uses TLS to track when a named thread is actively processing its queue.
        // This is used to prevent re-entrancy in TryWaitOnNamedThread.
        //
        // @param Thread The named thread to check
        // @return true if the thread is currently inside ProcessTasks/ProcessUntilRequestReturn
        bool IsThreadProcessingTasks(ENamedThread Thread) const
        {
            // Use TLS tracking for accurate detection
            return s_CurrentNamedThread == Thread && s_bIsProcessingTasks;
        }

        // @brief RAII scope guard for tracking when we're processing tasks
        //
        // Used internally by ProcessUntilRequestReturn and TryWaitOnNamedThread
        // to prevent re-entrancy.
        class FProcessingScope
        {
          public:
            FProcessingScope()
            {
                m_bWasProcessing = FNamedThreadManager::s_bIsProcessingTasks;
                FNamedThreadManager::s_bIsProcessingTasks = true;
            }
            ~FProcessingScope()
            {
                FNamedThreadManager::s_bIsProcessingTasks = m_bWasProcessing;
            }

            // Non-copyable
            FProcessingScope(const FProcessingScope&) = delete;
            FProcessingScope& operator=(const FProcessingScope&) = delete;

          private:
            bool m_bWasProcessing;
        };

        // @brief Get the queue for a named thread
        FNamedThreadQueue& GetQueue(ENamedThread Thread)
        {
            OLO_CORE_ASSERT(Thread != ENamedThread::Invalid && Thread < ENamedThread::Count,
                            "Invalid named thread");
            return m_Queues[static_cast<u32>(Thread)];
        }

      private:
        FNamedThreadManager() = default;
        ~FNamedThreadManager() = default;

        static u32 GetCurrentThreadIdInternal()
        {
            return static_cast<u32>(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFFFFF);
        }

        FNamedThreadQueue m_Queues[static_cast<u32>(ENamedThread::Count)];
        std::atomic<u32> m_ThreadIds[static_cast<u32>(ENamedThread::Count)] = {};

        static thread_local ENamedThread s_CurrentNamedThread;
        static thread_local bool s_bIsProcessingTasks;
    };

    // ============================================================================
    // Convenience functions for named thread dispatch
    // ============================================================================

    // @brief Enqueue a task to the game thread
    template<typename TaskBody>
    void EnqueueGameThreadTask(TaskBody&& Task, const char* DebugName = "GameThreadTask",
                               bool bHighPriority = false, bool bLocalQueue = false)
    {
        EExtendedTaskPriority Priority;
        if (bLocalQueue)
        {
            Priority = bHighPriority ? EExtendedTaskPriority::GameThreadHiPriLocalQueue
                                     : EExtendedTaskPriority::GameThreadNormalPriLocalQueue;
        }
        else
        {
            Priority = bHighPriority ? EExtendedTaskPriority::GameThreadHiPri
                                     : EExtendedTaskPriority::GameThreadNormalPri;
        }

        FNamedThreadManager::Get().EnqueueTask(Priority, Forward<TaskBody>(Task), DebugName);
    }

    // @brief Enqueue a task to the render thread
    template<typename TaskBody>
    void EnqueueRenderThreadTask(TaskBody&& Task, const char* DebugName = "RenderThreadTask",
                                 bool bHighPriority = false, bool bLocalQueue = false)
    {
        EExtendedTaskPriority Priority;
        if (bLocalQueue)
        {
            Priority = bHighPriority ? EExtendedTaskPriority::RenderThreadHiPriLocalQueue
                                     : EExtendedTaskPriority::RenderThreadNormalPriLocalQueue;
        }
        else
        {
            Priority = bHighPriority ? EExtendedTaskPriority::RenderThreadHiPri
                                     : EExtendedTaskPriority::RenderThreadNormalPri;
        }

        FNamedThreadManager::Get().EnqueueTask(Priority, Forward<TaskBody>(Task), DebugName);
    }

    // @brief Enqueue a task to the RHI thread
    template<typename TaskBody>
    void EnqueueRHIThreadTask(TaskBody&& Task, const char* DebugName = "RHIThreadTask",
                              bool bHighPriority = false, bool bLocalQueue = false)
    {
        EExtendedTaskPriority Priority;
        if (bLocalQueue)
        {
            Priority = bHighPriority ? EExtendedTaskPriority::RHIThreadHiPriLocalQueue
                                     : EExtendedTaskPriority::RHIThreadNormalPriLocalQueue;
        }
        else
        {
            Priority = bHighPriority ? EExtendedTaskPriority::RHIThreadHiPri
                                     : EExtendedTaskPriority::RHIThreadNormalPri;
        }

        FNamedThreadManager::Get().EnqueueTask(Priority, Forward<TaskBody>(Task), DebugName);
    }

    // @brief Enqueue a task to the Audio thread
    template<typename TaskBody>
    void EnqueueAudioThreadTask(TaskBody&& Task, const char* DebugName = "AudioThreadTask")
    {
        // Audio thread currently only supports one priority queue via named thread manager
        // We map it to "Normal" priority internally, but the thread itself runs at high priority
        FNamedThreadManager::Get().EnqueueTask(ENamedThread::AudioThread, FNamedThreadTask(MoveTemp(Task), EExtendedTaskPriority::None, DebugName));
    }

    // ============================================================================
    // Global Configuration
    // ============================================================================

    // @brief Global configuration for named thread wait behavior
    //
    // When true, waiting on any task will automatically process named thread
    // tasks if the current thread is a named thread. This helps prevent deadlocks
    // where a named thread waits on a task that might schedule work back to
    // that same thread.
    //
    // This mirrors UE5.7's GTaskGraphAlwaysWaitWithNamedThreadSupport.
    //
    // Default is false for backwards compatibility, but production code should
    // consider enabling this to avoid subtle deadlock scenarios.
    //
    // @see ShouldForceWaitWithNamedThreadsSupport
    extern bool GTaskGraphAlwaysWaitWithNamedThreadSupport;

    // @brief Check if extended priority should force named thread wait support
    //
    // @param Priority The extended priority to check
    // @return true if this priority should always use named thread wait support
    bool ShouldForceWaitWithNamedThreadsSupport(EExtendedTaskPriority Priority);

} // namespace OloEngine::Tasks
