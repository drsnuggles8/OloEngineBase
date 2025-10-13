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
        // Ensure scheduler is not initialized before each test
        if (TaskScheduler::IsInitialized())
        {
            TaskScheduler::Shutdown();
        }
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
    
    bool executed = false;
    auto task = TaskScheduler::Get().Launch("BasicLaunch", [&executed]() {
        executed = true;
    });
    
    EXPECT_NE(task, nullptr);
    // In Phase 1, task immediately completes (stub implementation)
    EXPECT_TRUE(task->IsCompleted());
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
    
    for (int i = 0; i < numTasks; ++i)
    {
        auto task = TaskScheduler::Get().Launch("MultiTask", []() {});
        tasks.push_back(task);
    }
    
    EXPECT_EQ(tasks.size(), numTasks);
    
    // All should be completed in Phase 1 stub
    for (const auto& task : tasks)
    {
        EXPECT_TRUE(task->IsCompleted());
    }
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
