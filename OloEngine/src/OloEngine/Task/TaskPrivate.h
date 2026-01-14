// TaskPrivate.h - Core task implementation with prerequisite support
// Ported from UE5.7 Tasks/TaskPrivate.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Task/LowLevelTask.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Task/ExtendedTaskPriority.h"
#include "OloEngine/Task/SmallTaskAllocator.h"
#include "OloEngine/Task/InheritedContext.h"
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/HAL/EventCount.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/Debug/TaskTrace.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "OloEngine/Templates/Invoke.h"
#include "OloEngine/Templates/RefCounting.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Core/Timeout.h"

#include <atomic>
#include <thread>
#include <type_traits>
#include <limits>

namespace OloEngine::Tasks
{
    using LowLevelTasks::ETaskPriority;
    using LowLevelTasks::ToString;
    using LowLevelTasks::ToTaskPriority;

    // Forward declarations
    class FPipe;
    template<typename ResultType>
    class TTask;

    namespace Private
    {
        // @brief Check if the current thread is retracting a task
        //
        // Used to detect and prevent re-entrant retraction scenarios.
        // Matches UE5.7's IsThreadRetractingTask() pattern.
        //
        // @return true if current thread is inside TryRetractAndExecute
        bool IsThreadRetractingTask();

        // @brief RAII scope for tracking task retraction
        //
        // Increments thread-local retraction counter on construction,
        // decrements on destruction. Used by TryRetractAndExecute.
        struct FThreadLocalRetractionScope
        {
            FThreadLocalRetractionScope();
            ~FThreadLocalRetractionScope();

            // Non-copyable, non-movable
            FThreadLocalRetractionScope(const FThreadLocalRetractionScope&) = delete;
            FThreadLocalRetractionScope& operator=(const FThreadLocalRetractionScope&) = delete;
        };
    } // namespace Private

    namespace Private
    {
        // @brief Translate regular priority and extended priority to named thread dispatch
        // @param Priority Base task priority
        // @param ExtendedPriority Extended priority (may include named thread targets)
        // @param OutNamedThread Output named thread if this is a named thread task
        // @param OutIsHighPriority Output whether this is high priority
        // @param OutIsLocalQueue Output whether this uses the local queue
        // @return true if this is a named thread task that should be routed specially
        bool TranslatePriority(
            ETaskPriority Priority,
            EExtendedTaskPriority ExtendedPriority,
            ENamedThread& OutNamedThread,
            bool& OutIsHighPriority,
            bool& OutIsLocalQueue);

        // @brief Check if currently executing on the rendering thread
        // @return true if on render thread
        bool IsInRenderingThread();
    } // namespace Private

    // @enum ETaskFlags
    // @brief Configuration options for high-level tasks
    enum class ETaskFlags
    {
        None = 0,
        DoNotRunInsideBusyWait = 1 << 0, // Do not pick this task for busy-waiting
    };
    ENUM_CLASS_FLAGS(ETaskFlags)

    namespace Private
    {
        class FTaskBase;

        // ============================================================================
        // Current Task Tracking (TLS)
        // ============================================================================

        // @brief Get the task currently being executed by this thread
        inline FTaskBase*& GetCurrentTaskRef()
        {
            static thread_local FTaskBase* s_CurrentTask = nullptr;
            return s_CurrentTask;
        }

        inline FTaskBase* GetCurrentTask()
        {
            return GetCurrentTaskRef();
        }

        inline FTaskBase* ExchangeCurrentTask(FTaskBase* Task)
        {
            FTaskBase*& CurrentRef = GetCurrentTaskRef();
            FTaskBase* PrevTask = CurrentRef;
            CurrentRef = Task;
            return PrevTask;
        }

        // ============================================================================
        // FTaskBase - Core task implementation
        // ============================================================================

        // @class FTaskBase
        // @brief Abstract base class for task implementation
        //
        // Implements:
        // - Intrusive ref-counting
        // - Prerequisites and subsequents for dependency tracking
        // - Nested task support
        // - Pipe integration
        // - Task retraction
        // - Inherited context
        //
        // The NumLocks field serves dual purpose:
        // - Before execution: counts prerequisites blocking execution
        // - After ExecutionFlag is set: counts nested tasks blocking completion
        class FTaskBase : private FInheritedContextBase
        {
            OLO_NONCOPYABLE(FTaskBase);

            // ExecutionFlag is set at the beginning of execution as the MSB of NumLocks
            static constexpr u32 ExecutionFlag = 0x80000000;
            static constexpr u32 NumInitialLocks = 1; // For launching

          public:
            // ====== Ref Counting ======

            void AddRef()
            {
                m_RefCount.fetch_add(1, std::memory_order_relaxed);
            }

            void Release()
            {
                if (m_RefCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    delete this;
                }
            }

            u32 GetRefCount(std::memory_order MemoryOrder = std::memory_order_relaxed) const
            {
                return m_RefCount.load(MemoryOrder);
            }

          protected:
            explicit FTaskBase(u32 InitRefCount, bool bUnlockPrerequisites = true)
                : m_RefCount(InitRefCount)
            {
                if (bUnlockPrerequisites)
                {
                    m_Prerequisites.Unlock();
                }
            }

