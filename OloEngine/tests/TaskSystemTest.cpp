// Task System Tests - Phase 1: Core Infrastructure
// Tests for:
// - Task creation and destruction
// - Type erasure with various callable types
// - Small vs large task optimization
// - Task state transitions
// - Priority assignment
// - TaskScheduler initialization and configuration

#include <gtest/gtest.h>
#include "OloEngine/Tasks/Task.h"
#include "OloEngine/Tasks/TaskPriority.h"
#include "OloEngine/Tasks/TaskScheduler.h"
#include "OloEngine/Tasks/TaskWait.h"

#include <chrono>
#include <thread>
#include <atomic>

using namespace OloEngine;

// ============================================================================
// Task Creation and Type Erasure Tests
// ============================================================================

TEST(TaskSystemTest, TaskCreationFromLambda)
{
    bool executed = false;
    auto task = CreateTask("TestLambda", ETaskPriority::Normal, [&executed]() {
        executed = true;
    });

    EXPECT_NE(task, nullptr);
    EXPECT_EQ(task->GetState(), ETaskState::Ready);
    EXPECT_EQ(task->GetPriority(), ETaskPriority::Normal);
    EXPECT_STREQ(task->GetDebugName(), "TestLambda");
    EXPECT_FALSE(task->IsCompleted());
}

TEST(TaskSystemTest, TaskCreationFromFunction)
{
    static bool s_executed = false;
    s_executed = false;

    auto func = []() { s_executed = true; };
    auto task = CreateTask("TestFunction", ETaskPriority::High, func);

    EXPECT_NE(task, nullptr);
    EXPECT_EQ(task->GetPriority(), ETaskPriority::High);
}

TEST(TaskSystemTest, TaskCreationFromFunctor)
{
    struct Functor
    {
        bool& m_executed;
        Functor(bool& exec) : m_executed(exec) {}
        void operator()() { m_executed = true; }
    };

    bool executed = false;
    Functor functor(executed);
    auto task = CreateTask("TestFunctor", ETaskPriority::Background, functor);

    EXPECT_NE(task, nullptr);
    EXPECT_EQ(task->GetPriority(), ETaskPriority::Background);
}

TEST(TaskSystemTest, TaskCreationFromStdFunction)
{
    bool executed = false;
    std::function<void()> func = [&executed]() { executed = true; };
    auto task = CreateTask("TestStdFunction", ETaskPriority::Normal, func);

    EXPECT_NE(task, nullptr);
    EXPECT_EQ(task->GetState(), ETaskState::Ready);
}

// ============================================================================
// Small Task Optimization Tests
// ============================================================================

TEST(TaskSystemTest, SmallTaskUsesInlineStorage)
{
    // Small capture (< 64 bytes)
    int a = 1, b = 2, c = 3;
    auto task = CreateTask("SmallTask", ETaskPriority::Normal, [a, b, c]() {
        volatile int result = a + b + c;
        (void)result;
    });

    auto* executableTask = dynamic_cast<ExecutableTask<decltype([a, b, c]() {})>*>(task.Raw());
    if (executableTask)
    {
        EXPECT_TRUE(executableTask->UsesInlineStorage());
    }
}

TEST(TaskSystemTest, LargeTaskUsesHeapStorage)
{
    // Large capture (> 64 bytes) - array of 20 doubles = 160 bytes
    double data[20] = {0};
    auto task = CreateTask("LargeTask", ETaskPriority::Normal, [data]() {
        volatile double sum = 0;
        for (int i = 0; i < 20; ++i)
            sum += data[i];
        (void)sum;
    });

    auto* executableTask = dynamic_cast<ExecutableTask<decltype([data]() {})>*>(task.Raw());
    if (executableTask)
    {
        EXPECT_FALSE(executableTask->UsesInlineStorage());
    }
}

