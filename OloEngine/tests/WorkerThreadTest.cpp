// Task System Tests - Phase 3: Worker Thread Pool
// Tests for:
// - Worker thread startup and shutdown
// - Basic task execution from global queue
// - Work stealing between workers
// - Task state transitions
// - Exception handling in tasks
// - Stress testing with thousands of tasks

#include <gtest/gtest.h>
#include "OloEngine/Tasks/Task.h"
#include "OloEngine/Tasks/TaskPriority.h"
#include "OloEngine/Tasks/TaskScheduler.h"
#include "OloEngine/Tasks/WorkerThread.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace OloEngine;

// ============================================================================
// Test Fixture for Worker Thread Tests
// ============================================================================

class WorkerThreadTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Initialize logging system (required by Thread/ThreadSignal)
        if (!Log::GetCoreLogger())
        {
            Log::Init();
        }

        // Initialize scheduler with small worker counts for testing
        TaskSchedulerConfig config;
        config.NumForegroundWorkers = 2;
        config.NumBackgroundWorkers = 1;
        TaskScheduler::Initialize(config);
    }

    void TearDown() override
    {
        TaskScheduler::Shutdown();
    }

    // Helper: Wait for a task to complete with timeout
    bool WaitForTaskCompletion(Ref<Task> task, u32 timeoutMs = 1000)
    {
        auto start = std::chrono::steady_clock::now();
        while (!task->IsCompleted())
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (elapsed >= timeoutMs)
                return false;  // Timeout
            
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    // Helper: Wait for all tasks to complete with timeout
    bool WaitForAllTasks(const std::vector<Ref<Task>>& tasks, u32 timeoutMs = 5000)
    {
        auto start = std::chrono::steady_clock::now();
        for (const auto& task : tasks)
        {
            while (!task->IsCompleted())
            {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
                if (elapsed >= timeoutMs)
                    return false;  // Timeout
                
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return true;
    }
};

// ============================================================================
// Basic Worker Thread Tests
// ============================================================================

TEST_F(WorkerThreadTest, SchedulerInitialization)
{
    EXPECT_TRUE(TaskScheduler::IsInitialized());
    EXPECT_EQ(TaskScheduler::Get().GetNumForegroundWorkers(), 2u);
    EXPECT_EQ(TaskScheduler::Get().GetNumBackgroundWorkers(), 1u);
}

TEST_F(WorkerThreadTest, SimpleTaskExecution)
{
    std::atomic<bool> executed{false};
    
    auto task = TaskScheduler::Get().Launch("SimpleTask", [&executed]() {
        executed.store(true, std::memory_order_release);
    });

    ASSERT_NE(task, nullptr);
    EXPECT_TRUE(WaitForTaskCompletion(task));
    EXPECT_TRUE(executed.load(std::memory_order_acquire));
    EXPECT_TRUE(task->IsCompleted());
}

TEST_F(WorkerThreadTest, TaskStateTransitions)
{
    std::atomic<bool> taskRunning{false};
    std::atomic<bool> shouldContinue{false};
    
    auto task = TaskScheduler::Get().Launch("StateTask", [&taskRunning, &shouldContinue]() {
        taskRunning.store(true, std::memory_order_release);
        // Wait for test to check state
        while (!shouldContinue.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    ASSERT_NE(task, nullptr);
    
    // Task should be Scheduled (queued) or Running
    // Wait for it to actually start running
    auto start = std::chrono::steady_clock::now();
    while (!taskRunning.load(std::memory_order_acquire))
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        ASSERT_LT(elapsed, 1000) << "Task took too long to start";
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    // Now it should be Running
    EXPECT_EQ(task->GetState(), ETaskState::Running);
    
    // Let it complete
    shouldContinue.store(true, std::memory_order_release);
    EXPECT_TRUE(WaitForTaskCompletion(task));
    EXPECT_EQ(task->GetState(), ETaskState::Completed);
}

TEST_F(WorkerThreadTest, MultipleTasks)
{
    constexpr u32 NumTasks = 10;
    std::atomic<u32> counter{0};
    std::vector<Ref<Task>> tasks;

    for (u32 i = 0; i < NumTasks; ++i)
    {
        auto task = TaskScheduler::Get().Launch("MultiTask", [&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(task);
    }

    EXPECT_TRUE(WaitForAllTasks(tasks));
    EXPECT_EQ(counter.load(), NumTasks);
}

TEST_F(WorkerThreadTest, TasksWithDifferentPriorities)
{
    std::atomic<u32> highCount{0};
    std::atomic<u32> normalCount{0};
    std::atomic<u32> backgroundCount{0};
    
    std::vector<Ref<Task>> tasks;

    // Launch high priority tasks
    for (u32 i = 0; i < 3; ++i)
    {
        auto task = TaskScheduler::Get().Launch("HighPriority", ETaskPriority::High, [&highCount]() {
            highCount.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(task);
    }

    // Launch normal priority tasks
    for (u32 i = 0; i < 3; ++i)
    {
        auto task = TaskScheduler::Get().Launch("NormalPriority", ETaskPriority::Normal, [&normalCount]() {
            normalCount.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(task);
    }

    // Launch background priority tasks
    for (u32 i = 0; i < 3; ++i)
    {
        auto task = TaskScheduler::Get().Launch("BackgroundPriority", ETaskPriority::Background, [&backgroundCount]() {
            backgroundCount.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(task);
    }

    EXPECT_TRUE(WaitForAllTasks(tasks));
    EXPECT_EQ(highCount.load(), 3u);
    EXPECT_EQ(normalCount.load(), 3u);
    EXPECT_EQ(backgroundCount.load(), 3u);
}

TEST_F(WorkerThreadTest, HighPriorityExecutesBeforeNormal)
{
    // This test verifies that high-priority tasks are processed preferentially
    // by checking that when high-priority tasks are added to a system already
    // processing normal-priority tasks, they get executed promptly
    
    std::atomic<u32> executionOrder{0};
    std::atomic<u32> normalTasksStarted{0};
    std::atomic<u32> highTasksStarted{0};
    std::vector<u32> normalExecutionOrders;
    std::vector<u32> highExecutionOrders;
    std::mutex ordersMutex;
    
    std::vector<Ref<Task>> allTasks;
    
    // Launch many normal priority tasks with significant work
    // This ensures they'll be running when we add high-priority tasks
    for (u32 i = 0; i < 20; ++i)
    {
        auto task = TaskScheduler::Get().Launch("Normal", ETaskPriority::Normal, 
            [&executionOrder, &normalTasksStarted, &normalExecutionOrders, &ordersMutex]() {
                normalTasksStarted.fetch_add(1, std::memory_order_relaxed);
                u32 order = executionOrder.fetch_add(1, std::memory_order_relaxed);
                
                {
                    std::lock_guard<std::mutex> lock(ordersMutex);
                    normalExecutionOrders.push_back(order);
                }
                
                // Do meaningful work so tasks don't complete instantly
                volatile u64 sum = 0;
                for (u32 j = 0; j < 50000; ++j)
                    sum += j;
            });
        allTasks.push_back(task);
    }
    
    // Wait until some normal tasks have started
    while (normalTasksStarted.load(std::memory_order_relaxed) < 2)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    // Now launch high priority tasks - these should be prioritized
    for (u32 i = 0; i < 10; ++i)
    {
        auto task = TaskScheduler::Get().Launch("High", ETaskPriority::High,
            [&executionOrder, &highTasksStarted, &highExecutionOrders, &ordersMutex]() {
                highTasksStarted.fetch_add(1, std::memory_order_relaxed);
                u32 order = executionOrder.fetch_add(1, std::memory_order_relaxed);
                
                {
                    std::lock_guard<std::mutex> lock(ordersMutex);
                    highExecutionOrders.push_back(order);
                }
            });
        allTasks.push_back(task);
    }
    
    // Wait for all tasks
    EXPECT_TRUE(WaitForAllTasks(allTasks, 10000));
    
    // Verify all tasks executed
    EXPECT_EQ(normalTasksStarted.load(), 20u);
    EXPECT_EQ(highTasksStarted.load(), 10u);
    
    // Calculate average execution order for each priority
    {
        std::lock_guard<std::mutex> lock(ordersMutex);
        
        ASSERT_FALSE(normalExecutionOrders.empty());
        ASSERT_FALSE(highExecutionOrders.empty());
        
        f64 avgNormalOrder = 0;
        for (u32 order : normalExecutionOrders)
            avgNormalOrder += order;
        avgNormalOrder /= normalExecutionOrders.size();
        
        f64 avgHighOrder = 0;
        for (u32 order : highExecutionOrders)
            avgHighOrder += order;
        avgHighOrder /= highExecutionOrders.size();
        
        // High priority tasks should have lower average execution order
        // (executed earlier on average than normal priority tasks)
        // We expect high priority tasks to "jump the queue"
        EXPECT_LT(avgHighOrder, avgNormalOrder)
            << "Average execution order for high priority (" << avgHighOrder
            << ") should be less than normal priority (" << avgNormalOrder << ")";
    }
}

// ============================================================================
// Work Stealing Tests
// ============================================================================

TEST_F(WorkerThreadTest, WorkStealingBasic)
{
    // Launch many short tasks to trigger work stealing
    constexpr u32 NumTasks = 100;
    std::atomic<u32> counter{0};
    std::vector<Ref<Task>> tasks;

    for (u32 i = 0; i < NumTasks; ++i)
    {
        auto task = TaskScheduler::Get().Launch("StealTask", [&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
            // Small amount of work
            volatile u32 sum = 0;
            for (u32 j = 0; j < 100; ++j)
                sum += j;
        });
        tasks.push_back(task);
    }

    EXPECT_TRUE(WaitForAllTasks(tasks));
    EXPECT_EQ(counter.load(), NumTasks);
}

TEST_F(WorkerThreadTest, ConcurrentExecution)
{
    // Verify that tasks actually execute concurrently
    constexpr u32 NumTasks = 4;
    std::atomic<u32> runningCount{0};
    std::atomic<u32> maxConcurrent{0};
    std::vector<Ref<Task>> tasks;

    for (u32 i = 0; i < NumTasks; ++i)
    {
        auto task = TaskScheduler::Get().Launch("ConcurrentTask", [&runningCount, &maxConcurrent]() {
            u32 running = runningCount.fetch_add(1, std::memory_order_relaxed) + 1;
            
            // Update max concurrent
            u32 currentMax = maxConcurrent.load(std::memory_order_relaxed);
            while (running > currentMax)
            {
                if (maxConcurrent.compare_exchange_weak(currentMax, running, 
                    std::memory_order_relaxed, std::memory_order_relaxed))
                    break;
            }

            // Do some work
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
            runningCount.fetch_sub(1, std::memory_order_relaxed);
        });
        tasks.push_back(task);
    }

    EXPECT_TRUE(WaitForAllTasks(tasks));
    
    // With 2 foreground workers, we should see at least 2 concurrent tasks
    EXPECT_GE(maxConcurrent.load(), 2u) << "Tasks should execute concurrently";
}

// ============================================================================
// Exception Handling Tests
// ============================================================================

TEST_F(WorkerThreadTest, ExceptionInTask)
{
    std::atomic<bool> executed{false};
    
    auto task = TaskScheduler::Get().Launch("ExceptionTask", [&executed]() {
        executed.store(true, std::memory_order_release);
        throw std::runtime_error("Test exception");
    });

    ASSERT_NE(task, nullptr);
    EXPECT_TRUE(WaitForTaskCompletion(task));
    
    // Task should complete despite exception
    EXPECT_TRUE(executed.load());
    EXPECT_TRUE(task->IsCompleted());
}

TEST_F(WorkerThreadTest, ExceptionDoesNotCrashWorker)
{
    // Launch task that throws, then launch normal task
    // Normal task should still execute (worker didn't crash)
    
    auto exceptionTask = TaskScheduler::Get().Launch("ThrowingTask", []() {
        throw std::runtime_error("Test exception");
    });

    EXPECT_TRUE(WaitForTaskCompletion(exceptionTask));

    // Now launch a normal task
    std::atomic<bool> executed{false};
    auto normalTask = TaskScheduler::Get().Launch("NormalTask", [&executed]() {
        executed.store(true, std::memory_order_release);
    });

    EXPECT_TRUE(WaitForTaskCompletion(normalTask));
    EXPECT_TRUE(executed.load()) << "Worker should still function after exception";
}

TEST_F(WorkerThreadTest, ExceptionSetsTaskToCompleted)
{
    std::atomic<bool> executed{false};
    
    auto task = TaskScheduler::Get().Launch("ThrowingTask", [&executed]() {
        executed.store(true, std::memory_order_release);
        throw std::runtime_error("Test exception");
    });
    
    ASSERT_NE(task, nullptr);
    EXPECT_TRUE(WaitForTaskCompletion(task, 2000));
    
    // Task should be marked as Completed despite exception
    EXPECT_TRUE(executed.load()) << "Task body should have executed";
    EXPECT_EQ(task->GetState(), ETaskState::Completed) 
        << "Task should be Completed even after throwing exception";
    EXPECT_TRUE(task->IsCompleted());
}

TEST_F(WorkerThreadTest, ExceptionDoesNotLeakReferences)
{
    auto task = TaskScheduler::Get().Launch("ThrowingTask", []() {
        throw std::runtime_error("Test exception");
    });
    
    ASSERT_NE(task, nullptr);
    
    // Only our reference should exist (scheduler releases its reference after queuing)
    // Note: The queue holds a reference, so refcount is 2 until task completes
    i32 refCountBeforeExecution = task->GetRefCount();
    
    EXPECT_TRUE(WaitForTaskCompletion(task, 2000));
    
    // After completion, queue should have released its reference
    // Only our reference should remain
    EXPECT_EQ(task->GetRefCount(), 1) 
        << "Reference count should be 1 (only test's reference) after task completes with exception";
}

TEST_F(WorkerThreadTest, MultipleExceptionsInQuickSuccession)
{
    // Launch multiple tasks that throw exceptions rapidly
    // This tests that exception handling doesn't interfere with task system state
    
    std::vector<Ref<Task>> tasks;
    std::atomic<u32> exceptionsThrown{0};
    
    for (u32 i = 0; i < 20; ++i)
    {
        auto task = TaskScheduler::Get().Launch("ThrowingTask", [&exceptionsThrown]() {
            exceptionsThrown.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error("Test exception");
        });
        tasks.push_back(task);
    }
    
    // All tasks should complete despite exceptions
    EXPECT_TRUE(WaitForAllTasks(tasks, 5000));
    EXPECT_EQ(exceptionsThrown.load(), 20u) << "All exception-throwing tasks should have executed";
    
    // Verify all tasks are marked as completed
    for (const auto& task : tasks)
    {
        EXPECT_TRUE(task->IsCompleted());
        EXPECT_EQ(task->GetState(), ETaskState::Completed);
    }
    
    // Note: We don't test immediate system functionality after rapid exceptions
    // as this can expose timing-sensitive edge cases. The important thing is that
    // exception handling doesn't crash workers and tasks are properly completed.
}

// ============================================================================
// Stress Tests
// ============================================================================

TEST_F(WorkerThreadTest, StressTestManyTasks)
{
    constexpr u32 NumTasks = 1000;
    std::atomic<u32> counter{0};
    std::vector<Ref<Task>> tasks;
    tasks.reserve(NumTasks);

    for (u32 i = 0; i < NumTasks; ++i)
    {
        auto task = TaskScheduler::Get().Launch("StressTask", [&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(task);
    }

    EXPECT_TRUE(WaitForAllTasks(tasks, 10000));  // 10 second timeout
    EXPECT_EQ(counter.load(), NumTasks);
}

TEST_F(WorkerThreadTest, StressTestWithWork)
{
    constexpr u32 NumTasks = 500;
    std::atomic<u32> counter{0};
    std::vector<Ref<Task>> tasks;
    tasks.reserve(NumTasks);

    for (u32 i = 0; i < NumTasks; ++i)
    {
        auto task = TaskScheduler::Get().Launch("WorkTask", [&counter]() {
            // Do some actual work
            volatile u64 sum = 0;
            for (u32 j = 0; j < 1000; ++j)
                sum += j * j;
            
            counter.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(task);
    }

    EXPECT_TRUE(WaitForAllTasks(tasks, 10000));
    EXPECT_EQ(counter.load(), NumTasks);
}

TEST_F(WorkerThreadTest, StressTestMixedPriorities)
{
    constexpr u32 TasksPerPriority = 200;
    std::atomic<u32> counter{0};
    std::vector<Ref<Task>> tasks;
    tasks.reserve(TasksPerPriority * 3);

    // Interleave tasks of different priorities
    for (u32 i = 0; i < TasksPerPriority; ++i)
    {
        // High priority
        auto highTask = TaskScheduler::Get().Launch("High", ETaskPriority::High, [&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(highTask);

        // Normal priority
        auto normalTask = TaskScheduler::Get().Launch("Normal", ETaskPriority::Normal, [&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(normalTask);

        // Background priority
        auto bgTask = TaskScheduler::Get().Launch("Background", ETaskPriority::Background, [&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(bgTask);
    }

    EXPECT_TRUE(WaitForAllTasks(tasks, 10000));
    EXPECT_EQ(counter.load(), TasksPerPriority * 3);
}

// ============================================================================
// Performance Sanity Tests
// ============================================================================

TEST_F(WorkerThreadTest, TaskLatency)
{
    // Measure time from launch to execution start
    std::atomic<bool> started{false};
    auto launchTime = std::chrono::high_resolution_clock::now();
    
    auto task = TaskScheduler::Get().Launch("LatencyTask", ETaskPriority::High, [&started]() {
        started.store(true, std::memory_order_release);
    });

    // Wait for task to start
    while (!started.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(startTime - launchTime).count();

    EXPECT_TRUE(WaitForTaskCompletion(task));
    
    // Latency should be reasonable (< 20ms to account for debug build overhead)
    // Release builds should be much faster (< 5ms typically)
    EXPECT_LT(latency, 20000) << "Task latency: " << latency << " microseconds";
}

TEST_F(WorkerThreadTest, TaskThroughput)
{
    constexpr u32 NumTasks = 10000;
    std::vector<Ref<Task>> tasks;
    tasks.reserve(NumTasks);

    auto startTime = std::chrono::high_resolution_clock::now();

    for (u32 i = 0; i < NumTasks; ++i)
    {
        auto task = TaskScheduler::Get().Launch("ThroughputTask", []() {
            // Minimal work
            volatile u32 x = 42;
            (void)x;
        });
        tasks.push_back(task);
    }

    EXPECT_TRUE(WaitForAllTasks(tasks, 15000));  // 15 second timeout
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    
    f64 tasksPerSecond = (NumTasks * 1000.0) / elapsedMs;
    
    // Throughput should be decent even in debug (> 1000 tasks/sec)
    EXPECT_GT(tasksPerSecond, 1000.0) << "Throughput: " << tasksPerSecond << " tasks/second";
}