            void Init(const char* InDebugName, ETaskPriority InPriority, EExtendedTaskPriority InExtendedPriority, ETaskFlags Flags)
            {
                m_ExtendedPriority = InExtendedPriority;

                // Store debug name and priority in low-level task
                m_LowLevelTask.Init(InDebugName, InPriority, [this, Deleter = LowLevelTasks::TDeleter<FTaskBase, &FTaskBase::Release>{ this }]()
                                    { TryExecuteTask(); }, LowLevelTasks::ETaskFlags::DefaultFlags);

                CaptureInheritedContext();
                TaskTrace::Launched(GetTraceId(), InDebugName, true, static_cast<i32>(InPriority), 0);
            }

            virtual ~FTaskBase()
            {
                OLO_CORE_ASSERT(IsCompleted(), "Task destroyed before completion");
                TaskTrace::Destroyed(GetTraceId());
            }

            virtual void ExecuteTask() = 0;

          public:
            // ====== State Queries ======

            bool IsAwaitable() const
            {
                return GetCurrentThreadId() != m_ExecutingThreadId.load(std::memory_order_relaxed);
            }

            bool IsNamedThreadTask() const
            {
                return IsNamedThreadPriority(m_ExtendedPriority);
            }

            ETaskPriority GetPriority() const
            {
                return m_LowLevelTask.GetPriority();
            }

            EExtendedTaskPriority GetExtendedPriority() const
            {
                return m_ExtendedPriority;
            }

            bool IsCompleted() const
            {
                return m_Subsequents.IsClosed();
            }

            TaskTrace::FId GetTraceId() const
            {
#if OLO_TASK_TRACE_ENABLED
                return m_TraceId.load(std::memory_order_relaxed);
#else
                return TaskTrace::InvalidId;
#endif
            }

            // ====== Prerequisites ======

            // @brief Add a single prerequisite task
            // @return true if successfully added, false if prerequisite already completed
            bool AddPrerequisites(FTaskBase& Prerequisite)
            {
                OLO_CORE_ASSERT(m_NumLocks.load(std::memory_order_relaxed) >= NumInitialLocks &&
                                    m_NumLocks.load(std::memory_order_relaxed) < ExecutionFlag,
                                "Prerequisites can only be added before the task is launched");

                // Increment lock count first (assuming we'll succeed)
                u32 PrevNumLocks = m_NumLocks.fetch_add(1, std::memory_order_relaxed);
                OLO_CORE_ASSERT(PrevNumLocks + 1 < ExecutionFlag, "Max number of prerequisites reached");

                if (!Prerequisite.AddSubsequent(*this))
                {
                    // Prerequisite already completed
                    m_NumLocks.fetch_sub(1, std::memory_order_relaxed);
                    return false;
                }

                Prerequisite.AddRef();
                m_Prerequisites.Push(&Prerequisite);
                return true;
            }

            // @brief Add prerequisites from a higher-level task handle
            template<typename HigherLevelTaskType, decltype(std::declval<HigherLevelTaskType>().Pimpl)* = nullptr>
            bool AddPrerequisites(const HigherLevelTaskType& Prerequisite)
            {
                return Prerequisite.IsValid() ? AddPrerequisites(*Prerequisite.Pimpl) : false;
            }

            // @brief Add prerequisites from an iterable collection
            template<typename PrerequisiteCollectionType, decltype(std::declval<PrerequisiteCollectionType>().begin())* = nullptr>
            void AddPrerequisites(const PrerequisiteCollectionType& InPrerequisites)
            {
                i32 NumAdded = 0;
                i32 NumPrereqs = 0;

                for (const auto& Prereq : InPrerequisites)
                {
                    ++NumPrereqs;
                }

                if (NumPrereqs == 0)
                    return;

                // Pre-increment locks for all prerequisites
                u32 PrevNumLocks = m_NumLocks.fetch_add(NumPrereqs, std::memory_order_relaxed);
                OLO_CORE_ASSERT(PrevNumLocks + NumPrereqs < ExecutionFlag, "Max number of prerequisites reached");

                bool bLockPrerequisite = true;
                u32 NumCompleted = 0;

                for (const auto& Prereq : InPrerequisites)
                {
                    FTaskBase* Prerequisite = nullptr;

                    // Handle different prerequisite types
                    if constexpr (std::is_same_v<std::decay_t<decltype(Prereq)>, FTaskBase*>)
                    {
                        Prerequisite = Prereq;
                    }
                    else if constexpr (std::is_pointer_v<std::decay_t<decltype(Prereq)>>)
                    {
                        Prerequisite = Prereq ? Prereq->Pimpl.GetReference() : nullptr;
                    }
                    else
                    {
                        Prerequisite = Prereq.IsValid() ? Prereq.Pimpl.GetReference() : nullptr;
                    }

                    if (Prerequisite == nullptr)
                    {
                        ++NumCompleted;
                        continue;
                    }

                    if (Prerequisite->AddSubsequent(*this))
                    {
                        Prerequisite->AddRef();
                        if (bLockPrerequisite)
                        {
                            m_Prerequisites.Lock();
                            bLockPrerequisite = false;
                        }
                        m_Prerequisites.PushNoLock(Prerequisite);
                    }
                    else
                    {
                        ++NumCompleted;
                    }
                }

                if (!bLockPrerequisite)
                {
                    m_Prerequisites.Unlock();
                }

                // Unlock for prerequisites that weren't added
                if (NumCompleted > 0)
                {
                    m_NumLocks.fetch_sub(NumCompleted, std::memory_order_release);
                }
            }

