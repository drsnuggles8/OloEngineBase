// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Task/LowLevelTask.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/TaskTag.h"
#include "OloEngine/Containers/ConsumeAllMpmcQueue.h"
#include "OloEngine/Memory/MallocAnsi.h"
#include "OloEngine/Memory/PlatformMallocCrash.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/HAL/PlatformMisc.h"
#include "OloEngine/HAL/RunnableThread.h"
#include "OloEngine/Core/PlatformTLS.h"
#include "OloEngine/Misc/Fork.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// Platform cache line size (typically 64 bytes on x86/x64)
#ifndef PLATFORM_CACHE_LINE_SIZE
#define PLATFORM_CACHE_LINE_SIZE 64
#endif

// Platform asymmetric fence support (only ARM platforms typically support this)
#ifndef PLATFORM_SUPPORTS_ASYMMETRIC_FENCES
#define PLATFORM_SUPPORTS_ASYMMETRIC_FENCES 0
#endif

namespace OloEngine::LowLevelTasks
{
    // Global configuration flags (can be set before starting workers)
    // These mirror UE5.7's GTaskGraph* variables for behavioral parity
    static bool g_TaskGraphUseDynamicPrioritization = true; // UE5.7 default is true (1)
    static float g_TaskGraphOversubscriptionRatio = 2.0f;
    static bool g_TaskGraphUseDynamicThreadCreation = false;
    static bool g_TaskGraphConfigInitialized = false;

    // @brief Parse command-line or environment configuration for task graph settings
    //
    // This mirrors UE5.7's approach of parsing command-line arguments like:
    // - TaskGraphUseDynamicPrioritization=0|1
    // - TaskGraphUseDynamicThreadCreation=0|1
    //
    // Since OloEngine may not have a unified command-line parser yet, we also support
    // environment variables:
    // - OLO_TASK_GRAPH_DYNAMIC_PRIORITIZATION=0|1
    // - OLO_TASK_GRAPH_DYNAMIC_THREAD_CREATION=0|1
    // - OLO_TASK_GRAPH_OVERSUBSCRIPTION_RATIO=<float>
    static void InitializeTaskGraphConfiguration()
    {
        if (g_TaskGraphConfigInitialized)
        {
            return;
        }
        g_TaskGraphConfigInitialized = true;

        // Check environment variables for configuration
        if (const char* EnvDynamicPrioritization = std::getenv("OLO_TASK_GRAPH_DYNAMIC_PRIORITIZATION"))
        {
            if (std::strcmp(EnvDynamicPrioritization, "0") == 0 || std::strcmp(EnvDynamicPrioritization, "false") == 0)
            {
                g_TaskGraphUseDynamicPrioritization = false;
            }
            else if (std::strcmp(EnvDynamicPrioritization, "1") == 0 || std::strcmp(EnvDynamicPrioritization, "true") == 0)
            {
                g_TaskGraphUseDynamicPrioritization = true;
            }
        }

        if (const char* EnvDynamicThreadCreation = std::getenv("OLO_TASK_GRAPH_DYNAMIC_THREAD_CREATION"))
        {
            if (std::strcmp(EnvDynamicThreadCreation, "0") == 0 || std::strcmp(EnvDynamicThreadCreation, "false") == 0)
            {
                g_TaskGraphUseDynamicThreadCreation = false;
            }
            else if (std::strcmp(EnvDynamicThreadCreation, "1") == 0 || std::strcmp(EnvDynamicThreadCreation, "true") == 0)
            {
                g_TaskGraphUseDynamicThreadCreation = true;
            }
        }

        if (const char* EnvOversubscriptionRatio = std::getenv("OLO_TASK_GRAPH_OVERSUBSCRIPTION_RATIO"))
        {
            float Value = static_cast<float>(std::atof(EnvOversubscriptionRatio));
            if (Value >= 1.0f)
            {
                g_TaskGraphOversubscriptionRatio = Value;
            }
        }
    }

    // Game thread ID for IsInGameThread() check
    static u32 g_GameThreadId = 0;
    static std::atomic<bool> g_GameThreadIdInitialized{ false };

    bool IsInGameThread()
    {
        return g_GameThreadIdInitialized.load(std::memory_order_acquire) &&
               FPlatformTLS::GetCurrentThreadId() == g_GameThreadId;
    }

    void InitGameThreadId()
    {
        g_GameThreadId = FPlatformTLS::GetCurrentThreadId();
        g_GameThreadIdInitialized.store(true, std::memory_order_release);
    }

    // Thread-local storage definitions
    thread_local FSchedulerTls::FTlsValuesHolder FSchedulerTls::s_TlsValuesHolder;
    // Note: FTask::s_ActiveTask is defined inline in Task.h (C++17 inline variable)
    thread_local bool Private::FOversubscriptionTls::s_bIsOversubscriptionAllowed = false;

    // SchedulerTls are allocated very early at thread creation and we need an allocator
    // that is OK with being called that early.
    void* FSchedulerTls::FTlsValues::operator new(sizet Size)
    {
        return AnsiMalloc(Size, PLATFORM_CACHE_LINE_SIZE);
    }

    void FSchedulerTls::FTlsValues::operator delete(void* Ptr)
    {
        AnsiFree(Ptr);
    }

    // Allocator for TConsumeAllMpmcQueue that uses ANSI allocator for early thread safety
    struct FTlsValuesAllocator
    {
        using SizeType = sizet;
        static constexpr bool NeedsElementType = false;

        static void* Malloc(sizet Count, u32 Alignment = DEFAULT_ALIGNMENT)
        {
            return AnsiMalloc(Count, Alignment);
        }

        static void Free(void* Ptr)
        {
            AnsiFree(Ptr);
        }
    };

