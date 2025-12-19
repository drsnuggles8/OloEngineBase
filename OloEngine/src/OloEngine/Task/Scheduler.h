// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/TaskTag.h"
#include "OloEngine/Containers/IntrusiveLinkedList.h"
#include "OloEngine/HAL/Thread.h"
#include "OloEngine/Task/Oversubscription.h"
#include "OloEngine/Task/LowLevelTask.h"
#include "OloEngine/Task/TaskShared.h"
#include "OloEngine/Task/TaskDelegate.h"
#include "OloEngine/Task/WaitingQueue.h"
#include "OloEngine/Task/LocalQueue.h"
#include "OloEngine/Threading/RecursiveMutex.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <atomic>
#include <memory>

namespace OloEngine::LowLevelTasks
{
    // @enum EQueuePreference
    // @brief Preference for which queue to use when launching tasks
    enum class EQueuePreference
    {
        GlobalQueuePreference,
        LocalQueuePreference,
        DefaultPreference = LocalQueuePreference,
    };

    // @enum EThreadPriority
    // @brief Thread priority levels for worker threads
    enum class EThreadPriority
    {
        TPri_Normal,
        TPri_AboveNormal,
        TPri_BelowNormal,
        TPri_Highest,
        TPri_Lowest,
        TPri_SlightlyBelowNormal,
        TPri_TimeCritical,
        TPri_Num,
    };

    // @enum EForkable
    // @brief Controls fork behavior for worker threads
    enum class EForkable
    {
        NonForkable,
        Forkable,
    };

    // @brief Check if current thread is the game/main thread.
    // Matching UE5.7's IsInGameThread() validation.
    // @return true if on the game thread
    bool IsInGameThread();

    // @brief Initialize the game thread ID. Call from main() before starting workers.
    void InitGameThreadId();

    // @class FSchedulerTls
    // @brief Thread-local storage management for the scheduler
    class FSchedulerTls
    {
    protected:
        class FImpl;

        using FQueueRegistry  = Private::TLocalQueueRegistry<>;
        using FLocalQueueType = FQueueRegistry::TLocalQueue;

        enum class EWorkerType
        {
            None,
            Background,
            Foreground,
        };

        // @struct FTlsValues
        // @brief Per-thread values stored in TLS for scheduler state
        struct FTlsValues : public TIntrusiveLinkedList<FTlsValues>
        {
            FSchedulerTls*         ActiveScheduler = nullptr;
            FLocalQueueType*       LocalQueue = nullptr;
            EWorkerType            WorkerType = EWorkerType::None;
            std::atomic<bool>      bPendingWakeUp = false;
            bool                   bIsStandbyWorker = false;

            OLO_FINLINE bool IsBackgroundWorker() const
            {
                return WorkerType == EWorkerType::Background;
            }

            OLO_FINLINE bool IsStandbyWorker() const
            {
                return bIsStandbyWorker;
            }

            OLO_FINLINE void SetStandbyWorker(bool bInIsStandbyWorker)
            {
                bIsStandbyWorker = bInIsStandbyWorker;
            }

            // Custom allocator for early thread creation safety
            static void* operator new(sizet Size);
            static void operator delete(void* Ptr);
        };

        // @struct FTlsValuesHolder
        // @brief RAII holder for FTlsValues in thread-local storage
        struct FTlsValuesHolder
        {
            FTlsValuesHolder();
            ~FTlsValuesHolder();

            FTlsValues* TlsValues = nullptr;
        };

    private:
        static thread_local FTlsValuesHolder s_TlsValuesHolder;

    public:
        bool IsWorkerThread() const;

    protected:
        bool HasPendingWakeUp() const;

        static FTlsValues& GetTlsValuesRef();
    };

    // @class FScheduler
    // @brief Main task scheduler for managing worker threads and task execution
    // 
    // The scheduler manages a pool of worker threads that execute tasks from
    // work-stealing queues. It supports both foreground and background workers
    // with different priorities and oversubscription for blocking operations.
    class FScheduler final : public FSchedulerTls
    {
    public:
        FScheduler(const FScheduler&) = delete;
        FScheduler& operator=(const FScheduler&) = delete;

    private:
        static constexpr u32 WorkerSpinCycles = 53;

        static FScheduler s_Singleton;