            // @brief Add a subsequent task that depends on this one
            // @return false if this task is already completed
            bool AddSubsequent(FTaskBase& Subsequent)
            {
                if (m_Subsequents.PushIfNotClosed(&Subsequent))
                {
                    TaskTrace::SubsequentAdded(GetTraceId(), Subsequent.GetTraceId());
                    return true;
                }
                return false;
            }

            // ====== Pipe Support ======

            void SetPipe(FPipe& InPipe)
            {
                // Keep task locked until pushed into pipe
                m_NumLocks.fetch_add(1, std::memory_order_relaxed);
                m_Pipe = &InPipe;
            }

            FPipe* GetPipe() const
            {
                return m_Pipe;
            }

            // ====== Nested Tasks ======

            // @brief Add a nested task that must complete before this task completes
            void AddNested(FTaskBase& Nested)
            {
                u32 PrevNumLocks = m_NumLocks.fetch_add(1, std::memory_order_relaxed);
                OLO_CORE_ASSERT(PrevNumLocks > ExecutionFlag, "Nested tasks can only be added during execution");
                OLO_CORE_ASSERT(PrevNumLocks + 1 < std::numeric_limits<u32>::max(), "Max nested tasks reached");

                if (Nested.AddSubsequent(*this))
                {
                    Nested.AddRef();
                    m_Prerequisites.Push(&Nested);
                }
                else
                {
                    m_NumLocks.fetch_sub(1, std::memory_order_relaxed);
                }
            }

            // ====== Launching ======

            // @brief Try to schedule task execution
            // @return false if blocked by prerequisites
            bool TryLaunch(u64 TaskSize)
            {
                TaskTrace::Launched(GetTraceId(), m_LowLevelTask.GetDebugName(), true,
                                    static_cast<i32>(m_LowLevelTask.GetPriority()), TaskSize);

                bool bWakeUpWorker = true;
                return TryUnlock(bWakeUpWorker);
            }

            // @brief Try to trigger the task (for task events that can be triggered multiple times)
            bool Trigger(u64 TaskSize)
            {
                if (m_TaskTriggered.exchange(true, std::memory_order_relaxed))
                {
                    return false; // Already triggered
                }

                AddRef(); // Keep alive during execution
                return TryLaunch(TaskSize);
            }

            // ====== Waiting ======

            // @brief Wait for task completion with timeout
            bool Wait(FTimeout Timeout)
            {
                if (IsCompleted())
                {
                    return true;
                }

                if (!IsAwaitable())
                {
                    OLO_CORE_ASSERT(false, "Cannot wait for a task being executed by the current thread");
                    return false;
                }

                return WaitImpl(Timeout);
            }

            // @brief Wait for task completion without timeout
            //
            // Uses GTaskGraphAlwaysWaitWithNamedThreadSupport to determine whether
            // to process named thread tasks while waiting. This can be overridden
            // for specific task priorities via ShouldForceWaitWithNamedThreadsSupport.
            void Wait()
            {
                if (IsCompleted())
                {
                    return;
                }

                OLO_CORE_ASSERT(IsAwaitable(), "Cannot wait for a task being executed by the current thread");

                // Use named thread support if globally enabled or required by this task's priority
                if (GTaskGraphAlwaysWaitWithNamedThreadSupport || ShouldForceWaitWithNamedThreadsSupport(m_ExtendedPriority))
                {
                    WaitWithNamedThreadsSupport();
                }
                else
                {
                    WaitImpl(FTimeout::Never());
                }
            }

            // @brief Wait with named thread support
            //
            // Mimics UE5.7's FTaskBase::WaitWithNamedThreadsSupport behavior:
            // When waiting on a named thread (GameThread, RenderThread, etc.), this
            // processes other tasks from that thread's queue while waiting, which
            // helps prevent deadlocks.
            //
            // On worker threads, this behaves like regular Wait().
            void WaitWithNamedThreadsSupport()
            {
                if (IsCompleted())
                {
                    return;
                }

                OLO_CORE_ASSERT(IsAwaitable(), "Cannot wait for a task being executed by the current thread");

                // First try to retract and execute inline
                TryRetractAndExecute(FTimeout::Never());
                if (IsCompleted())
                {
                    return;
                }

                // If we're on a named thread, process its queue while waiting
                if (!TryWaitOnNamedThread())
                {
                    WaitImpl(FTimeout::Never());
                }
            }

