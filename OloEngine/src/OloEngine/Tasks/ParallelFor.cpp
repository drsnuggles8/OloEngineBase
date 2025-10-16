#include "OloEnginePCH.h"
#include "ParallelFor.h"
#include "TaskScheduler.h"
#include "Task.h"

#include <algorithm>
#include <chrono>

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

        // Adaptive batch sizing enabled with batchSize = -1
        bool useAdaptiveSizing = (batchSize == -1);
        
        // Auto-detect batch size if not specified
        if (batchSize <= 0)
        {
            // Target: ~100-200 tasks to balance overhead vs parallelism
            // Minimum batch size: 1
            // Maximum batch size: count
            
            u32 numWorkers = TaskScheduler::Get().GetNumForegroundWorkers() +
                            TaskScheduler::Get().GetNumBackgroundWorkers();
            
            if (useAdaptiveSizing)
            {
                // Start with smaller batches for adaptive sizing
                // This allows us to measure work quickly and adjust
                batchSize = std::max(1, count / (static_cast<i32>(numWorkers) * 16));
                batchSize = std::clamp(batchSize, 1, 32);  // Start small
            }
            else
            {
                // Static batch sizing - aim for ~4x worker count for good load balancing
                i32 targetBatches = static_cast<i32>(numWorkers * 4);
                batchSize = std::max(1, count / targetBatches);
                
                // Clamp to reasonable bounds
                batchSize = std::clamp(batchSize, 1, 256);
            }
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

        // Share the function across all tasks using a shared_ptr
        auto sharedFunc = std::make_shared<std::function<void(i32)>>(std::move(func));

        // Adaptive sizing: run a calibration batch first
        i32 currentIndex = 0;
        if (useAdaptiveSizing && count > batchSize * 2)
        {
            // Run calibration batch to measure work time
            auto calibrationStart = std::chrono::high_resolution_clock::now();
            
            i32 calibrationBatchSize = std::min(batchSize, count);
            for (i32 i = 0; i < calibrationBatchSize; ++i)
            {
                (*sharedFunc)(i);
            }
            
            auto calibrationEnd = std::chrono::high_resolution_clock::now();
            auto calibrationTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
                calibrationEnd - calibrationStart).count();
            
            // Adjust batch size to target ~100μs per batch
            // Avoid division by zero
            if (calibrationTimeUs > 0)
            {
                constexpr i64 targetTimeUs = 100;  // Target 100μs per batch
                
                // Calculate time per iteration
                f64 timePerIterationUs = static_cast<f64>(calibrationTimeUs) / static_cast<f64>(calibrationBatchSize);
                
                // Calculate new batch size to hit target time
                if (timePerIterationUs > 0.0)
                {
                    f64 newBatchSizeF = static_cast<f64>(targetTimeUs) / timePerIterationUs;
                    i32 newBatchSize = static_cast<i32>(newBatchSizeF);
                    
                    // Clamp to reasonable range
                    newBatchSize = std::clamp(newBatchSize, 1, 4096);
                    
                    // Don't make batches larger than remaining work
                    batchSize = std::min(newBatchSize, count - calibrationBatchSize);
                }
            }
            
            currentIndex = calibrationBatchSize;
        }

        // Create tasks for remaining batches with adjusted batch size
        std::vector<Ref<Task>> tasks;
        i32 remainingCount = count - currentIndex;
        i32 numBatches = (remainingCount + batchSize - 1) / batchSize;
        tasks.reserve(numBatches);

        for (i32 batch = 0; batch < numBatches; ++batch)
        {
            i32 start = currentIndex + (batch * batchSize);
            i32 end = std::min(start + batchSize, count);

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
        if (!tasks.empty())
        {
            TaskWait::WaitForAll(tasks);
        }
    }

} // namespace OloEngine

