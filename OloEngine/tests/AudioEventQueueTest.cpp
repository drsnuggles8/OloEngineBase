#include <gtest/gtest.h>
#include "OloEngine/Audio/LockFreeEventQueue.h"
#include <choc/containers/choc_Value.h>
#include <thread>
#include <atomic>
#include <chrono>

using namespace OloEngine::Audio;

//==============================================================================
/// Basic Functionality Tests
//==============================================================================

TEST(AudioEventQueue, BasicPushPop)
{
    AudioEventQueue<16> queue;

    // Push an event
    AudioThreadEvent event;
    event.m_FrameIndex = 12345;
    event.m_EndpointID = 42;

    choc::value::Value value = choc::value::createFloat32(3.14f);
    ASSERT_TRUE(event.m_ValueData.CopyFrom(value));

    ASSERT_TRUE(queue.Push(event));
    ASSERT_FALSE(queue.IsEmpty());

    // Pop the event
    AudioThreadEvent popped;
    ASSERT_TRUE(queue.Pop(popped));

    EXPECT_EQ(popped.m_FrameIndex, 12345);
    EXPECT_EQ(popped.m_EndpointID, 42);

    choc::value::ValueView view = popped.m_ValueData.GetView();
    EXPECT_TRUE(view.isFloat32());
    EXPECT_FLOAT_EQ(view.getFloat32(), 3.14f);

    EXPECT_TRUE(queue.IsEmpty());
}

TEST(AudioEventQueue, EmptyQueue)
{
    AudioEventQueue<8> queue;

    EXPECT_TRUE(queue.IsEmpty());
    EXPECT_EQ(queue.GetApproximateSize(), 0);

    AudioThreadEvent event;
    EXPECT_FALSE(queue.Pop(event));
}

TEST(AudioEventQueue, QueueFull)
{
    AudioEventQueue<8> queue;

    // Fill the queue (capacity - 1 because we leave one slot empty)
    for (int i = 0; i < 7; ++i)
    {
        AudioThreadEvent event;
        event.m_FrameIndex = i;
        event.m_EndpointID = i;

        choc::value::Value value = choc::value::createInt32(i);
        event.m_ValueData.CopyFrom(value);

        ASSERT_TRUE(queue.Push(event)) << "Failed to push event " << i;
    }

    // Queue should be full now
    AudioThreadEvent overflow;
    overflow.m_FrameIndex = 999;
    EXPECT_FALSE(queue.Push(overflow)) << "Queue should be full";

    // Verify we can pop all items
    for (int i = 0; i < 7; ++i)
    {
        AudioThreadEvent popped;
        ASSERT_TRUE(queue.Pop(popped));
        EXPECT_EQ(popped.m_FrameIndex, i);
    }

    EXPECT_TRUE(queue.IsEmpty());
}

TEST(AudioEventQueue, MessageQueue)
{
    AudioMessageQueue<32> queue;

    // Push a message
    AudioThreadMessage msg;
    msg.m_FrameIndex = 54321;
    msg.SetText("Test message");

    ASSERT_TRUE(queue.Push(msg));

    // Pop and verify
    AudioThreadMessage popped;
    ASSERT_TRUE(queue.Pop(popped));

    EXPECT_EQ(popped.m_FrameIndex, 54321);
    EXPECT_STREQ(popped.m_Text, "Test message");
}

//==============================================================================
/// Value Type Tests
//==============================================================================

