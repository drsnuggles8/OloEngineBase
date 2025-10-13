#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "Task.h"
#include "TaskPriority.h"

#include <functional>

namespace OloEngine
{
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
         * @param debugName Debug name for profiling (must be string literal or long-lived)
         * @param func Callable to execute (lambda, function, functor)
         * @return Reference-counted task handle
         */
        template<typename Callable>
        Ref<Task> Launch(const char* debugName, Callable&& func)
        {
            return Launch(debugName, ETaskPriority::Normal, std::forward<Callable>(func));
        }

        /**
         * @brief Launch a task with specified priority
         * 
         * @param debugName Debug name for profiling
         * @param priority Task priority level
         * @param func Callable to execute
         * @return Reference-counted task handle
         */
        template<typename Callable>
        Ref<Task> Launch(const char* debugName, ETaskPriority priority, Callable&& func)
        {
            // Create the task
            auto task = CreateTask(debugName, priority, std::forward<Callable>(func));
            
            // Launch it (implementation in Phase 3)
            LaunchTask(task);
            
            return task;
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

    private:
        TaskScheduler() = default;
        ~TaskScheduler() = default;

        // Singleton - no copy/move
        TaskScheduler(const TaskScheduler&) = delete;
        TaskScheduler& operator=(const TaskScheduler&) = delete;
        TaskScheduler(TaskScheduler&&) = delete;
        TaskScheduler& operator=(TaskScheduler&&) = delete;

        /**
         * @brief Internal task launch implementation
         * 
         * Queues the task and wakes workers as needed.
         * Full implementation in Phase 3 (worker threads).
         * 
         * @param task The task to launch
         */
        void LaunchTask(Ref<Task> task);

    private:
        static TaskScheduler* s_Instance;  ///< Singleton instance

        u32 m_NumForegroundWorkers = 0;    ///< Number of foreground workers
        u32 m_NumBackgroundWorkers = 0;    ///< Number of background workers
        bool m_IsInitialized = false;      ///< Initialization state
    };

} // namespace OloEngine
