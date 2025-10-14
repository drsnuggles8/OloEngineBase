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
         */
        WorkerThread(TaskScheduler* scheduler, u32 workerIndex, EWorkerType workerType);

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

    private:
        TaskScheduler* m_Scheduler;              ///< Pointer to scheduler (for global queues)
        Thread m_Thread;                         ///< OS thread wrapper
        ThreadSignal m_WakeEvent;                ///< Event for waking sleeping worker
        LocalWorkQueue<1024> m_LocalQueue;       ///< Worker's local work queue
        
        std::atomic<bool> m_ShouldExit;          ///< Flag to signal thread exit
        u32 m_WorkerIndex;                       ///< Unique worker index
        EWorkerType m_WorkerType;                ///< Worker type (foreground/background)
        
        // Random number generator for work stealing (per-thread, no synchronization needed)
        std::mt19937 m_RandomEngine;             ///< RNG for random steal starting point
    };

} // namespace OloEngine