    // FSchedulerTls::FImpl - manages TLS values across threads
    class FSchedulerTls::FImpl
    {
      public:
        static FMutex s_ThreadTlsValuesMutex;
        static FSchedulerTls::FTlsValues* s_ThreadTlsValues;

        static TConsumeAllMpmcQueue<FSchedulerTls::FTlsValues*, FTlsValuesAllocator> s_PendingInsertTlsValues;
        static TConsumeAllMpmcQueue<FSchedulerTls::FTlsValues*, FTlsValuesAllocator> s_PendingDeleteTlsValues;

        static void ProcessPendingTlsValuesNoLock()
        {
            s_PendingInsertTlsValues.ConsumeAllLifo(
                [](FTlsValues* TlsValues)
                {
                    TlsValues->LinkHead(s_ThreadTlsValues);
                });

            s_PendingDeleteTlsValues.ConsumeAllLifo(
                [](FTlsValues* TlsValues)
                {
                    TlsValues->Unlink();
                    delete TlsValues;
                });
        }
    };

    FMutex FSchedulerTls::FImpl::s_ThreadTlsValuesMutex;
    FSchedulerTls::FTlsValues* FSchedulerTls::FImpl::s_ThreadTlsValues = nullptr;

    TConsumeAllMpmcQueue<FSchedulerTls::FTlsValues*, FTlsValuesAllocator> FSchedulerTls::FImpl::s_PendingInsertTlsValues;
    TConsumeAllMpmcQueue<FSchedulerTls::FTlsValues*, FTlsValuesAllocator> FSchedulerTls::FImpl::s_PendingDeleteTlsValues;

    FSchedulerTls::FTlsValuesHolder::FTlsValuesHolder()
    {
        // Avoid a deadlock on threads being spun up or down during a crash
        if (FPlatformMallocCrash::IsActive())
        {
            TlsValues = nullptr;
            return;
        }

        TlsValues = new FTlsValues();

        if (FImpl::s_ThreadTlsValuesMutex.TryLock())
        {
            FImpl::ProcessPendingTlsValuesNoLock();
            TlsValues->LinkHead(FImpl::s_ThreadTlsValues);
            FImpl::s_ThreadTlsValuesMutex.Unlock();
        }
        else
        {
            FImpl::s_PendingInsertTlsValues.ProduceItem(TlsValues);
        }
    }

    FSchedulerTls::FTlsValuesHolder::~FTlsValuesHolder()
    {
        // Avoid a deadlock on threads being spun up or down during a crash
        if (FPlatformMallocCrash::IsActive())
        {
            return;
        }

        if (TlsValues)
        {
            if (FImpl::s_ThreadTlsValuesMutex.TryLock())
            {
                FImpl::ProcessPendingTlsValuesNoLock();
                TlsValues->Unlink();
                FImpl::s_ThreadTlsValuesMutex.Unlock();
                delete TlsValues;
            }
            else
            {
                FImpl::s_PendingDeleteTlsValues.ProduceItem(TlsValues);
            }

            TlsValues = nullptr;
        }
    }

    bool& Private::FOversubscriptionTls::GetIsOversubscriptionAllowedRef()
    {
        return s_bIsOversubscriptionAllowed;
    }

    void FOversubscriptionScope::TryIncrementOversubscription()
    {
        if (Private::FOversubscriptionTls::IsOversubscriptionAllowed())
        {
            m_bIncrementOversubscriptionEmitted = true;
            FScheduler::Get().IncrementOversubscription();
        }
    }

    void FOversubscriptionScope::DecrementOversubscription()
    {
        FScheduler::Get().DecrementOversubscription();
        m_bIncrementOversubscriptionEmitted = false;
    }

    FSchedulerTls::FTlsValues& FSchedulerTls::GetTlsValuesRef()
    {
        return *s_TlsValuesHolder.TlsValues;
    }

    // Singleton instance
    FScheduler FScheduler::s_Singleton;

    // @brief Platform-specific function to get the number of worker threads to spawn
    static u32 NumberOfWorkerThreadsToSpawn()
    {
        u32 NumCores = std::thread::hardware_concurrency();
        return NumCores > 0 ? NumCores : 4;
    }

    // @brief Convert LowLevelTasks::EThreadPriority to OloEngine::EThreadPriority
    static OloEngine::EThreadPriority ConvertToPlatformPriority(EThreadPriority Priority)
    {
        switch (Priority)
        {
            case EThreadPriority::TPri_Normal:
                return OloEngine::EThreadPriority::TPri_Normal;
            case EThreadPriority::TPri_AboveNormal:
                return OloEngine::EThreadPriority::TPri_AboveNormal;
            case EThreadPriority::TPri_BelowNormal:
                return OloEngine::EThreadPriority::TPri_BelowNormal;
            case EThreadPriority::TPri_Highest:
                return OloEngine::EThreadPriority::TPri_Highest;
            case EThreadPriority::TPri_Lowest:
                return OloEngine::EThreadPriority::TPri_Lowest;
            case EThreadPriority::TPri_SlightlyBelowNormal:
                return OloEngine::EThreadPriority::TPri_SlightlyBelowNormal;
            case EThreadPriority::TPri_TimeCritical:
                return OloEngine::EThreadPriority::TPri_TimeCritical;
            default:
                return OloEngine::EThreadPriority::TPri_Normal;
        }
    }

