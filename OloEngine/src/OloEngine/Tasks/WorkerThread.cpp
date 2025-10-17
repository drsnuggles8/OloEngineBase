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
    WorkerThread::WorkerThread(TaskScheduler* scheduler, u32 workerIndex, EWorkerType workerType, bool isStandby)
        : m_Scheduler(scheduler)
        , m_Thread(std::string("Worker_") + std::to_string(workerIndex))
        , m_WakeFlag(false)  // C++20 atomic for wait/notify
        , m_ShouldExit(false)
        , m_WorkerIndex(workerIndex)
        , m_WorkerType(workerType)
        , m_IsStandby(isStandby)
        , m_IdleIterations(0)
    {
        OLO_PROFILE_FUNCTION();
        
        // Initialize random engine with a unique seed per worker
        m_RandomEngine.seed(static_cast<u32>(
            std::chrono::steady_clock::now().time_since_epoch().count() + workerIndex));

        // Start the worker thread (only if not standby - standby uses DetachAndRun)
        if (!m_IsStandby)
        {
            m_Thread.Dispatch([this]() { WorkerMain(); });
        }
    }

    WorkerThread::~WorkerThread()
    {
        OLO_PROFILE_FUNCTION();
        
        // Standby workers manage their own lifetime and will self-destruct
        // when idle. The destructor should NOT be called on standby workers
        // from external code - they call 'delete this' from their own thread.
        if (m_IsStandby)
        {
            // CRITICAL: Standby workers must only be deleted from their own thread
            // If this assertion fires, it means someone is incorrectly deleting
            // a standby worker from another thread, which will cause use-after-free
            OLO_CORE_ASSERT(std::this_thread::get_id() == m_Thread.GetID(),
                "Standby worker destructor called from wrong thread! "
                "Standby workers self-destruct and must not be deleted externally.");
            
            // If we somehow got here for a standby worker, just exit immediately
            // to avoid use-after-free. The thread will clean itself up.
            return;
        }
        
        // For normal workers: ensure exit flag is set
        m_ShouldExit.store(true, std::memory_order_release);
        
        // Wake the thread multiple times to ensure it sees the exit flag
        // C++20 atomic notify is more reliable than event signaling
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
        // C++20 atomic notify: Set flag and notify waiting thread
        m_WakeFlag.store(true, std::memory_order_release);
        m_WakeFlag.notify_one();
    }

    void WorkerThread::DetachAndRun()
    {
        // Standby workers run on their own thread and manage their own lifetime
        // We spawn the thread and it will delete itself when done
        m_Thread.Dispatch([this]() {
            WorkerMain();
            // When WorkerMain exits, delete ourselves
            delete this;
        });
    }

    void WorkerThread::WorkerMain()
    {
        OLO_PROFILE_FUNCTION();
        
        // Register this thread as a worker thread (for TaskWait detection)
        SetCurrentWorkerThread(this);

        while (!m_ShouldExit.load(std::memory_order_acquire))
        {
            Ref<Task> task = FindWork();
            
            if (task)
            {
                ExecuteTask(task);
                
                // Reset idle counter when we found work
                if (m_IsStandby)
                {
                    m_IdleIterations = 0;
                }
            }
            else
            {
                // No work found
                if (m_IsStandby)
                {
                    // Standby workers exit after being idle too long
                    ++m_IdleIterations;
                    if (m_IdleIterations >= s_StandbyIdleLimit)
                    {
                        // Decrement standby counter and exit
                        m_Scheduler->m_StandbyWorkerCount.fetch_sub(1, std::memory_order_relaxed);
                        break;
                    }
                }
                
                // Wait for more work
                WaitForWork();
            }
        }

        // Unregister this worker thread
        SetCurrentWorkerThread(nullptr);
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

        // Check if task was cancelled before we got to it
        if (task->IsCancelled())
        {
            // Task was cancelled while in queue - skip execution
            return;
        }

        // Transition from Scheduled to Running
        ETaskState expected = ETaskState::Scheduled;
        if (!task->TryTransitionState(expected, ETaskState::Running))
        {
            // State transition failed - task might have been retracted, cancelled, or is in wrong state
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

        // Notify scheduler for statistics
        m_Scheduler->OnTaskCompleted();

        // Notify dependent tasks (Phase 4: Dependencies)
        task->OnCompleted();
    }

    void WorkerThread::WaitForWork()
    {
        OLO_PROFILE_FUNCTION();
        
        // Phase 1: Spin briefly (40 iterations)
        // This is the fastest path for short waits
        for (u32 i = 0; i < 40; ++i)
        {
            // Check for work or exit request
            // CRITICAL: Check ALL queues, not just local!
            if (HasWorkAvailable() || m_ShouldExit.load(std::memory_order_acquire))
                return;

            // CPU pause instruction (reduces power and allows hyperthreading)
            _mm_pause();
        }

        // Phase 2: Yield to OS scheduler (10 iterations)
        // Let other threads run while we wait
        for (u32 i = 0; i < 10; ++i)
        {
            // CRITICAL: Check ALL queues, not just local!
            if (HasWorkAvailable() || m_ShouldExit.load(std::memory_order_acquire))
                return;

            std::this_thread::yield();
        }

        // Phase 3: C++20 atomic wait
        // More efficient than event objects - uses hardware-level wait (WFE on ARM, MONITOR/MWAIT on x86)
        // Loop to handle spurious wakeups
        while (true)
        {
            // Before sleeping, do one final check with memory fence to ensure visibility
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (HasWorkAvailable() || m_ShouldExit.load(std::memory_order_acquire))
                return;

            // Wait for wake flag to become true
            // This is a hardware-efficient wait that puts the CPU into a low-power state
            m_WakeFlag.wait(false, std::memory_order_acquire);
            
            // Reset the wake flag for next wait
            m_WakeFlag.store(false, std::memory_order_relaxed);
            
            // Check again after waking (handles spurious wakeups and ensures we see new work)
            if (HasWorkAvailable() || m_ShouldExit.load(std::memory_order_acquire))
                return;
        }
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

    bool WorkerThread::HasWorkAvailable() const
    {
        OLO_PROFILE_FUNCTION();
        
        // Check local queue first (fastest check)
        if (!m_LocalQueue.IsEmpty())
            return true;

        // Check global queues based on worker type
        if (m_WorkerType == EWorkerType::Foreground)
        {
            // Foreground workers can access High and Normal priority queues
            if (!m_Scheduler->GetGlobalQueue(ETaskPriority::High).IsEmpty())
                return true;
            
            if (!m_Scheduler->GetGlobalQueue(ETaskPriority::Normal).IsEmpty())
                return true;
        }
        else  // Background worker
        {
            // Background workers only access Background priority queue
            if (!m_Scheduler->GetGlobalQueue(ETaskPriority::Background).IsEmpty())
                return true;
        }

        // No work available
        return false;
    }

} // namespace OloEngine