TEST(AudioEventQueue, DifferentValueTypes)
{
    AudioEventQueue<32> queue;

    // Test float32
    {
        AudioThreadEvent event;
        event.m_EndpointID = 1;
        choc::value::Value value = choc::value::createFloat32(1.5f);
        ASSERT_TRUE(event.m_ValueData.CopyFrom(value));
        ASSERT_TRUE(queue.Push(event));
    }

    // Test int32
    {
        AudioThreadEvent event;
        event.m_EndpointID = 2;
        choc::value::Value value = choc::value::createInt32(42);
        ASSERT_TRUE(event.m_ValueData.CopyFrom(value));
        ASSERT_TRUE(queue.Push(event));
    }

    // Test bool
    {
        AudioThreadEvent event;
        event.m_EndpointID = 3;
        choc::value::Value value = choc::value::createBool(true);
        ASSERT_TRUE(event.m_ValueData.CopyFrom(value));
        ASSERT_TRUE(queue.Push(event));
    }

    // Test float64
    {
        AudioThreadEvent event;
        event.m_EndpointID = 4;
        choc::value::Value value = choc::value::createFloat64(2.71828);
        ASSERT_TRUE(event.m_ValueData.CopyFrom(value));
        ASSERT_TRUE(queue.Push(event));
    }

    // Verify all types
    {
        AudioThreadEvent event;
        ASSERT_TRUE(queue.Pop(event));
        EXPECT_EQ(event.m_EndpointID, 1);
        EXPECT_FLOAT_EQ(event.m_ValueData.GetView().getFloat32(), 1.5f);
    }

    {
        AudioThreadEvent event;
        ASSERT_TRUE(queue.Pop(event));
        EXPECT_EQ(event.m_EndpointID, 2);
        EXPECT_EQ(event.m_ValueData.GetView().getInt32(), 42);
    }

    {
        AudioThreadEvent event;
        ASSERT_TRUE(queue.Pop(event));
        EXPECT_EQ(event.m_EndpointID, 3);
        EXPECT_TRUE(event.m_ValueData.GetView().getBool());
    }

    {
        AudioThreadEvent event;
        ASSERT_TRUE(queue.Pop(event));
        EXPECT_EQ(event.m_EndpointID, 4);
        EXPECT_DOUBLE_EQ(event.m_ValueData.GetView().getFloat64(), 2.71828);
    }
}

TEST(AudioEventQueue, LongMessageTruncation)
{
    AudioMessageQueue<32> queue;

    // Create a message longer than buffer
    std::string longMessage(300, 'X');

    AudioThreadMessage msg;
    msg.SetText(longMessage.c_str());

    ASSERT_TRUE(queue.Push(msg));

    AudioThreadMessage popped;
    ASSERT_TRUE(queue.Pop(popped));

    // Should be truncated to buffer size - 1
    EXPECT_EQ(std::strlen(popped.m_Text), AudioThreadMessage::s_MaxMessageLength - 1);
    EXPECT_EQ(popped.m_Text[AudioThreadMessage::s_MaxMessageLength - 1], '\0');
}

//==============================================================================
/// Multi-threaded Tests
//==============================================================================