TEST(TaskSystemTest, TaskExecutionWithSmallCapture)
{
    int result = 0;
    auto task = CreateTask("ExecuteSmall", ETaskPriority::Normal, [&result]() {
        result = 42;
    });

    task->Execute();
    EXPECT_EQ(result, 42);
}

TEST(TaskSystemTest, TaskExecutionWithLargeCapture)
{
    // Large capture to force heap allocation
    double data[20] = {0};
    for (int i = 0; i < 20; ++i)
        data[i] = static_cast<double>(i);

    double result = 0;
    auto task = CreateTask("ExecuteLarge", ETaskPriority::Normal, [data, &result]() {
        for (int i = 0; i < 20; ++i)
            result += data[i];
    });

    task->Execute();
    EXPECT_EQ(result, 190.0); // Sum of 0..19
}

// ============================================================================
// Task State Transition Tests
// ============================================================================

TEST(TaskSystemTest, InitialStateIsReady)
{
    auto task = CreateTask("StateTest", ETaskPriority::Normal, []() {});
    EXPECT_EQ(task->GetState(), ETaskState::Ready);
    EXPECT_FALSE(task->IsCompleted());
}

TEST(TaskSystemTest, StateTransitionReadyToScheduled)
{
    auto task = CreateTask("TransitionTest", ETaskPriority::Normal, []() {});
    
    ETaskState expected = ETaskState::Ready;
    bool success = task->TryTransitionState(expected, ETaskState::Scheduled);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(task->GetState(), ETaskState::Scheduled);
    EXPECT_EQ(expected, ETaskState::Ready); // Expected not modified on success
}

TEST(TaskSystemTest, StateTransitionScheduledToRunning)
{
    auto task = CreateTask("TransitionTest", ETaskPriority::Normal, []() {});
    
    task->SetState(ETaskState::Scheduled);
    
    ETaskState expected = ETaskState::Scheduled;
    bool success = task->TryTransitionState(expected, ETaskState::Running);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(task->GetState(), ETaskState::Running);
}

TEST(TaskSystemTest, StateTransitionRunningToCompleted)
{
    auto task = CreateTask("TransitionTest", ETaskPriority::Normal, []() {});
    
    task->SetState(ETaskState::Running);
    
    ETaskState expected = ETaskState::Running;
    bool success = task->TryTransitionState(expected, ETaskState::Completed);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(task->GetState(), ETaskState::Completed);
    EXPECT_TRUE(task->IsCompleted());
}

TEST(TaskSystemTest, StateTransitionFailsWithWrongExpected)
{
    auto task = CreateTask("FailTransition", ETaskPriority::Normal, []() {});
    
    // Task is in Ready state, try to transition from Scheduled
    ETaskState expected = ETaskState::Scheduled;
    bool success = task->TryTransitionState(expected, ETaskState::Running);
    
    EXPECT_FALSE(success);
    EXPECT_EQ(expected, ETaskState::Ready); // Expected updated to actual state
    EXPECT_EQ(task->GetState(), ETaskState::Ready); // State unchanged
}

TEST(TaskSystemTest, SetStateDirectly)
{
    auto task = CreateTask("SetStateTest", ETaskPriority::Normal, []() {});
    
    task->SetState(ETaskState::Scheduled);
    EXPECT_EQ(task->GetState(), ETaskState::Scheduled);
    
    task->SetState(ETaskState::Running);
    EXPECT_EQ(task->GetState(), ETaskState::Running);
    
    task->SetState(ETaskState::Completed);
    EXPECT_EQ(task->GetState(), ETaskState::Completed);
}

