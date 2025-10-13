#include "OloEnginePCH.h"
#include "TaskScheduler.h"

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
        s_Instance->m_IsInitialized = true;

        // TODO Phase 3: Create worker thread pools
    }

    void TaskScheduler::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        
        if (!s_Instance)
            return;

        // TODO Phase 3: Stop worker threads and wait for completion

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
        
        // Phase 1: Stub implementation - just transition to Scheduled state
        // Full implementation in Phase 3 will queue the task and wake workers
        
        ETaskState expected = ETaskState::Ready;
        if (!task->TryTransitionState(expected, ETaskState::Scheduled))
        {
            return;
        }

        // TODO Phase 3: Queue task and wake worker
        // For now, immediately transition to Completed to prevent tasks from being stuck
        task->SetState(ETaskState::Completed);
    }

} // namespace OloEngine
