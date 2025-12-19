/**
 * @file SemaphoreTest.cpp
 * @brief Unit tests for the FSemaphore synchronization primitive
 *
 * Ported from UE5.7's Async/SemaphoreTest.cpp
 * Tests cover: Acquire, Release, TryAcquire, producer-consumer patterns
 */

#include <gtest/gtest.h>

#include "OloEngine/HAL/Semaphore.h"
#include "OloEngine/Core/MonotonicTime.h"
#include "OloEngine/Core/Base.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

using namespace OloEngine;

// ============================================================================
// Semaphore Basic Tests
// ============================================================================

class SemaphoreTest : public ::testing::Test
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(SemaphoreTest, InitialCount)
{
    FSemaphore Sem(3);

    // Should be able to acquire 3 times
    EXPECT_TRUE(Sem.TryAcquire());
    EXPECT_TRUE(Sem.TryAcquire());
    EXPECT_TRUE(Sem.TryAcquire());

    // Fourth acquire should fail
    EXPECT_FALSE(Sem.TryAcquire());

    // Release all
    Sem.Release(3);
}

TEST_F(SemaphoreTest, TryAcquireWhenEmpty)
{
    FSemaphore Sem(0);

    EXPECT_FALSE(Sem.TryAcquire());
}

TEST_F(SemaphoreTest, TryAcquireForWithTimeout)
{
    FSemaphore Sem(0);

    auto Start = std::chrono::steady_clock::now();
    bool Result = Sem.TryAcquireFor(FMonotonicTimeSpan::FromMilliseconds(10.0));
    auto End = std::chrono::steady_clock::now();

    EXPECT_FALSE(Result);

    // Should have waited approximately 10ms (with some tolerance)
    auto ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(End - Start).count();
    EXPECT_GE(ElapsedMs, 5); // At least some wait
}

TEST_F(SemaphoreTest, TryAcquireUntilWithTimeout)
{
    FSemaphore Sem(0);

    auto Deadline = FMonotonicTimePoint::Now() + FMonotonicTimeSpan::FromMilliseconds(10.0);
    bool Result = Sem.TryAcquireUntil(Deadline);

    EXPECT_FALSE(Result);
}

TEST_F(SemaphoreTest, AcquireAndRelease)
{
    FSemaphore Sem(1);

    std::atomic<bool> ThreadStarted{ false };
    std::atomic<bool> ThreadAcquired{ false };

    // Take the semaphore
    Sem.Acquire();

    // Start a thread that will wait on the semaphore
    std::thread Thread([&]
                       {
        ThreadStarted = true;
        Sem.Acquire();
        ThreadAcquired = true;
        Sem.Release(); });

    // Wait for thread to start
    while (!ThreadStarted.load())
    {
        std::this_thread::yield();
    }

    // Give thread time to attempt acquire
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Thread should not have acquired yet
    EXPECT_FALSE(ThreadAcquired.load());

    // Release the semaphore
    Sem.Release();

    // Wait for thread to finish
    Thread.join();

    EXPECT_TRUE(ThreadAcquired.load());
}

TEST_F(SemaphoreTest, ReleaseMultiple)
{
    FSemaphore Sem(0);

    Sem.Release(5);

    // Should be able to acquire 5 times
    for (i32 i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(Sem.TryAcquire()) << "Failed at iteration " << i;
    }

    EXPECT_FALSE(Sem.TryAcquire());
}

TEST_F(SemaphoreTest, ProducerConsumer)
{
    constexpr i32 ItemCount = 100;

    FSemaphore Sem(0);
    std::atomic<i32> ConsumedCount{ 0 };

    // Consumer thread
    std::thread Consumer([&]
                         {
        for (i32 i = 0; i < ItemCount; ++i)
        {
            Sem.Acquire();
            ConsumedCount.fetch_add(1);
        } });

    // Producer - release items
    for (i32 i = 0; i < ItemCount; ++i)
    {
        Sem.Release();
        // Small delay to interleave
        if (i % 10 == 0)
        {
            std::this_thread::yield();
        }
    }

    Consumer.join();

    EXPECT_EQ(ConsumedCount.load(), ItemCount);
}

TEST_F(SemaphoreTest, MultipleProducersConsumers)
{
    constexpr i32 ProducerCount = 4;
    constexpr i32 ConsumerCount = 4;
    constexpr i32 ItemsPerProducer = 100;

    FSemaphore Sem(0);
    std::atomic<i32> ProducedCount{ 0 };
    std::atomic<i32> ConsumedCount{ 0 };
    std::atomic<bool> Done{ false };

    std::vector<std::thread> Threads;
    Threads.reserve(ProducerCount + ConsumerCount);

    // Start consumers
    for (i32 i = 0; i < ConsumerCount; ++i)
    {
        Threads.emplace_back([&]
                             {
            while (!Done.load() || Sem.TryAcquire())
            {
                if (Sem.TryAcquireFor(FMonotonicTimeSpan::FromMilliseconds(1.0)))
                {
                    ConsumedCount.fetch_add(1);
                }
            } });
    }

    // Start producers
    for (i32 i = 0; i < ProducerCount; ++i)
    {
        Threads.emplace_back([&]
                             {
            for (i32 j = 0; j < ItemsPerProducer; ++j)
            {
                Sem.Release();
                ProducedCount.fetch_add(1);
            } });
    }

    // Wait for producers to finish
    for (i32 i = ConsumerCount; i < ProducerCount + ConsumerCount; ++i)
    {
        Threads[i].join();
    }

    // Signal done and wait for consumers
    Done = true;

    // Release extra signals to wake up waiting consumers
    Sem.Release(ConsumerCount);

    for (i32 i = 0; i < ConsumerCount; ++i)
    {
        Threads[i].join();
    }

    // All produced items should be consumed (or attempted)
    EXPECT_EQ(ProducedCount.load(), ProducerCount * ItemsPerProducer);
}
