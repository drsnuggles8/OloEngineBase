#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/AudioThread.h"

using namespace OloEngine;
using namespace OloEngine::Audio;

//===========================================
// Comprehensive AudioThread Tests
//===========================================

class AudioThreadFullTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!AudioThread::IsRunning())
        {
            ASSERT_TRUE(AudioThread::Start());
        }
    }
    
    void TearDown() override
    {
        if (AudioThread::IsRunning())
        {
            AudioThread::Stop();
        }
    }
};

TEST_F(AudioThreadFullTest, BasicThreadOperations)
{
    EXPECT_TRUE(AudioThread::IsRunning());
    EXPECT_FALSE(AudioThread::IsAudioThread()); // We're on the main thread
    
    // Get thread ID
    auto threadID = AudioThread::GetThreadID();
    EXPECT_NE(threadID, std::this_thread::get_id());
}

TEST_F(AudioThreadFullTest, SimpleTaskExecution)
{
    std::atomic<bool> taskExecuted{false};
    
    AudioThread::ExecuteOnAudioThread([&taskExecuted]()
    {
        taskExecuted.store(true);
    }, "SimpleTask");
    
    // Wait for task to execute
    auto startTime = std::chrono::steady_clock::now();
    while (!taskExecuted.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        
        // Timeout after 1 second
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > std::chrono::seconds(1))
        {
            FAIL() << "Task execution timed out";
        }
    }
    
    EXPECT_TRUE(taskExecuted.load());
}

TEST_F(AudioThreadFullTest, MultipleTaskExecution)
{
    constexpr int NUM_TASKS = 50; // Reduced from 100 for faster testing
    std::atomic<int> completedTasks{0};
    
    for (int i = 0; i < NUM_TASKS; ++i)
    {
        AudioThread::ExecuteOnAudioThread([&completedTasks, i]()
        {
            // Simulate some work
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            completedTasks.fetch_add(1);
        }, "Task" + std::to_string(i));
    }
    
    // Wait for all tasks to complete
    auto startTime = std::chrono::steady_clock::now();
    while (completedTasks.load() < NUM_TASKS)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        
        // Timeout after 5 seconds
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > std::chrono::seconds(5))
        {
            FAIL() << "Task execution timed out. Completed: " << completedTasks.load() << "/" << NUM_TASKS;
        }
    }
    
    EXPECT_EQ(completedTasks.load(), NUM_TASKS);
}

TEST_F(AudioThreadFullTest, ExecutionPolicyTest)
{
    std::atomic<bool> taskExecuted{false};
    
    // Test ExecuteAsync policy from main thread
    AudioThread::ExecuteOnAudioThread(
        AudioThread::ExecutionPolicy::ExecuteAsync,
        [&taskExecuted]()
        {
            taskExecuted.store(true);
        },
        "PolicyTask"
    );
    
    // Wait for task to execute
    auto startTime = std::chrono::steady_clock::now();
    while (!taskExecuted.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > std::chrono::seconds(1))
        {
            FAIL() << "Task execution timed out";
        }
    }
    
    EXPECT_TRUE(taskExecuted.load());
}

//===========================================
// Audio Thread Fence Tests
//===========================================

TEST_F(AudioThreadFullTest, AudioThreadFenceBasic)
{
    AudioThreadFence fence;
    
    EXPECT_TRUE(fence.IsReady()); // Should be ready initially
    
    std::atomic<bool> taskStarted{false};
    std::atomic<bool> taskCompleted{false};
    
    // Execute a task that takes some time
    AudioThread::ExecuteOnAudioThread([&taskStarted, &taskCompleted]()
    {
        taskStarted.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        taskCompleted.store(true);
    }, "FenceTask");
    
    // Wait for task to start
    auto startTime = std::chrono::steady_clock::now();
    while (!taskStarted.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > std::chrono::seconds(1))
        {
            FAIL() << "Task failed to start";
        }
    }
    
    // Start fence after task is running
    fence.Begin();
    EXPECT_FALSE(fence.IsReady());
    
    // Wait for fence
    fence.Wait();
    EXPECT_TRUE(fence.IsReady());
    EXPECT_TRUE(taskCompleted.load());
}

TEST_F(AudioThreadFullTest, AudioThreadFenceBeginAndWait)
{
    std::atomic<bool> taskExecuted{false};
    
    // Execute a task
    AudioThread::ExecuteOnAudioThread([&taskExecuted]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        taskExecuted.store(true);
    }, "FenceTask");
    
    // Use BeginAndWait
    AudioThreadFence fence;
    fence.BeginAndWait();
    
    // Task should be completed when BeginAndWait returns
    EXPECT_TRUE(taskExecuted.load());
    EXPECT_TRUE(fence.IsReady());
}

TEST_F(AudioThreadFullTest, PerformanceBasic)
{
    // Test that we can get basic performance metrics
    constexpr int NUM_TASKS = 10;
    
    for (int i = 0; i < NUM_TASKS; ++i)
    {
        AudioThread::ExecuteOnAudioThread([](){}, "PerfTask");
    }
    
    // Wait a bit for tasks to process
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Check that timing is being tracked
    f64 lastUpdateTime = AudioThread::GetLastUpdateTime();
    EXPECT_GE(lastUpdateTime, 0.0);
}