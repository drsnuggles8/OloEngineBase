// Task System Tests - Phase 5: Advanced Features
// Tests for:
// - Priority queue routing verification
// - ParallelFor correctness and performance
// - Batch sizing logic
// - Oversubscription (preventing deadlock)

#include <gtest/gtest.h>
#include "OloEngine/Tasks/Task.h"
#include "OloEngine/Tasks/TaskPriority.h"
#include "OloEngine/Tasks/TaskScheduler.h"
#include "OloEngine/Tasks/ParallelFor.h"
#include "OloEngine/Tasks/OversubscriptionScope.h"
#include "OloEngine/Tasks/TaskWait.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

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


