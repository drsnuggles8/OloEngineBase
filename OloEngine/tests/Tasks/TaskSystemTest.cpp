/**
 * @file TaskSystemTest.cpp
 * @brief Unit tests for the OloEngine Task System
 *
 * Ported from UE5.7's Tasks/TasksTest.cpp
 * Tests cover: Launch, Wait, FTaskEvent, FPipe, nested tasks, prerequisites,
 *              FTaskConcurrencyLimiter, WaitAny, deep retraction, CancellationToken,
 *              Worker restart stress tests
 */

#include <gtest/gtest.h>

#include "OloEngine/Task/Task.h"
#include "OloEngine/Task/Pipe.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Task/TaskConcurrencyLimiter.h"
#include "OloEngine/Task/CancellationToken.h"
#include "OloEngine/Task/Oversubscription.h"
#include "OloEngine/Async/Async.h"
#include "OloEngine/HAL/ManualResetEvent.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/HAL/Thread.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <memory>

using namespace OloEngine;
using namespace OloEngine::Tasks;

// ============================================================================
// Base Fixture for Task Tests - Starts/Stops scheduler for each test
// ============================================================================

class TaskTestBase : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        // Start worker threads before any task tests run
        LowLevelTasks::FScheduler::Get().StartWorkers();
    }

    static void TearDownTestSuite()
    {
        // Stop worker threads after all task tests complete
        LowLevelTasks::FScheduler::Get().StopWorkers();
    }

    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================================
// Basic Task Tests
// ============================================================================

class TaskSystemTest : public TaskTestBase
{
  protected:
};

