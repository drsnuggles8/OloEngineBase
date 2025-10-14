#include "OloEnginePCH.h"
#include "ParallelFor.h"
#include "TaskScheduler.h"
#include "Task.h"

#include <algorithm>

namespace OloEngine
{
    void ParallelFor(i32 count, std::function<void(i32)>&& func, i32 batchSize)
    {
        ParallelFor(count, std::move(func), ETaskPriority::Normal, batchSize);
    }

    void ParallelFor(i32 count, std::function<void(i32)>&& func, 
                     ETaskPriority priority, i32 batchSize)
    {
        OLO_PROFILE_FUNCTION();

        if (count <= 0)
        {
            return;  // Nothing to do
        }

        // Auto-detect batch size if not specified
        if (batchSize <= 0)
        {
            // Target: ~100-200 tasks to balance overhead vs parallelism
            // Minimum batch size: 1
            // Maximum batch size: count
            
            u32 numWorkers = TaskScheduler::Get().GetNumForegroundWorkers() +
                            TaskScheduler::Get().GetNumBackgroundWorkers();
            
            // Aim for ~4x worker count for good load balancing
            i32 targetBatches = static_cast<i32>(numWorkers * 4);
            batchSize = std::max(1, count / targetBatches);
            
            // Clamp to reasonable bounds
            batchSize = std::clamp(batchSize, 1, 256);
        }

        // If work is too small, execute inline
        if (count <= batchSize)
        {
            for (i32 i = 0; i < count; ++i)
            {
                func(i);
            }
            return;
        }

        // Calculate number of batches
        i32 numBatches = (count + batchSize - 1) / batchSize;
        
        // Create tasks for each batch
        std::vector<Ref<Task>> tasks;
        tasks.reserve(numBatches);

        // Share the function across all tasks using a shared_ptr
        // This ensures the function stays alive for all task executions
        auto sharedFunc = std::make_shared<std::function<void(i32)>>(std::move(func));

        for (i32 batch = 0; batch < numBatches; ++batch)
        {
            i32 start = batch * batchSize;
            i32 end = std::min(start + batchSize, count);

            // Capture sharedFunc by value - it's a shared_ptr so cheap to copy
            // Each task processes a range of indices
            auto task = TaskScheduler::Get().Launch("ParallelForBatch", priority, 
                [sharedFunc, start, end]() {
                    for (i32 i = start; i < end; ++i)
                    {
                        (*sharedFunc)(i);
                    }
                });

            tasks.push_back(task);
        }

        // Wait for all batches to complete
        TaskWait::WaitForAll(tasks);
    }

} // namespace OloEngine

