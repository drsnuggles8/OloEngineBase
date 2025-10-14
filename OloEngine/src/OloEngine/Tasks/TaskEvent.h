#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "Task.h"

#include <initializer_list>

namespace OloEngine
{
    /**
     * @brief Non-blocking synchronization event for tasks
     * 
     * TaskEvent provides a way to synchronize on multiple tasks without blocking
     * worker threads. Instead of sleeping, workers continue executing other tasks
     * while waiting for the event to be triggered.
     * 
     * The event is implemented as a special task that completes when triggered.
     * Other tasks can depend on the event, and waiting on the event keeps workers
     * productive by executing other work.
     * 
     * Usage:
     * @code
     * TaskEvent event("MyEvent");
     * 
     * // Create tasks that depend on the event
     * auto task1 = scheduler.Launch("Task1", []{ DoWork1(); });
     * auto task2 = scheduler.Launch("Task2", []{ DoWork2(); });
     * 
     * event.AddPrerequisites({task1, task2});
     * 
     * // Do other work...
     * 
     * event.Wait();  // Waits for task1 and task2, executing other tasks in the meantime
     * @endcode
     */
    class TaskEvent
    {
    public:
        /**
         * @brief Create a task event
         * @param debugName Debug name for profiling
         */
        explicit TaskEvent(const char* debugName = "TaskEvent");

        /**
         * @brief Add prerequisite tasks to the event
         * 
         * The event will not be triggered until all prerequisites complete.
         * 
         * @param prerequisites Tasks that must complete before event triggers
         */
        void AddPrerequisites(std::initializer_list<Ref<Task>> prerequisites);

        /**
         * @brief Add a single prerequisite task to the event
         * @param prerequisite Task that must complete before event triggers
         */
        void AddPrerequisite(Ref<Task> prerequisite);

        /**
         * @brief Manually trigger the event
         * 
         * This completes the internal event task, allowing any tasks or waits
         * that depend on it to proceed. Automatically called when all prerequisites complete.
         */
        void Trigger();

        /**
         * @brief Wait for the event to be triggered
         * 
         * This is a non-blocking wait that keeps the worker thread productive
         * by executing other tasks while waiting.
         */
        void Wait();

        /**
         * @brief Check if the event has been triggered
         * @return True if event is triggered (completed)
         */
        bool IsTriggered() const
        {
            return m_EventTask && m_EventTask->IsCompleted();
        }

        /**
         * @brief Get the event as a task for use as a prerequisite
         * 
         * This allows other tasks to depend on the event.
         * 
         * @return The internal event task
         */
        Ref<Task> AsPrerequisite() const
        {
            return m_EventTask;
        }

    private:
        Ref<Task> m_EventTask;  ///< Internal task that represents the event
    };

} // namespace OloEngine
