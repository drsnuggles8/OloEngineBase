// Task System Tests - Phase 4: Synchronization Primitives
// Tests for:
// - Task prerequisites and dependencies
// - Task events
// - Dependency chains
// - Complex dependency graphs

#include <gtest/gtest.h>
#include "OloEngine/Tasks/Task.h"
#include "OloEngine/Tasks/TaskPriority.h"
#include "OloEngine/Tasks/TaskScheduler.h"
#include "OloEngine/Tasks/TaskEvent.h"
#include "OloEngine/Tasks/TaskWait.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace OloEngine;

// ============================================================================
// Test Fixture
// ============================================================================

class TaskSynchronizationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Initialize logging system (required by Thread/ThreadSignal)
        if (!Log::GetCoreLogger())
        {
            Log::Init();
        }

        // Initialize scheduler
        TaskSchedulerConfig config;
        config.NumForegroundWorkers = 4;
        config.NumBackgroundWorkers = 2;
        TaskScheduler::Initialize(config);
    }

    void TearDown() override
    {
        TaskScheduler::Shutdown();
    }

    // Helper: Wait for a task to complete with timeout
    bool WaitForTaskCompletion(Ref<Task> task, u32 timeoutMs = 5000)
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

    // Helper: Wait for all tasks to complete
    bool WaitForAllTasks(const std::vector<Ref<Task>>& tasks, u32 timeoutMs = 5000)
    {
        for (const auto& task : tasks)
        {
            if (!WaitForTaskCompletion(task, timeoutMs))
                return false;
        }
        return true;
    }
};

// ============================================================================
// Basic Prerequisite Tests
// ============================================================================

TEST_F(TaskSynchronizationTest, AddPrerequisiteToTask)
{
    std::atomic<i32> executionOrder{0};
    std::atomic<i32> task1Order{-1};
    std::atomic<i32> task2Order{-1};

    // Create two tasks
    auto task1 = CreateTask("Task1", ETaskPriority::Normal, [&]() {
        task1Order.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    });

    auto task2 = CreateTask("Task2", ETaskPriority::Normal, [&]() {
        task2Order.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    });

    // Task2 depends on Task1
    task2->AddPrerequisite(task1);

    EXPECT_EQ(task2->GetPrerequisiteCount(), 1);
    EXPECT_FALSE(task2->ArePrerequisitesComplete());

    // Launch task1 (task2 will launch automatically when task1 completes)
    TaskScheduler::Get().LaunchTask(task1);

    // Wait for both tasks
    EXPECT_TRUE(WaitForTaskCompletion(task1));
    EXPECT_TRUE(WaitForTaskCompletion(task2));

    // Verify execution order: task1 should complete before task2
    EXPECT_EQ(task1Order.load(), 0);
    EXPECT_EQ(task2Order.load(), 1);
}

TEST_F(TaskSynchronizationTest, LaunchTaskWithPrerequisites)
{
    std::atomic<i32> executionOrder{0};
    std::atomic<i32> prereqOrder{-1};
    std::atomic<i32> dependentOrder{-1};

    auto prereq = TaskScheduler::Get().Launch("Prerequisite", [&]() {
        prereqOrder.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });

    auto dependent = TaskScheduler::Get().Launch("Dependent", ETaskPriority::Normal, [&]() {
        dependentOrder.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    }, {prereq});

    EXPECT_TRUE(WaitForTaskCompletion(prereq));
    EXPECT_TRUE(WaitForTaskCompletion(dependent));

    EXPECT_EQ(prereqOrder.load(), 0);
    EXPECT_EQ(dependentOrder.load(), 1);
}

TEST_F(TaskSynchronizationTest, MultiplePrerequisites)
{
    std::atomic<i32> executionOrder{0};
    std::atomic<i32> prereq1Order{-1};
    std::atomic<i32> prereq2Order{-1};
    std::atomic<i32> prereq3Order{-1};
    std::atomic<i32> dependentOrder{-1};

    auto prereq1 = TaskScheduler::Get().Launch("Prereq1", [&]() {
        prereq1Order.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    });

    auto prereq2 = TaskScheduler::Get().Launch("Prereq2", [&]() {
        prereq2Order.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    });

    auto prereq3 = TaskScheduler::Get().Launch("Prereq3", [&]() {
        prereq3Order.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    });

    auto dependent = TaskScheduler::Get().Launch("Dependent", ETaskPriority::Normal, [&]() {
        dependentOrder.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    }, {prereq1, prereq2, prereq3});

    EXPECT_TRUE(WaitForTaskCompletion(dependent));

    // All prerequisites should complete before dependent
    EXPECT_LT(prereq1Order.load(), dependentOrder.load());
    EXPECT_LT(prereq2Order.load(), dependentOrder.load());
    EXPECT_LT(prereq3Order.load(), dependentOrder.load());
}