    std::unique_ptr<OloEngine::FThread> FScheduler::CreateWorker(u32 WorkerId, const char* Name, bool bPermitBackgroundWork,
                                                                 EForkable IsForkable,
                                                                 Private::FWaitEvent* ExternalWorkerEvent,
                                                                 FSchedulerTls::FLocalQueueType* ExternalWorkerLocalQueue,
                                                                 EThreadPriority Priority, u64 InAffinity)
    {
        const u32 WaitTimes[8] = { 719, 991, 1361, 1237, 1597, 953, 587, 1439 };
        u32 WaitTime = WaitTimes[WorkerId % 8];

        (void)IsForkable; // Fork behavior is platform-specific (used on Unix for fork() handling)

        // Capture thread name for use inside the thread lambda
        // Using a fixed-size buffer that can be copied into the lambda
        char ThreadName[64];
        if (Name)
        {
            strncpy(ThreadName, Name, sizeof(ThreadName) - 1);
            ThreadName[sizeof(ThreadName) - 1] = '\0';
        }
        else
        {
            snprintf(ThreadName, sizeof(ThreadName), "Worker #%u", WorkerId);
        }

        // Calculate processor group for systems with >64 cores
        // We offset WorkerId by 2 to skip the Game/Main thread and Render/RHI thread slots
        // that typically occupy the first cores
        const FProcessorGroupDesc& ProcessorGroups = FPlatformMisc::GetProcessorGroupDesc();
        u16 CpuGroup = 0;
        u64 GroupWorkerId = static_cast<u64>(WorkerId) + 2; // Offset for Game, RHI/Render threads
        u64 ThreadAffinityMask = InAffinity;

        for (u16 GroupIndex = 0; GroupIndex < ProcessorGroups.NumProcessorGroups; ++GroupIndex)
        {
            CpuGroup = GroupIndex;
            u32 CpusInGroup = FPlatformMisc::CountBits(ProcessorGroups.ThreadAffinities[GroupIndex]);
            if (GroupWorkerId < CpusInGroup)
            {
                // Worker belongs in this group
                // If we're in a non-primary group, use full affinity within that group
                if (CpuGroup != 0)
                {
                    ThreadAffinityMask = ~0ULL; // All cores in the group
                }
                break;
            }
            GroupWorkerId -= CpusInGroup;
        }

        // Compute final affinity mask combined with processor group
        u64 FinalAffinityMask = ThreadAffinityMask;
        if (ProcessorGroups.NumProcessorGroups > 1)
        {
            FinalAffinityMask &= ProcessorGroups.ThreadAffinities[CpuGroup];
        }

        // Create the FThread with all configuration
        auto ThreadPtr = std::make_unique<OloEngine::FThread>(
            ThreadName,
            [this, ExternalWorkerEvent, ExternalWorkerLocalQueue, WaitTime, bPermitBackgroundWork]()
            {
                WorkerMain(ExternalWorkerEvent, ExternalWorkerLocalQueue, WaitTime, bPermitBackgroundWork);
            },
            0, // StackSize (0 = default)
            ConvertToPlatformPriority(Priority),
            OloEngine::FThreadAffinity{ FinalAffinityMask, CpuGroup },
            IsForkable == EForkable::Forkable ? OloEngine::FThread::Forkable : OloEngine::FThread::NonForkable);

        return ThreadPtr;
    }

