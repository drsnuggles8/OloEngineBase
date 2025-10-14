#include "OloEnginePCH.h"
#include "WorkerThread.h"
#include "TaskScheduler.h"
#include "GlobalWorkQueue.h"

#include <chrono>
#include <random>

#ifdef _MSC_VER
#include <intrin.h>  // For _mm_pause
#else
#include <emmintrin.h>  // For _mm_pause on other compilers
#endif

namespace OloEngine
{
    WorkerThread::WorkerThread(TaskScheduler* scheduler, u32 workerIndex, EWorkerType workerType)
        : m_Scheduler(scheduler)
        , m_Thread(std::string("Worker_") + std::to_string(workerIndex))
        , m_WakeEvent(std::string("WorkerWake_") + std::to_string(workerIndex), false)
        , m_ShouldExit(false)
        , m_WorkerIndex(workerIndex)
        , m_WorkerType(workerType)
    {
        OLO_PROFILE_FUNCTION();
        
        // Initialize random engine with a unique seed per worker
        m_RandomEngine.seed(static_cast<u32>(
            std::chrono::steady_clock::now().time_since_epoch().count() + workerIndex));

        // Start the worker thread
        m_Thread.Dispatch([this]() { WorkerMain(); });
    }

    WorkerThread::~WorkerThread()
    {
        OLO_PROFILE_FUNCTION();
        
        // Ensure exit flag is set
        m_ShouldExit.store(true, std::memory_order_release);
        
        // Wake the thread multiple times to ensure it sees the exit flag
        // This handles race conditions where the thread might miss the first wake signal
        // because it was between checking the flag and calling Wait()
        for (int i = 0; i < 3; ++i)
        {
            Wake();
            std::this_thread::yield();
        }
        
        // Wait for thread to finish
        Join();
    }

    void WorkerThread::RequestStop()
    {
        m_ShouldExit.store(true, std::memory_order_release);
        Wake();  // Wake the thread so it can see the exit flag
    }

    void WorkerThread::Join()
    {
        m_Thread.Join();
    }

    void WorkerThread::Wake()
    {
        m_WakeEvent.Signal();
    }

    void WorkerThread::WorkerMain()
    {
        OLO_PROFILE_FUNCTION();
        
        // Note: No logging - Log system may not be initialized

        while (!m_ShouldExit.load(std::memory_order_acquire))
        {
            Ref<Task> task = FindWork();
            
            if (task)
            {
                ExecuteTask(task);
            }
            else
            {
                // No work found - wait for more
                WaitForWork();
            }
        }
    }

    Ref<Task> WorkerThread::FindWork()
    {
        OLO_PROFILE_FUNCTION();
        
        // Strategy 1: Check local queue first (best cache locality)
        Ref<Task> task = m_LocalQueue.Pop();
        if (task)
        {
            return task;
        }

        // Strategy 2: Check global queue for appropriate priority levels
        // Foreground workers can take High and Normal priority tasks
        // Background workers can only take Background priority tasks
        if (m_WorkerType == EWorkerType::Foreground)
        {
            // Try high priority first
            task = m_Scheduler->GetGlobalQueue(ETaskPriority::High).Pop();
            if (task)
                return task;

            // Then normal priority
            task = m_Scheduler->GetGlobalQueue(ETaskPriority::Normal).Pop();
            if (task)
                return task;
        }
        else  // Background worker
        {
            task = m_Scheduler->GetGlobalQueue(ETaskPriority::Background).Pop();
            if (task)
                return task;
        }

        // Strategy 3: Try stealing from other workers
        task = StealFromOtherWorkers();
        if (task)
        {
            return task;
        }

        // No work found
        return nullptr;
    }

    void WorkerThread::ExecuteTask(Ref<Task> task)
    {
        OLO_PROFILE_FUNCTION();
        
        OLO_CORE_ASSERT(task, "Cannot execute null task");

        // Check if we're shutting down before starting task execution
        if (m_ShouldExit.load(std::memory_order_acquire))
        {
            // Shutdown requested - abandon task execution
            // Task will remain in Scheduled state
            return;
        }

        // Transition from Scheduled to Running
        ETaskState expected = ETaskState::Scheduled;
        if (!task->TryTransitionState(expected, ETaskState::Running))
        {
            // State transition failed - task might have been retracted or is in wrong state
            // Fail silently (no logging)
            return;
        }

        // Execute the task with exception handling
        try
        {
            task->Execute();
        }
        catch (const std::exception& e)
        {
            // Exception caught - task will still be marked as completed
            // No logging (Log system may not be initialized)
            (void)e;  // Suppress unused variable warning
        }
        catch (...)
        {
            // Unknown exception - task will still be marked as completed
        }

        // Transition to Completed (even if exception was thrown)
        task->SetState(ETaskState::Completed);

        // TODO Phase 4: Notify dependent tasks (subsequents)
    }

    void WorkerThread::WaitForWork()
    {
        OLO_PROFILE_FUNCTION();
        
        // Phase 1: Spin briefly (40 iterations)
        // This is the fastest path for short waits
        for (u32 i = 0; i < 40; ++i)
        {
            // Check for work or exit request
            if (!m_LocalQueue.IsEmpty() || m_ShouldExit.load(std::memory_order_acquire))
                return;

            // CPU pause instruction (reduces power and allows hyperthreading)
            _mm_pause();
        }

        // Phase 2: Yield to OS scheduler (10 iterations)
        // Let other threads run while we wait
        for (u32 i = 0; i < 10; ++i)
        {
            if (!m_LocalQueue.IsEmpty() || m_ShouldExit.load(std::memory_order_acquire))
                return;

            std::this_thread::yield();
        }

        // Phase 3: Event wait
        // Use infinite wait - wake will be called on new work or shutdown
        // The WorkerMain loop checks exit flag on every iteration, so even if
        // we somehow miss a wake signal, we'll check exit flag next time around
        m_WakeEvent.Wait();
    }

    Ref<Task> WorkerThread::StealFromOtherWorkers()
    {
        OLO_PROFILE_FUNCTION();
        
        // Get total number of workers of our type
        u32 numWorkers = (m_WorkerType == EWorkerType::Foreground)
            ? m_Scheduler->GetNumForegroundWorkers()
            : m_Scheduler->GetNumBackgroundWorkers();

        if (numWorkers <= 1)
        {
            // No other workers to steal from
            return nullptr;
        }

        // Random starting point to distribute stealing load
        std::uniform_int_distribution<u32> dist(0, numWorkers - 1);
        u32 startIndex = dist(m_RandomEngine);

        // Try to steal from each worker in round-robin order
        for (u32 i = 0; i < numWorkers; ++i)
        {
            // Check exit flag to allow quick shutdown even during stealing
            if (m_ShouldExit.load(std::memory_order_acquire))
                return nullptr;

            u32 victimIndex = (startIndex + i) % numWorkers;

            // Don't steal from ourselves
            if (victimIndex == m_WorkerIndex)
                continue;

            // Get the victim worker
            WorkerThread* victim = m_Scheduler->GetWorker(m_WorkerType, victimIndex);
            if (!victim)
                continue;

            // Try to steal from victim's local queue
            Ref<Task> stolenTask = victim->GetLocalQueue().Steal();
            if (stolenTask)
            {
                // Successfully stolen!
                return stolenTask;
            }
        }

        // No work stolen
        return nullptr;
    }

} // namespace OloEngine