TEST_F(TaskSynchronizationTest, DependencyChain)
{
    std::atomic<i32> executionOrder{0};
    std::vector<i32> taskOrders(5, -1);

    std::vector<Ref<Task>> tasks;

    // Create chain: task0 -> task1 -> task2 -> task3 -> task4
    for (u32 i = 0; i < 5; ++i)
    {
        auto task = CreateTask("ChainTask", ETaskPriority::Normal, [&, i]() {
            taskOrders[i] = executionOrder.fetch_add(1, std::memory_order_relaxed);
        });

        if (!tasks.empty())
        {
            // Depend on previous task
            task->AddPrerequisite(tasks.back());
        }

        tasks.push_back(task);
    }

    // Launch the first task (others will cascade)
    TaskScheduler::Get().LaunchTask(tasks[0]);

    // Wait for all tasks
    EXPECT_TRUE(WaitForAllTasks(tasks));

    // Verify strict ordering
    for (u32 i = 0; i < 5; ++i)
    {
        EXPECT_EQ(taskOrders[i], static_cast<i32>(i));
    }
}

// ============================================================================
// Task Retraction Tests
// ============================================================================

TEST_F(TaskSynchronizationTest, TaskRetractionBasic)
{
    std::atomic<bool> executed{false};
    
    // Create but don't launch the task yet
    auto task = CreateTask("RetractionTask", ETaskPriority::Normal, [&]() {
        executed.store(true, std::memory_order_release);
    });

    // Launch it
    TaskScheduler::Get().LaunchTask(task);

    // Immediately try to retract and execute inline
    bool retracted = TaskWait::TryRetractAndExecute(task);

    // If we couldn't retract it (already running/completed), wait for completion
    if (!retracted)
    {
        TaskWait::Wait(task);
    }

    // Should complete either way
    EXPECT_TRUE(task->IsCompleted());
    EXPECT_TRUE(executed.load(std::memory_order_acquire));
}