    void FScheduler::StartWorkers(u32 NumForegroundWorkers, u32 NumBackgroundWorkers,
                                  EForkable IsForkable,
                                  EThreadPriority InWorkerPriority, EThreadPriority InBackgroundPriority,
                                  u64 InWorkerAffinity, u64 InBackgroundAffinity)
    {
        OLO_PROFILE_FUNCTION();

        // Validation: Only the game thread should start workers (matches UE5.7 check(IsInGameThread()))
        OLO_CORE_ASSERT(IsInGameThread() || !g_GameThreadIdInitialized.load(std::memory_order_acquire),
                        "StartWorkers should only be called from the game thread");

        // Initialize configuration from command-line/environment variables (matches UE5.7 FParse::Value calls)
        InitializeTaskGraphConfiguration();

        if (NumForegroundWorkers == 0 && NumBackgroundWorkers == 0)
        {
            u32 TotalWorkers = NumberOfWorkerThreadsToSpawn();
            NumForegroundWorkers = std::max<u32>(1, std::min<u32>(2, TotalWorkers - 1));
            NumBackgroundWorkers = std::max<u32>(1, TotalWorkers - NumForegroundWorkers);
        }

        m_WorkerPriority = InWorkerPriority;
        m_BackgroundPriority = InBackgroundPriority;

        if (InWorkerAffinity)
        {
            m_WorkerAffinity = InWorkerAffinity;
        }
        if (InBackgroundAffinity)
        {
            m_BackgroundAffinity = InBackgroundAffinity;
        }

        // Check if multithreading is supported
        // UE5.7 logic: enable multithreading if platform supports it OR we're a forked multithread instance
        const bool bSupportsMultithreading = FPlatformProcess::SupportsMultithreading() || FForkProcessHelper::IsForkedMultithreadInstance();

        u32 OldActiveWorkers = m_ActiveWorkers.load(std::memory_order_relaxed);

        if (OldActiveWorkers == 0 && bSupportsMultithreading && m_ActiveWorkers.compare_exchange_strong(OldActiveWorkers, NumForegroundWorkers + NumBackgroundWorkers, std::memory_order_relaxed))
        {
            TUniqueLock<FRecursiveMutex> Lock(m_WorkerThreadsCS);

            OLO_CORE_ASSERT(!m_WorkerThreads, "WorkerThreads should be null");
            OLO_CORE_ASSERT(m_WorkerLocalQueues.IsEmpty(), "WorkerLocalQueues should be empty");
            OLO_CORE_ASSERT(m_WorkerEvents.IsEmpty(), "WorkerEvents should be empty");
            OLO_CORE_ASSERT(m_NextWorkerId == 0, "NextWorkerId should be 0");

            m_ForegroundCreationIndex = 0;
            m_BackgroundCreationIndex = 0;

            const float OversubscriptionRatio = std::max(1.0f, g_TaskGraphOversubscriptionRatio);
            const i32 MaxForegroundWorkers = static_cast<i32>(std::ceil(static_cast<float>(NumForegroundWorkers) * OversubscriptionRatio));
            const i32 MaxBackgroundWorkers = static_cast<i32>(std::ceil(static_cast<float>(NumBackgroundWorkers) * OversubscriptionRatio));
            const i32 MaxWorkers = MaxForegroundWorkers + MaxBackgroundWorkers;
            const EThreadPriority ActualBackgroundPriority = g_TaskGraphUseDynamicPrioritization ? m_WorkerPriority : m_BackgroundPriority;

            if (m_GameThreadLocalQueue == nullptr)
            {
                m_GameThreadLocalQueue = std::make_unique<FSchedulerTls::FLocalQueueType>(m_QueueRegistry, Private::ELocalQueueType::EForeground);
            }
            FSchedulerTls::GetTlsValuesRef().LocalQueue = m_GameThreadLocalQueue.get();

            m_WorkerEvents.SetNum(MaxWorkers);
            m_WorkerLocalQueues.SetNum(MaxWorkers);
            m_WorkerThreads.reset(new std::atomic<OloEngine::FThread*>[static_cast<sizet>(MaxWorkers)]());

            auto CreateThread = [this, IsForkable](Private::ELocalQueueType LocalQueueType, const char* Prefix,
                                                   std::atomic<i32>& CreationIndex, i32 NumWorkers, i32 NumMaxWorkers,
                                                   EThreadPriority Priority, u64 Affinity)
            {
                // Thread creation can end up waiting, we don't want to recursively oversubscribe if that happens.
                Private::FOversubscriptionAllowedScope _(false);

                const i32 LocalCreationIndex = CreationIndex++;
                OLO_CORE_ASSERT(LocalCreationIndex < NumMaxWorkers, "Creation index exceeds max workers");
                const bool bIsStandbyWorker = LocalCreationIndex >= NumWorkers;

                char WorkerName[64];
                if (bIsStandbyWorker)
                {
                    snprintf(WorkerName, sizeof(WorkerName), "%s Worker (Standby #%d)", Prefix, LocalCreationIndex - NumWorkers);
                }
                else
                {
                    snprintf(WorkerName, sizeof(WorkerName), "%s Worker #%d", Prefix, LocalCreationIndex);
                }

                u32 WorkerId = m_NextWorkerId++;
                m_WorkerLocalQueues[WorkerId].Init(m_QueueRegistry, LocalQueueType);
                m_WorkerEvents[WorkerId].bIsStandby = bIsStandbyWorker;
                m_WorkerThreads[WorkerId] = CreateWorker(
                                                WorkerId,
                                                WorkerName,
                                                LocalQueueType == Private::ELocalQueueType::EBackground,
                                                IsForkable,
                                                &m_WorkerEvents[WorkerId],
                                                &m_WorkerLocalQueues[WorkerId],
                                                Priority,
                                                Affinity)
                                                .release();
            };

            auto ForegroundCreateThreadFn = [this, CreateThread, NumWorkers = NumForegroundWorkers, MaxWorkers = MaxForegroundWorkers]()
            {
                OLO_PROFILE_SCOPE("CreateWorkerThread");
                CreateThread(Private::ELocalQueueType::EForeground, "Foreground", m_ForegroundCreationIndex, NumWorkers, MaxWorkers, m_WorkerPriority, m_WorkerAffinity);
            };

            auto BackgroundCreateThreadFn = [this, CreateThread, NumWorkers = NumBackgroundWorkers, MaxWorkers = MaxBackgroundWorkers, ActualBackgroundPriority]()
            {
                OLO_PROFILE_SCOPE("CreateWorkerThread");
                CreateThread(Private::ELocalQueueType::EBackground, "Background", m_BackgroundCreationIndex, NumWorkers, MaxWorkers, ActualBackgroundPriority, m_BackgroundAffinity);
            };

            // Initialize waiting queues FIRST (before thread creation) to prevent race conditions
            // where threads start running before the queues are ready to accept them.
            // This matches UE5.7's ordering.
            TFunction<void()> fgFunc(ForegroundCreateThreadFn);
            m_WaitingQueue[0].Init(NumForegroundWorkers, static_cast<u32>(MaxForegroundWorkers), MoveTemp(fgFunc),
                                   g_TaskGraphUseDynamicThreadCreation ? 0 : static_cast<u32>(MaxForegroundWorkers));
            m_WaitingQueue[1].Init(NumBackgroundWorkers, static_cast<u32>(MaxBackgroundWorkers), TFunction<void()>(BackgroundCreateThreadFn),
                                   g_TaskGraphUseDynamicThreadCreation ? 0 : static_cast<u32>(MaxBackgroundWorkers));

            // Precreate all the threads AFTER initializing waiting queues.
            // This prevents a race condition where threads start and try to use
            // uninitialized waiting queues.
            if (!g_TaskGraphUseDynamicThreadCreation)
            {
                for (i32 Index = 0; Index < MaxForegroundWorkers; Index++)
                {
                    ForegroundCreateThreadFn();
                }

                for (i32 Index = 0; Index < MaxBackgroundWorkers; Index++)
                {
                    BackgroundCreateThreadFn();
                }
            }

            if (g_TaskGraphUseDynamicThreadCreation && m_TemporaryShutdown)
            {
                // Since the global queue is not drained during temporary shutdown, kick threads
                // here so we can continue work if there was any tasks left when we stopped the workers.
                m_WaitingQueue[0].Notify();
                m_WaitingQueue[1].Notify();
            }
        }
    }

