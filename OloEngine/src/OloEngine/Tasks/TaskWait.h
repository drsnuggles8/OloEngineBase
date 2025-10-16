#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "Task.h"

#include <vector>
#include <initializer_list>

namespace OloEngine
{
    /**
     * @brief Task waiting and synchronization utilities
     * 
     * Provides high-level waiting strategies that keep worker threads productive
     * instead of blocking. Implements task retraction and hybrid waiting.
     */
    namespace TaskWait
    {
        /**
         * @brief Try to retract a scheduled task and execute it inline
         * 
         * If the task is still in Scheduled state (not yet grabbed by a worker),
         * this will transition it back to Ready, then execute it on the calling thread.
         * This avoids a context switch and is faster than waiting for a worker.
         * 
         * Note: We don't need to remove the task from queues - if a worker grabs it,
         * the state will no longer be Scheduled, so the worker's state transition will
         * fail and it will skip the task.
         * 
         * @param task The task to retract and execute
         * @return True if task was retracted and executed, false if already running/completed
         */
        bool TryRetractAndExecute(Ref<Task> task);

        /**
         * @brief Wait for a task to complete using hybrid strategy
         * 
         * Strategy (in order):
         * 1. Try retraction - execute inline if possible (best case - no wait)
         * 2. If on worker thread: execute other tasks while waiting (keep worker productive)
         * 3. If not on worker thread or no work: spin-then-sleep wait
         * 
         * @param task The task to wait for
         */
        void Wait(Ref<Task> task);

        /**
         * @brief Wait for all tasks in a collection to complete
         * 
         * Creates a TaskEvent that depends on all tasks, then waits for the event.
         * This is more efficient than waiting for each task individually.
         * 
         * @param tasks Vector of tasks to wait for
         */
        void WaitForAll(const std::vector<Ref<Task>>& tasks);

        /**
         * @brief Wait for all tasks in an initializer list to complete
         * 
         * @param tasks Initializer list of tasks to wait for
         */
        void WaitForAll(std::initializer_list<Ref<Task>> tasks);

        /**
         * @brief Wait for ANY task in a collection to complete
         * 
         * Returns immediately when the first task completes. This is useful for:
         * - Timeout patterns (race a task against a timer)
         * - First-responder scenarios (take the first result available)
         * - Cancellation patterns (wait for work or cancel signal)
         * 
         * If multiple tasks are already complete, returns the index of the first
         * completed task found (not guaranteed to be the first chronologically).
         * 
         * @param tasks Vector of tasks to wait for
         * @return Index of the first completed task, or -1 if tasks vector is empty
         */
        i32 WaitForAny(const std::vector<Ref<Task>>& tasks);

        /**
         * @brief Wait for ANY task in an initializer list to complete
         * 
         * @param tasks Initializer list of tasks to wait for
         * @return Index of the first completed task, or -1 if tasks list is empty
         */
        i32 WaitForAny(std::initializer_list<Ref<Task>> tasks);

    } // namespace TaskWait

} // namespace OloEngine
