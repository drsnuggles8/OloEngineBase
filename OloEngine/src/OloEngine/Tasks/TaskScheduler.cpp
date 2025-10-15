#include "OloEnginePCH.h"
#include "TaskScheduler.h"
#include "WorkerThread.h"

#include <thread>

namespace OloEngine
{
    // Static singleton instance
    TaskScheduler* TaskScheduler::s_Instance = nullptr;

    void TaskSchedulerConfig::AutoDetectWorkerCounts()
    {
        u32 numCores = std::thread::hardware_concurrency();
        
        // Fallback if hardware_concurrency returns 0
        if (numCores == 0)
            numCores = 4;

        // Foreground workers: Leave 2 cores for main/render threads
        // Minimum of 1 worker
        NumForegroundWorkers = numCores >= 3 ? (numCores - 2) : 1;

        // Background workers: ~25% of cores, minimum 1
        NumBackgroundWorkers = std::max<u32>(1, numCores / 4);
    }

    void TaskScheduler::Initialize(const TaskSchedulerConfig& config)
    {
        OLO_PROFILE_FUNCTION();
        
        OLO_CORE_ASSERT(!s_Instance, "TaskScheduler already initialized!");

        s_Instance = new TaskScheduler();
        
        // Auto-detect worker counts if not specified
        TaskSchedulerConfig finalConfig = config;
        if (finalConfig.NumForegroundWorkers == 0 || finalConfig.NumBackgroundWorkers == 0)
        {
            finalConfig.AutoDetectWorkerCounts();
        }

        s_Instance->m_NumForegroundWorkers = finalConfig.NumForegroundWorkers;
        s_Instance->m_NumBackgroundWorkers = finalConfig.NumBackgroundWorkers;

        // Create global work queues with configured size BEFORE starting workers
        s_Instance->m_HighPriorityQueue = std::make_unique<GlobalWorkQueue>(finalConfig.MaxQueueNodes);
        s_Instance->m_NormalPriorityQueue = std::make_unique<GlobalWorkQueue>(finalConfig.MaxQueueNodes);
        s_Instance->m_BackgroundPriorityQueue = std::make_unique<GlobalWorkQueue>(finalConfig.MaxQueueNodes);

        // Mark as initialized AFTER queues are created but BEFORE workers start
        s_Instance->m_IsInitialized = true;

        // Create foreground worker pool (threads will start immediately)
        // Note: No logging here - Log system may not be initialized in test contexts
        s_Instance->m_ForegroundWorkers.reserve(finalConfig.NumForegroundWorkers);
        for (u32 i = 0; i < finalConfig.NumForegroundWorkers; ++i)
        {
            s_Instance->m_ForegroundWorkers.push_back(
                std::make_unique<WorkerThread>(s_Instance, i, EWorkerType::Foreground));
        }

        // Create background worker pool
        s_Instance->m_BackgroundWorkers.reserve(finalConfig.NumBackgroundWorkers);
        for (u32 i = 0; i < finalConfig.NumBackgroundWorkers; ++i)
        {
            s_Instance->m_BackgroundWorkers.push_back(
                std::make_unique<WorkerThread>(s_Instance, i, EWorkerType::Background));
        }
    }

    void TaskScheduler::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        
        if (!s_Instance)
            return;

        // Phase 1: Request all workers to stop
        for (auto& worker : s_Instance->m_ForegroundWorkers)
        {
            worker->RequestStop();
        }
        for (auto& worker : s_Instance->m_BackgroundWorkers)
        {
            worker->RequestStop();
        }

        // Memory fence to ensure exit flags are visible to all threads
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Phase 2: Join all workers (destructor wakes again and joins)
        // The WorkerThread destructor will wake the thread again before joining,
        // ensuring we don't miss the wake signal due to timing
        s_Instance->m_ForegroundWorkers.clear();
        s_Instance->m_BackgroundWorkers.clear();