    bool FScheduler::IsOversubscriptionLimitReached(ETaskPriority TaskPriority) const
    {
        const bool bIsBackgroundTask = TaskPriority >= ETaskPriority::ForegroundCount;
        if (bIsBackgroundTask)
        {
            return m_WaitingQueue[1].IsOversubscriptionLimitReached();
        }
        else
        {
            // Since we are allowing background thread to run foreground task we need both waiting queue
            // to reach their limit to consider that priority's limit reached.
            return m_WaitingQueue[0].IsOversubscriptionLimitReached() && m_WaitingQueue[1].IsOversubscriptionLimitReached();
        }
    }

    FOversubscriptionLimitReached& FScheduler::GetOversubscriptionLimitReachedEvent()
    {
        return m_OversubscriptionLimitReachedEvent;
    }

    FTask* FScheduler::ExecuteTask(FTask* InTask)
    {
        OLO_PROFILE_SCOPE("Scheduler::ExecuteTask");

        FTask* ParentTask = FTask::s_ActiveTask;
        FTask::s_ActiveTask = InTask;
        FTask* OutTask;

        if (!InTask->IsBackgroundTask())
        {
            OutTask = InTask->ExecuteTask();
        }
        else
        {
            // Dynamic priority only enables for root task when we're not inside a named thread (i.e. GT, RT)
            const bool bSkipPriorityChange = ParentTask || !g_TaskGraphUseDynamicPrioritization || !FSchedulerTls::IsWorkerThread() || InTask->WasCanceledOrIsExpediting();

            OloEngine::FRunnableThread* RunnableThread = nullptr;
            if (!bSkipPriorityChange)
            {
                // Get the current thread via TLS for proper priority management
                RunnableThread = OloEngine::FRunnableThread::GetRunnableThread();
                if (RunnableThread)
                {
                    // Lower thread priority for background task execution
                    // This helps prevent background work from interfering with foreground responsiveness
                    RunnableThread->SetThreadPriority(ConvertToPlatformPriority(m_BackgroundPriority));
                }
            }

            OutTask = InTask->ExecuteTask();

            if (!bSkipPriorityChange && RunnableThread)
            {
                // Restore thread priority after background task execution
                RunnableThread->SetThreadPriority(ConvertToPlatformPriority(m_WorkerPriority));
            }
        }

        FTask::s_ActiveTask = ParentTask;
        return OutTask;
    }

    void FScheduler::StopWorkers(bool bDrainGlobalQueue)
    {
        OLO_PROFILE_FUNCTION();
        // Validation: Only the game thread should stop workers (matches UE5.7 check(IsInGameThread()))
        OLO_CORE_ASSERT(IsInGameThread() || !g_GameThreadIdInitialized.load(std::memory_order_acquire),
                        "StopWorkers should only be called from the game thread");

        u32 OldActiveWorkers = m_ActiveWorkers.load(std::memory_order_relaxed);
        if (OldActiveWorkers != 0 && m_ActiveWorkers.compare_exchange_strong(OldActiveWorkers, 0, std::memory_order_relaxed))
        {
            TUniqueLock<FRecursiveMutex> Lock(m_WorkerThreadsCS);

            m_WaitingQueue[0].StartShutdown();
            m_WaitingQueue[1].StartShutdown();

            // We wait on threads to exit, once we're done with that
            // it means no more threads can possibly get created.
            for (i32 i = 0; i < m_WorkerLocalQueues.Num(); ++i)
            {
                if (OloEngine::FThread* Thread = m_WorkerThreads[i].exchange(nullptr))
                {
                    if (Thread->IsJoinable())
                    {
                        Thread->Join();
                    }
                    delete Thread;
                }
            }

            m_WaitingQueue[0].FinishShutdown();
            m_WaitingQueue[1].FinishShutdown();

            m_GameThreadLocalQueue.reset();
            FSchedulerTls::GetTlsValuesRef().LocalQueue = nullptr;

            m_NextWorkerId = 0;
            m_WorkerThreads.reset();
            m_WorkerLocalQueues.Reset();
            for (i32 i = 0; i < m_WorkerEvents.Num(); ++i)
            {
            }
            m_WorkerEvents.Reset();

            if (bDrainGlobalQueue)
            {
                for (FTask* Task = m_QueueRegistry.DequeueGlobal(); Task != nullptr; Task = m_QueueRegistry.DequeueGlobal())
                {
                    while (Task)
                    {
                        if ((Task = ExecuteTask(Task)) != nullptr)
                        {
                            // Use OLO_CORE_VERIFY_SLOW to ensure TryPrepareLaunch() is always called
                            // (not optimized away in release builds)
                            OLO_CORE_VERIFY_SLOW(Task->TryPrepareLaunch(), "Task should be launchable");
                        }
                    }
                }
            }

            m_QueueRegistry.Reset();
        }
    }

    bool FSchedulerTls::HasPendingWakeUp() const
    {
        FUniqueLock ScopedLock(FImpl::s_ThreadTlsValuesMutex);
        FImpl::ProcessPendingTlsValuesNoLock();

#if PLATFORM_SUPPORTS_ASYMMETRIC_FENCES
        // Heavy barrier since bPendingWakeUp is only written to with a relaxed write,
        // we need all cores to flush their store buffer to memory
        // FPlatformMisc::AsymmetricThreadFenceHeavy(); // TODO: Implement for ARM platforms
        constexpr std::memory_order MemoryOrder = std::memory_order_relaxed;
#else
        constexpr std::memory_order MemoryOrder = std::memory_order_acquire;
#endif

        for (FTlsValues* It = FImpl::s_ThreadTlsValues; It != nullptr; It = It->Next())
        {
            if (It->ActiveScheduler != this && It->bPendingWakeUp.load(MemoryOrder))
            {
                return true;
            }
        }

        return false;
    }