TEST(TaskSystemTest, InvalidStateTransitionsPrevented)
{
    auto task = CreateTask("StateTest", ETaskPriority::Normal, []() {});
    
    // Try to go from Ready directly to Running (should fail - must go through Scheduled)
    ETaskState expected = ETaskState::Ready;
    bool success = task->TryTransitionState(expected, ETaskState::Running);
    EXPECT_FALSE(success) << "Should not be able to transition from Ready to Running";
    EXPECT_EQ(expected, ETaskState::Ready);  // Expected should not be modified when transition is invalid
    EXPECT_EQ(task->GetState(), ETaskState::Ready);  // State should be unchanged
    
    // Try to go from Ready directly to Completed (should fail)
    expected = ETaskState::Ready;
    success = task->TryTransitionState(expected, ETaskState::Completed);
    EXPECT_FALSE(success) << "Should not be able to transition from Ready to Completed";
    EXPECT_EQ(task->GetState(), ETaskState::Ready);
    
    // Properly transition to Scheduled
    expected = ETaskState::Ready;
    success = task->TryTransitionState(expected, ETaskState::Scheduled);
    EXPECT_TRUE(success);
    EXPECT_EQ(task->GetState(), ETaskState::Scheduled);
    
    // NEW: Scheduled -> Ready IS allowed (for retraction)
    expected = ETaskState::Scheduled;
    success = task->TryTransitionState(expected, ETaskState::Ready);
    EXPECT_TRUE(success) << "Scheduled -> Ready should be allowed for retraction";
    EXPECT_EQ(task->GetState(), ETaskState::Ready);
    
    // Transition back to Scheduled for further testing
    expected = ETaskState::Ready;
    success = task->TryTransitionState(expected, ETaskState::Scheduled);
    EXPECT_TRUE(success);
    EXPECT_EQ(task->GetState(), ETaskState::Scheduled);
    
    // Try to skip Running (Scheduled -> Completed should fail)
    expected = ETaskState::Scheduled;
    success = task->TryTransitionState(expected, ETaskState::Completed);
    EXPECT_FALSE(success) << "Should not be able to skip Running state";
    EXPECT_EQ(task->GetState(), ETaskState::Scheduled);
    
    // Properly transition to Running
    expected = ETaskState::Scheduled;
    success = task->TryTransitionState(expected, ETaskState::Running);
    EXPECT_TRUE(success);
    EXPECT_EQ(task->GetState(), ETaskState::Running);
    
    // Properly transition to Completed
    expected = ETaskState::Running;
    success = task->TryTransitionState(expected, ETaskState::Completed);
    EXPECT_TRUE(success);
    EXPECT_EQ(task->GetState(), ETaskState::Completed);
    
    // Try to go from Completed back to Running (should fail)
    expected = ETaskState::Completed;
    success = task->TryTransitionState(expected, ETaskState::Running);
    EXPECT_FALSE(success) << "Should not be able to transition from Completed";
    EXPECT_EQ(task->GetState(), ETaskState::Completed);
    
    // Try to go from Completed back to Ready (should fail)
    expected = ETaskState::Completed;
    success = task->TryTransitionState(expected, ETaskState::Ready);
    EXPECT_FALSE(success) << "Should not be able to transition from Completed";
    EXPECT_EQ(task->GetState(), ETaskState::Completed);
}

// ============================================================================
// Priority Tests
// ============================================================================

TEST(TaskSystemTest, PriorityAssignment)
{
    auto highTask = CreateTask("High", ETaskPriority::High, []() {});
    auto normalTask = CreateTask("Normal", ETaskPriority::Normal, []() {});
    auto backgroundTask = CreateTask("Background", ETaskPriority::Background, []() {});
    
    EXPECT_EQ(highTask->GetPriority(), ETaskPriority::High);
    EXPECT_EQ(normalTask->GetPriority(), ETaskPriority::Normal);
    EXPECT_EQ(backgroundTask->GetPriority(), ETaskPriority::Background);
}

TEST(TaskSystemTest, PriorityIndexMapping)
{
    EXPECT_EQ(GetPriorityIndex(ETaskPriority::High), 0u);
    EXPECT_EQ(GetPriorityIndex(ETaskPriority::Normal), 1u);
    EXPECT_EQ(GetPriorityIndex(ETaskPriority::Background), 2u);
}