            // @brief Try to retract and execute the task inline
            bool TryRetractAndExecute(FTimeout Timeout, u32 RecursionDepth = 0)
            {
                if (IsCompleted() || Timeout.IsExpired())
                {
                    return IsCompleted();
                }

                if (!IsAwaitable())
                {
                    OLO_CORE_ASSERT(false, "Deadlock detected! A task can't be waited here, e.g. because it's being executed by the current thread");
                    return false;
                }

                // Task retraction is not supported for named thread tasks
                if (IsNamedThreadTask())
                {
                    return false;
                }

                // Avoid stack overflow - not expected in real-life cases but happens in stress tests
                if (RecursionDepth >= 200)
                {
                    return false;
                }
                ++RecursionDepth;

                // Check if locked by prerequisites
                auto IsLockedByPrerequisites = [this]()
                {
                    u32 LocalNumLocks = m_NumLocks.load(std::memory_order_relaxed);
                    return LocalNumLocks > 0 && LocalNumLocks < ExecutionFlag;
                };

                // Try to retract prerequisites first
                if (IsLockedByPrerequisites())
                {
                    // Prerequisites are "consumed" here even if their retraction fails.
                    // This means that once prerequisite retraction failed, it won't be performed again.
                    auto LocalPrereqs = m_Prerequisites.PopAll();
                    for (FTaskBase* Prereq : LocalPrereqs)
                    {
                        // Ignore if retraction failed, as this thread still can try to help with other prerequisites
                        Prereq->TryRetractAndExecute(Timeout, RecursionDepth);
                        Prereq->Release();
                    }
                }

                // If we don't have any more prerequisites, let TryUnlock execute these to avoid any race
                // condition where we could clear the last reference before TryUnlock finishes.
                // These are super fast to process anyway so we can just consider them done for retraction purpose.
                if (m_ExtendedPriority == EExtendedTaskPriority::TaskEvent ||
                    m_ExtendedPriority == EExtendedTaskPriority::Inline)
                {
                    return true;
                }

                if (Timeout.IsExpired())
                {
                    return IsCompleted();
                }

                // Try to get execution permission (wrapped in retraction scope)
                {
                    FThreadLocalRetractionScope ThreadLocalRetractionScope;

                    if (!TryExecuteTask())
                    {
                        return false; // Still locked by prerequisites, or another thread got execution permission first
                    }
                }

                // The task was launched so the scheduler will handle the internal reference held by low-level task

                // Retract nested tasks, if any
                {
                    // Keep trying retracting all nested tasks even if some of them fail
                    bool bSucceeded = true;
                    auto LocalPrereqs = m_Prerequisites.PopAll();
                    for (FTaskBase* Prereq : LocalPrereqs)
                    {
                        if (!Prereq->TryRetractAndExecute(Timeout, RecursionDepth))
                        {
                            bSucceeded = false;
                        }
                        Prereq->Release();
                    }

                    if (!bSucceeded)
                    {
                        return false;
                    }
                }

                // At this point the task is executed and has no pending nested tasks, but still can be "not completed"
                // (nested tasks can be in the process of completing it concurrently)
                return true;
            }

            // Legacy TryRetractAndExecute that attempts inline execution (for compatibility)
            bool TryRetractAndExecuteOld(FTimeout Timeout, u32 RecursionDepth = 0)
            {
                if (IsCompleted())
                {
                    return true;
                }

                if (Timeout.IsExpired())
                {
                    return false;
                }

                // Avoid stack overflow
                if (RecursionDepth >= 200)
                {
                    return false;
                }

                // Check if locked by prerequisites
                auto IsLockedByPrerequisites = [this]()
                {
                    u32 LocalNumLocks = m_NumLocks.load(std::memory_order_relaxed);
                    return LocalNumLocks > 0 && LocalNumLocks < ExecutionFlag;
                };

                // Try to retract prerequisites first
                if (IsLockedByPrerequisites())
                {
                    auto LocalPrereqs = m_Prerequisites.PopAll();
                    for (FTaskBase* Prereq : LocalPrereqs)
                    {
                        Prereq->TryRetractAndExecute(Timeout, RecursionDepth + 1);
                        Prereq->Release();
                    }
                }

                // Try to get execution permission
                if (TrySetExecutionFlag())
                {
                    AddRef(); // Keep alive during execution

                    ReleasePrerequisites();

                    FTaskBase* PrevTask = ExchangeCurrentTask(this);
                    m_ExecutingThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

                    if (GetPipe() != nullptr)
                    {
                        StartPipeExecution();
                    }

                    {
                        FInheritedContextScope InheritedContextScope = RestoreInheritedContext();
                        TaskTrace::FTaskTimingEventScope TaskEventScope(GetTraceId());
                        ExecuteTask();
                    }

                    if (GetPipe() != nullptr)
                    {
                        FinishPipeExecution();
                    }

                    m_ExecutingThreadId.store(0, std::memory_order_relaxed);
                    ExchangeCurrentTask(PrevTask);

                    // Check for pending nested tasks
                    u32 LocalNumLocks = m_NumLocks.fetch_sub(1, std::memory_order_acq_rel) - 1;
                    if (LocalNumLocks == ExecutionFlag)
                    {
                        Close();
                        Release();
                    }

                    return true;
                }

                return false;
            }

            // @brief Release internal reference for unlaunched tasks
            void ReleaseInternalReference()
            {
                OLO_CORE_VERIFY_SLOW(m_LowLevelTask.TryCancel(), "Failed to cancel unlaunched task");
            }