    void FScheduler::RestartWorkers(u32 NumForegroundWorkers, u32 NumBackgroundWorkers,
                                    EForkable IsForkable,
                                    EThreadPriority InWorkerPriority, EThreadPriority InBackgroundPriority,
                                    u64 InWorkerAffinity, u64 InBackgroundAffinity)
    {
        TUniqueLock<FRecursiveMutex> Lock(m_WorkerThreadsCS);
        m_TemporaryShutdown.store(true, std::memory_order_release);
        while (HasPendingWakeUp())
        {
            FPlatformProcess::Yield();
        }
        const bool bDrainGlobalQueue = false;
        StopWorkers(bDrainGlobalQueue);
        StartWorkers(NumForegroundWorkers, NumBackgroundWorkers, IsForkable, InWorkerPriority, InBackgroundPriority, InWorkerAffinity, InBackgroundAffinity);
        m_TemporaryShutdown.store(false, std::memory_order_release);
    }

    void FScheduler::LaunchInternal(FTask& Task, EQueuePreference QueuePreference, bool bWakeUpWorker)
    {
        if (m_ActiveWorkers.load(std::memory_order_relaxed) || m_TemporaryShutdown.load(std::memory_order_acquire))
        {
            FTlsValues& LocalTlsValues = GetTlsValuesRef();

            const bool bIsBackgroundTask = Task.IsBackgroundTask();
            const bool bIsBackgroundWorker = LocalTlsValues.IsBackgroundWorker();
            const bool bIsStandbyWorker = LocalTlsValues.IsStandbyWorker();
            FSchedulerTls::FLocalQueueType* const CachedLocalQueue = LocalTlsValues.LocalQueue;

            // Standby workers always enqueue to the global queue and perform wakeup
            // as they can go to sleep whenever the oversubscription period is done
            // and we don't want that to happen without another thread picking up
            // this task.
            if ((bIsBackgroundTask && !bIsBackgroundWorker) || bIsStandbyWorker)
            {
                QueuePreference = EQueuePreference::GlobalQueuePreference;

                // Always wake up a worker if we're scheduling a background task from a foreground thread
                // since foreground threads are not allowed to process them.
                bWakeUpWorker = true;
            }
            else
            {
                bWakeUpWorker |= CachedLocalQueue == nullptr;
            }

            // Always force local queue usage when launching from the game thread to minimize cost.
            if (CachedLocalQueue && CachedLocalQueue == m_GameThreadLocalQueue.get())
            {
                QueuePreference = EQueuePreference::LocalQueuePreference;
                bWakeUpWorker = true; // The game thread is never pumping its local queue directly, need to always perform a wakeup.
            }

            if (CachedLocalQueue && QueuePreference != EQueuePreference::GlobalQueuePreference)
            {
                CachedLocalQueue->Enqueue(&Task, static_cast<u32>(Task.GetPriority()));
            }
            else
            {
                m_QueueRegistry.Enqueue(&Task, static_cast<u32>(Task.GetPriority()));
            }

            if (bWakeUpWorker)
            {
#if PLATFORM_SUPPORTS_ASYMMETRIC_FENCES
                constexpr std::memory_order MemoryOrder = std::memory_order_relaxed;
#else
                constexpr std::memory_order MemoryOrder = std::memory_order_seq_cst;
#endif
                // We don't need to pay this cost for worker threads because we already manage their shutdown gracefully.
                const bool bExternalThread = LocalTlsValues.ActiveScheduler != this || LocalTlsValues.WorkerType == EWorkerType::None;
                if (bExternalThread)
                {
#if PLATFORM_SUPPORTS_ASYMMETRIC_FENCES
                    // FPlatformMisc::AsymmetricThreadFenceLight(); // TODO: Implement for ARM platforms
#endif
                    LocalTlsValues.bPendingWakeUp.store(true, MemoryOrder);

#if PLATFORM_SUPPORTS_ASYMMETRIC_FENCES
                    // FPlatformMisc::AsymmetricThreadFenceLight(); // TODO: Implement for ARM platforms
#endif

                    if (m_TemporaryShutdown.load(std::memory_order_acquire))
                    {
                        LocalTlsValues.bPendingWakeUp.store(false, MemoryOrder);
                        return;
                    }
                }

                if (!WakeUpWorker(bIsBackgroundTask) && !bIsBackgroundTask)
                {
                    WakeUpWorker(true);
                }

                if (bExternalThread)
                {
#if PLATFORM_SUPPORTS_ASYMMETRIC_FENCES
                    // FPlatformMisc::AsymmetricThreadFenceLight(); // TODO: Implement for ARM platforms
#endif
                    LocalTlsValues.bPendingWakeUp.store(false, MemoryOrder);

#if PLATFORM_SUPPORTS_ASYMMETRIC_FENCES
                    // FPlatformMisc::AsymmetricThreadFenceLight(); // TODO: Implement for ARM platforms
#endif
                }
            }
        }
        else
        {
            FTask* TaskPtr = &Task;
            while (TaskPtr)
            {
                if ((TaskPtr = ExecuteTask(TaskPtr)) != nullptr)
                {
                    // Use OLO_CORE_VERIFY_SLOW to ensure TryPrepareLaunch() is always called
                    // (not optimized away in release builds)
                    OLO_CORE_VERIFY_SLOW(TaskPtr->TryPrepareLaunch(), "Task should be launchable");
                }
            }
        }
    }