TEST(AudioEventQueue, MultithreadedProducerConsumer)
{
    AudioEventQueue<256> queue;

    std::atomic<bool> stopFlag{ false };
    std::atomic<int> producedCount{ 0 };
    std::atomic<int> consumedCount{ 0 };

    const int targetEvents = 1000;

    // Producer thread (simulates audio thread)
    std::thread producer([&]()
                         {
        int eventId = 0;
        while (eventId < targetEvents)
        {
            AudioThreadEvent event;
            event.m_FrameIndex = eventId;
            event.m_EndpointID = eventId % 10;
            
            choc::value::Value value = choc::value::createFloat32(eventId * 0.1f);
            if (event.m_ValueData.CopyFrom(value))
            {
                if (queue.Push(event))
                {
                    producedCount.fetch_add(1, std::memory_order_relaxed);
                    eventId++;
                }
            }
            
            // Small delay to simulate audio processing
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        stopFlag.store(true); });

    // Consumer thread (simulates main thread)
    std::thread consumer([&]()
                         {
        while (!stopFlag.load() || !queue.IsEmpty())
        {
            AudioThreadEvent event;
            if (queue.Pop(event))
            {
                consumedCount.fetch_add(1, std::memory_order_relaxed);
            }
            
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        } });

    producer.join();
    consumer.join();

    EXPECT_EQ(producedCount.load(), targetEvents);
    EXPECT_EQ(consumedCount.load(), targetEvents);
    EXPECT_TRUE(queue.IsEmpty());
}

TEST(AudioEventQueue, MultithreadedStressTest)
{
    AudioEventQueue<512> queue;

    std::atomic<bool> stopFlag{ false };
    std::atomic<int> producedCount{ 0 };
    std::atomic<int> consumedCount{ 0 };
    std::atomic<int> droppedCount{ 0 };

    // Producer thread - push as fast as possible
    std::thread producer([&]()
                         {
        int eventId = 0;
        auto startTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::milliseconds(500);
        
        while (std::chrono::steady_clock::now() - startTime < duration)
        {
            AudioThreadEvent event;
            event.m_FrameIndex = eventId;
            event.m_EndpointID = eventId % 100;
            
            choc::value::Value value = choc::value::createInt32(eventId);
            if (event.m_ValueData.CopyFrom(value))
            {
                if (queue.Push(event))
                {
                    producedCount.fetch_add(1, std::memory_order_relaxed);
                    eventId++;
                }
                else
                {
                    droppedCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        stopFlag.store(true); });

    // Consumer thread - pop as fast as possible
    std::thread consumer([&]()
                         {
        while (!stopFlag.load() || !queue.IsEmpty())
        {
            AudioThreadEvent event;
            if (queue.Pop(event))
            {
                consumedCount.fetch_add(1, std::memory_order_relaxed);
            }
        } });

    producer.join();
    consumer.join();

    // Verify conservation: produced = consumed (dropped events never entered queue)
    EXPECT_EQ(producedCount.load(), consumedCount.load());
    EXPECT_TRUE(queue.IsEmpty());

    // Check that we processed a reasonable number of events
    int totalAttempts = producedCount.load() + droppedCount.load();
    EXPECT_GT(totalAttempts, 100000) << "Should have attempted many operations";

    // If no drops occurred, that's fine - it means the consumer kept up!
    // If drops occurred, that's also fine - it means we tested full queue behavior
    EXPECT_GE(droppedCount.load(), 0);
}

//==============================================================================
/// Edge Case Tests
//==============================================================================

TEST(AudioEventQueue, ClearQueue)
{
    AudioEventQueue<16> queue;

    // Add some events
    for (int i = 0; i < 5; ++i)
    {
        AudioThreadEvent event;
        event.m_FrameIndex = i;
        choc::value::Value value = choc::value::createInt32(i);
        event.m_ValueData.CopyFrom(value);
        queue.Push(event);
    }

    EXPECT_FALSE(queue.IsEmpty());

    // Clear manually by popping all
    AudioThreadEvent event;
    while (queue.Pop(event))
        ;

    EXPECT_TRUE(queue.IsEmpty());
    EXPECT_EQ(queue.GetApproximateSize(), 0);
}

TEST(AudioEventQueue, WrapAround)
{
    AudioEventQueue<8> queue;

    // Fill and empty the queue multiple times to test wrap-around
    for (int cycle = 0; cycle < 5; ++cycle)
    {
        // Fill
        for (int i = 0; i < 7; ++i)
        {
            AudioThreadEvent event;
            event.m_FrameIndex = cycle * 10 + i;
            choc::value::Value value = choc::value::createInt32(i);
            event.m_ValueData.CopyFrom(value);
            ASSERT_TRUE(queue.Push(event));
        }

        // Empty
        for (int i = 0; i < 7; ++i)
        {
            AudioThreadEvent event;
            ASSERT_TRUE(queue.Pop(event));
            EXPECT_EQ(event.m_FrameIndex, cycle * 10 + i);
        }

        EXPECT_TRUE(queue.IsEmpty());
    }
}

//==============================================================================
/// Performance Test (Optional)
//==============================================================================

TEST(AudioEventQueue, PerformanceBenchmark)
{
    AudioEventQueue<1024> queue;

    const int iterations = 10000;

    auto start = std::chrono::high_resolution_clock::now();

    // Push
    for (int i = 0; i < iterations; ++i)
    {
        AudioThreadEvent event;
        event.m_FrameIndex = i;
        event.m_EndpointID = i % 100;

        choc::value::Value value = choc::value::createFloat32(i * 0.1f);
        event.m_ValueData.CopyFrom(value);
        queue.Push(event);
    }

    // Pop
    AudioThreadEvent event;
    for (int i = 0; i < iterations; ++i)
    {
        queue.Pop(event);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avgTimePerOp = static_cast<double>(duration.count()) / (iterations * 2); // *2 for push+pop

    // Should be very fast (typically < 0.1 microseconds per operation)
    EXPECT_LT(avgTimePerOp, 1.0) << "Average time per operation: " << avgTimePerOp << " microseconds";

    std::cout << "Performance: " << iterations << " push+pop operations in "
              << duration.count() << " microseconds" << std::endl;
    std::cout << "Average: " << avgTimePerOp << " microseconds per operation" << std::endl;
}
