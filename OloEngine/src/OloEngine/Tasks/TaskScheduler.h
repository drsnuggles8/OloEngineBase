#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "Task.h"
#include "TaskPriority.h"
#include "GlobalWorkQueue.h"
#include "WorkerThread.h"

#include <functional>
#include <vector>
#include <memory>

namespace OloEngine
{
    /**
     * @brief Task scheduler statistics for monitoring and debugging
     * 
     * Provides insights into task system health and performance.
     * All counts are cumulative except queue depths which are current state.
     */
    struct TaskSchedulerStatistics
    {
        // Task counts
        u64 TotalTasksLaunched = 0;          ///< Total number of tasks launched since initialization
        u64 TotalTasksCompleted = 0;         ///< Total number of tasks completed
        u64 TotalTasksCancelled = 0;         ///< Total number of tasks cancelled
        
        // Current queue depths
        u32 HighPriorityQueueDepth = 0;      ///< Current tasks in high priority queue
        u32 NormalPriorityQueueDepth = 0;    ///< Current tasks in normal priority queue
        u32 BackgroundPriorityQueueDepth = 0;///< Current tasks in background priority queue
        
        // Worker statistics
        u32 NumForegroundWorkers = 0;        ///< Number of foreground worker threads
        u32 NumBackgroundWorkers = 0;        ///< Number of background worker threads
        u32 NumStandbyWorkers = 0;           ///< Number of currently active standby workers
        u32 OversubscriptionLevel = 0;       ///< Number of workers currently blocked
        
        // Computed metrics
        u64 TotalTasksPending() const
        {
            return TotalTasksLaunched - TotalTasksCompleted - TotalTasksCancelled;
        }
        
        u32 TotalQueueDepth() const
        {
            return HighPriorityQueueDepth + NormalPriorityQueueDepth + BackgroundPriorityQueueDepth;
        }
    };

    /**
     * @brief Configuration for the task scheduler
     * 
     * Controls worker thread pool sizes and behavior.
     * A value of 0 for worker counts means auto-detect based on CPU core count.
     */
    struct TaskSchedulerConfig
    {
        /**
         * @brief Number of foreground worker threads
         * 
         * Default: 0 (auto-detect as NumCores - 2)
         * Foreground workers handle High and Normal priority tasks.
         * We reserve cores for main thread and render thread.
         */
        u32 NumForegroundWorkers = 0;

        /**
         * @brief Number of background worker threads
         * 
         * Default: 0 (auto-detect as Max(1, NumCores / 4))
         * Background workers handle Background priority tasks.
         * Typically ~25% of available cores.
         */
        u32 NumBackgroundWorkers = 0;

        /**
         * @brief Maximum nodes in each global work queue
         * 
         * Default: 4096
         * Each priority level (High, Normal, Background) has its own queue.
         * Increase this if you expect to queue thousands of tasks simultaneously.
         * Note: This is per-queue, so total memory is 3 * MaxQueueNodes * sizeof(Node)
         */
        u32 MaxQueueNodes = 4096;

        /**
         * @brief Auto-detect worker counts based on CPU
         * 
         * Called automatically if worker counts are 0.
         * Can be called manually to get recommended values.
         */
        void AutoDetectWorkerCounts();
    };

    /**
     * @brief Central task scheduler (singleton)
     * 
     * The TaskScheduler manages worker thread pools and coordinates task execution.
     * It provides the main API for launching tasks and manages:
     * - Worker thread pools (foreground and background)
     * - Work queues (per-priority global queues)
     * - Work stealing coordination
     * - Thread-local worker context
     * 
     * The scheduler is a singleton and must be initialized before use.
     * 
     * Usage:
     * @code
     * TaskScheduler::Initialize(config);
     * auto task = TaskScheduler::Get().Launch("MyTask", []{ DoWork(); });
     * task->Wait();
     * TaskScheduler::Shutdown();
     * @endcode
     */
    class TaskScheduler
    {
        // WorkerThread needs access to standby worker count
        friend class WorkerThread;
        
    public:
        /**
         * @brief Initialize the task scheduler
         * 
         * This must be called once at application startup before launching any tasks.
         * Creates worker thread pools and initializes internal data structures.
         * 
         * @param config Scheduler configuration (worker counts, profiling, etc.)
         */
        static void Initialize(const TaskSchedulerConfig& config = TaskSchedulerConfig{});

        /**
         * @brief Shutdown the task scheduler
         * 
         * Stops all worker threads and cleans up resources.
         * Should be called at application shutdown.
         * Waits for all in-flight tasks to complete before returning.
         */
        static void Shutdown();

        /**
         * @brief Get the singleton scheduler instance
         * 
         * @return Reference to the global scheduler
         * @note Asserts if called before Initialize()
         */
        static TaskScheduler& Get();

        /**
         * @brief Check if the scheduler has been initialized
         * @return True if initialized, false otherwise
         */
        static bool IsInitialized();