    void FScheduler::IncrementOversubscription()
    {
        FSchedulerTls::EWorkerType LocalWorkerType = FSchedulerTls::GetTlsValuesRef().WorkerType;

        if (LocalWorkerType != EWorkerType::None)
        {
            const bool bPermitBackgroundWork = LocalWorkerType == FSchedulerTls::EWorkerType::Background;
            m_WaitingQueue[bPermitBackgroundWork].IncrementOversubscription();
        }
    }

    void FScheduler::DecrementOversubscription()
    {
        FSchedulerTls::EWorkerType LocalWorkerType = FSchedulerTls::GetTlsValuesRef().WorkerType;

        if (LocalWorkerType != EWorkerType::None)
        {
            const bool bPermitBackgroundWork = LocalWorkerType == FSchedulerTls::EWorkerType::Background;
            m_WaitingQueue[bPermitBackgroundWork].DecrementOversubscription();
        }
    }

    bool FSchedulerTls::IsWorkerThread() const
    {
        FTlsValues& LocalTlsValues = FSchedulerTls::GetTlsValuesRef();
        return LocalTlsValues.WorkerType != FSchedulerTls::EWorkerType::None && LocalTlsValues.ActiveScheduler == this;
    }

    template<typename QueueType, FTask* (QueueType::*DequeueFunction)(bool), bool bIsStandbyWorker>
    bool FScheduler::TryExecuteTaskFrom(Private::FWaitEvent* WaitEvent, QueueType* Queue, Private::FOutOfWork& OutOfWork, bool bPermitBackgroundWork)
    {
        bool AnyExecuted = false;

        FTask* Task = (Queue->*DequeueFunction)(bPermitBackgroundWork);
        while (Task)
        {
            OLO_CORE_ASSERT(FTask::s_ActiveTask == nullptr, "Active task should be null");

            if (OutOfWork.Stop())
            {
                // Standby workers don't need cancellation, this logic doesn't apply to them.
                if constexpr (bIsStandbyWorker == false)
                {
                    // CancelWait will tell us if we need to start a new worker to replace
                    // a potential wakeup we might have consumed during the cancellation.
                    if (m_WaitingQueue[bPermitBackgroundWork].CancelWait(WaitEvent))
                    {
                        if (!WakeUpWorker(bPermitBackgroundWork) && !GetTlsValuesRef().IsBackgroundWorker())
                        {
                            WakeUpWorker(!bPermitBackgroundWork);
                        }
                    }
                }
            }

            AnyExecuted = true;

            // Executing a task can return a continuation.
            if ((Task = ExecuteTask(Task)) != nullptr)
            {
                // Use OLO_CORE_VERIFY_SLOW to ensure TryPrepareLaunch() is always called
                // (not optimized away in release builds)
                OLO_CORE_VERIFY_SLOW(Task->TryPrepareLaunch(), "Task should be launchable");
            }
        }
        return AnyExecuted;
    }

    void FScheduler::StandbyLoop(Private::FWaitEvent* WorkerEvent, FSchedulerTls::FLocalQueueType* WorkerLocalQueue,
                                 [[maybe_unused]] u32 WaitCycles, bool bPermitBackgroundWork)
    {
        bool bPreparingStandby = false;
        Private::FOutOfWork OutOfWork;
        while (true)
        {
            bool bExecutedSomething = false;
            while (TryExecuteTaskFrom<FSchedulerTls::FLocalQueueType, &FSchedulerTls::FLocalQueueType::StealLocal, true>(WorkerEvent, m_GameThreadLocalQueue.get(), OutOfWork, bPermitBackgroundWork) || TryExecuteTaskFrom<FSchedulerTls::FLocalQueueType, &FSchedulerTls::FLocalQueueType::Dequeue, true>(WorkerEvent, WorkerLocalQueue, OutOfWork, bPermitBackgroundWork) || TryExecuteTaskFrom<FSchedulerTls::FLocalQueueType, &FSchedulerTls::FLocalQueueType::DequeueSteal, true>(WorkerEvent, WorkerLocalQueue, OutOfWork, bPermitBackgroundWork))
            {
                bPreparingStandby = false;
                bExecutedSomething = true;

                // If we're currently oversubscribed... we might be selected for standby even when there is work left.
                m_WaitingQueue[bPermitBackgroundWork].ConditionalStandby(WorkerEvent);
            }

            // Check if we're shutting down
            if (m_ActiveWorkers.load(std::memory_order_relaxed) == 0)
            {
                OutOfWork.Stop();
                break;
            }

            if (bExecutedSomething == false)
            {
                if (!bPreparingStandby)
                {
                    OutOfWork.Start();
                    m_WaitingQueue[bPermitBackgroundWork].PrepareStandby(WorkerEvent);
                    bPreparingStandby = true;
                }
                else if (m_WaitingQueue[bPermitBackgroundWork].CommitStandby(WorkerEvent, OutOfWork))
                {
                    // Only reset this when the commit succeeded, otherwise we're backing off the commit and looking at the queue again
                    bPreparingStandby = false;
                }
            }
        }
    }

