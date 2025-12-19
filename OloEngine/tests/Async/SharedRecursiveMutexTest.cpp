/**
 * @file SharedRecursiveMutexTest.cpp
 * @brief Unit tests for the FSharedRecursiveMutex synchronization primitive
 *
 * Ported from UE5.7's Async/SharedRecursiveMutexTest.cpp
 * Tests cover: Recursive locking, shared locking, concurrent access patterns
 */

#include <gtest/gtest.h>

#include "OloEngine/Threading/SharedRecursiveMutex.h"
#include "OloEngine/Threading/SharedLock.h"
#include "OloEngine/HAL/ManualResetEvent.h"
#include "OloEngine/Core/Base.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <functional>

using namespace OloEngine;

// ============================================================================
// SharedRecursiveMutex Basic Tests
// ============================================================================

class SharedRecursiveMutexTest : public ::testing::Test
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(SharedRecursiveMutexTest, SingleThreadExclusiveLock)
{
    FSharedRecursiveMutex Mutex;

    Mutex.Lock();
    EXPECT_FALSE(TDynamicSharedLock<FSharedRecursiveMutex>(Mutex, DeferLock).TryLock());
    Mutex.Unlock();

    EXPECT_TRUE(Mutex.TryLock());
    Mutex.Unlock();
}

TEST_F(SharedRecursiveMutexTest, SingleThreadRecursiveSharedLock)
{
    FSharedRecursiveMutex Mutex;

    // This test performs recursive locking because it exercises the current implementation
    // but it is not technically supported by the mutex and can deadlock if used generally.
    {
        TSharedLock<FSharedRecursiveMutex> Lock1(Mutex);
        TSharedLock<FSharedRecursiveMutex> Lock2(Mutex);
        TSharedLock<FSharedRecursiveMutex> Lock3(Mutex);
        EXPECT_FALSE(Mutex.TryLock());
    }
}

TEST_F(SharedRecursiveMutexTest, SingleThreadDynamicSharedLock)
{
    FSharedRecursiveMutex Mutex;

    {
        TDynamicSharedLock<FSharedRecursiveMutex> Lock1(Mutex, DeferLock);
        TDynamicSharedLock<FSharedRecursiveMutex> Lock2(Mutex, DeferLock);
        TDynamicSharedLock<FSharedRecursiveMutex> Lock3(Mutex, DeferLock);
        EXPECT_TRUE(Lock1.TryLock());
        EXPECT_TRUE(Lock2.TryLock());
        EXPECT_TRUE(Lock3.TryLock());
        EXPECT_FALSE(Mutex.TryLock());
    }

    EXPECT_TRUE(Mutex.TryLock());
    Mutex.Unlock();
}

TEST_F(SharedRecursiveMutexTest, SingleThreadRecursiveExclusiveLock)
{
    FSharedRecursiveMutex Mutex;

    EXPECT_TRUE(Mutex.TryLock());
    EXPECT_TRUE(Mutex.TryLock());
    Mutex.Unlock();
    Mutex.Unlock();

    Mutex.Lock();
    Mutex.Lock();
    Mutex.Unlock();
    Mutex.Unlock();
}