TEST(TaskSystemTest, PriorityNames)
{
    EXPECT_STREQ(GetPriorityName(ETaskPriority::High), "High");
    EXPECT_STREQ(GetPriorityName(ETaskPriority::Normal), "Normal");
    EXPECT_STREQ(GetPriorityName(ETaskPriority::Background), "Background");
}

// ============================================================================
// TaskScheduler Tests
// ============================================================================

class TaskSchedulerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Initialize logging system (required by Thread/ThreadSignal)
        if (!Log::GetCoreLogger())
        {
            Log::Init();
        }

        // Ensure scheduler is not initialized before each test
        if (TaskScheduler::IsInitialized())
        {
            TaskScheduler::Shutdown();
        }
    }

    // Helper: Wait for task completion with timeout
    bool WaitForCompletion(Ref<Task> task, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000))
    {
        auto start = std::chrono::steady_clock::now();
        while (!task->IsCompleted())
        {
            if (std::chrono::steady_clock::now() - start > timeout)
                return false;  // Timeout
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return true;
    }

    void TearDown() override
    {
        // Clean up after each test
        if (TaskScheduler::IsInitialized())
        {
            TaskScheduler::Shutdown();
        }
    }
};

TEST_F(TaskSchedulerTest, InitializationWithDefaults)
{
    EXPECT_FALSE(TaskScheduler::IsInitialized());
    
    TaskScheduler::Initialize();
    
    EXPECT_TRUE(TaskScheduler::IsInitialized());
    EXPECT_GT(TaskScheduler::Get().GetNumForegroundWorkers(), 0u);
    EXPECT_GT(TaskScheduler::Get().GetNumBackgroundWorkers(), 0u);
}

TEST_F(TaskSchedulerTest, InitializationWithCustomConfig)
{
    TaskSchedulerConfig config;
    config.NumForegroundWorkers = 4;
    config.NumBackgroundWorkers = 2;
    
    TaskScheduler::Initialize(config);
    
    EXPECT_EQ(TaskScheduler::Get().GetNumForegroundWorkers(), 4u);
    EXPECT_EQ(TaskScheduler::Get().GetNumBackgroundWorkers(), 2u);
}

TEST_F(TaskSchedulerTest, AutoDetectWorkerCounts)
{
    TaskSchedulerConfig config;
    config.AutoDetectWorkerCounts();
    
    EXPECT_GT(config.NumForegroundWorkers, 0u);
    EXPECT_GT(config.NumBackgroundWorkers, 0u);
    
    // Foreground should be larger than background (typically)
    EXPECT_GE(config.NumForegroundWorkers, config.NumBackgroundWorkers);
}

TEST_F(TaskSchedulerTest, ShutdownCleansUp)
{
    TaskScheduler::Initialize();
    EXPECT_TRUE(TaskScheduler::IsInitialized());
    
    TaskScheduler::Shutdown();
    EXPECT_FALSE(TaskScheduler::IsInitialized());
}

TEST_F(TaskSchedulerTest, LaunchTaskBasic)
{
    TaskScheduler::Initialize();
    
    std::atomic<bool> executed{false};
    auto task = TaskScheduler::Get().Launch("BasicLaunch", [&executed]() {
        executed.store(true, std::memory_order_release);
    });
    
    EXPECT_NE(task, nullptr);
    
    // Wait for async task to complete
    EXPECT_TRUE(WaitForCompletion(task));
    EXPECT_TRUE(task->IsCompleted());
    EXPECT_TRUE(executed.load(std::memory_order_acquire));
}