    public:
        /**
         * @brief Launch a task with default priority (Normal)
         * 
         * The callable must satisfy the TaskCallable concept (invocable, returns void, move-constructible).
         * 
         * @param debugName Debug name for profiling (must be string literal or long-lived)
         * @param func Callable to execute (lambda, function, functor)
         * @return Reference-counted task handle
         */
        template<TaskCallable Callable>
        Ref<Task> Launch(const char* debugName, Callable&& func)
        {
            return Launch(debugName, ETaskPriority::Normal, std::forward<Callable>(func));
        }

        /**
         * @brief Launch a task with specified priority
         * 
         * The callable must satisfy the TaskCallable concept (invocable, returns void, move-constructible).
         * 
         * @param debugName Debug name for profiling
         * @param priority Task priority level
         * @param func Callable to execute
         * @return Reference-counted task handle
         */
        template<TaskCallable Callable>
        Ref<Task> Launch(const char* debugName, ETaskPriority priority, Callable&& func)
        {
            // Create the task
            auto task = CreateTask(debugName, priority, std::forward<Callable>(func));
            
            // Launch it (implementation in Phase 3)
            LaunchTask(task);
            
            return task;
        }

        /**
         * @brief Launch a task with prerequisites (Phase 4)
         * 
         * The task will not be scheduled until all prerequisites complete.
         * The callable must satisfy the TaskCallable concept (invocable, returns void, move-constructible).
         * 
         * @param debugName Debug name for profiling
         * @param priority Task priority level
         * @param func Callable to execute
         * @param prerequisites Tasks that must complete before this one
         * @return Reference-counted task handle
         */
        template<TaskCallable Callable>
        Ref<Task> Launch(const char* debugName, ETaskPriority priority, Callable&& func,
                        std::initializer_list<Ref<Task>> prerequisites)
        {
            // Create the task
            auto task = CreateTask(debugName, priority, std::forward<Callable>(func));
            
            // Add prerequisites
            for (const auto& prereq : prerequisites)
            {
                task->AddPrerequisite(prereq);
            }
            
            // Launch task if it has no outstanding prerequisites
            // Otherwise, it will be launched by the last prerequisite when it completes
            if (task->ArePrerequisitesComplete())
            {
                LaunchTask(task);
            }
            
            return task;
        }

        /**
         * @brief Internal task launch implementation (made public for Phase 4 dependencies)
         * 
         * Queues the task and wakes workers as needed.
         * This is called by Launch() and also by Task::OnCompleted() when dependencies complete.
         * 
         * @param task The task to launch
         */
        void LaunchTask(Ref<Task> task);

        /**
         * @brief Launch multiple tasks as a batch (Phase 5 optimization)
         * 
         * Queues all tasks before waking workers, reducing wake-up overhead.
         * More efficient than calling Launch() repeatedly when launching many tasks.
         * The callable must satisfy the TaskCallable concept (invocable, returns void, move-constructible).
         * 
         * @param debugName Debug name prefix for tasks
         * @param priority Task priority level
         * @param funcs Vector of callables to execute
         * @return Vector of task handles (same size as funcs)
         */
        template<TaskCallable Callable>
        std::vector<Ref<Task>> LaunchBatch(const char* debugName, ETaskPriority priority,
                                          const std::vector<Callable>& funcs)
        {
            std::vector<Ref<Task>> tasks;
            tasks.reserve(funcs.size());
            
            // Create and queue all tasks first
            for (const auto& func : funcs)
            {
                auto task = CreateTask(debugName, priority, func);
                
                // Transition to Scheduled state
                ETaskState expected = ETaskState::Ready;
                if (task->TryTransitionState(expected, ETaskState::Scheduled))
                {
                    // Queue to appropriate global queue
                    GlobalWorkQueue& queue = GetGlobalQueue(priority);
                    if (queue.Push(task))
                    {
                        tasks.push_back(task);
                    }
                    else
                    {
                        // Queue full - mark task as completed (failed to schedule)
                        task->SetState(ETaskState::Completed);
                    }
                }
            }
            
            // Now wake workers once for all tasks (instead of once per task)
            if (!tasks.empty())
            {
                // Wake multiple workers based on batch size
                u32 workersToWake = std::min(static_cast<u32>(tasks.size()), 
                                             priority == ETaskPriority::Background ? m_NumBackgroundWorkers : m_NumForegroundWorkers);
                for (u32 i = 0; i < workersToWake; ++i)
                {
                    WakeWorker(priority);
                }
                
                // Update statistics
                m_TotalTasksLaunched.fetch_add(tasks.size(), std::memory_order_relaxed);
            }
            
            return tasks;
        }

        /**
         * @brief Launch multiple tasks as a batch with default priority (Normal)
         * 
         * The callable must satisfy the TaskCallable concept (invocable, returns void, move-constructible).
         * 
         * @param debugName Debug name prefix for tasks
         * @param funcs Vector of callables to execute
         * @return Vector of task handles
         */
        template<TaskCallable Callable>
        std::vector<Ref<Task>> LaunchBatch(const char* debugName, const std::vector<Callable>& funcs)
        {
            return LaunchBatch(debugName, ETaskPriority::Normal, funcs);
        }