            // @brief Try to set the execution flag atomically
            //
            // Only one thread can successfully set execution flag, that grants task execution permission.
            // @returns false if another thread got execution permission first
            bool TrySetExecutionFlag()
            {
                u32 ExpectedUnlocked = 0;
                // Set the execution flag and simultaneously lock it (+1) so a nested task completion
                // doesn't close it before its execution is finished
                return m_NumLocks.compare_exchange_strong(ExpectedUnlocked, ExecutionFlag + 1,
                                                          std::memory_order_acq_rel, std::memory_order_relaxed);
            }

          protected:
            bool TryExecuteTask()
            {
                if (!TrySetExecutionFlag())
                {
                    return false;
                }

                AddRef(); // Keep alive for nested tasks

                ReleasePrerequisites();

                FTaskBase* PrevTask = ExchangeCurrentTask(this);
                m_ExecutingThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

                if (GetPipe() != nullptr)
                {
                    StartPipeExecution();
                }

                {
                    FInheritedContextScope InheritedContextScope = RestoreInheritedContext();
                    TaskTrace::FTaskTimingEventScope TaskEventScope(GetTraceId());
                    ExecuteTask();
                }

                if (GetPipe() != nullptr)
                {
                    FinishPipeExecution();
                }

                m_ExecutingThreadId.store(0, std::memory_order_relaxed);
                ExchangeCurrentTask(PrevTask);

                // Check for pending nested tasks
                u32 LocalNumLocks = m_NumLocks.fetch_sub(1, std::memory_order_acq_rel) - 1;
                if (LocalNumLocks == ExecutionFlag)
                {
                    Close();
                    Release();
                }

                return true;
            }

            void Close()
            {
                OLO_CORE_ASSERT(!IsCompleted(), "Task already closed");

                bool bWakeUpWorker = m_ExtendedPriority == EExtendedTaskPriority::TaskEvent;

                for (FTaskBase* Subsequent : m_Subsequents.Close())
                {
                    Subsequent->TryUnlock(bWakeUpWorker);
                }

                if (GetPipe() != nullptr)
                {
                    ClearPipe();
                }

                ReleasePrerequisites();
                TaskTrace::Completed(GetTraceId());
                m_StateChangeEvent.NotifyWeak();
            }

            void ClearPipe();
            void StartPipeExecution();
            void FinishPipeExecution();
            FTaskBase* TryPushIntoPipe();

          private:
            bool TryUnlock(bool& bWakeUpWorker)
            {
                FPipe* LocalPipe = GetPipe();

                u32 PrevNumLocks = m_NumLocks.fetch_sub(1, std::memory_order_acq_rel);
                u32 LocalNumLocks = PrevNumLocks - 1;

                if (PrevNumLocks < ExecutionFlag)
                {
                    // Pre-execution: try to schedule
                    OLO_CORE_ASSERT(PrevNumLocks != 0, "Task is not locked");

                    bool bPrerequisitesCompleted = LocalPipe == nullptr ? LocalNumLocks == 0 : LocalNumLocks <= 1;
                    if (!bPrerequisitesCompleted)
                    {
                        return false;
                    }

                    // Handle piping
                    if (LocalPipe != nullptr)
                    {
                        bool bFirstPipingAttempt = LocalNumLocks == 1;
                        if (bFirstPipingAttempt)
                        {
                            FTaskBase* PrevPipedTask = TryPushIntoPipe();
                            if (PrevPipedTask != nullptr)
                            {
                                m_Prerequisites.Push(PrevPipedTask);
                                return false;
                            }
                            m_NumLocks.store(0, std::memory_order_release);
                        }
                    }

                    if (m_ExtendedPriority == EExtendedTaskPriority::Inline)
                    {
                        TryExecuteTask();
                        ReleaseInternalReference();
                    }
                    else if (m_ExtendedPriority == EExtendedTaskPriority::TaskEvent)
                    {
                        if (TrySetExecutionFlag())
                        {
                            ReleasePrerequisites();
                            Close();
                            ReleaseInternalReference();
                        }
                    }
                    else if (IsNamedThreadTask())
                    {
                        // Route to named thread queue
                        ScheduleOnNamedThread();
                    }
                    else
                    {
                        // Regular task: schedule on worker thread
                        Schedule(bWakeUpWorker);
                    }

                    return true;
                }

                // Post-execution: close if no pending nested tasks
                if (LocalNumLocks == ExecutionFlag)
                {
                    Close();
                    Release();
                    return true;
                }

                return false;
            }

            void Schedule(bool& bWakeUpWorker)
            {
                TaskTrace::Scheduled(GetTraceId());

                // In case a thread is waiting on us to perform retraction, now is the time to try retraction again.
                // This needs to be before the launch as performing the execution can destroy the task.
                m_StateChangeEvent.NotifyWeak();

                // Use local queue preference for first subsequent
                // This needs to be the last line touching any of the task's properties.
                LowLevelTasks::EQueuePreference QueuePref = bWakeUpWorker ? LowLevelTasks::EQueuePreference::GlobalQueuePreference : LowLevelTasks::EQueuePreference::LocalQueuePreference;

                bWakeUpWorker |= LowLevelTasks::TryLaunch(m_LowLevelTask, QueuePref, bWakeUpWorker);
                // Use-after-free territory, do not touch any of the task's properties here.
            }