        delete s_Instance;
        s_Instance = nullptr;
    }

    TaskScheduler& TaskScheduler::Get()
    {
        OLO_CORE_ASSERT(s_Instance, "TaskScheduler not initialized! Call TaskScheduler::Initialize() first.");
        return *s_Instance;
    }

    bool TaskScheduler::IsInitialized()
    {
        if (!s_Instance)
            return false;
        return s_Instance->m_IsInitialized;
    }

    void TaskScheduler::LaunchTask(Ref<Task> task)
    {
        OLO_PROFILE_FUNCTION();
        
        // Increment launched counter
        m_TotalTasksLaunched.fetch_add(1, std::memory_order_relaxed);
        
        // Transition from Ready to Scheduled
        ETaskState expected = ETaskState::Ready;
        if (!task->TryTransitionState(expected, ETaskState::Scheduled))
        {
            // Task is not in Ready state - cannot launch (fail silently)
            return;
        }

        // TODO Phase 3+: Try local queue first if we're on a worker thread
        // For now, always use global queue

        // Queue the task in the appropriate global queue
        ETaskPriority priority = task->GetPriority();
        bool queued = GetGlobalQueue(priority).Push(task);
        
        if (!queued)
        {
            // Queue full - mark as completed to prevent task from being stuck
            task->SetState(ETaskState::Completed);
            return;
        }

        // Wake a worker to process this task
        WakeWorker(priority);
    }

    WorkerThread* TaskScheduler::GetWorker(EWorkerType workerType, u32 workerIndex)
    {
        if (workerType == EWorkerType::Foreground)
        {
            if (workerIndex >= m_ForegroundWorkers.size())
                return nullptr;
            return m_ForegroundWorkers[workerIndex].get();
        }
        else
        {
            if (workerIndex >= m_BackgroundWorkers.size())
                return nullptr;
            return m_BackgroundWorkers[workerIndex].get();
        }
    }

    GlobalWorkQueue& TaskScheduler::GetGlobalQueue(ETaskPriority priority)
    {
        switch (priority)
        {
        case ETaskPriority::High:
            return *m_HighPriorityQueue;
        case ETaskPriority::Normal:
            return *m_NormalPriorityQueue;
        case ETaskPriority::Background:
            return *m_BackgroundPriorityQueue;
        default:
            OLO_CORE_ASSERT(false, "Invalid task priority");
            return *m_NormalPriorityQueue;
        }
    }

    void TaskScheduler::WakeWorker(ETaskPriority priority)
    {
        // Determine which worker pool to wake
        if (priority == ETaskPriority::Background)
        {
            // Wake a background worker
            if (m_BackgroundWorkers.empty())
                return;

            u32 wakeIndex = m_NextWakeIndexBackground.fetch_add(1, std::memory_order_relaxed) 
                % m_BackgroundWorkers.size();
            m_BackgroundWorkers[wakeIndex]->Wake();
        }
        else
        {
            // Wake a foreground worker (for High or Normal priority)
            if (m_ForegroundWorkers.empty())
                return;

            u32 wakeIndex = m_NextWakeIndexForeground.fetch_add(1, std::memory_order_relaxed) 
                % m_ForegroundWorkers.size();
            m_ForegroundWorkers[wakeIndex]->Wake();
        }
    }

    void TaskScheduler::IncrementOversubscription()
    {
        u32 level = m_OversubscriptionLevel.fetch_add(1, std::memory_order_relaxed) + 1;
        
        // If we have too many workers blocked and haven't reached standby limit, spawn a standby worker
        // We only spawn standby workers if more than 50% of permanent workers are blocked
        u32 totalWorkers = m_NumForegroundWorkers + m_NumBackgroundWorkers;
        u32 threshold = totalWorkers / 2;
        
        if (level > threshold)
        {
            u32 currentStandby = m_StandbyWorkerCount.load(std::memory_order_relaxed);
            if (currentStandby < s_MaxStandbyWorkers)
            {
                // Try to increment standby count
                if (m_StandbyWorkerCount.compare_exchange_strong(currentStandby, currentStandby + 1,
                                                                  std::memory_order_relaxed))
                {
                    // Successfully claimed a standby slot - spawn a temporary foreground worker
                    // Note: Standby workers are not tracked in the m_ForegroundWorkers vector
                    // They will self-destruct after idle timeout
                    auto standbyWorker = std::make_unique<WorkerThread>(
                        this, 
                        m_NumForegroundWorkers + currentStandby,  // Unique index
                        EWorkerType::Foreground,
                        true  // isStandby flag
                    );
                    
                    // Detach the thread - it will manage its own lifetime
                    standbyWorker->DetachAndRun();
                    
                    // Worker object will delete itself when done
                    standbyWorker.release();
                }
            }
        }
    }

    void TaskScheduler::DecrementOversubscription()
    {
        m_OversubscriptionLevel.fetch_sub(1, std::memory_order_relaxed);
    }

    TaskSchedulerStatistics TaskScheduler::GetStatistics() const
    {
        TaskSchedulerStatistics stats;
        
        // Task counts
        stats.TotalTasksLaunched = m_TotalTasksLaunched.load(std::memory_order_relaxed);
        stats.TotalTasksCompleted = m_TotalTasksCompleted.load(std::memory_order_relaxed);
        stats.TotalTasksCancelled = m_TotalTasksCancelled.load(std::memory_order_relaxed);
        
        // Queue depths
        stats.HighPriorityQueueDepth = m_HighPriorityQueue->ApproximateSize();
        stats.NormalPriorityQueueDepth = m_NormalPriorityQueue->ApproximateSize();
        stats.BackgroundPriorityQueueDepth = m_BackgroundPriorityQueue->ApproximateSize();
        
        // Worker statistics
        stats.NumForegroundWorkers = m_NumForegroundWorkers;
        stats.NumBackgroundWorkers = m_NumBackgroundWorkers;
        stats.NumStandbyWorkers = m_StandbyWorkerCount.load(std::memory_order_relaxed);
        stats.OversubscriptionLevel = m_OversubscriptionLevel.load(std::memory_order_relaxed);
        
        return stats;
    }

} // namespace OloEngine