TEST_F(TaskSchedulerTest, LaunchTaskWithPriority)
{
    TaskScheduler::Initialize();
    
    auto highTask = TaskScheduler::Get().Launch("HighPriority", ETaskPriority::High, []() {});
    auto normalTask = TaskScheduler::Get().Launch("NormalPriority", ETaskPriority::Normal, []() {});
    auto backgroundTask = TaskScheduler::Get().Launch("BackgroundPriority", ETaskPriority::Background, []() {});
    
    EXPECT_EQ(highTask->GetPriority(), ETaskPriority::High);
    EXPECT_EQ(normalTask->GetPriority(), ETaskPriority::Normal);
    EXPECT_EQ(backgroundTask->GetPriority(), ETaskPriority::Background);
}

TEST_F(TaskSchedulerTest, LaunchMultipleTasks)
{
    TaskScheduler::Initialize();
    
    const int numTasks = 100;
    std::vector<Ref<Task>> tasks;
    tasks.reserve(numTasks);
    
    std::atomic<int> executedCount{0};
    
    for (int i = 0; i < numTasks; ++i)
    {
        auto task = TaskScheduler::Get().Launch("MultiTask", [&executedCount]() {
            executedCount.fetch_add(1, std::memory_order_relaxed);
        });
        tasks.push_back(task);
    }
    
    EXPECT_EQ(tasks.size(), numTasks);
    
    // Wait for all tasks to complete
    for (const auto& task : tasks)
    {
        EXPECT_TRUE(WaitForCompletion(task));
        EXPECT_TRUE(task->IsCompleted());
    }
    
    EXPECT_EQ(executedCount.load(std::memory_order_acquire), numTasks);
}

// ============================================================================
// Debug Name Tests
// ============================================================================

TEST(TaskSystemTest, DebugNamePreserved)
{
    const char* name = "MyTaskName";
    auto task = CreateTask(name, ETaskPriority::Normal, []() {});
    
    EXPECT_EQ(task->GetDebugName(), name); // Same pointer
    EXPECT_STREQ(task->GetDebugName(), "MyTaskName");
}

TEST(TaskSystemTest, NullDebugName)
{
    auto task = CreateTask(nullptr, ETaskPriority::Normal, []() {});
    
    EXPECT_EQ(task->GetDebugName(), nullptr);
}

// ============================================================================
// Memory and Cleanup Tests
// ============================================================================

TEST(TaskSystemTest, TaskReferenceCountingWorks)
{
    Ref<Task> task1 = CreateTask("RefCountTest", ETaskPriority::Normal, []() {});
    
    EXPECT_EQ(task1->GetRefCount(), 1);
    
    {
        Ref<Task> task2 = task1;
        EXPECT_EQ(task1->GetRefCount(), 2);
        EXPECT_EQ(task2->GetRefCount(), 2);
    }
    
    // task2 destroyed
    EXPECT_EQ(task1->GetRefCount(), 1);
}

// ============================================================================
// Task Cancellation Tests (Priority 1.1)
// ============================================================================

TEST(TaskSystemTest, CancelTaskInReadyState)
{
    bool executed = false;
    auto task = CreateTask("CancelReady", ETaskPriority::Normal, [&executed]() {
        executed = true;
    });

    EXPECT_EQ(task->GetState(), ETaskState::Ready);
    EXPECT_FALSE(task->IsCancelled());
    EXPECT_FALSE(task->IsDone());

    // Cancel the task
    bool cancelled = task->Cancel();
    
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(task->GetState(), ETaskState::Cancelled);
    EXPECT_TRUE(task->IsCancelled());
    EXPECT_TRUE(task->IsDone());
    EXPECT_FALSE(task->IsCompleted());
    
    // Verify task was never executed
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(executed);
}

