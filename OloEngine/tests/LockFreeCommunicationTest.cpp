#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/LockFreeFIFO.h"
#include "OloEngine/Audio/AudioThread.h"

using namespace OloEngine;
using namespace OloEngine::Audio;

//===========================================
// Lock-Free FIFO Tests
//===========================================

class LockFreeFIFOTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Test with power-of-2 capacity
        m_FIFO.reset(8);
    }

    SingleReaderSingleWriterFIFO<i32> m_FIFO;
};

TEST_F(LockFreeFIFOTest, BasicPushPop)
{
    EXPECT_TRUE(m_FIFO.isEmpty());
    EXPECT_FALSE(m_FIFO.isFull());
    EXPECT_EQ(m_FIFO.getUsedSlots(), 0);
    EXPECT_EQ(m_FIFO.getFreeSlots(), 8);

    // Push some items
    EXPECT_TRUE(m_FIFO.push(1));
    EXPECT_TRUE(m_FIFO.push(2));
    EXPECT_TRUE(m_FIFO.push(3));

    EXPECT_FALSE(m_FIFO.isEmpty());
    EXPECT_EQ(m_FIFO.getUsedSlots(), 3);
    EXPECT_EQ(m_FIFO.getFreeSlots(), 5);

    // Pop items
    i32 result;
    EXPECT_TRUE(m_FIFO.pop(result));
    EXPECT_EQ(result, 1);
    
    EXPECT_TRUE(m_FIFO.pop(result));
    EXPECT_EQ(result, 2);
    
    EXPECT_TRUE(m_FIFO.pop(result));
    EXPECT_EQ(result, 3);

    EXPECT_TRUE(m_FIFO.isEmpty());
    EXPECT_EQ(m_FIFO.getUsedSlots(), 0);
}

TEST_F(LockFreeFIFOTest, MoveSemantics)
{
    std::string testString = "Hello World";
    std::string originalString = testString;

    // Test move push
    EXPECT_TRUE(m_FIFO.push(42));
    
    // Pop and verify
    i32 result;
    EXPECT_TRUE(m_FIFO.pop(result));
    EXPECT_EQ(result, 42);
}

TEST_F(LockFreeFIFOTest, FillAndEmpty)
{
    // Fill the FIFO completely
    for (i32 i = 0; i < 8; ++i)
    {
        EXPECT_TRUE(m_FIFO.push(i)) << "Failed to push item " << i;
    }

    EXPECT_TRUE(m_FIFO.isFull());
    EXPECT_EQ(m_FIFO.getUsedSlots(), 8);
    EXPECT_EQ(m_FIFO.getFreeSlots(), 0);

    // Try to push when full
    EXPECT_FALSE(m_FIFO.push(999));

    // Empty the FIFO
    for (i32 i = 0; i < 8; ++i)
    {
        i32 result;
        EXPECT_TRUE(m_FIFO.pop(result)) << "Failed to pop item " << i;
        EXPECT_EQ(result, i) << "Wrong value at position " << i;
    }

    EXPECT_TRUE(m_FIFO.isEmpty());

    // Try to pop when empty
    i32 result;
    EXPECT_FALSE(m_FIFO.pop(result));
}

TEST_F(LockFreeFIFOTest, PeekFunctionality)
{
    EXPECT_TRUE(m_FIFO.push(42));
    EXPECT_TRUE(m_FIFO.push(84));

    // Peek at first item
    i32 result;
    EXPECT_TRUE(m_FIFO.peek(result));
    EXPECT_EQ(result, 42);

    // Peek should not remove item
    EXPECT_EQ(m_FIFO.getUsedSlots(), 2);
    
    // Pop should get the same item
    EXPECT_TRUE(m_FIFO.pop(result));
    EXPECT_EQ(result, 42);
    
    // Peek at next item
    EXPECT_TRUE(m_FIFO.peek(result));
    EXPECT_EQ(result, 84);
}

TEST_F(LockFreeFIFOTest, ClearFunctionality)
{
    // Add some items
    for (i32 i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(m_FIFO.push(i));
    }
    
    EXPECT_EQ(m_FIFO.getUsedSlots(), 5);
    
    // Clear and verify
    m_FIFO.clear();
    EXPECT_TRUE(m_FIFO.isEmpty());
    EXPECT_EQ(m_FIFO.getUsedSlots(), 0);
    EXPECT_EQ(m_FIFO.getFreeSlots(), 8);
}

//===========================================
// Multiple Writer FIFO Tests
//===========================================

class MultipleWriterFIFOTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_FIFO.reset(16); // Larger for multi-threading tests
    }

    SingleReaderMultipleWriterFIFO<i32> m_FIFO;
};

TEST_F(MultipleWriterFIFOTest, BasicOperations)
{
    EXPECT_TRUE(m_FIFO.isEmpty());
    EXPECT_EQ(m_FIFO.getUsedSlots(), 0);

    // Push and pop like single writer FIFO
    EXPECT_TRUE(m_FIFO.push(100));
    EXPECT_TRUE(m_FIFO.push(200));
    
    i32 result;
    EXPECT_TRUE(m_FIFO.pop(result));
    EXPECT_EQ(result, 100);
    
    EXPECT_TRUE(m_FIFO.pop(result));
    EXPECT_EQ(result, 200);
    
    EXPECT_TRUE(m_FIFO.isEmpty());
}

TEST_F(MultipleWriterFIFOTest, ConcurrentWrites)
{
    constexpr int NUM_WRITERS = 4;
    constexpr int ITEMS_PER_WRITER = 100;
    
    std::vector<std::thread> writers;
    
    // Create multiple writer threads
    for (int writerID = 0; writerID < NUM_WRITERS; ++writerID)
    {
        writers.emplace_back([this, writerID, ITEMS_PER_WRITER]()
        {
            for (int i = 0; i < ITEMS_PER_WRITER; ++i)
            {
                int value = writerID * 1000 + i; // Unique value per writer/item
                while (!m_FIFO.push(value))
                {
                    std::this_thread::yield(); // Wait if FIFO is full
                }
            }
        });
    }
    
    // Single reader thread
    std::vector<i32> receivedValues;
    std::thread reader([this, &receivedValues, NUM_WRITERS, ITEMS_PER_WRITER]()
    {
        int expectedTotal = NUM_WRITERS * ITEMS_PER_WRITER;
        receivedValues.reserve(expectedTotal);
        
        while (receivedValues.size() < expectedTotal)
        {
            i32 value;
            if (m_FIFO.pop(value))
            {
                receivedValues.push_back(value);
            }
            else
            {
                std::this_thread::yield();
            }
        }
    });
    
    // Wait for all threads
    for (auto& writer : writers)
    {
        writer.join();
    }
    reader.join();
    
    // Verify all values were received
    EXPECT_EQ(receivedValues.size(), NUM_WRITERS * ITEMS_PER_WRITER);
    
    // Verify each writer's values are present
    for (int writerID = 0; writerID < NUM_WRITERS; ++writerID)
    {
        for (int i = 0; i < ITEMS_PER_WRITER; ++i)
        {
            int expectedValue = writerID * 1000 + i;
            auto it = std::find(receivedValues.begin(), receivedValues.end(), expectedValue);
            EXPECT_NE(it, receivedValues.end()) << "Missing value: " << expectedValue;
        }
    }
}

//===========================================
// Audio Thread Tests
//===========================================

class AudioThreadTest : public ::testing::Test
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

TEST_F(AudioThreadTest, BasicThreadOperations)
{
    EXPECT_TRUE(AudioThread::IsRunning());
    EXPECT_FALSE(AudioThread::IsAudioThread()); // We're on the main thread
    
    // Get thread ID
    auto threadID = AudioThread::GetThreadID();
    EXPECT_NE(threadID, std::this_thread::get_id());
}

TEST_F(AudioThreadTest, SimpleTaskExecution)
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

TEST_F(AudioThreadTest, MultipleTaskExecution)
{
    constexpr int NUM_TASKS = 100;
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

TEST_F(AudioThreadTest, ExecutionPolicyTest)
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

TEST_F(AudioThreadTest, AudioThreadFenceBasic)
{
    AudioThreadFence fence;
    
    EXPECT_TRUE(fence.IsReady()); // Should be ready initially
    
    std::atomic<bool> taskStarted{false};
    std::atomic<bool> taskCompleted{false};
    
    // Execute a task that takes some time
    AudioThread::ExecuteOnAudioThread([&taskStarted, &taskCompleted]()
    {
        taskStarted.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        taskCompleted.store(true);
    }, "FenceTask");
    
    // Start fence after task is running
    fence.Begin();
    EXPECT_FALSE(fence.IsReady());
    
    // Wait for fence
    fence.Wait();
    EXPECT_TRUE(fence.IsReady());
    EXPECT_TRUE(taskCompleted.load());
}

TEST_F(AudioThreadTest, AudioThreadFenceBeginAndWait)
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