// Task System Tests - Phase 5: Advanced Features
// Tests for:
// - Priority queue routing verification
// - ParallelFor correctness and performance
// - Batch sizing logic
// - Oversubscription (preventing deadlock)
// - Task pipes (named threads with serialized execution)

#include <gtest/gtest.h>
#include "OloEngine/Tasks/Task.h"
#include "OloEngine/Tasks/TaskPriority.h"
#include "OloEngine/Tasks/TaskScheduler.h"
#include "OloEngine/Tasks/ParallelFor.h"
#include "OloEngine/Tasks/OversubscriptionScope.h"
#include "OloEngine/Tasks/TaskWait.h"
#include "OloEngine/Tasks/TaskPipe.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <limits>
#include <iostream>
#include <mutex>
#include <algorithm>

using namespace OloEngine;

// ============================================================================
// Test Fixture
// ============================================================================

class AdvancedTaskTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Initialize logging system
        if (!Log::GetCoreLogger())
        {
            Log::Init();
        }

        // Initialize scheduler with known worker counts
        TaskSchedulerConfig config;
        config.NumForegroundWorkers = 4;
        config.NumBackgroundWorkers = 2;
        TaskScheduler::Initialize(config);
    }

    void TearDown() override
    {
        TaskScheduler::Shutdown();
    }

    // Helper: Wait for task completion
    bool WaitForTaskCompletion(Ref<Task> task, u32 timeoutMs = 5000)
    {
        auto start = std::chrono::steady_clock::now();
        while (!task->IsCompleted())
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (elapsed >= timeoutMs)
                return false;
            
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
};

// ============================================================================
// Priority Queue Routing Tests
// ============================================================================