    void FScheduler::WorkerLoop(Private::FWaitEvent* WorkerEvent, FSchedulerTls::FLocalQueueType* WorkerLocalQueue,
                                u32 WaitCycles, bool bPermitBackgroundWork)
    {
        bool bPreparingWait = false;
        Private::FOutOfWork OutOfWork;
        while (true)
        {
            bool bExecutedSomething = false;
            while (TryExecuteTaskFrom<FSchedulerTls::FLocalQueueType, &FSchedulerTls::FLocalQueueType::StealLocal, false>(WorkerEvent, m_GameThreadLocalQueue.get(), OutOfWork, bPermitBackgroundWork) || TryExecuteTaskFrom<FSchedulerTls::FLocalQueueType, &FSchedulerTls::FLocalQueueType::Dequeue, false>(WorkerEvent, WorkerLocalQueue, OutOfWork, bPermitBackgroundWork) || TryExecuteTaskFrom<FSchedulerTls::FLocalQueueType, &FSchedulerTls::FLocalQueueType::DequeueSteal, false>(WorkerEvent, WorkerLocalQueue, OutOfWork, bPermitBackgroundWork))
            {
                bPreparingWait = false;
                bExecutedSomething = true;
            }

            // Check if we're shutting down
            if (m_ActiveWorkers.load(std::memory_order_relaxed) == 0)
            {
                // Don't leave the waiting queue in a bad state
                if (OutOfWork.Stop())
                {
                    m_WaitingQueue[bPermitBackgroundWork].CancelWait(WorkerEvent);
                }
                break;
            }

            if (bExecutedSomething == false)
            {
                if (!bPreparingWait)
                {
                    OutOfWork.Start();
                    m_WaitingQueue[bPermitBackgroundWork].PrepareWait(WorkerEvent);
                    bPreparingWait = true;
                }
                else if (m_WaitingQueue[bPermitBackgroundWork].CommitWait(WorkerEvent, OutOfWork, WorkerSpinCycles, WaitCycles))
                {
                    // Only reset this when the commit succeeded, otherwise we're backing off the commit and looking at the queue again
                    bPreparingWait = false;
                }
            }
        }
    }

    void FScheduler::WorkerMain(Private::FWaitEvent* WorkerEvent, FSchedulerTls::FLocalQueueType* WorkerLocalQueue,
                                u32 WaitCycles, bool bPermitBackgroundWork)
    {

        OLO_PROFILE_FUNCTION();

        FTlsValues& LocalTlsValues = GetTlsValuesRef();

        OLO_CORE_ASSERT(LocalTlsValues.LocalQueue == nullptr, "LocalQueue should be null");
        OLO_CORE_ASSERT(WorkerLocalQueue != nullptr, "WorkerLocalQueue should not be null");
        OLO_CORE_ASSERT(WorkerEvent != nullptr, "WorkerEvent should not be null");

        // Clear the EStaticInit tag that new threads inherit from their thread_local default
        // This allows worker threads to be tagged with EWorkerThread
        OLO::FTaskTagScope::SetTagNone();

        // Mark this thread as a worker thread (matches UE5.7's FTaskTagScope)
        OLO::FTaskTagScope WorkerScope(OLO::ETaskTag::EWorkerThread);
        LocalTlsValues.ActiveScheduler = this;

        // Setup TLS caches for this thread so the memory allocator can use thread-local pools
        FMemory::SetupTLSCachesOnCurrentThread();

        LocalTlsValues.WorkerType = bPermitBackgroundWork ? FSchedulerTls::EWorkerType::Background : FSchedulerTls::EWorkerType::Foreground;
        LocalTlsValues.SetStandbyWorker(WorkerEvent->bIsStandby);
        LocalTlsValues.LocalQueue = WorkerLocalQueue;

        {
            Private::FOversubscriptionAllowedScope _(true);

            if (WorkerEvent->bIsStandby)
            {
                StandbyLoop(WorkerEvent, WorkerLocalQueue, WaitCycles, bPermitBackgroundWork);
            }
            else
            {
                WorkerLoop(WorkerEvent, WorkerLocalQueue, WaitCycles, bPermitBackgroundWork);
            }
        }

        LocalTlsValues.LocalQueue = nullptr;
        LocalTlsValues.ActiveScheduler = nullptr;
        LocalTlsValues.SetStandbyWorker(false);
        LocalTlsValues.WorkerType = FSchedulerTls::EWorkerType::None;

        // Clear TLS caches on thread exit to free memory back to the allocator
        FMemory::ClearAndDisableTLSCachesOnCurrentThread();
    }

    // Explicit template instantiations
    template bool FScheduler::TryExecuteTaskFrom<FSchedulerTls::FLocalQueueType, &FSchedulerTls::FLocalQueueType::StealLocal, true>(
        Private::FWaitEvent*, FSchedulerTls::FLocalQueueType*, Private::FOutOfWork&, bool);
    template bool FScheduler::TryExecuteTaskFrom<FSchedulerTls::FLocalQueueType, &FSchedulerTls::FLocalQueueType::Dequeue, true>(
        Private::FWaitEvent*, FSchedulerTls::FLocalQueueType*, Private::FOutOfWork&, bool);
    template bool FScheduler::TryExecuteTaskFrom<FSchedulerTls::FLocalQueueType, &FSchedulerTls::FLocalQueueType::DequeueSteal, true>(
        Private::FWaitEvent*, FSchedulerTls::FLocalQueueType*, Private::FOutOfWork&, bool);
    template bool FScheduler::TryExecuteTaskFrom<FSchedulerTls::FLocalQueueType, &FSchedulerTls::FLocalQueueType::StealLocal, false>(
        Private::FWaitEvent*, FSchedulerTls::FLocalQueueType*, Private::FOutOfWork&, bool);
    template bool FScheduler::TryExecuteTaskFrom<FSchedulerTls::FLocalQueueType, &FSchedulerTls::FLocalQueueType::Dequeue, false>(
        Private::FWaitEvent*, FSchedulerTls::FLocalQueueType*, Private::FOutOfWork&, bool);
    template bool FScheduler::TryExecuteTaskFrom<FSchedulerTls::FLocalQueueType, &FSchedulerTls::FLocalQueueType::DequeueSteal, false>(
        Private::FWaitEvent*, FSchedulerTls::FLocalQueueType*, Private::FOutOfWork&, bool);

} // namespace OloEngine::LowLevelTasks
