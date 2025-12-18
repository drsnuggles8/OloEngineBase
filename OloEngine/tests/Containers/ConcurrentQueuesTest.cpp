/**
 * @file ConcurrentQueuesTest.cpp
 * @brief Unit tests for concurrent queue implementations
 * 
 * Ported from UE5.7's Containers/ConcurrentQueuesTest.cpp
 * Tests cover: TSpscQueue, TMpscQueue, TClosableMpscQueue, TConsumeAllMpmcQueue
 */

#include <gtest/gtest.h>

#include "OloEngine/Containers/SpscQueue.h"
#include "OloEngine/Containers/MpscQueue.h"
#include "OloEngine/Containers/ClosableMpscQueue.h"
#include "OloEngine/Containers/ConsumeAllMpmcQueue.h"
#include "OloEngine/Core/Base.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>

using namespace OloEngine;

// ============================================================================
// SPSC Queue Tests (Single Producer Single Consumer)
// ============================================================================

class SpscQueueTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(SpscQueueTest, BasicPushPop)
{
    TSpscQueue<i32> Queue;
    
    Queue.Enqueue(1);
    Queue.Enqueue(2);
    Queue.Enqueue(3);
    
    i32 Value;
    EXPECT_TRUE(Queue.Dequeue(Value));
    EXPECT_EQ(Value, 1);
    
    EXPECT_TRUE(Queue.Dequeue(Value));
    EXPECT_EQ(Value, 2);
    
    EXPECT_TRUE(Queue.Dequeue(Value));
    EXPECT_EQ(Value, 3);
    
    EXPECT_FALSE(Queue.Dequeue(Value));
}

TEST_F(SpscQueueTest, EmptyQueue)
{
    TSpscQueue<i32> Queue;
    
    i32 Value;
    EXPECT_FALSE(Queue.Dequeue(Value));
}

TEST_F(SpscQueueTest, SingleProducerSingleConsumer)
{
    constexpr i32 ItemCount = 10000;
    
    TSpscQueue<i32> Queue;
    std::atomic<bool> ProducerDone{ false };
    std::vector<i32> ConsumedItems;
    ConsumedItems.reserve(ItemCount);
    
    // Consumer thread
    std::thread Consumer([&]
    {
        i32 Value;
        while (true)
        {
            if (Queue.Dequeue(Value))
            {
                ConsumedItems.push_back(Value);
            }
            else if (ProducerDone.load(std::memory_order_acquire))
            {
                // Producer is done, drain remaining items
                while (Queue.Dequeue(Value))
                {
                    ConsumedItems.push_back(Value);
                }
                break;
            }
            else
            {
                std::this_thread::yield();
            }
        }
    });
    
    // Producer
    for (i32 i = 0; i < ItemCount; ++i)
    {
        Queue.Enqueue(i);
    }
    ProducerDone.store(true, std::memory_order_release);
    
    Consumer.join();
    
    // Verify all items were consumed in order
    ASSERT_EQ(ConsumedItems.size(), ItemCount);
    for (i32 i = 0; i < ItemCount; ++i)
    {
        EXPECT_EQ(ConsumedItems[i], i);
    }
}

TEST_F(SpscQueueTest, MoveOnlyType)
{
    TSpscQueue<std::unique_ptr<i32>> Queue;
    
    Queue.Enqueue(std::make_unique<i32>(42));
    
    std::unique_ptr<i32> Value;
    EXPECT_TRUE(Queue.Dequeue(Value));
    EXPECT_NE(Value, nullptr);
    EXPECT_EQ(*Value, 42);
}

// ============================================================================
// MPSC Queue Tests (Multiple Producer Single Consumer)
// ============================================================================

class MpscQueueTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(MpscQueueTest, BasicPushPop)
{
    TMpscQueue<i32> Queue;
    
    Queue.Enqueue(1);
    Queue.Enqueue(2);
    Queue.Enqueue(3);
    
    i32 Value;
    EXPECT_TRUE(Queue.Dequeue(Value));
    EXPECT_EQ(Value, 1);
    
    EXPECT_TRUE(Queue.Dequeue(Value));
    EXPECT_EQ(Value, 2);
    
    EXPECT_TRUE(Queue.Dequeue(Value));
    EXPECT_EQ(Value, 3);
    
    EXPECT_FALSE(Queue.Dequeue(Value));
}