TEST_F(SharedRecursiveMutexTest, MultipleThreadsBasic)
{
    FSharedRecursiveMutex Mutex;
    u32 Counter = 0;
    FManualResetEvent Events[4];

    auto MakeWait = [&Events](i32 Index)
    {
        return [&Events, Index]
        {
            Events[Index].Wait();
            Events[Index].Reset();
        };
    };

    auto Wake = [&Events](i32 Index)
    {
        Events[Index].Notify();
        std::this_thread::yield();
    };

    // Countdown event helper class
    class FCountdownEvent
    {
      public:
        void Reset(i32 Count)
        {
            m_Counter.store(Count, std::memory_order_relaxed);
            m_Event.Reset();
        }

        void Notify()
        {
            if (m_Counter.fetch_sub(1, std::memory_order_release) == 1)
            {
                m_Event.Notify();
            }
        }

        void Wait()
        {
            m_Event.Wait();
        }

      private:
        std::atomic<i32> m_Counter{ 0 };
        FManualResetEvent m_Event;
    };
    FCountdownEvent CountdownEvent;

    // Thread 0
    std::thread Thread0([&Mutex, &Counter, &CountdownEvent, &Wake, Wait = MakeWait(0)]
                        {
        TDynamicSharedLock<FSharedRecursiveMutex> SharedLock1(Mutex, DeferLock);
        TDynamicSharedLock<FSharedRecursiveMutex> SharedLock2(Mutex, DeferLock);

        // Test 1: Exclusive w/ one waiting exclusive lock.
        Mutex.Lock();
        Wake(1);
        Wait();
        Counter = 1;
        Mutex.Unlock();

        // Test 2: Exclusive w/ one waiting shared lock.
        Wait();
        Wake(2);
        SharedLock1.Lock();
        EXPECT_EQ(Counter, 2u);

        // Test 3: Shared w/ one waiting exclusive lock.
        Wake(1);
        Wait();
        Counter = 3;
        EXPECT_TRUE(SharedLock2.TryLock());
        SharedLock2.Unlock();
        SharedLock1.Unlock();

        // Test 4: Exclusive w/ three waiting shared locks.
        Wait();
        Wake(1);
        SharedLock1.Lock();
        EXPECT_EQ(Counter, 4u);
        Wait();
        SharedLock1.Unlock();

        // Test 5: Shared w/ no exclusive contention.
        CountdownEvent.Reset(3);
        Wake(1);
        Wake(2);
        Wake(3);
        for (i32 I = 0; I < 1024; ++I)  // Reduced iterations for test speed
        {
            TSharedLock<FSharedRecursiveMutex> Lock(Mutex);
            std::this_thread::yield();
        }
        CountdownEvent.Wait();

        // Test 6: Shared w/ one waiting exclusive lock and one waiting shared lock.
        SharedLock1.Lock();
        Counter = 5;
        Wake(1);
        Wait();
        SharedLock2.Lock();
        SharedLock1.Unlock();
        SharedLock2.Unlock(); });

    // Thread 1
    std::thread Thread1([&Mutex, &Counter, &CountdownEvent, &Wake, Wait = MakeWait(1)]
                        {
        // Test 1: Exclusive w/ one waiting exclusive lock.
        Wait();
        Wake(2);
        Mutex.Lock();
        EXPECT_EQ(Counter, 1u);

        // Test 2: Exclusive w/ one waiting shared lock.
        Wake(0);
        Wait();
        Counter = 2;
        Mutex.Unlock();

        // Test 3: Shared w/ one waiting exclusive lock.
        Wait();
        Wake(2);
        Mutex.Lock();
        EXPECT_EQ(Counter, 3u);

        // Test 4: Exclusive w/ three waiting shared locks.
        Wake(2);
        Wait();
        Counter = 4;
        Mutex.Unlock();

        // Test 5: Shared w/ no exclusive contention.
        Wait();
        for (i32 I = 0; I < 1024; ++I)
        {
            TSharedLock<FSharedRecursiveMutex> Lock(Mutex);
            std::this_thread::yield();
        }
        CountdownEvent.Notify();

        // Test 6: Shared w/ one waiting exclusive lock and one waiting shared lock.
        Wait();
        Wake(2);
        Mutex.Lock();
        EXPECT_EQ(Counter, 5u);
        Counter = 6;
        Mutex.Unlock(); });

    // Thread 2
    std::thread Thread2([&Mutex, &Counter, &CountdownEvent, &Wake, Wait = MakeWait(2)]
                        {
        TDynamicSharedLock<FSharedRecursiveMutex> SharedLock(Mutex, DeferLock);

        // Test 1: Exclusive w/ one waiting exclusive lock.
        Wait();
        Wake(0);

        // Test 2: Exclusive w/ one waiting shared lock.
        Wait();
        Wake(1);

        // Test 3: Shared w/ one waiting exclusive lock.
        Wait();
        Wake(0);

        // Test 4: Exclusive w/ three waiting shared locks.
        Wait();
        Wake(3);
        SharedLock.Lock();
        EXPECT_EQ(Counter, 4u);
        Wake(3);
        SharedLock.Unlock();

        // Test 5: Shared w/ no exclusive contention.
        Wait();
        for (i32 I = 0; I < 1024; ++I)
        {
            TSharedLock<FSharedRecursiveMutex> Lock(Mutex);
            std::this_thread::yield();
        }
        CountdownEvent.Notify();

        // Test 6: Shared w/ one waiting exclusive lock and one waiting shared lock.
        Wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Hopefully reliable enough
        Wake(0);
        SharedLock.Lock();
        EXPECT_EQ(Counter, 6u);
        SharedLock.Unlock(); });

    // Thread 3
    std::thread Thread3([&Mutex, &Counter, &CountdownEvent, &Wake, Wait = MakeWait(3)]
                        {
        TDynamicSharedLock<FSharedRecursiveMutex> SharedLock(Mutex, DeferLock);

        // Test 4: Exclusive w/ three waiting shared locks.
        Wait();
        Wake(0);
        SharedLock.Lock();
        EXPECT_EQ(Counter, 4u);
        Wait();
        Wake(0);
        SharedLock.Unlock();

        // Test 5: Shared w/ no exclusive contention.
        Wait();
        for (i32 I = 0; I < 1024; ++I)
        {
            TSharedLock<FSharedRecursiveMutex> Lock(Mutex);
            std::this_thread::yield();
        }
        CountdownEvent.Notify(); });

    Thread0.join();
    Thread1.join();
    Thread2.join();
    Thread3.join();
}

TEST_F(SharedRecursiveMutexTest, StressTest)
{
    constexpr i32 ThreadCount = 8;
    constexpr i32 OperationsPerThread = 500;

    FSharedRecursiveMutex Mutex;
    i32 Counter = 0;
    std::vector<std::thread> Threads;
    Threads.reserve(ThreadCount);

    for (i32 i = 0; i < ThreadCount; ++i)
    {
        Threads.emplace_back([&, threadId = i]
                             {
            for (i32 j = 0; j < OperationsPerThread; ++j)
            {
                if (j % 5 == 0)  // 20% writes
                {
                    Mutex.Lock();
                    // Test recursive locking
                    if (j % 10 == 0)
                    {
                        Mutex.Lock();
                        ++Counter;
                        Mutex.Unlock();
                    }
                    else
                    {
                        ++Counter;
                    }
                    Mutex.Unlock();
                }
                else  // 80% reads
                {
                    TSharedLock<FSharedRecursiveMutex> Lock(Mutex);
                    volatile i32 Val = Counter;  // Just read
                    (void)Val;
                }
            } });
    }

    for (std::thread& Thread : Threads)
    {
        Thread.join();
    }

    // Expected writes: ThreadCount * OperationsPerThread * 0.2
    EXPECT_EQ(Counter, ThreadCount * (OperationsPerThread / 5));
}