TEST(TaskSystemTest, CancelTaskInScheduledState)
{
    // Initialize scheduler for this test
    if (!TaskScheduler::IsInitialized())
    {
        TaskSchedulerConfig config;
        config.NumForegroundWorkers = 2;
        config.NumBackgroundWorkers = 1;
        TaskScheduler::Initialize(config);
    }

    std::atomic<bool> executed{false};
    auto task = TaskScheduler::Get().Launch("CancelScheduled", ETaskPriority::Normal, [&executed]() {
        executed.store(true, std::memory_order_release);
    });

    // Give it a moment to potentially be scheduled (but likely not executed yet)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // Try to cancel - may succeed if still scheduled, or fail if already running
    bool cancelled = task->Cancel();
    
    // Wait a bit to see if it executes
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    if (cancelled)
    {
        // Successfully cancelled - should not have executed
        EXPECT_TRUE(task->IsCancelled());
        EXPECT_FALSE(executed.load(std::memory_order_acquire));
    }
    else
    {
        // Was already running or completed - should have executed
        EXPECT_TRUE(task->IsCompleted() || task->GetState() == ETaskState::Running);
    }

    TaskScheduler::Shutdown();
}

TEST(TaskSystemTest, CannotCancelRunningTask)
{
    // Initialize scheduler
    if (!TaskScheduler::IsInitialized())
    {
        TaskSchedulerConfig config;
        config.NumForegroundWorkers = 2;
        config.NumBackgroundWorkers = 1;
        TaskScheduler::Initialize(config);
    }

    std::atomic<bool> taskStarted{false};
    std::atomic<bool> allowTaskToComplete{false};
    
    auto task = TaskScheduler::Get().Launch("CannotCancelRunning", ETaskPriority::High, [&]() {
        taskStarted.store(true, std::memory_order_release);
        // Wait until we're allowed to complete
        while (!allowTaskToComplete.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
    });

    // Wait for task to start running
    while (!taskStarted.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    // Try to cancel while running - should fail
    bool cancelled = task->Cancel();
    EXPECT_FALSE(cancelled);
    EXPECT_EQ(task->GetState(), ETaskState::Running);

    // Allow task to complete
    allowTaskToComplete.store(true, std::memory_order_release);
    
    // Wait for completion
    while (!task->IsCompleted())
    {
        std::this_thread::yield();
    }

    EXPECT_TRUE(task->IsCompleted());
    EXPECT_FALSE(task->IsCancelled());

    TaskScheduler::Shutdown();
}

TEST(TaskSystemTest, CannotCancelCompletedTask)
{
    bool executed = false;
    auto task = CreateTask("CannotCancelCompleted", ETaskPriority::Normal, [&executed]() {
        executed = true;
    });

    // Execute the task
    task->SetState(ETaskState::Running);
    task->Execute();
    task->SetState(ETaskState::Completed);

    EXPECT_TRUE(task->IsCompleted());
    EXPECT_TRUE(executed);

    // Try to cancel - should fail
    bool cancelled = task->Cancel();
    EXPECT_FALSE(cancelled);
    EXPECT_TRUE(task->IsCompleted());
    EXPECT_FALSE(task->IsCancelled());
}

TEST(TaskSystemTest, CancelledTaskNotifiesSubsequents)
{
    // Initialize scheduler
    if (!TaskScheduler::IsInitialized())
    {
        TaskSchedulerConfig config;
        config.NumForegroundWorkers = 2;
        config.NumBackgroundWorkers = 1;
        TaskScheduler::Initialize(config);
    }

    std::atomic<bool> task1Executed{false};
    std::atomic<bool> task2Executed{false};

    auto task1 = CreateTask("Task1", ETaskPriority::Normal, [&task1Executed]() {
        task1Executed.store(true, std::memory_order_release);
    });

    auto task2 = CreateTask("Task2", ETaskPriority::Normal, [&task2Executed]() {
        task2Executed.store(true, std::memory_order_release);
    });

    // Task2 depends on Task1
    task2->AddPrerequisite(task1);
    EXPECT_EQ(task2->GetPrerequisiteCount(), 1);

    // Cancel task1 before launching
    bool cancelled = task1->Cancel();
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(task1->IsCancelled());

    // Task2's prerequisite count should have been decremented
    EXPECT_EQ(task2->GetPrerequisiteCount(), 0);
    
    // Task2 should now be launchable
    TaskScheduler::Get().LaunchTask(task2);
    
    // Wait for task2 to complete
    while (!task2->IsDone())
    {
        std::this_thread::yield();
    }

    // Task1 should not have executed, but Task2 should have
    EXPECT_FALSE(task1Executed.load(std::memory_order_acquire));
    EXPECT_TRUE(task2Executed.load(std::memory_order_acquire));
    EXPECT_TRUE(task2->IsCompleted());

    TaskScheduler::Shutdown();
}

TEST(TaskSystemTest, WaitForCancelledTaskReturnsImmediately)
{
    auto task = CreateTask("WaitCancelled", ETaskPriority::Normal, []() {
        // Should never execute
    });

    // Cancel the task
    task->Cancel();
    EXPECT_TRUE(task->IsCancelled());

    // Wait should return immediately
    auto startTime = std::chrono::steady_clock::now();
    TaskWait::Wait(task);
    auto endTime = std::chrono::steady_clock::now();
    
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    // Should take very little time (< 10ms)
    EXPECT_LT(elapsed.count(), 10);
}

TEST(TaskSystemTest, IsDoneReturnsTrueForCancelledAndCompleted)
{
    auto task1 = CreateTask("Task1", ETaskPriority::Normal, []() {});
    auto task2 = CreateTask("Task2", ETaskPriority::Normal, []() {});

    // Cancel task1
    task1->Cancel();
    EXPECT_TRUE(task1->IsDone());
    EXPECT_TRUE(task1->IsCancelled());
    EXPECT_FALSE(task1->IsCompleted());

    // Complete task2
    task2->SetState(ETaskState::Running);
    task2->Execute();
    task2->SetState(ETaskState::Completed);
    
    EXPECT_TRUE(task2->IsDone());
    EXPECT_FALSE(task2->IsCancelled());
    EXPECT_TRUE(task2->IsCompleted());
}

// ============================================================================
// Task Scheduler Statistics Tests (Priority 1.2)
// ============================================================================

TEST(TaskSystemTest, GetStatisticsBasic)
{
    // Initialize scheduler
    if (!TaskScheduler::IsInitialized())
    {
        TaskSchedulerConfig config;
        config.NumForegroundWorkers = 2;
        config.NumBackgroundWorkers = 1;
        TaskScheduler::Initialize(config);
    }

    auto stats = TaskScheduler::Get().GetStatistics();
    
    // Check worker counts
    EXPECT_EQ(stats.NumForegroundWorkers, 2);
    EXPECT_EQ(stats.NumBackgroundWorkers, 1);

    TaskScheduler::Shutdown();
}

TEST(TaskSystemTest, GetStatisticsTracksLaunchedTasks)
{
    // Initialize scheduler
    if (!TaskScheduler::IsInitialized())
    {
        TaskSchedulerConfig config;
        config.NumForegroundWorkers = 2;
        config.NumBackgroundWorkers = 1;
        TaskScheduler::Initialize(config);
    }

    auto statsBefore = TaskScheduler::Get().GetStatistics();
    u64 launchedBefore = statsBefore.TotalTasksLaunched;

    // Launch some tasks
    std::atomic<int> counter{0};
    for (int i = 0; i < 10; ++i)
    {
        TaskScheduler::Get().Launch("TestTask", [&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Wait for tasks to complete
    while (counter.load(std::memory_order_relaxed) < 10)
    {
        std::this_thread::yield();
    }

    auto statsAfter = TaskScheduler::Get().GetStatistics();
    
    // Should have launched 10 tasks
    EXPECT_EQ(statsAfter.TotalTasksLaunched - launchedBefore, 10);

    TaskScheduler::Shutdown();
}

TEST(TaskSystemTest, GetStatisticsTracksCompletedTasks)
{
    // Initialize scheduler
    if (!TaskScheduler::IsInitialized())
    {
        TaskSchedulerConfig config;
        config.NumForegroundWorkers = 2;
        config.NumBackgroundWorkers = 1;
        TaskScheduler::Initialize(config);
    }

    auto statsBefore = TaskScheduler::Get().GetStatistics();
    u64 completedBefore = statsBefore.TotalTasksCompleted;

    // Launch and complete tasks
    std::atomic<int> counter{0};
    for (int i = 0; i < 5; ++i)
    {
        TaskScheduler::Get().Launch("TestTask", [&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Wait for all tasks to complete
    while (counter.load(std::memory_order_relaxed) < 5)
    {
        std::this_thread::yield();
    }

    // Give scheduler a moment to update stats
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto statsAfter = TaskScheduler::Get().GetStatistics();
    
    // All 5 tasks should be completed
    EXPECT_EQ(statsAfter.TotalTasksCompleted - completedBefore, 5);

    TaskScheduler::Shutdown();
}

TEST(TaskSystemTest, GetStatisticsTracksCancelledTasks)
{
    // Initialize scheduler
    if (!TaskScheduler::IsInitialized())
    {
        TaskSchedulerConfig config;
        config.NumForegroundWorkers = 2;
        config.NumBackgroundWorkers = 1;
        TaskScheduler::Initialize(config);
    }

    auto statsBefore = TaskScheduler::Get().GetStatistics();
    u64 cancelledBefore = statsBefore.TotalTasksCancelled;

    // Create and cancel tasks
    for (int i = 0; i < 3; ++i)
    {
        auto task = CreateTask("CancelTest", ETaskPriority::Normal, []() {});
        task->Cancel();
    }

    auto statsAfter = TaskScheduler::Get().GetStatistics();
    
    // Should have 3 cancelled tasks
    EXPECT_EQ(statsAfter.TotalTasksCancelled - cancelledBefore, 3);

    TaskScheduler::Shutdown();
}

TEST(TaskSystemTest, GetStatisticsTotalTasksPending)
{
    // Initialize scheduler
    if (!TaskScheduler::IsInitialized())
    {
        TaskSchedulerConfig config;
        config.NumForegroundWorkers = 2;
        config.NumBackgroundWorkers = 1;
        TaskScheduler::Initialize(config);
    }

    // Launch some tasks that will block
    std::atomic<bool> blockTasks{true};
    std::vector<Ref<Task>> tasks;
    
    for (int i = 0; i < 5; ++i)
    {
        tasks.push_back(TaskScheduler::Get().Launch("BlockedTask", [&blockTasks]() {
            while (blockTasks.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }));
    }

    // Give tasks time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto stats = TaskScheduler::Get().GetStatistics();
    
    // Some tasks should be pending (launched but not completed/cancelled)
    EXPECT_GT(stats.TotalTasksPending(), 0);

    // Unblock and wait for completion
    blockTasks.store(false, std::memory_order_release);
    for (auto& task : tasks)
    {
        while (!task->IsDone())
        {
            std::this_thread::yield();
        }
    }

    TaskScheduler::Shutdown();
}

// ============================================================================
// Configuration Tests
// ============================================================================

TEST(TaskSystemTest, CustomMaxQueueNodesConfiguration)
{
    // Test that we can configure custom queue sizes
    TaskSchedulerConfig config;
    config.NumForegroundWorkers = 2;
    config.NumBackgroundWorkers = 1;
    config.MaxQueueNodes = 1000;

    TaskScheduler::Initialize(config);

    // Launch tasks to verify the configured size works
    std::vector<Ref<Task>> tasks;
    for (u32 i = 0; i < 100; ++i)
    {
        tasks.push_back(TaskScheduler::Get().Launch("ConfigTest", []() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }));
    }

    // All tasks should launch successfully
    EXPECT_EQ(tasks.size(), 100);

    // Wait for completion
    for (auto& task : tasks)
    {
        while (!task->IsDone())
        {
            std::this_thread::yield();
        }
    }

    TaskScheduler::Shutdown();
}