            void ScheduleOnNamedThread()
            {
                // Route task to the appropriate named thread queue
                ENamedThread TargetThread;
                bool bIsHighPriority;
                bool bIsLocalQueue;

                if (Private::TranslatePriority(GetPriority(), m_ExtendedPriority,
                                               TargetThread, bIsHighPriority, bIsLocalQueue))
                {
                    // Wrap the low-level task execution in a named thread task
                    AddRef(); // Keep task alive for named thread execution

                    FNamedThreadTask::FTaskFunction TaskFunc = [this]()
                    {
                        // Execute the low-level task wrapper (which will call TryExecuteTask)
                        if (m_LowLevelTask.TryPrepareLaunch())
                        {
                            m_LowLevelTask.ExecuteTask();
                        }
                        Release(); // Release the ref we added
                    };

                    FNamedThreadManager::Get().EnqueueTask(
                        TargetThread,
                        FNamedThreadTask(MoveTemp(TaskFunc), m_ExtendedPriority, m_LowLevelTask.GetDebugName()));
                }
                else
                {
                    // Fallback to regular scheduler if translation failed
                    bool bWakeUpWorker = true;
                    Schedule(bWakeUpWorker);
                }
            }

            void ReleasePrerequisites()
            {
                for (FTaskBase* Prerequisite : m_Prerequisites.PopAll())
                {
                    Prerequisite->Release();
                }
            }

            bool WaitImpl(FTimeout Timeout)
            {
                while (true)
                {
                    // Ignore the result as we still have to make sure the task is completed upon returning
                    TryRetractAndExecute(Timeout);

                    // Spin for a while with hope the task is getting completed right now,
                    // to avoid getting blocked by a pricey syscall
                    const u32 MaxSpinCount = 40;
                    for (u32 SpinCount = 0; SpinCount != MaxSpinCount && !IsCompleted() && !Timeout.IsExpired(); ++SpinCount)
                    {
                        FPlatformProcess::Yield();
                    }

                    if (IsCompleted() || Timeout.IsExpired())
                    {
                        return IsCompleted();
                    }

                    auto Token = m_StateChangeEvent.PrepareWait();

                    // Important to check the condition a second time after PrepareWait has been called
                    // to make sure we don't miss an important state change event.
                    if (IsCompleted())
                    {
                        return true;
                    }

                    if (Timeout.WillNeverExpire())
                    {
                        m_StateChangeEvent.Wait(Token);
                    }
                    else
                    {
                        FMonotonicTimeSpan Remaining = Timeout.GetRemainingTime();
                        if (Remaining <= FMonotonicTimeSpan::Zero())
                        {
                            return false;
                        }
                        m_StateChangeEvent.WaitFor(Token, Remaining);
                    }

                    // Once the state of the task has changed (either closed or scheduled),
                    // it's time to do another round of retraction to help if possible.
                }
            }

            static u32 GetCurrentThreadId()
            {
                return static_cast<u32>(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFFFFF);
            }

            // @brief Try to wait while processing named thread tasks
            //
            // If called from a named thread (GameThread, RenderThread),
            // this processes tasks from that thread's queue while waiting for the
            // task to complete. This helps prevent deadlocks when a named thread
            // waits on a task that might schedule work back to that same thread.
            //
            // This follows the UE5.7 pattern of keeping the named thread productive
            // while waiting, rather than blocking.
            //
            // @return true if wait was handled on a named thread, false otherwise
            bool TryWaitOnNamedThread()
            {
                FNamedThreadManager& Manager = FNamedThreadManager::Get();
                ENamedThread CurrentThread = Manager.GetCurrentThreadIfKnown();

                if (CurrentThread == ENamedThread::Invalid)
                {
                    // Not on a named thread
                    return false;
                }

                // Don't process if the thread is already processing tasks
                // (avoid re-entrancy issues)
                if (Manager.IsThreadProcessingTasks(CurrentThread))
                {
                    return false;
                }

                FNamedThreadQueue& Queue = Manager.GetQueue(CurrentThread);

                // Use processing scope to track that we're processing
                FNamedThreadManager::FProcessingScope ProcessingScope;

                // Process named thread tasks while waiting for our target task.
                // This keeps the thread productive and allows tasks that depend on
                // this thread to make progress, preventing potential deadlocks.
                while (!IsCompleted())
                {
                    // Try to process one task from the queue
                    if (!Queue.TryExecuteOne(true))
                    {
                        // No tasks available, wait on our completion event
                        auto Token = m_StateChangeEvent.PrepareWait();
                        if (IsCompleted())
                        {
                            break;
                        }
                        // Brief timeout to periodically check for new queued tasks
                        m_StateChangeEvent.WaitFor(Token, FMonotonicTimeSpan::FromMilliseconds(1));
                    }
                }

                return true;
            }

          private:
            std::atomic<u32> m_RefCount;
            std::atomic<u32> m_NumLocks{ NumInitialLocks };

            FPipe* m_Pipe = nullptr;
            FEventCount m_StateChangeEvent;
            EExtendedTaskPriority m_ExtendedPriority = EExtendedTaskPriority::None;
            std::atomic<bool> m_TaskTriggered{ false };
            std::atomic<u32> m_ExecutingThreadId{ 0 };

#if OLO_TASK_TRACE_ENABLED
            std::atomic<TaskTrace::FId> m_TraceId{ TaskTrace::GenerateTaskId() };
#endif

