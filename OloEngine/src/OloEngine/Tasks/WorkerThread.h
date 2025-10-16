#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Thread.h"
#include "OloEngine/Core/Ref.h"
#include "Task.h"
#include "LocalWorkQueue.h"

#include <atomic>
#include <random>

namespace OloEngine
{
    // Forward declaration
    class TaskScheduler;
    class WorkerThread;

    /**
     * @brief Set the current worker thread (for thread-local tracking)
     * 
     * This is called by WorkerThread::WorkerMain() to register the current thread
     * as a worker thread. Used by TaskWait to detect if we're on a worker thread.
     * 
     * @param worker Pointer to current worker, or nullptr to clear
     */
    void SetCurrentWorkerThread(WorkerThread* worker);

    /**
     * @brief Get the current worker thread (if any)
     * 
     * @return Pointer to current worker thread, or nullptr if not on a worker thread
     */
    WorkerThread* GetCurrentWorkerThread();

    /**
     * @brief Worker thread type classification
     * 
     * Foreground workers handle high/normal priority tasks
     * Background workers handle background priority tasks
     */
    enum class EWorkerType : u8
    {
        Foreground,  ///< Handles High and Normal priority tasks
        Background   ///< Handles Background priority tasks
    };

    /**
     * @brief Worker thread that executes tasks from queues
     * 
     * Each worker thread has:
     * - A local work queue (push/pop for owner, steal for others)
     * - Access to global work queues via the TaskScheduler
     * - Work stealing capability to balance load
     * - Efficient wake/sleep strategy to minimize latency
     * 
     * Work Finding Strategy (in order):
     * 1. Check own local queue (best cache locality)
     * 2. Check global queue for this priority level
     * 3. Try stealing from other workers
     * 4. Wait for more work (spin-then-sleep)
     */
    class WorkerThread
    {
    public:
        /**
         * @brief Create a worker thread
         * 
         * The thread will start immediately and begin looking for work.
         * 
         * @param scheduler Pointer to the task scheduler (for accessing global queues)
         * @param workerIndex Unique index for this worker
         * @param workerType Type of worker (foreground or background)
         * @param isStandby True if this is a temporary standby worker (default: false)
         */
        WorkerThread(TaskScheduler* scheduler, u32 workerIndex, EWorkerType workerType, bool isStandby = false);

        /**
         * @brief Destructor - stops the worker thread
         */
        ~WorkerThread();

        // Disable copy/move (worker threads are unique)
        WorkerThread(const WorkerThread&) = delete;
        WorkerThread& operator=(const WorkerThread&) = delete;
        WorkerThread(WorkerThread&&) = delete;
        WorkerThread& operator=(WorkerThread&&) = delete;

        /**
         * @brief Request the worker thread to stop
         * 
         * This signals the thread to exit after completing its current task.
         * Call Join() to wait for the thread to actually exit.
         */
        void RequestStop();

        /**
         * @brief Wait for the worker thread to exit
         * 
         * Blocks until the thread has finished its main loop and exited.
         */
        void Join();

        /**
         * @brief Wake the worker thread if it's sleeping
         * 
         * This signals the wake event to wake up a sleeping worker.
         */
        void Wake();

        /**
         * @brief Get the worker's local queue
         * @return Reference to the local work queue
         */
        LocalWorkQueue<1024>& GetLocalQueue() { return m_LocalQueue; }
        const LocalWorkQueue<1024>& GetLocalQueue() const { return m_LocalQueue; }

        /**
         * @brief Get the worker index
         * @return Worker index
         */
        u32 GetWorkerIndex() const { return m_WorkerIndex; }

        /**
         * @brief Get the worker type
         * @return Worker type (foreground or background)
         */
        EWorkerType GetWorkerType() const { return m_WorkerType; }

        /**
         * @brief Get the underlying thread
         * @return Reference to the Thread object
         */
        Thread& GetThread() { return m_Thread; }

        /**
         * @brief Find work to execute (public version for TaskWait)
         * 
         * This is used by TaskWait::Wait() to allow waiting threads to execute
         * other tasks while waiting, keeping workers productive.
         * 
         * @return Task to execute, or null if no work found
         */
        Ref<Task> FindWorkPublic() { return FindWork(); }

        /**
         * @brief Execute a task (public version for TaskWait)
         * 
         * This is used by TaskWait::Wait() to execute tasks on the current thread.
         * 
         * @param task The task to execute
         */
        void ExecuteTaskPublic(Ref<Task> task) { ExecuteTask(task); }

        /**
         * @brief Detach thread and run (for standby workers)
         * 
         * This is used for standby workers that manage their own lifetime.
         * The worker will delete itself when it exits.
         */
        void DetachAndRun();

    private:
        /**
         * @brief Main worker loop (runs on worker thread)
         * 
         * This is the entry point for the worker thread. It continuously
         * looks for work, executes tasks, and waits when no work is available.
         */
        void WorkerMain();

        /**
         * @brief Find work to execute
         * 
         * Search strategy:
         * 1. Check local queue
         * 2. Check global queue
         * 3. Try stealing from other workers
         * 
         * @return Task to execute, or null if no work found
         */
        Ref<Task> FindWork();

        /**
         * @brief Execute a task
         * 
         * Handles state transitions and exception catching.
         * 
         * @param task The task to execute
         */
        void ExecuteTask(Ref<Task> task);

        /**
         * @brief Wait for work to become available
         * 
         * Three-phase strategy:
         * 1. Spin briefly (40 iterations with pause)
         * 2. Yield to OS scheduler (10 iterations)
         * 3. Wait on event with timeout (100ms)
         */
        void WaitForWork();

        /**
         * @brief Try to steal work from other workers
         * 
         * Uses random starting point and round-robin strategy to
         * distribute stealing attempts evenly.
         * 
         * @return Stolen task, or null if no work stolen
         */
        Ref<Task> StealFromOtherWorkers();

        /**
         * @brief Check if work is available in any queue this worker can access
         * 
         * This checks:
         * - Local queue
         * - Global queues appropriate for this worker type
         * 
         * Used by WaitForWork to avoid sleeping when work exists.
         * 
         * @return True if work is available, false otherwise
         */
        bool HasWorkAvailable() const;

    private:
        TaskScheduler* m_Scheduler;              ///< Pointer to scheduler (for global queues)
        Thread m_Thread;                         ///< OS thread wrapper
        ThreadSignal m_WakeEvent;                ///< Event for waking sleeping worker
        LocalWorkQueue<1024> m_LocalQueue;       ///< Worker's local work queue
        
        std::atomic<bool> m_ShouldExit;          ///< Flag to signal thread exit
        u32 m_WorkerIndex;                       ///< Unique worker index
        EWorkerType m_WorkerType;                ///< Worker type (foreground/background)
        bool m_IsStandby;                        ///< True if this is a temporary standby worker
        u32 m_IdleIterations;                    ///< Counter for idle cycles (standby workers only)
        
        /// Idle iterations before standby worker exits (~10-100ms depending on workload)
        /// Increased from 100 to allow standby workers to do useful work before exiting
        static constexpr u32 s_StandbyIdleLimit = 10000;
        
        // Random number generator for work stealing (per-thread, no synchronization needed)
        std::mt19937 m_RandomEngine;             ///< RNG for random steal starting point
    };

} // namespace OloEngine