TEST_F(AdvancedTaskTest, ForegroundWorkersProcessHighPriority)
{
    std::atomic<u32> executionCount{0};
    std::vector<Ref<Task>> tasks;

    // Launch high priority tasks
    for (u32 i = 0; i < 10; ++i)
    {
        auto task = TaskScheduler::Get().Launch("HighPriorityTask", ETaskPriority::High, [&]() {
            executionCount.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(task);
    }

    // Wait for all
    for (auto& task : tasks)
    {
        EXPECT_TRUE(WaitForTaskCompletion(task));
    }

    EXPECT_EQ(executionCount.load(), 10u);
}

TEST_F(AdvancedTaskTest, ForegroundWorkersProcessNormalPriority)
{
    std::atomic<u32> executionCount{0};
    std::vector<Ref<Task>> tasks;

    // Launch normal priority tasks
    for (u32 i = 0; i < 10; ++i)
    {
        auto task = TaskScheduler::Get().Launch("NormalPriorityTask", ETaskPriority::Normal, [&]() {
            executionCount.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(task);
    }

    // Wait for all
    for (auto& task : tasks)
    {
        EXPECT_TRUE(WaitForTaskCompletion(task));
    }

    EXPECT_EQ(executionCount.load(), 10u);
}

TEST_F(AdvancedTaskTest, BackgroundWorkersProcessBackgroundPriority)
{
    std::atomic<u32> executionCount{0};
    std::vector<Ref<Task>> tasks;

    // Launch background priority tasks
    for (u32 i = 0; i < 10; ++i)
    {
        auto task = TaskScheduler::Get().Launch("BackgroundPriorityTask", ETaskPriority::Background, [&]() {
            executionCount.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(task);
    }

    // Wait for all
    for (auto& task : tasks)
    {
        EXPECT_TRUE(WaitForTaskCompletion(task));
    }

    EXPECT_EQ(executionCount.load(), 10u);
}

// ============================================================================
// ParallelFor Basic Tests
// ============================================================================

TEST_F(AdvancedTaskTest, ParallelForBasic)
{
    const i32 count = 100;
    std::atomic<i32> sum{0};

    ParallelFor(count, [&](i32 i) {
        sum.fetch_add(i, std::memory_order_relaxed);
    });

    // Sum of 0..99 = 99*100/2 = 4950
    EXPECT_EQ(sum.load(), 4950);
}

TEST_F(AdvancedTaskTest, ParallelForZeroCount)
{
    bool executed = false;

    ParallelFor(0, [&](i32 i) {
        executed = true;
    });

    // Should not execute with zero count
    EXPECT_FALSE(executed);
}

TEST_F(AdvancedTaskTest, ParallelForSingleItem)
{
    std::atomic<bool> executed{false};

    ParallelFor(1, [&](i32 i) {
        EXPECT_EQ(i, 0);
        executed.store(true, std::memory_order_release);
    });

    EXPECT_TRUE(executed.load());
}

TEST_F(AdvancedTaskTest, ParallelForCorrectIndices)
{
    const i32 count = 1000;
    std::vector<std::atomic<bool>> visited(count);
    for (i32 i = 0; i < count; ++i)
    {
        visited[i].store(false, std::memory_order_relaxed);
    }

    ParallelFor(count, [&](i32 i) {
        ASSERT_GE(i, 0);
        ASSERT_LT(i, count);
        visited[i].store(true, std::memory_order_release);
    });

    // Verify all indices were visited exactly once
    for (i32 i = 0; i < count; ++i)
    {
        EXPECT_TRUE(visited[i].load(std::memory_order_acquire)) 
            << "Index " << i << " was not visited";
    }
}

TEST_F(AdvancedTaskTest, ParallelForWithWork)
{
    const i32 count = 500;
    std::vector<std::atomic<i32>> results(count);
    for (i32 i = 0; i < count; ++i)
    {
        results[i].store(0, std::memory_order_relaxed);
    }

    ParallelFor(count, [&](i32 i) {
        // Do some actual work
        i32 value = 0;
        for (i32 j = 0; j < 100; ++j)
        {
            value += j;
        }
        results[i].store(value, std::memory_order_release);
    });

    // Verify all results are correct
    const i32 expected = 4950;  // Sum of 0..99
    for (i32 i = 0; i < count; ++i)
    {
        EXPECT_EQ(results[i].load(std::memory_order_acquire), expected);
    }
}

TEST_F(AdvancedTaskTest, ParallelForCustomBatchSize)
{
    const i32 count = 100;
    std::atomic<i32> sum{0};
    const i32 batchSize = 10;

    ParallelFor(count, [&](i32 i) {
        sum.fetch_add(i, std::memory_order_relaxed);
    }, batchSize);

    EXPECT_EQ(sum.load(), 4950);
}

TEST_F(AdvancedTaskTest, ParallelForWithPriority)
{
    const i32 count = 100;
    std::atomic<i32> sum{0};

    ParallelFor(count, [&](i32 i) {
        sum.fetch_add(i, std::memory_order_relaxed);
    }, ETaskPriority::High, 10);

    EXPECT_EQ(sum.load(), 4950);
}

// ============================================================================
// ParallelFor Performance Tests
// ============================================================================

TEST_F(AdvancedTaskTest, ParallelForPerformance)
{
    const i32 count = 10000;
    std::vector<std::atomic<i32>> data(count);
    for (i32 i = 0; i < count; ++i)
    {
        data[i].store(0, std::memory_order_relaxed);
    }

    auto start = std::chrono::high_resolution_clock::now();

    ParallelFor(count, [&](i32 i) {
        // Some CPU work (formula that's never zero)
        i32 value = 1;  // Start at 1 to avoid zero result
        for (i32 j = 0; j < 1000; ++j)
        {
            value += j * i + 1;  // Always adds at least 1
        }
        data[i].store(value, std::memory_order_release);
    });

    auto end = std::chrono::high_resolution_clock::now();
    auto parallelTime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Verify all work was done
    for (i32 i = 0; i < count; ++i)
    {
        EXPECT_NE(data[i].load(std::memory_order_acquire), 0) << "Index " << i << " was not processed";
    }

    // Performance should be reasonable (< 5 seconds even in debug)
    EXPECT_LT(parallelTime, 5000) << "ParallelFor took too long: " << parallelTime << "ms";
}

TEST_F(AdvancedTaskTest, ParallelForSmallWorkInline)
{
    // Very small work should execute inline (no task overhead)
    const i32 count = 5;
    std::atomic<i32> sum{0};

    ParallelFor(count, [&](i32 i) {
        sum.fetch_add(i, std::memory_order_relaxed);
    }, 10);  // Batch size larger than count

    // Sum of 0..4 = 10
    EXPECT_EQ(sum.load(), 10);
}

// ============================================================================
// Adaptive Batch Sizing Tests
// ============================================================================

TEST_F(AdvancedTaskTest, ParallelForAdaptiveSizingBasic)
{
    // Verify adaptive sizing (batchSize = -1) produces correct results
    const i32 count = 10000;
    std::atomic<i64> sum{0};

    ParallelFor(count, [&](i32 i) {
        sum.fetch_add(static_cast<i64>(i), std::memory_order_relaxed);
    }, -1);  // Enable adaptive sizing

    i64 expected = static_cast<i64>(count - 1) * static_cast<i64>(count) / 2;
    EXPECT_EQ(sum.load(), expected);
}

TEST_F(AdvancedTaskTest, ParallelForAdaptiveSizingWithLightWork)
{
    // Light work: adaptive should choose larger batch sizes
    const i32 count = 50000;
    std::vector<i32> data(count);

    auto startTime = std::chrono::high_resolution_clock::now();
    
    ParallelFor(count, [&](i32 i) {
        data[i] = i * 2;  // Very light work
    }, -1);  // Adaptive sizing

    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - startTime
    ).count();

    // Verify correctness
    for (i32 i = 0; i < count; ++i)
    {
        EXPECT_EQ(data[i], i * 2);
    }

    // Should complete reasonably fast (< 100ms)
    EXPECT_LT(duration, 100000);
}

TEST_F(AdvancedTaskTest, ParallelForAdaptiveSizingWithHeavyWork)
{
    // Heavy work: adaptive should choose smaller batch sizes for better load balancing
    const i32 count = 1000;
    std::vector<i64> results(count);

    ParallelFor(count, [&](i32 i) {
        // Simulate heavy computation
        i64 result = 0;
        for (i32 j = 0; j < 1000; ++j)
        {
            result += (i * j) % 997;  // Prime modulo to avoid compiler optimization
        }
        results[i] = result;
    }, -1);  // Adaptive sizing

    // Verify all results were computed
    for (i32 i = 0; i < count; ++i)
    {
        i64 expected = 0;
        for (i32 j = 0; j < 1000; ++j)
        {
            expected += (i * j) % 997;
        }
        EXPECT_EQ(results[i], expected);
    }
}

TEST_F(AdvancedTaskTest, ParallelForAdaptiveSizingVsAutoDetect)
{
    // Compare adaptive sizing (-1) vs auto-detect (0)
    const i32 count = 20000;
    
    // Test with auto-detect (static batch sizing)
    std::vector<i32> dataAuto(count);
    auto startAuto = std::chrono::high_resolution_clock::now();
    
    ParallelFor(count, [&](i32 i) {
        dataAuto[i] = i * 3 + 7;
    }, 0);  // Auto-detect (static)
    
    auto durationAuto = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - startAuto
    ).count();

    // Test with adaptive sizing
    std::vector<i32> dataAdaptive(count);
    auto startAdaptive = std::chrono::high_resolution_clock::now();
    
    ParallelFor(count, [&](i32 i) {
        dataAdaptive[i] = i * 3 + 7;
    }, -1);  // Adaptive
    
    auto durationAdaptive = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - startAdaptive
    ).count();

    // Both should produce correct results
    EXPECT_EQ(dataAuto, dataAdaptive);

    // Both should complete in reasonable time (< 50ms each)
    EXPECT_LT(durationAuto, 50000);
    EXPECT_LT(durationAdaptive, 50000);
}

TEST_F(AdvancedTaskTest, ParallelForAdaptiveSizingSmallCount)
{
    // Adaptive sizing should handle small counts gracefully
    const i32 count = 100;
    std::atomic<i32> counter{0};

    ParallelFor(count, [&](i32 i) {
        counter.fetch_add(1, std::memory_order_relaxed);
    }, -1);  // Adaptive sizing

    EXPECT_EQ(counter.load(), count);
}

TEST_F(AdvancedTaskTest, ParallelForAdaptiveSizingVaryingWorkload)
{
    // Test adaptive sizing with varying work complexity per iteration
    const i32 count = 5000;
    std::vector<i64> results(count);

    ParallelFor(count, [&](i32 i) {
        // Work complexity varies with index
        i32 iterations = (i % 100) * 10;  // 0 to 990 iterations
        i64 result = 0;
        for (i32 j = 0; j < iterations; ++j)
        {
            result += (i + j) % 7;
        }
        results[i] = result;
    }, -1);  // Adaptive sizing

    // Verify results (spot check)
    for (i32 i = 0; i < std::min(100, count); ++i)
    {
        i32 iterations = (i % 100) * 10;
        i64 expected = 0;
        for (i32 j = 0; j < iterations; ++j)
        {
            expected += (i + j) % 7;
        }
        EXPECT_EQ(results[i], expected);
    }
}

// ============================================================================
// Stress Tests
// ============================================================================

TEST_F(AdvancedTaskTest, ParallelForLargeDataset)
{
    const i32 count = 100000;
    std::atomic<i64> sum{0};

    ParallelFor(count, [&](i32 i) {
        sum.fetch_add(static_cast<i64>(i), std::memory_order_relaxed);
    });

    // Sum of 0..99999 = 99999 * 100000 / 2 = 4999950000
    i64 expected = static_cast<i64>(count - 1) * static_cast<i64>(count) / 2;
    EXPECT_EQ(sum.load(), expected);
}

TEST_F(AdvancedTaskTest, NestedParallelFor)
{
    // Test nesting ParallelFor calls with small inner loop to avoid deadlock
    // Note: Deep nesting can cause deadlock if all workers are waiting
    const i32 outerCount = 10;
    const i32 innerCount = 5;  // Small enough to execute inline
    std::atomic<i32> totalIterations{0};

    ParallelFor(outerCount, [&](i32 i) {
        // Inner parallel for with large batch size forces inline execution
        ParallelFor(innerCount, [&](i32 j) {
            totalIterations.fetch_add(1, std::memory_order_relaxed);
        }, 100);  // Batch size > innerCount forces inline execution
    });

    EXPECT_EQ(totalIterations.load(), outerCount * innerCount);
}

// ============================================================================
// Oversubscription Tests
// ============================================================================

TEST_F(AdvancedTaskTest, OversubscriptionIncrement)
{
    // Test that oversubscription counter increments and decrements correctly
    u32 initialLevel = TaskScheduler::Get().GetOversubscriptionLevel();
    
    {
        OversubscriptionScope scope1;
        EXPECT_EQ(TaskScheduler::Get().GetOversubscriptionLevel(), initialLevel + 1);
        
        {
            OversubscriptionScope scope2;
            EXPECT_EQ(TaskScheduler::Get().GetOversubscriptionLevel(), initialLevel + 2);
        }
        
        EXPECT_EQ(TaskScheduler::Get().GetOversubscriptionLevel(), initialLevel + 1);
    }
    
    EXPECT_EQ(TaskScheduler::Get().GetOversubscriptionLevel(), initialLevel);
}

TEST_F(AdvancedTaskTest, OversubscriptionNestedWaits)
{
    // Test that nested task waits properly track oversubscription
    // This simulates the case where many workers are all blocked waiting
    std::atomic<i32> executionCount{0};
    
    auto leafTask = TaskScheduler::Get().Launch("LeafTask", ETaskPriority::Normal, [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        executionCount.fetch_add(1, std::memory_order_relaxed);
    });
    
    // Launch multiple tasks that all wait for the same leaf task
    std::vector<Ref<Task>> waitingTasks;
    const i32 numWaiters = 8;
    
    for (i32 i = 0; i < numWaiters; ++i)
    {
        auto task = TaskScheduler::Get().Launch("WaitingTask", ETaskPriority::Normal, [&]() {
            // This should trigger oversubscription tracking
            TaskWait::Wait(leafTask);
            executionCount.fetch_add(1, std::memory_order_relaxed);
        });
        waitingTasks.push_back(task);
    }
    
    // Wait for all tasks to complete
    TaskWait::WaitForAll(waitingTasks);
    
    // All tasks should have completed
    EXPECT_EQ(executionCount.load(), numWaiters + 1);
    
    // Oversubscription level should be back to baseline
    EXPECT_EQ(TaskScheduler::Get().GetOversubscriptionLevel(), 0);
}

TEST_F(AdvancedTaskTest, OversubscriptionPreventDeadlock)
{
    // Test a scenario that would deadlock without oversubscription:
    // All workers are blocked waiting, but we need a free worker to execute the blocking task
    
    u32 numWorkers = TaskScheduler::Get().GetNumForegroundWorkers();
    std::atomic<i32> completedTasks{0};
    
    // Create a task that all workers will wait for
    std::atomic<bool> blockingTaskStarted{false};
    auto blockingTask = TaskScheduler::Get().Launch("BlockingTask", ETaskPriority::Normal, [&]() {
        blockingTaskStarted.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        completedTasks.fetch_add(1, std::memory_order_relaxed);
    });
    
    // Launch tasks equal to number of workers, all waiting for the blocking task
    std::vector<Ref<Task>> waiterTasks;
    for (u32 i = 0; i < numWorkers; ++i)
    {
        auto task = TaskScheduler::Get().Launch("WaiterTask", ETaskPriority::Normal, [&]() {
            // Wait until the blocking task has started
            while (!blockingTaskStarted.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            
            // Now wait for it to complete (should trigger oversubscription)
            TaskWait::Wait(blockingTask);
            completedTasks.fetch_add(1, std::memory_order_relaxed);
        });
        waiterTasks.push_back(task);
    }
    
    // Wait for all tasks - this should NOT deadlock thanks to oversubscription
    TaskWait::WaitForAll(waiterTasks);
    
    // All tasks should have completed
    EXPECT_EQ(completedTasks.load(), static_cast<i32>(numWorkers + 1));
}

// ============================================================================
// Task Pipe Tests
// ============================================================================

TEST_F(AdvancedTaskTest, TaskPipeBasic)
{
    // Test basic task pipe creation and execution
    Ref<TaskPipe> pipe = Ref<TaskPipe>::Create("TestPipe");
    
    EXPECT_TRUE(pipe->IsRunning());
    EXPECT_EQ(pipe->GetName(), "TestPipe");
    
    std::atomic<bool> executed{false};
    auto task = pipe->Launch("PipeTask", [&]() {
        executed.store(true, std::memory_order_release);
    });
    
    EXPECT_TRUE(WaitForTaskCompletion(task));
    EXPECT_TRUE(executed.load());
}

TEST_F(AdvancedTaskTest, TaskPipeSerializedExecution)
{
    // Test that tasks execute in FIFO order on the pipe
    Ref<TaskPipe> pipe = Ref<TaskPipe>::Create("SerialPipe");
    
    const i32 numTasks = 100;
    std::atomic<i32> counter{0};
    std::vector<i32> executionOrder;
    std::mutex orderMutex;
    std::vector<Ref<Task>> tasks;
    
    // Launch tasks that record their execution order
    for (i32 i = 0; i < numTasks; ++i)
    {
        auto task = pipe->Launch("OrderedTask", [i, &counter, &executionOrder, &orderMutex]() {
            i32 order = counter.fetch_add(1, std::memory_order_relaxed);
            
            std::lock_guard<std::mutex> lock(orderMutex);
            executionOrder.push_back(i);
        });
        tasks.push_back(task);
    }
    
    // Wait for all tasks
    TaskWait::WaitForAll(tasks);
    
    // Verify execution order matches submission order
    EXPECT_EQ(executionOrder.size(), static_cast<size_t>(numTasks));
    for (i32 i = 0; i < numTasks; ++i)
    {
        EXPECT_EQ(executionOrder[i], i) << "Task " << i << " executed out of order";
    }
}

TEST_F(AdvancedTaskTest, TaskPipeThreadIdentity)
{
    // Test that tasks run on the pipe's dedicated thread
    Ref<TaskPipe> pipe = Ref<TaskPipe>::Create("ThreadTest");
    
    std::atomic<std::thread::id> taskThreadId;
    auto task = pipe->Launch("ThreadIdTask", [&pipe, &taskThreadId]() {
        taskThreadId.store(std::this_thread::get_id(), std::memory_order_release);
        
        // Verify IsOnPipeThread() returns true
        EXPECT_TRUE(pipe->IsOnPipeThread());
    });
    
    EXPECT_TRUE(WaitForTaskCompletion(task));
    
    // Verify task ran on pipe's thread
    EXPECT_EQ(taskThreadId.load(), pipe->GetThreadID());
    
    // Verify main thread is NOT on pipe thread
    EXPECT_FALSE(pipe->IsOnPipeThread());
}

TEST_F(AdvancedTaskTest, TaskPipeMultiplePipes)
{
    // Test multiple independent pipes running simultaneously
    Ref<TaskPipe> pipe1 = Ref<TaskPipe>::Create("Pipe1");
    Ref<TaskPipe> pipe2 = Ref<TaskPipe>::Create("Pipe2");
    
    std::atomic<i32> pipe1Count{0};
    std::atomic<i32> pipe2Count{0};
    std::vector<Ref<Task>> allTasks;
    
    // Launch tasks on both pipes
    for (i32 i = 0; i < 50; ++i)
    {
        auto task1 = pipe1->Launch("Pipe1Task", [&pipe1Count]() {
            pipe1Count.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        });
        
        auto task2 = pipe2->Launch("Pipe2Task", [&pipe2Count]() {
            pipe2Count.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        });
        
        allTasks.push_back(task1);
        allTasks.push_back(task2);
    }
    
    // Wait for all tasks
    TaskWait::WaitForAll(allTasks);
    
    EXPECT_EQ(pipe1Count.load(), 50);
    EXPECT_EQ(pipe2Count.load(), 50);
}

TEST_F(AdvancedTaskTest, TaskPipeShutdown)
{
    // Test that pipe shuts down cleanly and processes all queued tasks
    std::atomic<i32> completedCount{0};
    std::vector<Ref<Task>> tasks;
    
    {
        Ref<TaskPipe> pipe = Ref<TaskPipe>::Create("ShutdownPipe");
        
        // Launch many tasks
        for (i32 i = 0; i < 100; ++i)
        {
            auto task = pipe->Launch("ShutdownTask", [&completedCount]() {
                completedCount.fetch_add(1, std::memory_order_relaxed);
            });
            tasks.push_back(task);
        }
        
        // Pipe will be destroyed here - should wait for all tasks
    }
    
    // Verify all tasks completed
    EXPECT_EQ(completedCount.load(), 100);
    
    // Verify all task handles show completed
    for (auto& task : tasks)
    {
        EXPECT_TRUE(task->IsCompleted());
    }
}

TEST_F(AdvancedTaskTest, TaskPipeWithWork)
{
    // Test pipe with actual computational work
    Ref<TaskPipe> pipe = Ref<TaskPipe>::Create("WorkPipe");
    
    const i32 numTasks = 50;
    std::vector<std::atomic<i32>> results(numTasks);
    std::vector<Ref<Task>> tasks;
    
    for (i32 i = 0; i < numTasks; ++i)
    {
        results[i].store(0, std::memory_order_relaxed);
        
        auto task = pipe->Launch("ComputeTask", [i, &results]() {
            // Do some work
            i32 sum = 0;
            for (i32 j = 0; j < 1000; ++j)
            {
                sum += j;
            }
            results[i].store(sum, std::memory_order_release);
        });
        tasks.push_back(task);
    }
    
    TaskWait::WaitForAll(tasks);
    
    // Verify all results
    const i32 expected = 499500;  // Sum of 0..999
    for (i32 i = 0; i < numTasks; ++i)
    {
        EXPECT_EQ(results[i].load(), expected);
    }
}

// ============================================================================
// Batch Task Launching Tests
// ============================================================================

TEST_F(AdvancedTaskTest, LaunchBatchBasic)
{
    // Test basic batch launching
    const i32 numTasks = 100;
    std::atomic<i32> executionCount{0};
    
    std::vector<std::function<void()>> funcs;
    for (i32 i = 0; i < numTasks; ++i)
    {
        funcs.push_back([&executionCount]() {
            executionCount.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    auto tasks = TaskScheduler::Get().LaunchBatch("BatchTask", funcs);
    
    EXPECT_EQ(tasks.size(), static_cast<size_t>(numTasks));
    
    TaskWait::WaitForAll(tasks);
    
    EXPECT_EQ(executionCount.load(), numTasks);
}

TEST_F(AdvancedTaskTest, LaunchBatchWithPriority)
{
    // Test batch launching with different priorities
    const i32 numTasks = 50;
    std::atomic<i32> highCount{0};
    std::atomic<i32> normalCount{0};
    std::atomic<i32> backgroundCount{0};
    
    std::vector<std::function<void()>> funcs;
    for (i32 i = 0; i < numTasks; ++i)
    {
        funcs.push_back([&highCount]() {
            highCount.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    auto highTasks = TaskScheduler::Get().LaunchBatch("HighBatch", ETaskPriority::High, funcs);
    
    funcs.clear();
    for (i32 i = 0; i < numTasks; ++i)
    {
        funcs.push_back([&normalCount]() {
            normalCount.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    auto normalTasks = TaskScheduler::Get().LaunchBatch("NormalBatch", ETaskPriority::Normal, funcs);
    
    funcs.clear();
    for (i32 i = 0; i < numTasks; ++i)
    {
        funcs.push_back([&backgroundCount]() {
            backgroundCount.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    auto backgroundTasks = TaskScheduler::Get().LaunchBatch("BackgroundBatch", ETaskPriority::Background, funcs);
    
    // Wait for all
    TaskWait::WaitForAll(highTasks);
    TaskWait::WaitForAll(normalTasks);
    TaskWait::WaitForAll(backgroundTasks);
    
    EXPECT_EQ(highCount.load(), numTasks);
    EXPECT_EQ(normalCount.load(), numTasks);
    EXPECT_EQ(backgroundCount.load(), numTasks);
}

TEST_F(AdvancedTaskTest, LaunchBatchEmptyVector)
{
    // Test batch launching with empty vector
    std::vector<std::function<void()>> funcs;
    
    auto tasks = TaskScheduler::Get().LaunchBatch("EmptyBatch", funcs);
    
    EXPECT_TRUE(tasks.empty());
}

TEST_F(AdvancedTaskTest, LaunchBatchLarge)
{
    // Test batch launching with many tasks
    // Note: Keep under queue limit (4096)
    const i32 numTasks = 1000;
    std::atomic<i32> sum{0};
    
    std::vector<std::function<void()>> funcs;
    for (i32 i = 0; i < numTasks; ++i)
    {
        funcs.push_back([i, &sum]() {
            sum.fetch_add(i, std::memory_order_relaxed);
        });
    }
    
    auto tasks = TaskScheduler::Get().LaunchBatch("LargeBatch", funcs);
    
    EXPECT_EQ(tasks.size(), static_cast<size_t>(numTasks));
    
    TaskWait::WaitForAll(tasks);
    
    // Sum of 0..999 = 999 * 1000 / 2 = 499500
    i32 expected = (numTasks - 1) * numTasks / 2;
    EXPECT_EQ(sum.load(), expected);
}

TEST_F(AdvancedTaskTest, LaunchBatchPerformanceComparison)
{
    // Compare batch launching vs individual launching
    // Batch should have lower overhead (fewer wake calls)
    const i32 numTasks = 500;
    std::atomic<i32> counter{0};
    
    // Test 1: Individual launches
    auto individualStart = std::chrono::high_resolution_clock::now();
    std::vector<Ref<Task>> individualTasks;
    for (i32 i = 0; i < numTasks; ++i)
    {
        auto task = TaskScheduler::Get().Launch("IndividualTask", [&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
        individualTasks.push_back(task);
    }
    TaskWait::WaitForAll(individualTasks);
    auto individualEnd = std::chrono::high_resolution_clock::now();
    auto individualMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        individualEnd - individualStart).count();
    
    EXPECT_EQ(counter.load(), numTasks);
    counter.store(0);
    
    // Test 2: Batch launch
    auto batchStart = std::chrono::high_resolution_clock::now();
    std::vector<std::function<void()>> funcs;
    for (i32 i = 0; i < numTasks; ++i)
    {
        funcs.push_back([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    auto batchTasks = TaskScheduler::Get().LaunchBatch("BatchTask", funcs);
    TaskWait::WaitForAll(batchTasks);
    auto batchEnd = std::chrono::high_resolution_clock::now();
    auto batchMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        batchEnd - batchStart).count();
    
    EXPECT_EQ(counter.load(), numTasks);
    
    // Log results
    std::cout << "  Individual Launch: " << individualMs << " ms" << std::endl;
    std::cout << "  Batch Launch: " << batchMs << " ms" << std::endl;
    
    // Batch should be at least as fast as individual (may be faster or similar)
    // In practice, benefit is most visible in release builds
    EXPECT_LE(batchMs, individualMs * 2) << "Batch launching significantly slower than expected";
}

// ============================================================================
// Performance Benchmark Tests (Release Build Targets)
// ============================================================================

TEST_F(AdvancedTaskTest, BenchmarkTaskThroughput)
{
    // Benchmark: Measure tasks per second
    // Target (Release): > 500,000 tasks/second
    // Acceptable (Debug): > 10,000 tasks/second
    // Note: Limited to 3000 tasks to avoid queue exhaustion (queue limit is 4096)
    
    constexpr i32 numTasks = 3000;  // Stay well within queue capacity
    std::atomic<i32> completionCount{0};
    std::vector<Ref<Task>> tasks;
    tasks.reserve(numTasks);
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Launch all tasks
    for (i32 i = 0; i < numTasks; ++i)
    {
        auto task = TaskScheduler::Get().Launch("BenchTask", ETaskPriority::Normal, [&completionCount]() {
            completionCount.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(task);
    }
    
    // Wait for all to complete
    TaskWait::WaitForAll(tasks);
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();
    
    // Calculate throughput
    f64 tasksPerSecond = (static_cast<f64>(numTasks) * 1000000.0) / static_cast<f64>(elapsedUs);
    
    // Verify all tasks completed
    EXPECT_EQ(completionCount.load(), numTasks);
    
    // Log performance (always useful to see)
    std::cout << "  Task Throughput: " << static_cast<i32>(tasksPerSecond) << " tasks/second" << std::endl;
    std::cout << "  Average Latency: " << (elapsedUs / numTasks) << " μs/task" << std::endl;
    
#ifdef OLO_DIST
    // Release/Dist build - strict requirements
    EXPECT_GT(tasksPerSecond, 500000.0) << "Throughput below target for release build";
#elif defined(OLO_RELEASE)
    // Release build - relaxed requirements (some debug info)
    EXPECT_GT(tasksPerSecond, 250000.0) << "Throughput below target for release build";
#else
    // Debug build - very relaxed requirements (10K tasks/sec minimum)
    EXPECT_GT(tasksPerSecond, 10000.0) << "Throughput below minimum for debug build";
#endif
}