            // Prerequisites container with inline storage for common case
            template<typename AllocatorType = FDefaultAllocator>
            class TPrerequisites
            {
              public:
                void Push(FTaskBase* Prerequisite)
                {
                    TUniqueLock<FMutex> Lock(m_Mutex);
                    m_Prerequisites.Emplace(Prerequisite);
                }

                void PushNoLock(FTaskBase* Prerequisite)
                {
                    m_Prerequisites.Emplace(Prerequisite);
                }

                TArray<FTaskBase*, AllocatorType> PopAll()
                {
                    TUniqueLock<FMutex> Lock(m_Mutex);
                    return MoveTemp(m_Prerequisites);
                }

                void Lock()
                {
                    m_Mutex.Lock();
                }
                void Unlock()
                {
                    m_Mutex.Unlock();
                }

              private:
                TArray<FTaskBase*, AllocatorType> m_Prerequisites;
                FMutex m_Mutex{ FAcquireLock{} }; // Start locked
            };

            TPrerequisites<TInlineAllocator<1>> m_Prerequisites;

            // Subsequents container
            template<typename AllocatorType = FDefaultAllocator>
            class TSubsequents
            {
              public:
                bool PushIfNotClosed(FTaskBase* NewItem)
                {
                    if (m_bIsClosed.load(std::memory_order_acquire))
                    {
                        return false;
                    }
                    TUniqueLock<FMutex> Lock(m_Mutex);
                    if (m_bIsClosed.load(std::memory_order_relaxed))
                    {
                        return false;
                    }
                    m_Subsequents.Emplace(NewItem);
                    return true;
                }

                TArray<FTaskBase*, AllocatorType> Close()
                {
                    TUniqueLock<FMutex> Lock(m_Mutex);
                    m_bIsClosed.store(true, std::memory_order_release);
                    return MoveTemp(m_Subsequents);
                }

                bool IsClosed() const
                {
                    return m_bIsClosed.load(std::memory_order_acquire);
                }

              private:
                TArray<FTaskBase*, AllocatorType> m_Subsequents;
                std::atomic<bool> m_bIsClosed{ false };
                FMutex m_Mutex;
            };

            TSubsequents<TInlineAllocator<1>> m_Subsequents;

            LowLevelTasks::FTask m_LowLevelTask;

          protected:
            void UnlockPrerequisites()
            {
                m_Prerequisites.Unlock();
            }
        };

        // ============================================================================
        // TTaskWithResult - Task with result storage
        // ============================================================================

        // @class TTaskWithResult
        // @brief Extension of FTaskBase for tasks that return a result
        //
        // Stores task execution result using TTypeCompatibleBytes for proper alignment.
        // Result is constructed during ExecuteTask() and destructed in the destructor.
        template<typename ResultType>
        class TTaskWithResult : public FTaskBase
        {
          protected:
            explicit TTaskWithResult(const char* InDebugName, ETaskPriority InPriority,
                                     EExtendedTaskPriority InExtendedPriority, u32 InitRefCount, ETaskFlags Flags)
                : FTaskBase(InitRefCount)
            {
                Init(InDebugName, InPriority, InExtendedPriority, Flags);
            }

            virtual ~TTaskWithResult() override
            {
                // Destroy result storage - it was constructed during execution
                DestructItem(m_ResultStorage.GetTypedPtr());
            }

          public:
            ResultType& GetResult()
            {
                checkSlow(IsCompleted());
                return *m_ResultStorage.GetTypedPtr();
            }

          protected:
            TTypeCompatibleBytes<ResultType> m_ResultStorage;
        };

        // ============================================================================
        // TExecutableTaskBase - Task with body storage
        // ============================================================================

        // @class TExecutableTaskBase
        // @brief Task implementation that stores and executes a callable body
        //
        // Generic version for tasks that return non-void results.
        // Stores the task body and executes it, storing the result in TTaskWithResult::m_ResultStorage.
        template<typename TaskBodyType, typename ResultType = TInvokeResult_T<TaskBodyType>, typename Enable = void>
        class TExecutableTaskBase : public TTaskWithResult<ResultType>
        {
            OLO_NONCOPYABLE(TExecutableTaskBase);

          public:
            virtual void ExecuteTask() override final
            {
                // Execute task body and store result
                new (this->m_ResultStorage.GetTypedPtr()) ResultType(Invoke(*m_TaskBodyStorage.GetTypedPtr()));

                // Destroy the task body as soon as we are done with it, as it can have
                // captured data sensitive to destruction order
                DestructItem(m_TaskBodyStorage.GetTypedPtr());
            }

          protected:
            TExecutableTaskBase(const char* InDebugName, TaskBodyType&& TaskBody,
                                ETaskPriority InPriority, EExtendedTaskPriority InExtendedPriority, ETaskFlags Flags)
                : TTaskWithResult<ResultType>(InDebugName, InPriority, InExtendedPriority, 2, Flags)
            // 2 init refs: one for the initial reference (we don't increment it on passing to TRefCountPtr),
            // and one for the internal reference that keeps the task alive while it's in the system.
            // Released either on task completion or by the scheduler after trying to execute the task.
            {
                new (m_TaskBodyStorage.GetTypedPtr()) TaskBodyType(MoveTemp(TaskBody));
            }

