#pragma once

#include "OloEngine/Core/Base.h"
#include "TaskWait.h"

#include <functional>
#include <vector>

namespace OloEngine
{
    /**
     * @brief Execute a function in parallel across a range of indices
     * 
     * Divides the work into batches and distributes them across worker threads.
     * Uses the task system for execution, with automatic batch sizing.
     * 
     * @param count Total number of iterations
     * @param func Function to execute for each index: void(i32 index)
     * @param batchSize Number of iterations per task (0 = auto-detect)
     * 
     * Usage:
     * @code
     * ParallelFor(1000, [](i32 i) {
     *     ProcessItem(i);
     * });
     * @endcode
     */
    void ParallelFor(i32 count, std::function<void(i32)>&& func, i32 batchSize = 0);

    /**
     * @brief Execute a function in parallel across a range with priority
     * 
     * Same as ParallelFor but allows specifying task priority.
     * 
     * @param count Total number of iterations
     * @param func Function to execute for each index
     * @param priority Task priority for parallel work
     * @param batchSize Number of iterations per task (0 = auto-detect)
     */
    void ParallelFor(i32 count, std::function<void(i32)>&& func, 
                     ETaskPriority priority, i32 batchSize = 0);

} // namespace OloEngine