        /**
         * @brief Get the number of foreground workers
         * @return Worker count
         */
        u32 GetNumForegroundWorkers() const { return m_NumForegroundWorkers; }

        /**
         * @brief Get the number of background workers
         * @return Worker count
         */
        u32 GetNumBackgroundWorkers() const { return m_NumBackgroundWorkers; }

        /**
         * @brief Get a worker by type and index
         * @param workerType The worker type (foreground or background)
         * @param workerIndex The worker index within that type
         * @return Pointer to worker, or nullptr if invalid index
         */
        WorkerThread* GetWorker(EWorkerType workerType, u32 workerIndex);

        /**
         * @brief Get the global work queue for a priority level
         * @param priority The task priority
         * @return Reference to the appropriate global queue
         */
        GlobalWorkQueue& GetGlobalQueue(ETaskPriority priority);

        /**
         * @brief Increment oversubscription counter
         * 
         * When a worker blocks waiting for a task, it should call this to indicate
         * that the system may be undersubscribed. This may spawn a standby worker
         * to prevent deadlock scenarios where all workers are blocked.
         */
        void IncrementOversubscription();

        /**
         * @brief Decrement oversubscription counter
         * 
         * Called when a blocked worker resumes execution.
         */
        void DecrementOversubscription();

        /**
         * @brief Get current oversubscription level
         * @return Number of currently blocked workers
         */
        u32 GetOversubscriptionLevel() const { return m_OversubscriptionLevel.load(std::memory_order_relaxed); }

        /**
         * @brief Get current task system statistics
         * 
         * Provides metrics for monitoring task system health and performance.
         * Statistics are gathered atomically but represent a snapshot in time.
         * 
         * @return Current statistics snapshot
         */
        TaskSchedulerStatistics GetStatistics() const;

        /**
         * @brief Notify scheduler that a task was completed
         * 
         * Called by WorkerThread when a task execution finishes.
         * Updates statistics counters.
         */
        void OnTaskCompleted()
        {
            m_TotalTasksCompleted.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief Notify scheduler that a task was cancelled
         * 
         * Called by Task::Cancel() when a task is successfully cancelled.
         * Updates statistics counters.
         */
        void OnTaskCancelled()
        {
            m_TotalTasksCancelled.fetch_add(1, std::memory_order_relaxed);
        }

    private:

    private:
        TaskScheduler() = default;
        ~TaskScheduler() = default;

        // Singleton - no copy/move
        TaskScheduler(const TaskScheduler&) = delete;
        TaskScheduler& operator=(const TaskScheduler&) = delete;
        TaskScheduler(TaskScheduler&&) = delete;
        TaskScheduler& operator=(TaskScheduler&&) = delete;

        /**
         * @brief Wake an appropriate worker for a task priority
         * @param priority The task priority that was just queued
         */
        void WakeWorker(ETaskPriority priority);

    private:
        static TaskScheduler* s_Instance;  ///< Singleton instance

        u32 m_NumForegroundWorkers = 0;    ///< Number of foreground workers
        u32 m_NumBackgroundWorkers = 0;    ///< Number of background workers
        bool m_IsInitialized = false;      ///< Initialization state

        // Worker thread pools
        std::vector<std::unique_ptr<WorkerThread>> m_ForegroundWorkers;  ///< Foreground worker threads
        std::vector<std::unique_ptr<WorkerThread>> m_BackgroundWorkers;  ///< Background worker threads

        // Global work queues (one per priority level)
        std::unique_ptr<GlobalWorkQueue> m_HighPriorityQueue;     ///< High priority global queue
        std::unique_ptr<GlobalWorkQueue> m_NormalPriorityQueue;   ///< Normal priority global queue
        std::unique_ptr<GlobalWorkQueue> m_BackgroundPriorityQueue;  ///< Background priority global queue

        // Round-robin wake index for load distribution
        std::atomic<u32> m_NextWakeIndexForeground{0};
        std::atomic<u32> m_NextWakeIndexBackground{0};

        // Oversubscription tracking
        std::atomic<u32> m_OversubscriptionLevel{0};  ///< Number of workers currently blocked
        std::atomic<u32> m_StandbyWorkerCount{0};     ///< Number of active standby workers
        static constexpr u32 s_MaxStandbyWorkers = 8;  ///< Maximum standby workers to spawn
        
        // Statistics tracking
        std::atomic<u64> m_TotalTasksLaunched{0};     ///< Total tasks launched
        std::atomic<u64> m_TotalTasksCompleted{0};    ///< Total tasks completed
        std::atomic<u64> m_TotalTasksCancelled{0};    ///< Total tasks cancelled
    };

} // namespace OloEngine