          private:
            TTypeCompatibleBytes<TaskBodyType> m_TaskBodyStorage;
        };

        // @brief Specialization for tasks that don't return results (void)
        template<typename TaskBodyType>
        class TExecutableTaskBase<TaskBodyType, typename TEnableIf<std::is_same_v<TInvokeResult_T<TaskBodyType>, void>>::Type>
            : public FTaskBase
        {
            OLO_NONCOPYABLE(TExecutableTaskBase);

          public:
            virtual void ExecuteTask() override final
            {
                Invoke(*m_TaskBodyStorage.GetTypedPtr());

                // Destroy the task body as soon as we are done with it, as it can have
                // captured data sensitive to destruction order
                DestructItem(m_TaskBodyStorage.GetTypedPtr());
            }

          protected:
            TExecutableTaskBase(const char* InDebugName, TaskBodyType&& TaskBody,
                                ETaskPriority InPriority, EExtendedTaskPriority InExtendedPriority, ETaskFlags Flags)
                : FTaskBase(2) // 2 refs: one initial, one for scheduler
            {
                Init(InDebugName, InPriority, InExtendedPriority, Flags);
                new (m_TaskBodyStorage.GetTypedPtr()) TaskBodyType(MoveTemp(TaskBody));
            }

          private:
            TTypeCompatibleBytes<TaskBodyType> m_TaskBodyStorage;
        };

        // ============================================================================
        // TExecutableTask - Final task class with small allocation optimization
        // ============================================================================

        template<typename TaskBodyType>
        class alignas(OLO_PLATFORM_CACHE_LINE_SIZE) TExecutableTask final : public TExecutableTaskBase<TaskBodyType>
        {
          public:
            TExecutableTask(const char* InDebugName, TaskBodyType&& TaskBody,
                            ETaskPriority InPriority, EExtendedTaskPriority InExtendedPriority, ETaskFlags Flags)
                : TExecutableTaskBase<TaskBodyType>(InDebugName, MoveTemp(TaskBody), InPriority, InExtendedPriority, Flags)
            {
            }

            static TExecutableTask* Create(const char* InDebugName, TaskBodyType&& TaskBody,
                                           ETaskPriority InPriority, EExtendedTaskPriority InExtendedPriority, ETaskFlags Flags)
            {
                return new TExecutableTask(InDebugName, MoveTemp(TaskBody), InPriority, InExtendedPriority, Flags);
            }

            static void* operator new(size_t Size)
            {
                if (Size <= SmallTaskSize)
                {
                    return GetSmallTaskAllocator().Allocate();
                }
                return FMemory::Malloc(Size, OLO_PLATFORM_CACHE_LINE_SIZE);
            }

            static void operator delete(void* Ptr, size_t Size)
            {
                if (Size <= SmallTaskSize)
                {
                    GetSmallTaskAllocator().Free(Ptr);
                }
                else
                {
                    FMemory::Free(Ptr);
                }
            }
        };

        // ============================================================================
        // FTaskEventBase - Signaling task with no body
        // ============================================================================

        class FTaskEventBase : public FTaskBase
        {
          public:
            static FTaskEventBase* Create(const char* DebugName)
            {
                return new FTaskEventBase(DebugName);
            }

            static void* operator new(size_t Size);
            static void operator delete(void* Ptr);

          private:
            FTaskEventBase(const char* InDebugName)
                : FTaskBase(1) // Just initial reference
            {
                TaskTrace::Created(GetTraceId(), sizeof(*this));
                Init(InDebugName, ETaskPriority::Normal, EExtendedTaskPriority::TaskEvent, ETaskFlags::None);
            }

            virtual void ExecuteTask() override final
            {
                OLO_CORE_ASSERT(false, "TaskEvent should never be executed");
            }
        };

        // Allocator for task events
        // UE5.7 declares: using FTaskEventBaseAllocator = TLockFreeFixedSizeAllocator_TLSCache<sizeof(FTaskEventBase), PLATFORM_CACHE_LINE_SIZE>;
        using FTaskEventBaseAllocator = TFixedSizeTaskAllocator<sizeof(FTaskEventBase), OLO_PLATFORM_CACHE_LINE_SIZE>;

        inline FTaskEventBaseAllocator& GetTaskEventAllocator()
        {
            static FTaskEventBaseAllocator Allocator;
            return Allocator;
        }

        inline void* FTaskEventBase::operator new(size_t /*Size*/)
        {
            return GetTaskEventAllocator().Allocate();
        }

        inline void FTaskEventBase::operator delete(void* Ptr)
        {
            GetTaskEventAllocator().Free(Ptr);
        }

        // ============================================================================
        // Helper for task retraction
        // ============================================================================

        template<typename TaskCollectionType>
        bool TryRetractAndExecute(const TaskCollectionType& Tasks, FTimeout Timeout)
        {
            bool bResult = true;

            for (auto& Task : Tasks)
            {
                if (Task.IsValid() && !Task.Pimpl->TryRetractAndExecute(Timeout))
                {
                    bResult = false;
                }

                if (Timeout.IsExpired())
                {
                    return false;
                }
            }

            return bResult;
        }

    } // namespace Private

} // namespace OloEngine::Tasks