TEST_F(TaskSynchronizationTest, TaskRetractionAlreadyRunning)
{
    std::atomic<bool> taskStarted{false};
    std::atomic<bool> shouldContinue{false};
    
    // Create a task that will take some time
    auto task = TaskScheduler::Get().Launch("SlowTask", [&]() {
        taskStarted.store(true, std::memory_order_release);
        while (!shouldContinue.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Wait for task to start
    while (!taskStarted.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Try to retract - should fail because task is already running
    bool retracted = TaskWait::TryRetractAndExecute(task);
    EXPECT_FALSE(retracted);

    // Let task complete
    shouldContinue.store(true, std::memory_order_release);
    EXPECT_TRUE(WaitForTaskCompletion(task));
}

TEST_F(TaskSynchronizationTest, TaskWaitWithRetraction)
{
    std::atomic<i32> executionCount{0};
    
    auto task = CreateTask("WaitTask", ETaskPriority::Normal, [&]() {
        executionCount.fetch_add(1, std::memory_order_relaxed);
    });

    TaskScheduler::Get().LaunchTask(task);

    // Wait using the hybrid strategy (may retract)
    TaskWait::Wait(task);

    EXPECT_TRUE(task->IsCompleted());
    EXPECT_EQ(executionCount.load(), 1);
}

// ============================================================================
// WaitForAll Tests
// ============================================================================

TEST_F(TaskSynchronizationTest, WaitForAllBasic)
{
    std::atomic<i32> completedCount{0};
    std::vector<Ref<Task>> tasks;

    // Launch multiple tasks
    for (i32 i = 0; i < 10; ++i)
    {
        auto task = TaskScheduler::Get().Launch("Task", [&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            completedCount.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(task);
    }

    // Wait for all
    TaskWait::WaitForAll(tasks);

    // All should be completed
    EXPECT_EQ(completedCount.load(), 10);
    for (const auto& task : tasks)
    {
        EXPECT_TRUE(task->IsCompleted());
    }
}

TEST_F(TaskSynchronizationTest, WaitForAllInitializerList)
{
    std::atomic<i32> count1{0}, count2{0}, count3{0};

    auto task1 = TaskScheduler::Get().Launch("Task1", [&]() { count1.store(1); });
    auto task2 = TaskScheduler::Get().Launch("Task2", [&]() { count2.store(1); });
    auto task3 = TaskScheduler::Get().Launch("Task3", [&]() { count3.store(1); });

    // Wait using initializer list
    TaskWait::WaitForAll({task1, task2, task3});

    EXPECT_EQ(count1.load(), 1);
    EXPECT_EQ(count2.load(), 1);
    EXPECT_EQ(count3.load(), 1);
}

TEST_F(TaskSynchronizationTest, WaitForAllWithDependencies)
{
    std::atomic<i32> executionOrder{0};
    std::vector<i32> orders(5, -1);

    // Create tasks with dependencies
    auto task0 = TaskScheduler::Get().Launch("Task0", [&]() {
        orders[0] = executionOrder.fetch_add(1, std::memory_order_relaxed);
    });

    auto task1 = TaskScheduler::Get().Launch("Task1", ETaskPriority::Normal, [&]() {
        orders[1] = executionOrder.fetch_add(1, std::memory_order_relaxed);
    }, {task0});

    auto task2 = TaskScheduler::Get().Launch("Task2", ETaskPriority::Normal, [&]() {
        orders[2] = executionOrder.fetch_add(1, std::memory_order_relaxed);
    }, {task0});

    auto task3 = TaskScheduler::Get().Launch("Task3", ETaskPriority::Normal, [&]() {
        orders[3] = executionOrder.fetch_add(1, std::memory_order_relaxed);
    }, {task1, task2});

    auto task4 = TaskScheduler::Get().Launch("Task4", ETaskPriority::Normal, [&]() {
        orders[4] = executionOrder.fetch_add(1, std::memory_order_relaxed);
    }, {task3});

    // Wait for all tasks
    TaskWait::WaitForAll({task0, task1, task2, task3, task4});

    // Verify all completed
    for (i32 i = 0; i < 5; ++i)
    {
        EXPECT_GE(orders[i], 0) << "Task " << i << " did not execute";
    }

    // Verify ordering constraints
    EXPECT_LT(orders[0], orders[1]);
    EXPECT_LT(orders[0], orders[2]);
    EXPECT_LT(orders[1], orders[3]);
    EXPECT_LT(orders[2], orders[3]);
    EXPECT_LT(orders[3], orders[4]);
}

// ============================================================================
// Task Event Tests
// ============================================================================

TEST_F(TaskSynchronizationTest, TaskEventBasic)
{
    TaskEvent event("TestEvent");

    EXPECT_FALSE(event.IsTriggered());

    event.Trigger();
    event.Wait();

    EXPECT_TRUE(event.IsTriggered());
}

TEST_F(TaskSynchronizationTest, TaskEventWithPrerequisites)
{
    std::atomic<bool> task1Done{false};
    std::atomic<bool> task2Done{false};

    auto task1 = TaskScheduler::Get().Launch("Task1", [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        task1Done.store(true, std::memory_order_release);
    });

    auto task2 = TaskScheduler::Get().Launch("Task2", [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        task2Done.store(true, std::memory_order_release);
    });

    TaskEvent event("WaitForBoth");
    event.AddPrerequisites({task1, task2});
    event.Trigger();

    EXPECT_FALSE(event.IsTriggered());

    event.Wait();

    EXPECT_TRUE(event.IsTriggered());
    EXPECT_TRUE(task1Done.load(std::memory_order_acquire));
    EXPECT_TRUE(task2Done.load(std::memory_order_acquire));
}

TEST_F(TaskSynchronizationTest, TaskEventAsPrerequisite)
{
    std::atomic<i32> executionOrder{0};
    std::atomic<i32> prereqOrder{-1};
    std::atomic<i32> eventOrder{-1};
    std::atomic<i32> dependentOrder{-1};

    auto prereq = TaskScheduler::Get().Launch("Prereq", [&]() {
        prereqOrder.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    });

    TaskEvent event("Event");
    event.AddPrerequisite(prereq);

    // Use event as prerequisite for another task
    auto dependent = TaskScheduler::Get().Launch("Dependent", ETaskPriority::Normal, [&]() {
        dependentOrder.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    }, {event.AsPrerequisite()});

    event.Trigger();

    EXPECT_TRUE(WaitForTaskCompletion(prereq));
    EXPECT_TRUE(WaitForTaskCompletion(dependent));

    // Event and dependent should execute after prereq
    EXPECT_LT(prereqOrder.load(), dependentOrder.load());
}

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

TEST_F(TaskSynchronizationTest, AddPrerequisiteAlreadyCompleted)
{
    auto task1 = TaskScheduler::Get().Launch("Task1", []() {});
    
    EXPECT_TRUE(WaitForTaskCompletion(task1));
    EXPECT_TRUE(task1->IsCompleted());

    // Add completed task as prerequisite - should not block
    auto task2 = CreateTask("Task2", ETaskPriority::Normal, []() {});
    task2->AddPrerequisite(task1);

    EXPECT_EQ(task2->GetPrerequisiteCount(), 0);  // Should not increment
    EXPECT_TRUE(task2->ArePrerequisitesComplete());
}

TEST_F(TaskSynchronizationTest, PrerequisiteCountAccuracy)
{
    auto task1 = CreateTask("Task1", ETaskPriority::Normal, []() {});
    auto prereq1 = CreateTask("Prereq1", ETaskPriority::Normal, []() {});
    auto prereq2 = CreateTask("Prereq2", ETaskPriority::Normal, []() {});
    auto prereq3 = CreateTask("Prereq3", ETaskPriority::Normal, []() {});

    EXPECT_EQ(task1->GetPrerequisiteCount(), 0);

    task1->AddPrerequisite(prereq1);
    EXPECT_EQ(task1->GetPrerequisiteCount(), 1);

    task1->AddPrerequisite(prereq2);
    EXPECT_EQ(task1->GetPrerequisiteCount(), 2);

    task1->AddPrerequisite(prereq3);
    EXPECT_EQ(task1->GetPrerequisiteCount(), 3);
}

// ============================================================================
// Complex Dependency Graph Tests
// ============================================================================

TEST_F(TaskSynchronizationTest, DiamondDependency)
{
    // Diamond pattern:
    //     A
    //    / \
    //   B   C
    //    \ /
    //     D

    std::atomic<i32> executionOrder{0};
    std::atomic<i32> orderA{-1}, orderB{-1}, orderC{-1}, orderD{-1};

    auto taskA = TaskScheduler::Get().Launch("A", [&]() {
        orderA.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    });

    auto taskB = TaskScheduler::Get().Launch("B", ETaskPriority::Normal, [&]() {
        orderB.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    }, {taskA});

    auto taskC = TaskScheduler::Get().Launch("C", ETaskPriority::Normal, [&]() {
        orderC.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    }, {taskA});

    auto taskD = TaskScheduler::Get().Launch("D", ETaskPriority::Normal, [&]() {
        orderD.store(executionOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    }, {taskB, taskC});

    EXPECT_TRUE(WaitForTaskCompletion(taskD));

    // A should execute first
    EXPECT_EQ(orderA.load(), 0);
    
    // B and C should execute after A
    EXPECT_GT(orderB.load(), orderA.load());
    EXPECT_GT(orderC.load(), orderA.load());
    
    // D should execute after both B and C
    EXPECT_GT(orderD.load(), orderB.load());
    EXPECT_GT(orderD.load(), orderC.load());
}

TEST_F(TaskSynchronizationTest, ComplexDependencyGraph)
{
    // Create a more complex dependency graph with ~20 tasks
    constexpr u32 NumTasks = 20;
    std::vector<Ref<Task>> tasks;
    std::vector<std::atomic<i32>> executionOrders(NumTasks);
    std::atomic<i32> globalOrder{0};

    for (u32 i = 0; i < NumTasks; ++i)
    {
        executionOrders[i].store(-1, std::memory_order_relaxed);
    }

    // Build dependency structure
    for (u32 i = 0; i < NumTasks; ++i)
    {
        auto task = CreateTask("ComplexTask", ETaskPriority::Normal, [&, i]() {
            executionOrders[i].store(globalOrder.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
            // Small amount of work
            volatile u32 sum = 0;
            for (u32 j = 0; j < 100; ++j)
                sum += j;
        });

        // Add dependencies to previous tasks (creating a semi-ordered graph)
        if (i > 0 && i % 3 == 0)
        {
            task->AddPrerequisite(tasks[i - 1]);
        }
        if (i > 1 && i % 5 == 0)
        {
            task->AddPrerequisite(tasks[i - 2]);
        }

        tasks.push_back(task);
    }

    // Launch all tasks that have no prerequisites
    for (auto& task : tasks)
    {
        if (task->ArePrerequisitesComplete())
        {
            TaskScheduler::Get().LaunchTask(task);
        }
    }

    // Wait for all tasks to complete
    EXPECT_TRUE(WaitForAllTasks(tasks, 10000));

    // Verify all tasks executed
    for (u32 i = 0; i < NumTasks; ++i)
    {
        EXPECT_GE(executionOrders[i].load(), 0) << "Task " << i << " did not execute";
    }

    // Verify dependency ordering constraints
    for (u32 i = 0; i < NumTasks; ++i)
    {
        if (i > 0 && i % 3 == 0)
        {
            EXPECT_LT(executionOrders[i - 1].load(), executionOrders[i].load())
                << "Task " << (i - 1) << " should execute before task " << i;
        }
        if (i > 1 && i % 5 == 0)
        {
            EXPECT_LT(executionOrders[i - 2].load(), executionOrders[i].load())
                << "Task " << (i - 2) << " should execute before task " << i;
        }
    }
}
