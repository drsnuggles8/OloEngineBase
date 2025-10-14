#pragma once

#include "TaskScheduler.h"

namespace OloEngine
{
    /**
     * @brief RAII scope guard for task system oversubscription
     * 
     * When a worker thread blocks waiting for a task (e.g., using TaskWait),
     * it should use this scope to indicate that the system may be undersubscribed.
     * This allows the scheduler to spawn standby workers to prevent deadlock scenarios.
     * 
     * Usage:
     * @code
     * void SomeTask()
     * {
     *     auto task = LaunchSubTask();
     *     
     *     // Indicate we're blocking - may spawn standby worker
     *     OversubscriptionScope scope;
     *     task->Wait();
     * }
     * @endcode
     * 
     * The scope automatically decrements the oversubscription counter when destroyed,
     * allowing the system to reclaim resources as threads resume.
     */
    class OversubscriptionScope
    {
    public:
        /**
         * @brief Constructor - increments oversubscription counter
         * 
         * May trigger spawning of a standby worker if many workers are blocked.
         */
        OversubscriptionScope()
        {
            if (TaskScheduler::IsInitialized())
            {
                TaskScheduler::Get().IncrementOversubscription();
            }
        }

        /**
         * @brief Destructor - decrements oversubscription counter
         * 
         * Signals that this worker is no longer blocked.
         */
        ~OversubscriptionScope()
        {
            if (TaskScheduler::IsInitialized())
            {
                TaskScheduler::Get().DecrementOversubscription();
            }
        }

        // Non-copyable, non-movable
        OversubscriptionScope(const OversubscriptionScope&) = delete;
        OversubscriptionScope& operator=(const OversubscriptionScope&) = delete;
        OversubscriptionScope(OversubscriptionScope&&) = delete;
        OversubscriptionScope& operator=(OversubscriptionScope&&) = delete;
    };

} // namespace OloEngine