        // Using 16 bytes here because it fits the vtable and one additional pointer
        using FConditional = TTaskDelegate<bool(), 16>;

    public: // Public Interface of the Scheduler
        OLO_FINLINE static FScheduler& Get();

        // @brief Start worker threads
        // @param NumForegroundWorkers Number of foreground workers (0 = system default)
        // @param NumBackgroundWorkers Number of background workers (0 = system default)
        // @param IsForkable Fork behavior for worker threads (NonForkable by default)
        // @param InWorkerPriority Thread priority for foreground workers
        // @param InBackgroundPriority Thread priority for background workers
        // @param InWorkerAffinity CPU affinity mask for foreground workers
        // @param InBackgroundAffinity CPU affinity mask for background workers
        void StartWorkers(u32 NumForegroundWorkers = 0, u32 NumBackgroundWorkers = 0, 
                          EForkable IsForkable = EForkable::NonForkable,
                          EThreadPriority InWorkerPriority = EThreadPriority::TPri_Normal, 
                          EThreadPriority InBackgroundPriority = EThreadPriority::TPri_BelowNormal, 
                          u64 InWorkerAffinity = 0, u64 InBackgroundAffinity = 0);

        // @brief Stop all worker threads
        // @param DrainGlobalQueue If true, execute remaining tasks before stopping
        void StopWorkers(bool DrainGlobalQueue = true);

        // @brief Restart workers with new configuration
        void RestartWorkers(u32 NumForegroundWorkers = 0, u32 NumBackgroundWorkers = 0, 
                           EForkable IsForkable = EForkable::NonForkable,
                           EThreadPriority WorkerPriority = EThreadPriority::TPri_Normal, 
                           EThreadPriority BackgroundPriority = EThreadPriority::TPri_BelowNormal, 
                           u64 InWorkerAffinity = 0, u64 InBackgroundAffinity = 0);

        // @brief Try to launch a task
        // @param Task The task to launch
        // @param QueuePreference Which queue to prefer for the task
        // @param bWakeUpWorker Whether to wake up a worker thread
        // @return true if the task was in the ready state and has been launched
        OLO_FINLINE bool TryLaunch(FTask& Task, EQueuePreference QueuePreference = EQueuePreference::DefaultPreference, bool bWakeUpWorker = true);

        // @brief Get number of active workers
        OLO_FINLINE u32 GetNumWorkers() const;

        // @brief Get maximum number of workers including standby workers
        OLO_FINLINE u32 GetMaxNumWorkers() const;

        // @brief Get foreground worker thread priority
        OLO_FINLINE EThreadPriority GetWorkerPriority() const { return m_WorkerPriority; }

        // @brief Get background worker thread priority
        OLO_FINLINE EThreadPriority GetBackgroundPriority() const { return m_BackgroundPriority; }

        // @brief Check if we're out of workers for a given task priority
        bool IsOversubscriptionLimitReached(ETaskPriority TaskPriority) const;

        // @brief Get the event that fires when oversubscription limit is reached
        // @note This event can be broadcasted from any thread so the receiver needs to be thread-safe
        FOversubscriptionLimitReached& GetOversubscriptionLimitReachedEvent();

    public:
        FScheduler() = default;
        ~FScheduler();

    private:
        [[nodiscard]] FTask* ExecuteTask(FTask* InTask);

        std::unique_ptr<OloEngine::FThread> CreateWorker(u32 WorkerId, const char* Name, bool bPermitBackgroundWork = false, 
                                                   EForkable IsForkable = EForkable::NonForkable,
                                                   Private::FWaitEvent* ExternalWorkerEvent = nullptr, 
                                                   FSchedulerTls::FLocalQueueType* ExternalWorkerLocalQueue = nullptr, 
                                                   EThreadPriority Priority = EThreadPriority::TPri_Normal, u64 InAffinity = 0);

        void WorkerMain(Private::FWaitEvent* WorkerEvent, FSchedulerTls::FLocalQueueType* ExternalWorkerLocalQueue, 
                       u32 WaitCycles, bool bPermitBackgroundWork);

        void StandbyLoop(Private::FWaitEvent* WorkerEvent, FSchedulerTls::FLocalQueueType* ExternalWorkerLocalQueue, 
                        u32 WaitCycles, bool bPermitBackgroundWork);