TEST_F(MpscQueueTest, MultipleProducers)
{
    constexpr i32 ProducerCount = 4;
    constexpr i32 ItemsPerProducer = 1000;
    
    TMpscQueue<i32> Queue;
    std::atomic<i32> ProducersDone{ 0 };
    std::vector<i32> ConsumedItems;
    ConsumedItems.reserve(ProducerCount * ItemsPerProducer);
    
    // Consumer thread
    std::thread Consumer([&]
    {
        i32 Value;
        while (true)
        {
            if (Queue.Dequeue(Value))
            {
                ConsumedItems.push_back(Value);
            }
            else if (ProducersDone.load(std::memory_order_acquire) >= ProducerCount)
            {
                // All producers done, drain remaining
                while (Queue.Dequeue(Value))
                {
                    ConsumedItems.push_back(Value);
                }
                break;
            }
            else
            {
                std::this_thread::yield();
            }
        }
    });
    
    // Producer threads
    std::vector<std::thread> Producers;
    for (i32 p = 0; p < ProducerCount; ++p)
    {
        Producers.emplace_back([&, producerId = p]
        {
            for (i32 i = 0; i < ItemsPerProducer; ++i)
            {
                Queue.Enqueue(producerId * ItemsPerProducer + i);
            }
            ProducersDone.fetch_add(1, std::memory_order_release);
        });
    }
    
    for (std::thread& Producer : Producers)
    {
        Producer.join();
    }
    
    Consumer.join();
    
    // Verify all items were consumed
    ASSERT_EQ(ConsumedItems.size(), ProducerCount * ItemsPerProducer);
    
    // Sort and verify all values are present
    std::sort(ConsumedItems.begin(), ConsumedItems.end());
    for (i32 i = 0; i < ProducerCount * ItemsPerProducer; ++i)
    {
        EXPECT_EQ(ConsumedItems[i], i);
    }
}

// ============================================================================
// Closable MPSC Queue Tests
// ============================================================================

class ClosableMpscQueueTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ClosableMpscQueueTest, BasicOperations)
{
    TClosableMpscQueue<i32> Queue;
    
    EXPECT_TRUE(Queue.Enqueue(1));
    EXPECT_TRUE(Queue.Enqueue(2));
    EXPECT_FALSE(Queue.IsClosed());
    
    std::vector<i32> Items;
    Queue.Close([&Items](i32 Value) { Items.push_back(Value); });
    
    EXPECT_TRUE(Queue.IsClosed());
    ASSERT_EQ(Items.size(), 2u);
    EXPECT_EQ(Items[0], 1);
    EXPECT_EQ(Items[1], 2);
}

TEST_F(ClosableMpscQueueTest, CloseQueue)
{
    TClosableMpscQueue<i32> Queue;
    
    Queue.Enqueue(1);
    Queue.Enqueue(2);
    
    std::vector<i32> Items;
    Queue.Close([&Items](i32 Value) { Items.push_back(Value); });
    
    // Should not be able to enqueue after close
    EXPECT_FALSE(Queue.Enqueue(3));
    EXPECT_TRUE(Queue.IsClosed());
    
    // All items should have been consumed
    ASSERT_EQ(Items.size(), 2u);
}

TEST_F(ClosableMpscQueueTest, CloseEmptyQueue)
{
    TClosableMpscQueue<i32> Queue;
    
    i32 Count = 0;
    Queue.Close([&Count](i32 Value) { ++Count; });
    
    EXPECT_EQ(Count, 0);
    EXPECT_TRUE(Queue.IsClosed());
}

TEST_F(ClosableMpscQueueTest, MultipleProducersBeforeClose)
{
    constexpr i32 ProducerCount = 4;
    constexpr i32 ItemsPerProducer = 100;
    
    TClosableMpscQueue<i32> Queue;
    std::atomic<i32> ProducersDone{ 0 };
    
    // Producer threads
    std::vector<std::thread> Producers;
    for (i32 p = 0; p < ProducerCount; ++p)
    {
        Producers.emplace_back([&, producerId = p]
        {
            for (i32 i = 0; i < ItemsPerProducer; ++i)
            {
                Queue.Enqueue(producerId * ItemsPerProducer + i);
            }
            ProducersDone.fetch_add(1);
        });
    }
    
    // Wait for all producers to finish
    for (std::thread& Producer : Producers)
    {
        Producer.join();
    }
    
    // Close and consume
    std::vector<i32> Items;
    Queue.Close([&Items](i32 Value) { Items.push_back(Value); });
    
    // Verify all items were consumed
    ASSERT_EQ(Items.size(), ProducerCount * ItemsPerProducer);
    
    // Sort and verify all values are present
    std::sort(Items.begin(), Items.end());
    for (i32 i = 0; i < ProducerCount * ItemsPerProducer; ++i)
    {
        EXPECT_EQ(Items[i], i);
    }
}

// ============================================================================
// ConsumeAll MPMC Queue Tests
// ============================================================================

class ConsumeAllMpmcQueueTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ConsumeAllMpmcQueueTest, BasicOperations)
{
    TConsumeAllMpmcQueue<i32> Queue;
    
    Queue.ProduceItem(1);
    Queue.ProduceItem(2);
    Queue.ProduceItem(3);
    
    std::vector<i32> Items;
    Queue.ConsumeAllFifo([&Items](i32&& Item) { Items.push_back(Item); });
    
    ASSERT_EQ(Items.size(), 3u);
    EXPECT_EQ(Items[0], 1);
    EXPECT_EQ(Items[1], 2);
    EXPECT_EQ(Items[2], 3);
}

TEST_F(ConsumeAllMpmcQueueTest, EmptyQueue)
{
    TConsumeAllMpmcQueue<i32> Queue;
    
    i32 Count = 0;
    auto Result = Queue.ConsumeAllFifo([&Count](i32&& Item) { ++Count; });
    
    EXPECT_EQ(Count, 0);
    EXPECT_EQ(Result, EConsumeAllMpmcQueueResult::WasEmpty);
}