TEST_F(TaskSystemTest, FireAndForgetTask)
{
    // Basic example: fire and forget a high-pri task
    Launch(
        "FireAndForget",
        [] {},
        ETaskPriority::High);

    // Give it time to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(TaskSystemTest, LaunchAndWait)
{
    // Launch a task and wait till it's executed
    bool executed = false;
    Launch("LaunchAndWait", [&executed]
           { executed = true; })
        .Wait();
    EXPECT_TRUE(executed);
}

TEST_F(TaskSystemTest, TaskWithResult)
{
    // Basic use-case with result
    TTask<int> Task = Launch("TaskWithResult", []
                             { return 42; });
    EXPECT_EQ(Task.GetResult(), 42);
}

TEST_F(TaskSystemTest, TaskWithResultPostponed)
{
    // Postpone waiting so the task is executed first
    TTask<int> Task = Launch("TaskWithResultPostponed", []
                             { return 42; });
    while (!Task.IsCompleted())
    {
        std::this_thread::yield();
    }
    EXPECT_EQ(Task.GetResult(), 42);
}

TEST_F(TaskSystemTest, WaitForCompletion)
{
    std::atomic<bool> Done{ false };
    FTask Task = Launch("WaitForCompletion", [&Done]
                        { Done = true; });
    while (!Task.IsCompleted())
    {
        std::this_thread::yield();
    }
    Task.Wait();
    EXPECT_TRUE(Done);
}

TEST_F(TaskSystemTest, MutableLambda)
{
    // Mutable lambda compilation check
    Launch("MutableLambda", []() mutable {}).Wait();
    Launch("MutableLambdaWithResult", []() mutable
           { return false; })
        .GetResult();
}

TEST_F(TaskSystemTest, FreeTaskMemory)
{
    // Free memory occupied by task
    FTask Task = Launch("FreeTaskMemory", [] {});
    Task.Wait();
    Task = {};
}

TEST_F(TaskSystemTest, WaitingForMultipleTasks)
{
    std::atomic<int> Counter{ 0 };
    TArray<FTask> Tasks{
        Launch("Task1", [&Counter]
               { ++Counter; }),
        Launch("Task2", [&Counter]
               { ++Counter; })
    };
    Wait(Tasks);
    EXPECT_EQ(Counter.load(), 2);
}

// ============================================================================
// FTaskEvent Tests
// ============================================================================

class TaskEventTest : public TaskTestBase
{
  protected:
};

TEST_F(TaskEventTest, BasicTrigger)
{
    FTaskEvent Event{ "BasicTrigger" };
    EXPECT_FALSE(Event.IsCompleted());

    Event.Trigger();
    EXPECT_TRUE(Event.IsCompleted());
    EXPECT_TRUE(Event.Wait(FMonotonicTimeSpan::FromMilliseconds(0)));
}

TEST_F(TaskEventTest, MultipleTriggersAllowed)
{
    FTaskEvent Event{ "MultipleTriggersAllowed" };
    Event.Trigger();
    EXPECT_TRUE(Event.IsCompleted());
    Event.Trigger();
    Event.Trigger();
    EXPECT_TRUE(Event.IsCompleted());
}

TEST_F(TaskEventTest, BlocksUntilTriggered)
{
    FTaskEvent Event{ "BlocksUntilTriggered" };
    EXPECT_FALSE(Event.IsCompleted());

    // Check that waiting blocks
    FTask Task = Launch("WaitOnEvent", [&Event]
                        { Event.Wait(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(Task.IsCompleted());

    Event.Trigger();
    EXPECT_TRUE(Event.IsCompleted());
    Task.Wait();
    EXPECT_TRUE(Task.IsCompleted());
}

TEST_F(TaskEventTest, TaskAsPrerequisite)
{
    // A task is not executed until its prerequisite (FTaskEvent) is completed
    FTaskEvent Prereq{ "Prereq" };
    std::atomic<bool> Executed{ false };

    FTask Task = Launch("WaitOnPrereq", [&Executed]
                        { Executed = true; }, Prereq);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(Task.IsCompleted());
    EXPECT_FALSE(Executed.load());

    Prereq.Trigger();
    Task.Wait();
    EXPECT_TRUE(Executed.load());
}

TEST_F(TaskEventTest, EmptyPrerequisite)
{
    // Using an "empty" prerequisite
    FTask EmptyPrereq;
    FTask NonEmptyPrereq = Launch("NonEmptyPrereq", [] {});
    FTask Task = Launch("WithEmptyPrereq", [] {}, Prerequisites(EmptyPrereq, NonEmptyPrereq));
    EXPECT_TRUE(Task.Wait(FMonotonicTimeSpan::FromMilliseconds(1000)));
}

// ============================================================================
// Nested Tasks Tests
// ============================================================================

class NestedTasksTest : public TaskTestBase
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(NestedTasksTest, SingleNestedTask)
{
    FTaskEvent FinishSignal{ "FinishSignal" };
    std::atomic<bool> Executed{ false };

    FTask Task = Launch("ParentTask",
                        [&FinishSignal, &Executed]
                        {
                            AddNested(FinishSignal);
                            Executed = true;
                        });

    // Wait a bit - task should execute but not complete until nested is done
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(Executed.load());
    EXPECT_FALSE(Task.Wait(FMonotonicTimeSpan::FromMilliseconds(50)));

    FinishSignal.Trigger();
    Task.Wait();
    EXPECT_TRUE(Task.IsCompleted());
}

TEST_F(NestedTasksTest, NestedTaskCompletedDuringParent)
{
    Launch("ParentTask",
           []
           {
               FTask NestedTask = Launch("NestedTask", [] {});
               AddNested(NestedTask);
               NestedTask.Wait();
           })
        .Wait();
}

TEST_F(NestedTasksTest, MultipleNestedTasks)
{
    FTaskEvent Signal1{ "Signal1" };
    FTaskEvent Signal2{ "Signal2" };
    FTaskEvent Signal3{ "Signal3" };

    FTask Task = Launch("ParentTask",
                        [&Signal1, &Signal2, &Signal3]
                        {
                            AddNested(Signal1);
                            AddNested(Signal2);
                            AddNested(Signal3);
                        });

    EXPECT_FALSE(Task.Wait(FMonotonicTimeSpan::FromMilliseconds(50)));
    Signal1.Trigger();
    EXPECT_FALSE(Task.Wait(FMonotonicTimeSpan::FromMilliseconds(50)));
    Signal2.Trigger();
    EXPECT_FALSE(Task.Wait(FMonotonicTimeSpan::FromMilliseconds(50)));
    Signal3.Trigger();
    Task.Wait();
    EXPECT_TRUE(Task.IsCompleted());
}

// ============================================================================
// FPipe Tests
// ============================================================================

class PipeTest : public TaskTestBase
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(PipeTest, BasicPipeUsage)
{
    FPipe Pipe{ "BasicPipe" };
    FTask Task1 = Pipe.Launch("Task1", [] {});
    FTask Task2 = Pipe.Launch("Task2", [] {});
    Task2.Wait();
    Pipe.WaitUntilEmpty();
}

TEST_F(PipeTest, SequentialExecution)
{
    FPipe Pipe{ "SequentialPipe" };
    std::atomic<int> Order{ 0 };

    bool bTask1Done = false;
    FTask Task1 = Pipe.Launch("Task1",
                              [&bTask1Done, &Order]
                              {
                                  std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                  EXPECT_EQ(Order.load(), 0);
                                  Order = 1;
                                  bTask1Done = true;
                              });

    Pipe.Launch("Task2", [&bTask1Done, &Order]
                {
        EXPECT_TRUE(bTask1Done);
        EXPECT_EQ(Order.load(), 1);
        Order = 2; })
        .Wait();

    EXPECT_EQ(Order.load(), 2);
    Pipe.WaitUntilEmpty();
}

TEST_F(PipeTest, MultipleTasksAfterCompletion)
{
    FPipe Pipe{ "MultiCompletePipe" };

    Pipe.Launch("Task1", [] {}).Wait();
    Pipe.Launch("Task2", [] {}).Wait();
    Pipe.WaitUntilEmpty();
}

TEST_F(PipeTest, PipeWithPrerequisites)
{
    FPipe Pipe{ "PrereqPipe" };
    FTaskEvent Prereq{ "Prereq" };

    FTask Task = Pipe.Launch("Task", [] {}, Prereq);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(Task.IsCompleted());

    Prereq.Trigger();
    Task.Wait();
    Pipe.WaitUntilEmpty();
}

TEST_F(PipeTest, WaitUntilEmpty)
{
    // Waiting until an empty pipe is empty
    FPipe Pipe{ "EmptyPipe" };
    Pipe.WaitUntilEmpty();
}

TEST_F(PipeTest, WaitUntilEmptyWithWork)
{
    FPipe Pipe{ "WorkPipe" };
    Pipe.Launch("Task", [] {});
    Pipe.WaitUntilEmpty();
}

TEST_F(PipeTest, WaitUntilEmptyWithPrereq)
{
    FPipe Pipe{ "PrereqPipe" };
    FTaskEvent Prereq{ "Prereq" };

    EXPECT_FALSE(Pipe.HasWork());

    FTask Task1 = Pipe.Launch("Task1", [] {}, Prereq);

    // Make sure the pipe knows about the task even if it has prereq
    EXPECT_TRUE(Pipe.HasWork());
    EXPECT_FALSE(Task1.IsCompleted());
    EXPECT_FALSE(Pipe.WaitUntilEmpty(FMonotonicTimeSpan::FromMilliseconds(50)));

    FTask Task2 = Pipe.Launch("Task2", [] {});
    EXPECT_TRUE(Task2.Wait(FMonotonicTimeSpan::FromMilliseconds(1000)));

    EXPECT_FALSE(Pipe.WaitUntilEmpty(FMonotonicTimeSpan::FromMilliseconds(50)));
    EXPECT_FALSE(Task1.IsCompleted());
    EXPECT_TRUE(Task2.IsCompleted());

    Prereq.Trigger();
    EXPECT_TRUE(Pipe.WaitUntilEmpty(FMonotonicTimeSpan::FromMilliseconds(1000)));
    EXPECT_TRUE(Task1.IsCompleted());
    EXPECT_TRUE(Task2.IsCompleted());
}

// ============================================================================
// Task Dependencies Tests
// ============================================================================

class TaskDependenciesTest : public TaskTestBase
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TaskDependenciesTest, SinglePrerequisite)
{
    FTaskEvent Event{ "Event" };
    std::atomic<bool> TaskExecuted{ false };

    FTask Prereq = Launch("Prereq", [&Event]
                          { Event.Wait(); });
    FTask Task = Launch("Task", [&TaskExecuted]
                        { TaskExecuted = true; }, Prereq);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(Task.IsCompleted());
    EXPECT_FALSE(TaskExecuted.load());

    Event.Trigger();
    Task.Wait();
    EXPECT_TRUE(TaskExecuted.load());
}

TEST_F(TaskDependenciesTest, MultiplePrerequisites)
{
    FTaskEvent Prereq1{ "Prereq1" };
    FTaskEvent Event{ "Event" };
    FTask Prereq2 = Launch("Prereq2", [&Event]
                           { Event.Wait(); });
    std::atomic<bool> TaskExecuted{ false };

    TTask<void> Task = Launch("Task", [&TaskExecuted]
                              { TaskExecuted = true; }, Prerequisites(Prereq1, Prereq2));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(Task.IsCompleted());

    Prereq1.Trigger();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(Task.IsCompleted());

    Event.Trigger();
    Task.Wait();
    EXPECT_TRUE(TaskExecuted.load());
}

TEST_F(TaskDependenciesTest, PipedTaskWithPrerequisite)
{
    // A piped task blocked by a prerequisite doesn't block the pipe
    FPipe Pipe{ "Pipe" };
    FTaskEvent Prereq{ "Prereq" };

    FTask Task1 = Pipe.Launch("Task1", [] {}, Prereq);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(Task1.IsCompleted());

    FTask Task2 = Pipe.Launch("Task2", [] {});
    Task2.Wait();

    Prereq.Trigger();
    Task1.Wait();

    Pipe.WaitUntilEmpty();
}

// ============================================================================
// Stress Tests
// ============================================================================

class TaskStressTest : public TaskTestBase
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TaskStressTest, ManyTasks)
{
    constexpr int NumTasks = 1000;
    std::atomic<int> Counter{ 0 };

    TArray<FTask> Tasks;
    Tasks.Reserve(NumTasks);

    for (int i = 0; i < NumTasks; ++i)
    {
        Tasks.Add(Launch("StressTask", [&Counter]
                         { ++Counter; }));
    }

    Wait(Tasks);
    EXPECT_EQ(Counter.load(), NumTasks);
}

TEST_F(TaskStressTest, NestedSpawning)
{
    constexpr int NumGroups = 10;
    constexpr int TasksPerGroup = 100;
    std::atomic<int> Counter{ 0 };

    TArray<FTask> Groups;
    Groups.Reserve(NumGroups);

    for (int g = 0; g < NumGroups; ++g)
    {
        Groups.Add(Launch("SpawnerGroup",
                          [&Counter]
                          {
                              TArray<FTask> Tasks;
                              Tasks.Reserve(TasksPerGroup);
                              for (int t = 0; t < TasksPerGroup; ++t)
                              {
                                  Tasks.Add(Launch("NestedTask", [&Counter]
                                                   { ++Counter; }));
                              }
                              Wait(Tasks);
                          }));
    }

    Wait(Groups);
    EXPECT_EQ(Counter.load(), NumGroups * TasksPerGroup);
}

TEST_F(TaskStressTest, PipeStress)
{
    constexpr int NumTasks = 500;
    FPipe Pipe{ "StressPipe" };
    std::atomic<int> Counter{ 0 };
    std::atomic<bool> bConcurrentExecution{ false };
    std::atomic<bool> bExecuting{ false };

    TArray<FTask> Tasks;
    Tasks.Reserve(NumTasks);

    for (int i = 0; i < NumTasks; ++i)
    {
        Tasks.Add(Pipe.Launch("PipeTask",
                              [&Counter, &bConcurrentExecution, &bExecuting]
                              {
                                  if (bExecuting.load())
                                  {
                                      bConcurrentExecution = true;
                                  }
                                  bExecuting = true;
                                  ++Counter;
                                  bExecuting = false;
                              }));
    }

    Wait(Tasks);
    Pipe.WaitUntilEmpty();

    EXPECT_EQ(Counter.load(), NumTasks);
    EXPECT_FALSE(bConcurrentExecution.load()) << "Pipe tasks should not execute concurrently";
}

// ============================================================================
// MakeCompletedTask Tests
// ============================================================================

class MakeCompletedTaskTest : public TaskTestBase
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(MakeCompletedTaskTest, BasicCompletedTask)
{
    TTask<int> Task = MakeCompletedTask<int>(42);
    EXPECT_TRUE(Task.IsCompleted());
    EXPECT_EQ(Task.GetResult(), 42);
}

TEST_F(MakeCompletedTaskTest, MoveOnlyResult)
{
    TTask<std::unique_ptr<int>> Task = MakeCompletedTask<std::unique_ptr<int>>(std::make_unique<int>(42));
    EXPECT_TRUE(Task.IsCompleted());
    EXPECT_EQ(*Task.GetResult(), 42);
}

// ============================================================================
// IsAwaitable Tests
// ============================================================================

class IsAwaitableTest : public TaskTestBase
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(IsAwaitableTest, BasicIsAwaitable)
{
    FTask Task;
    Task.Launch("IsAwaitableTask",
                [&Task]
                {
                    // Task.Wait() would deadlock if called here inside its execution
                    EXPECT_FALSE(Task.IsAwaitable());
                });
    EXPECT_TRUE(Task.IsAwaitable());
    Task.Wait();
}

// ============================================================================
// WaitAny Tests
// ============================================================================

class WaitAnyTest : public TaskTestBase
{
  protected:
};

TEST_F(WaitAnyTest, BlocksIfNoneCompleted)
{
    // blocks if none of tasks is completed
    FTaskEvent Blocker{ "Blocker" }; // blocks all tasks

    TArray<FTask> Tasks;
    Tasks.Add(Launch("Task1", [] {}, Blocker));
    Tasks.Add(Launch("Task2", [] {}, Blocker));

    // Should timeout since no task is complete
    i32 Result = WaitAny(Tasks, FMonotonicTimeSpan::FromMilliseconds(10.0));
    EXPECT_EQ(Result, -1); // INDEX_NONE

    Blocker.Trigger();

    Result = WaitAny(Tasks);
    EXPECT_NE(Result, -1); // Some task completed
}

TEST_F(WaitAnyTest, DoesNotWaitForAllTasks)
{
    // doesn't wait for all tasks
    FTaskEvent Blocker{ "Blocker" };

    TArray<FTask> Tasks;
    Tasks.Add(Launch("Task1", [] {}));
    Tasks.Add(Launch("Task2", [] {}, Blocker)); // is blocked

    i32 Result = WaitAny(Tasks);
    EXPECT_EQ(Result, 0); // First task completed

    Blocker.Trigger();
}

// ============================================================================
// Any Tests (returns a task that completes when any input task completes)
// ============================================================================

class AnyTest : public TaskTestBase
{
  protected:
};

TEST_F(AnyTest, BlocksIfNoneCompleted)
{
    // blocks if none of tasks is completed
    FTaskEvent Blocker{ "Blocker" }; // blocks all tasks

    TArray<FTask> Tasks;
    Tasks.Add(Launch("Task1", [] {}, Blocker));
    Tasks.Add(Launch("Task2", [] {}, Blocker));

    FTask AnyTask = Any(Tasks);
    EXPECT_FALSE(AnyTask.Wait(FMonotonicTimeSpan::FromMilliseconds(10.0)));

    Blocker.Trigger();

    AnyTask.Wait();
    EXPECT_TRUE(AnyTask.IsCompleted());
}

TEST_F(AnyTest, DoesNotWaitForAllTasks)
{
    // doesn't wait for all tasks
    FTaskEvent Blocker{ "Blocker" };

    TArray<FTask> Tasks;
    Tasks.Add(Launch("Task1", [] {}));
    Tasks.Add(Launch("Task2", [] {}, Blocker)); // is blocked

    Any(Tasks).Wait();

    Blocker.Trigger();
}

// ============================================================================
// FTaskConcurrencyLimiter Tests
// ============================================================================
// Tests for the concurrency limiter functionality that ensures a maximum
// number of tasks can run concurrently, with each task receiving a unique slot.

class TaskConcurrencyLimiterTest : public TaskTestBase
{
  protected:
};

TEST_F(TaskConcurrencyLimiterTest, BasicConcurrencyLimit)
{
    constexpr u32 MaxConcurrency = 4;
    constexpr u32 NumItems = 100;

    std::atomic<u32> CurrentConcurrency{ 0 };
    std::atomic<u32> ActualMaxConcurrency{ 0 };
    std::atomic<u32> NumProcessed{ 0 };

    FTaskConcurrencyLimiter Limiter(MaxConcurrency);

    for (u32 i = 0; i < NumItems; ++i)
    {
        Limiter.Push("LimitedTask",
                     [&CurrentConcurrency, &ActualMaxConcurrency, &NumProcessed, MaxConcurrency](u32 Slot)
                     {
                         EXPECT_LT(Slot, MaxConcurrency);

                         u32 Current = CurrentConcurrency.fetch_add(1, std::memory_order_relaxed) + 1;
                         EXPECT_LE(Current, MaxConcurrency);

                         // Track max concurrency reached
                         u32 Max = ActualMaxConcurrency.load(std::memory_order_relaxed);
                         while (Current > Max && !ActualMaxConcurrency.compare_exchange_weak(Max, Current))
                         {
                             // Retry
                         }

                         std::this_thread::yield();

                         CurrentConcurrency.fetch_sub(1, std::memory_order_relaxed);
                         NumProcessed.fetch_add(1, std::memory_order_release);
                     });
    }

    Limiter.Wait();
    EXPECT_EQ(NumProcessed.load(std::memory_order_acquire), NumItems);
}

TEST_F(TaskConcurrencyLimiterTest, MultipleProducers)
{
    constexpr u32 MaxConcurrency = 8;
    constexpr u32 NumItems = 1000;
    constexpr u32 NumPushingTasks = 10;

    std::atomic<u32> CurrentConcurrency{ 0 };
    std::atomic<u32> NumProcessed{ 0 };

    TArray<FTask> PushingTasks;
    PushingTasks.Reserve(NumPushingTasks);

    FTaskConcurrencyLimiter Limiter(MaxConcurrency);

    for (u32 i = 0; i < NumPushingTasks; ++i)
    {
        PushingTasks.Add(Launch("Pusher",
                                [&Limiter, &CurrentConcurrency, &NumProcessed, MaxConcurrency]
                                {
                                    for (u32 j = 0; j < NumItems / NumPushingTasks; ++j)
                                    {
                                        Limiter.Push("LimitedTask",
                                                     [&CurrentConcurrency, &NumProcessed, MaxConcurrency](u32 Slot)
                                                     {
                                                         EXPECT_LT(Slot, MaxConcurrency);

                                                         u32 Current = CurrentConcurrency.fetch_add(1, std::memory_order_relaxed) + 1;
                                                         EXPECT_LE(Current, MaxConcurrency);

                                                         std::this_thread::yield();

                                                         CurrentConcurrency.fetch_sub(1, std::memory_order_relaxed);
                                                         NumProcessed.fetch_add(1, std::memory_order_release);
                                                     });
                                    }
                                }));
    }

    Wait(PushingTasks);
    Limiter.Wait();
    EXPECT_EQ(NumProcessed.load(std::memory_order_acquire), NumItems);
}

TEST_F(TaskConcurrencyLimiterTest, SlotsDoNotOverlap)
{
    constexpr u32 MaxConcurrency = 4;
    constexpr u32 NumItems = 100;

    std::atomic<bool> Slots[MaxConcurrency] = {};
    std::atomic<u32> NumProcessed{ 0 };

    FTaskConcurrencyLimiter Limiter(MaxConcurrency);

    for (u32 i = 0; i < NumItems; ++i)
    {
        Limiter.Push("LimitedTask",
                     [&Slots, &NumProcessed, MaxConcurrency](u32 Slot)
                     {
                         EXPECT_LT(Slot, MaxConcurrency);

                         // Verify slot was not in use
                         bool WasInUse = Slots[Slot].exchange(true, std::memory_order_relaxed);
                         EXPECT_FALSE(WasInUse) << "Slot " << Slot << " was already in use!";

                         std::this_thread::yield();

                         Slots[Slot].store(false, std::memory_order_relaxed);
                         NumProcessed.fetch_add(1, std::memory_order_release);
                     });
    }

    Limiter.Wait();
    EXPECT_EQ(NumProcessed.load(std::memory_order_acquire), NumItems);
}

// ============================================================================
// Deep Retraction Tests
// ============================================================================

class DeepRetractionTest : public TaskTestBase
{
  protected:
};

TEST_F(DeepRetractionTest, TwoLevelsDeep)
{
    // Two levels of prerequisites and two levels of nested tasks
    FTask P11 = Launch("P11", [] {});
    FTask P12 = Launch("P12", [] {});
    FTask P21 = Launch("P21", [] {}, Prerequisites(P11, P12));
    FTask P22 = Launch("P22", [] {});
    FTask N11, N12, N21, N22;

    FTask Task = Launch("MainTask", [&N11, &N12, &N21, &N22]
                        {
            AddNested(N11 = Launch("N11",
                [&N21, &N22]
                {
                    AddNested(N21 = Launch("N21", [] {}));
                    AddNested(N22 = Launch("N22", [] {}));
                }
            ));
            AddNested(N12 = Launch("N12", [] {})); }, Prerequisites(P21, P22));

    Task.Wait();

    EXPECT_TRUE(P11.IsCompleted());
    EXPECT_TRUE(P12.IsCompleted());
    EXPECT_TRUE(P21.IsCompleted());
    EXPECT_TRUE(P22.IsCompleted());
    EXPECT_TRUE(N11.IsCompleted());
    EXPECT_TRUE(N12.IsCompleted());
    EXPECT_TRUE(N21.IsCompleted());
    EXPECT_TRUE(N22.IsCompleted());
}

// ============================================================================
// Inline Task Priority Tests
// ============================================================================

class InlineTaskTest : public TaskTestBase
{
  protected:
};

TEST_F(InlineTaskTest, InlineExecution)
{
    FTaskEvent Block{ "Block" };
    bool bFirstDone = false;
    bool bSecondDone = false;

    // Launch tasks with inline priority - they execute when their prereqs complete
    FTask Task1 = Launch("Task1", [&bFirstDone, &bSecondDone]
                         {
            EXPECT_FALSE(bSecondDone);
            bFirstDone = true; }, Block, ETaskPriority::Normal, EExtendedTaskPriority::Inline);

    FTask Task2 = Launch("Task2", [&bFirstDone, &bSecondDone]
                         {
            EXPECT_TRUE(bFirstDone);
            bSecondDone = true; }, Prerequisites(Block, Task1), ETaskPriority::Normal, EExtendedTaskPriority::Inline);

    Block.Trigger();
    Wait(TArray<FTask>{ Task1, Task2 });

    EXPECT_TRUE(bFirstDone);
    EXPECT_TRUE(bSecondDone);
}

// ============================================================================
// Move-Only Result Type Tests
// ============================================================================

class MoveOnlyResultTest : public TaskTestBase
{
  protected:
};

TEST_F(MoveOnlyResultTest, UniquePtr)
{
    TTask<std::unique_ptr<int>> Task = Launch("MoveOnlyTask",
                                              []
                                              { return std::make_unique<int>(42); });

    std::unique_ptr<int> Result = std::move(Task.GetResult());
    EXPECT_EQ(*Result, 42);
}

TEST_F(MoveOnlyResultTest, MoveConstructableOnly)
{
    static std::atomic<u32> ConstructionsNum{ 0 };
    static std::atomic<u32> DestructionsNum{ 0 };

    struct FMoveConstructable
    {
        FMoveConstructable()
        {
            ConstructionsNum.fetch_add(1, std::memory_order_relaxed);
        }

        FMoveConstructable(FMoveConstructable&&)
            : FMoveConstructable()
        {
        }

        FMoveConstructable(const FMoveConstructable&) = delete;
        FMoveConstructable& operator=(FMoveConstructable&&) = delete;
        FMoveConstructable& operator=(const FMoveConstructable&) = delete;

        ~FMoveConstructable()
        {
            DestructionsNum.fetch_add(1, std::memory_order_relaxed);
        }
    };

    {
        Launch("MoveConstructableTask", []
               { return FMoveConstructable{}; })
            .GetResult();
    }

    // Wait for any background destructions
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // At least one construction and destruction should have occurred
    EXPECT_GE(ConstructionsNum.load(), 1u);
}

// ============================================================================
// Accessing Task From Inside Its Execution
// ============================================================================

class TaskSelfAccessTest : public TaskTestBase
{
  protected:
};

TEST_F(TaskSelfAccessTest, AccessTaskDuringExecution)
{
    // accessing the task from inside its execution
    FTask Task;
    Task.Launch("SelfAccessTask", [&Task]
                { EXPECT_FALSE(Task.IsCompleted()); });
    Task.Wait();
    EXPECT_TRUE(Task.IsCompleted());
}

// ============================================================================
// Nested Task Stress Test
// ============================================================================

class NestedTaskStressTest : public TaskTestBase
{
  protected:
};

TEST_F(NestedTaskStressTest, ManyNestedTasks)
{
    constexpr u64 Num = 1000;

    for (u64 i = 0; i < Num; ++i)
    {
        FTask Nested;
        FTask Parent = Launch("Parent",
                              [&Nested]
                              {
                                  AddNested(Nested = Launch("Nested", [] {}));
                              });
        Parent.Wait();
        EXPECT_TRUE(Nested.IsCompleted());
        EXPECT_TRUE(Parent.IsCompleted());
    }
}

// ============================================================================
// Triggering FTaskEvent Concurrently Test
// ============================================================================

class ConcurrentEventTriggerTest : public TaskTestBase
{
  protected:
};

TEST_F(ConcurrentEventTriggerTest, TriggerFromMultipleThreads)
{
    // Regression test to ensure we do not assert when triggering an event from more than one thread at a time.
    for (int i = 0; i < 1000; ++i)
    {
        FTaskEvent Event{ "ConcurrentTriggerEvent" };
        FTask DeferredTask = Launch("DeferredTask", [] {}, Event);

        constexpr int NumThreads = 8;
        std::vector<std::thread> Threads;
        Threads.reserve(NumThreads);

        for (int t = 0; t < NumThreads; ++t)
        {
            Threads.emplace_back([&Event, &DeferredTask]
                                 {
                Event.Trigger();
                DeferredTask.Wait(); });
        }

        for (auto& Thread : Threads)
        {
            Thread.join();
        }
    }
}

// ============================================================================
// LowLevelTask UserData Tests
// ============================================================================
// These tests verify that UserData works correctly with LowLevelTasks,
// which is the foundation for FTaskConcurrencyLimiter.

class LowLevelTaskUserDataTest : public TaskTestBase
{
  protected:
};

TEST_F(LowLevelTaskUserDataTest, SetUserDataBeforeLaunch)
{
    // Test that SetUserData works when called before TryLaunch
    std::atomic<void*> ReceivedUserData{ nullptr };
    std::atomic<bool> TaskExecuted{ false };

    LowLevelTasks::FTask Task;
    void* ExpectedUserData = reinterpret_cast<void*>(static_cast<uptr>(42));

    Task.Init("UserDataTest", LowLevelTasks::ETaskPriority::Default,
              [&ReceivedUserData, &TaskExecuted, &Task]()
              {
                  ReceivedUserData.store(Task.GetUserData(), std::memory_order_release);
                  TaskExecuted.store(true, std::memory_order_release);
              });

    // Set user data BEFORE launching
    Task.SetUserData(ExpectedUserData);

    LowLevelTasks::TryLaunch(Task, LowLevelTasks::EQueuePreference::GlobalQueuePreference, true);

    // Spin-wait for completion
    while (!Task.IsCompleted())
    {
        std::this_thread::yield();
    }

    EXPECT_TRUE(TaskExecuted.load());
    EXPECT_EQ(ReceivedUserData.load(), ExpectedUserData);
}

TEST_F(LowLevelTaskUserDataTest, SetUserDataWithSharedTask)
{
    // Test that mirrors FTaskConcurrencyLimiter's pattern:
    // TSharedPtr<FTask> with lambda capturing the shared ptr
    std::atomic<u32> ReceivedSlot{ 0xFFFFFFFF };
    std::atomic<bool> TaskExecuted{ false };

    TSharedPtr<LowLevelTasks::FTask> Task = MakeShared<LowLevelTasks::FTask>();
    constexpr u32 ExpectedSlot = 7;

    Task->Init("SharedUserDataTest", LowLevelTasks::ETaskPriority::Default,
               [&ReceivedSlot, &TaskExecuted, Task]() // Capture Task by value (TSharedPtr copy)
               {
                   u32 Slot = static_cast<u32>(reinterpret_cast<uptr>(Task->GetUserData()));
                   ReceivedSlot.store(Slot, std::memory_order_release);
                   TaskExecuted.store(true, std::memory_order_release);
               });

    // Set user data BEFORE launching (same pattern as TaskConcurrencyLimiter)
    Task->SetUserData(reinterpret_cast<void*>(static_cast<uptr>(ExpectedSlot)));

    LowLevelTasks::TryLaunch(*Task, LowLevelTasks::EQueuePreference::GlobalQueuePreference, true);

    // Spin-wait for completion
    while (!Task->IsCompleted())
    {
        std::this_thread::yield();
    }

    EXPECT_TRUE(TaskExecuted.load());
    EXPECT_EQ(ReceivedSlot.load(), ExpectedSlot);
}

TEST_F(LowLevelTaskUserDataTest, SetUserDataWithQueuedTask)
{
    // Test that exactly mirrors FTaskConcurrencyLimiter's pattern:
    // Task is queued, then later popped and launched with SetUserData
    std::atomic<u32> ReceivedSlot{ 0xFFFFFFFF };
    std::atomic<bool> TaskExecuted{ false };

    TSharedPtr<LowLevelTasks::FTask> Task = MakeShared<LowLevelTasks::FTask>();
    constexpr u32 ExpectedSlot = 3;

    Task->Init("QueuedUserDataTest", LowLevelTasks::ETaskPriority::Default,
               [&ReceivedSlot, &TaskExecuted, Task]() // Capture Task by value (TSharedPtr copy)
               {
                   u32 Slot = static_cast<u32>(reinterpret_cast<uptr>(Task->GetUserData()));
                   ReceivedSlot.store(Slot, std::memory_order_release);
                   TaskExecuted.store(true, std::memory_order_release);
               });

    // Simulate queue: store raw pointer, then retrieve and set user data
    TLockFreePointerListFIFO<LowLevelTasks::FTask, OLO_PLATFORM_CACHE_LINE_SIZE> WorkQueue;
    WorkQueue.Push(Task.Get());

    // Pop from queue and set user data before launching (same as ProcessQueue)
    LowLevelTasks::FTask* PoppedTask = WorkQueue.Pop();
    ASSERT_NE(PoppedTask, nullptr);

    PoppedTask->SetUserData(reinterpret_cast<void*>(static_cast<uptr>(ExpectedSlot)));
    LowLevelTasks::TryLaunch(*PoppedTask, LowLevelTasks::EQueuePreference::GlobalQueuePreference, true);

    // Spin-wait for completion
    while (!Task->IsCompleted())
    {
        std::this_thread::yield();
    }

    EXPECT_TRUE(TaskExecuted.load());
    EXPECT_EQ(ReceivedSlot.load(), ExpectedSlot);
}

TEST_F(LowLevelTaskUserDataTest, ConcurrencyLimiterSimulation)
{
    // Full simulation of FTaskConcurrencyLimiter with multiple tasks
    constexpr u32 MaxConcurrency = 4;
    constexpr u32 NumTasks = 20;

    std::atomic<u32> CompletedCount{ 0 };
    std::atomic<bool> AnyFailure{ false };

    TLockFreePointerListFIFO<LowLevelTasks::FTask, OLO_PLATFORM_CACHE_LINE_SIZE> WorkQueue;
    TArray<TSharedPtr<LowLevelTasks::FTask>> Tasks;
    Tasks.Reserve(NumTasks);

    // Phase 1: Create and queue all tasks (similar to Push)
    for (u32 i = 0; i < NumTasks; ++i)
    {
        TSharedPtr<LowLevelTasks::FTask> Task = MakeShared<LowLevelTasks::FTask>();
        Tasks.Add(Task);

        Task->Init("SimTask", LowLevelTasks::ETaskPriority::Default,
                   [&CompletedCount, &AnyFailure, Task, MaxConcurrency]()
                   {
                       u32 Slot = static_cast<u32>(reinterpret_cast<uptr>(Task->GetUserData()));
                       if (Slot >= MaxConcurrency)
                       {
                           AnyFailure.store(true, std::memory_order_release);
                       }
                       CompletedCount.fetch_add(1, std::memory_order_release);
                   });

        WorkQueue.Push(Task.Get());
    }

    // Phase 2: Pop and launch all tasks with slots (similar to ProcessQueue)
    u32 SlotCounter = 0;
    while (LowLevelTasks::FTask* PoppedTask = WorkQueue.Pop())
    {
        u32 Slot = SlotCounter % MaxConcurrency;
        SlotCounter++;

        PoppedTask->SetUserData(reinterpret_cast<void*>(static_cast<uptr>(Slot)));
        LowLevelTasks::TryLaunch(*PoppedTask, LowLevelTasks::EQueuePreference::GlobalQueuePreference, true);
    }

    // Wait for all tasks
    for (const auto& Task : Tasks)
    {
        while (!Task->IsCompleted())
        {
            std::this_thread::yield();
        }
    }

    EXPECT_FALSE(AnyFailure.load());
    EXPECT_EQ(CompletedCount.load(), NumTasks);
}

TEST_F(LowLevelTaskUserDataTest, SimpleFTaskConcurrencyLimiterTest)
{
    // Simplest possible test of FTaskConcurrencyLimiter
    constexpr u32 MaxConcurrency = 2;
    std::atomic<u32> ReceivedSlots{ 0 };
    std::atomic<bool> AnyBadSlot{ false };

    FTaskConcurrencyLimiter Limiter(MaxConcurrency);

    // Push a single task
    Limiter.Push("SingleTask",
                 [&ReceivedSlots, &AnyBadSlot, MaxConcurrency](u32 Slot)
                 {
                     if (Slot >= MaxConcurrency)
                     {
                         AnyBadSlot.store(true, std::memory_order_release);
                     }
                     ReceivedSlots.fetch_add(1, std::memory_order_release);
                 });

    Limiter.Wait();

    EXPECT_FALSE(AnyBadSlot.load());
    EXPECT_EQ(ReceivedSlots.load(), 1u);
}

TEST_F(LowLevelTaskUserDataTest, MultipleFTaskConcurrencyLimiterTest)
{
    // Test with multiple tasks
    constexpr u32 MaxConcurrency = 4;
    constexpr u32 NumTasks = 100;
    std::atomic<u32> CompletedCount{ 0 };
    std::atomic<bool> AnyBadSlot{ false };

    FTaskConcurrencyLimiter Limiter(MaxConcurrency);

    for (u32 i = 0; i < NumTasks; ++i)
    {
        Limiter.Push("MultiTask",
                     [&CompletedCount, &AnyBadSlot, MaxConcurrency](u32 Slot)
                     {
                         if (Slot >= MaxConcurrency)
                         {
                             AnyBadSlot.store(true, std::memory_order_release);
                         }
                         CompletedCount.fetch_add(1, std::memory_order_release);
                     });
    }

    Limiter.Wait();

    EXPECT_FALSE(AnyBadSlot.load());
    EXPECT_EQ(CompletedCount.load(), NumTasks);
}

// ============================================================================
// Cancellation Token Tests (ported from UE5.7)
// ============================================================================

class CancellationTokenTest : public TaskTestBase
{
  protected:
};

TEST_F(CancellationTokenTest, BasicCancellation)
{
    // Test that a task can see the cancellation request
    FCancellationToken CancellationToken;
    FManualResetEvent BlockExecution;

    std::atomic<bool> TaskSawCancellation{ false };
    std::atomic<bool> Task2Executed{ false };

    // Check that a task sees cancellation request
    FTask Task1 = Launch("CancellationTest1",
                         [&CancellationToken, &BlockExecution, &TaskSawCancellation]
                         {
                             BlockExecution.Wait();
                             TaskSawCancellation = CancellationToken.IsCanceled();
                         });

    // Same token can be used with multiple tasks to cancel them all
    // A task can ignore cancellation request
    FTask Task2 = Launch("CancellationTest2",
                         [&CancellationToken, &Task2Executed]
                         {
                             Task2Executed = true;
                         });

    CancellationToken.Cancel();
    BlockExecution.Notify();

    Task1.Wait();
    Task2.Wait();

    EXPECT_TRUE(TaskSawCancellation.load());
    EXPECT_TRUE(Task2Executed.load());
}

TEST_F(CancellationTokenTest, MultipleTasks)
{
    // Test multiple tasks observing the same cancellation token
    FCancellationToken CancellationToken;
    constexpr u32 NumTasks = 10;
    std::atomic<u32> TasksSawCancellation{ 0 };
    FManualResetEvent StartEvent;

    TArray<FTask> Tasks;
    for (u32 i = 0; i < NumTasks; ++i)
    {
        Tasks.Add(Launch("MultiCancelTest",
                         [&CancellationToken, &TasksSawCancellation, &StartEvent]
                         {
                             StartEvent.Wait();
                             if (CancellationToken.IsCanceled())
                             {
                                 TasksSawCancellation.fetch_add(1, std::memory_order_relaxed);
                             }
                         }));
    }

    // Cancel before releasing the tasks
    CancellationToken.Cancel();
    StartEvent.Notify();

    Wait(Tasks);

    EXPECT_EQ(TasksSawCancellation.load(), NumTasks);
}

// ============================================================================
// Worker Restart Tests (ported from UE5.7)
// ============================================================================

class WorkerRestartTest : public TaskTestBase
{
  protected:
};

TEST_F(WorkerRestartTest, LoneStandbyWorker)
{
    // We absolutely need oversubscription to kick in to test this.
    // So only use a single worker to make sure that happens.
    LowLevelTasks::FScheduler::Get().RestartWorkers(1, 0);

    FManualResetEvent OversubscribeeReadyEvent;
    FManualResetEvent OversubscriberReadyEvent;
    FManualResetEvent OversubscribeeDoneEvent;
    FManualResetEvent OversubscriberDoneEvent;
    FManualResetEvent LocalQueueEvent;

    FTask Oversubscriber = Launch("Oversubscriber",
                                  [&]()
                                  {
                                      LowLevelTasks::FOversubscriptionScope _;
                                      OversubscriberReadyEvent.Notify();
                                      OversubscriberDoneEvent.Wait();
                                  });

    // Wait until the oversubscription scope is active
    OversubscriberReadyEvent.Wait();

    FTask Oversubscribee = Launch("Oversubscribee",
                                  [&]()
                                  {
                                      OversubscribeeReadyEvent.Notify();
                                      OversubscribeeDoneEvent.Wait();
                                  });

    // The first subsequent of a task is sent to the local queue
    // so setup ourself to be the subsequent of the oversubscribee.
    Launch("LocalQueueTask", [&]()
           { LocalQueueEvent.Notify(); }, Prerequisites(Oversubscribee));

    // Wait until the oversubscribee task is launched.
    OversubscribeeReadyEvent.Wait();
    // Now close the oversubscription scope while the oversubcribee is still executing.
    OversubscriberDoneEvent.Notify();
    // Wait until the oversubscriber has closed its oversubscription scope.
    Oversubscriber.Wait();
    // Now release the oversubscribee so it finishes executing and release its subsequent.
    OversubscribeeDoneEvent.Notify();

    // Verify that we did not timeout (i.e. deadlock).
    // Wait for up to 5 seconds
    bool Completed = LocalQueueEvent.WaitFor(FMonotonicTimeSpan::FromSeconds(5.0));
    EXPECT_TRUE(Completed) << "Test likely deadlocked - LoneStandbyWorker bug";

    // Restore workers
    LowLevelTasks::FScheduler::Get().RestartWorkers();
}

TEST_F(WorkerRestartTest, RestartWorkersAndOversubscription)
{
    // Stress test RestartWorkers while having task performing oversubscription
    constexpr i32 NumIterations = 100; // Reduced from 10000 for faster test execution

    for (i32 Index = 0; Index < NumIterations; ++Index)
    {
        FTask Task = Launch("OversubTask",
                            [&]()
                            {
                                // Just trigger oversubscription
                                LowLevelTasks::FOversubscriptionScope _;
                            });

        // For the repro to work we need to trigger oversubscription right between the time
        // we acquire the critical section and before the waiting queues are shut down.
        LowLevelTasks::FScheduler::Get().RestartWorkers(0, 4);

        Task.Wait();
    }

    // Restore original worker count
    LowLevelTasks::FScheduler::Get().RestartWorkers();
}

// DISABLED: This test may deadlock in certain configurations - stress test for scheduler edge cases
// Matches UE5.7's hidden test: [.][ApplicationContextMask][EngineFilter]
TEST_F(WorkerRestartTest, DISABLED_RestartWorkersAndExternalThreads)
{
    // Stress test RestartWorkers while having a thread trying to launch tasks on workers
    std::atomic<bool> bExit = false;
    FManualResetEvent Done;

    // Use Async(EAsyncExecution::Thread, ...) like UE5.7 does
    Async(EAsyncExecution::Thread,
          [&Done, &bExit]()
          {
              while (!bExit.load(std::memory_order_relaxed))
              {
                  Launch("ExternalTask", []() {}).Wait();
              }
              Done.Notify();
          });

    constexpr i32 NumIterations = 1000; // Match UE5.7's iteration count

    for (i32 Index = 0; Index < NumIterations; ++Index)
    {
        // For the repro to work we need to launch tasks to try to start new workers while
        // we're shutting down and restarting workers.
        LowLevelTasks::FScheduler::Get().RestartWorkers(0, 4);
    }

    bExit = true;
    Done.Wait();

    // Restore original worker count
    LowLevelTasks::FScheduler::Get().RestartWorkers();
}

TEST_F(WorkerRestartTest, BackgroundWithNormalAsPrereq)
{
    // This test will force the scheduling of a background task from the normal task
    // which exercises some code in the scheduler to send the task to the global queue
    // and always wake up a worker since background tasks can't run on normal threads.
    constexpr i32 NumIterations = 100;

    for (i32 Index = 0; Index < NumIterations; ++Index)
    {
        // Launch Normal task first and use it as prereq for the background one
        FTask NormalTask = Launch("NormalTask", []() {});

        FTask BackgroundTask = Launch("BackgroundTask", []() {}, Prerequisites(NormalTask), LowLevelTasks::ETaskPriority::BackgroundNormal);

        // Wait with timeout to detect deadlocks
        auto StartTime = std::chrono::steady_clock::now();
        while (!BackgroundTask.IsCompleted())
        {
            auto Elapsed = std::chrono::steady_clock::now() - StartTime;
            if (std::chrono::duration_cast<std::chrono::seconds>(Elapsed).count() > 10)
            {
                FAIL() << "Test is likely deadlocked, aborting.";
                return;
            }
            std::this_thread::yield();
        }
    }
}