        void WorkerLoop(Private::FWaitEvent* WorkerEvent, FSchedulerTls::FLocalQueueType* ExternalWorkerLocalQueue, 
                       u32 WaitCycles, bool bPermitBackgroundWork);

        void LaunchInternal(FTask& Task, EQueuePreference QueuePreference, bool bWakeUpWorker);

        OLO_FINLINE bool WakeUpWorker(bool bBackgroundWorker);

        void IncrementOversubscription();
        void DecrementOversubscription();

        template<typename QueueType, FTask* (QueueType::*DequeueFunction)(bool), bool bIsStandbyWorker>
        bool TryExecuteTaskFrom(Private::FWaitEvent* WaitEvent, QueueType* Queue, Private::FOutOfWork& OutOfWork, bool bPermitBackgroundWork);

        friend class FOversubscriptionScope;

    private:
        // NOTE: Member ordering matters! m_WorkerEvents and m_OversubscriptionLimitReachedEvent
        // MUST be declared before m_WaitingQueue because m_WaitingQueue takes references to them
        // in its initializer. C++ initializes members in declaration order, not initializer order.
        TAlignedArray<Private::FWaitEvent>                        m_WorkerEvents;
        FOversubscriptionLimitReached                             m_OversubscriptionLimitReachedEvent;
        Private::FWaitingQueue                                    m_WaitingQueue[2] = { { m_WorkerEvents, m_OversubscriptionLimitReachedEvent }, { m_WorkerEvents, m_OversubscriptionLimitReachedEvent } };
        FSchedulerTls::FQueueRegistry                             m_QueueRegistry;
        FRecursiveMutex                                           m_WorkerThreadsCS;
        std::unique_ptr<std::atomic<OloEngine::FThread*>[]>      m_WorkerThreads;
        TAlignedArray<FSchedulerTls::FLocalQueueType>             m_WorkerLocalQueues;
        std::unique_ptr<FSchedulerTls::FLocalQueueType>           m_GameThreadLocalQueue;
        std::atomic_uint                                          m_ActiveWorkers { 0 };
        std::atomic_uint                                          m_NextWorkerId { 0 };
        std::atomic<i32>                                          m_ForegroundCreationIndex{ 0 };
        std::atomic<i32>                                          m_BackgroundCreationIndex{ 0 };
        u64                                                       m_WorkerAffinity = 0;
        u64                                                       m_BackgroundAffinity = 0;
        EThreadPriority                                           m_WorkerPriority = EThreadPriority::TPri_Normal;
        EThreadPriority                                           m_BackgroundPriority = EThreadPriority::TPri_BelowNormal;
        std::atomic_bool                                          m_TemporaryShutdown{ false };
    };

    // @brief Free function to launch a task on the default scheduler
    OLO_FINLINE bool TryLaunch(FTask& Task, EQueuePreference QueuePreference = EQueuePreference::DefaultPreference, bool bWakeUpWorker = true)
    {
        return FScheduler::Get().TryLaunch(Task, QueuePreference, bWakeUpWorker);
    }

    // ******************
    // * IMPLEMENTATION *
    // ******************
    OLO_FINLINE bool FScheduler::TryLaunch(FTask& Task, EQueuePreference QueuePreference, bool bWakeUpWorker)
    {
        if (Task.TryPrepareLaunch())
        {
            LaunchInternal(Task, QueuePreference, bWakeUpWorker);
            return true;
        }
        return false;
    }

    OLO_FINLINE u32 FScheduler::GetNumWorkers() const
    {
        return m_ActiveWorkers.load(std::memory_order_relaxed);
    }

    // Return the maximum number of worker threads, including Standby Workers
    OLO_FINLINE u32 FScheduler::GetMaxNumWorkers() const
    {
        return static_cast<u32>(m_WorkerLocalQueues.Num());
    }

    OLO_FINLINE bool FScheduler::WakeUpWorker(bool bBackgroundWorker)
    {
        return m_WaitingQueue[bBackgroundWorker].Notify() != 0;
    }

    OLO_FINLINE FScheduler& FScheduler::Get()
    {
        return s_Singleton;
    }

    OLO_FINLINE FScheduler::~FScheduler()
    {
        StopWorkers();
    }

} // namespace OloEngine::LowLevelTasks