TEST_F(ConsumeAllMpmcQueueTest, ConsumeAllLifo)
{
    TConsumeAllMpmcQueue<i32> Queue;
    
    Queue.ProduceItem(1);
    Queue.ProduceItem(2);
    Queue.ProduceItem(3);
    
    std::vector<i32> Items;
    Queue.ConsumeAllLifo([&Items](i32&& Item) { Items.push_back(Item); });
    
    ASSERT_EQ(Items.size(), 3u);
    // LIFO order: 3, 2, 1
    EXPECT_EQ(Items[0], 3);
    EXPECT_EQ(Items[1], 2);
    EXPECT_EQ(Items[2], 1);
}

TEST_F(ConsumeAllMpmcQueueTest, MultipleProducersMultipleConsumers)
{
    constexpr i32 ProducerCount = 4;
    constexpr i32 ConsumerCount = 4;
    constexpr i32 ItemsPerProducer = 1000;
    
    TConsumeAllMpmcQueue<i32> Queue;
    std::atomic<i32> ProducersDone{ 0 };
    std::atomic<i32> TotalConsumed{ 0 };
    std::atomic<bool> AllProducersDone{ false };
    
    // Consumer threads
    std::vector<std::thread> Consumers;
    for (i32 c = 0; c < ConsumerCount; ++c)
    {
        Consumers.emplace_back([&]
        {
            while (!AllProducersDone.load())
            {
                Queue.ConsumeAllFifo([&TotalConsumed](i32&& Item)
                {
                    TotalConsumed.fetch_add(1);
                });
                std::this_thread::yield();
            }
            
            // Final drain
            Queue.ConsumeAllFifo([&TotalConsumed](i32&& Item)
            {
                TotalConsumed.fetch_add(1);
            });
        });
    }
    
    // Producer threads
    std::vector<std::thread> Producers;
    for (i32 p = 0; p < ProducerCount; ++p)
    {
        Producers.emplace_back([&, producerId = p]
        {
            for (i32 i = 0; i < ItemsPerProducer; ++i)
            {
                Queue.ProduceItem(producerId * ItemsPerProducer + i);
            }
            if (ProducersDone.fetch_add(1) + 1 == ProducerCount)
            {
                AllProducersDone = true;
            }
        });
    }
    
    for (std::thread& Producer : Producers)
    {
        Producer.join();
    }
    
    for (std::thread& Consumer : Consumers)
    {
        Consumer.join();
    }
    
    EXPECT_EQ(TotalConsumed.load(), ProducerCount * ItemsPerProducer);
}

// ============================================================================
// Queue Stress Tests
// ============================================================================

class QueueStressTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(QueueStressTest, SpscHighThroughput)
{
    constexpr i32 ItemCount = 100000;
    
    TSpscQueue<i32> Queue;
    std::atomic<bool> ProducerDone{ false };
    i64 Sum = 0;
    
    std::thread Consumer([&]
    {
        i32 Value;
        i32 Consumed = 0;
        while (Consumed < ItemCount)
        {
            if (Queue.Dequeue(Value))
            {
                Sum += Value;
                ++Consumed;
            }
        }
    });
    
    for (i32 i = 0; i < ItemCount; ++i)
    {
        Queue.Enqueue(i);
    }
    
    Consumer.join();
    
    // Sum of 0 to N-1 = N*(N-1)/2
    i64 ExpectedSum = static_cast<i64>(ItemCount) * (ItemCount - 1) / 2;
    EXPECT_EQ(Sum, ExpectedSum);
}

TEST_F(QueueStressTest, MpscHighContention)
{
    constexpr i32 ProducerCount = 8;
    constexpr i32 ItemsPerProducer = 10000;
    
    TMpscQueue<i32> Queue;
    std::atomic<i32> ProducersDone{ 0 };
    i32 ConsumedCount = 0;
    
    std::thread Consumer([&]
    {
        i32 Value;
        while (true)
        {
            if (Queue.Dequeue(Value))
            {
                ++ConsumedCount;
            }
            else if (ProducersDone.load(std::memory_order_acquire) >= ProducerCount)
            {
                // All producers done, final drain
                while (Queue.Dequeue(Value))
                {
                    ++ConsumedCount;
                }
                break;
            }
            else
            {
                std::this_thread::yield();
            }
        }
    });
    
    std::vector<std::thread> Producers;
    for (i32 p = 0; p < ProducerCount; ++p)
    {
        Producers.emplace_back([&]
        {
            for (i32 i = 0; i < ItemsPerProducer; ++i)
            {
                Queue.Enqueue(i);
            }
            ProducersDone.fetch_add(1, std::memory_order_release);
        });
    }
    
    for (std::thread& Producer : Producers)
    {
        Producer.join();
    }
    
    Consumer.join();
    
    EXPECT_EQ(ConsumedCount, ProducerCount * ItemsPerProducer);
}


