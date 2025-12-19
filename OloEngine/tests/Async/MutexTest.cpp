/**
 * @file MutexTest.cpp
 * @brief Unit tests for the FMutex synchronization primitive
 *
 * Ported from UE5.7's Async/MutexTest.cpp
 * Tests cover: TryLock, IsLocked, TUniqueLock, multi-threaded contention
 */

#include <gtest/gtest.h>

#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "OloEngine/Core/Base.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

using namespace OloEngine;

// ============================================================================
// Mutex Basic Tests
// ============================================================================

class MutexTest : public ::testing::Test
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(MutexTest, TryLockWhenUnlocked)
{
    FMutex Mutex;

    EXPECT_TRUE(Mutex.TryLock());
    Mutex.Unlock();
}

TEST_F(MutexTest, TryLockWhenLocked)
{
    FMutex Mutex;

    Mutex.Lock();

    // Create a thread that tries to acquire the lock
    std::atomic<bool> TryLockResult{ true };
    std::thread Thread([&]
                       { TryLockResult = Mutex.TryLock(); });

    Thread.join();

    EXPECT_FALSE(TryLockResult.load());
    Mutex.Unlock();
}

TEST_F(MutexTest, IsLocked)
{
    FMutex Mutex;

    EXPECT_FALSE(Mutex.IsLocked());

    Mutex.Lock();
    EXPECT_TRUE(Mutex.IsLocked());

    Mutex.Unlock();
    EXPECT_FALSE(Mutex.IsLocked());
}

TEST_F(MutexTest, UniqueLockBasic)
{
    FMutex Mutex;

    {
        TUniqueLock<FMutex> Lock(Mutex);
        EXPECT_TRUE(Mutex.IsLocked());
    }

    EXPECT_FALSE(Mutex.IsLocked());
}

TEST_F(MutexTest, DynamicUniqueLockMoveConstruction)
{
    FMutex Mutex;

    {
        TDynamicUniqueLock<FMutex> Lock1(Mutex);
        TDynamicUniqueLock<FMutex> Lock2(std::move(Lock1));
        EXPECT_TRUE(Mutex.IsLocked());
        EXPECT_FALSE(Lock1.OwnsLock());
        EXPECT_TRUE(Lock2.OwnsLock());
    }

    EXPECT_FALSE(Mutex.IsLocked());
}

TEST_F(MutexTest, DynamicUniqueLockMoveAssignment)
{
    FMutex Mutex1;
    FMutex Mutex2;

    {
        TDynamicUniqueLock<FMutex> Lock1(Mutex1);
        TDynamicUniqueLock<FMutex> Lock2(Mutex2);

        Lock1 = std::move(Lock2);

        // Mutex1 should now be unlocked, Mutex2 should be locked via Lock1
        EXPECT_FALSE(Mutex1.IsLocked());
        EXPECT_TRUE(Mutex2.IsLocked());
        EXPECT_TRUE(Lock1.OwnsLock());
        EXPECT_FALSE(Lock2.OwnsLock());
    }

    EXPECT_FALSE(Mutex2.IsLocked());
}

TEST_F(MutexTest, MultiThreadedContention)
{
    constexpr i32 ThreadCount = 5;
    constexpr i32 IterationsPerThread = 1000;

    FMutex Mutex;
    i32 Counter = 0;
    std::vector<std::thread> Threads;
    Threads.reserve(ThreadCount);

    for (i32 i = 0; i < ThreadCount; ++i)
    {
        Threads.emplace_back([&]
                             {
            for (i32 j = 0; j < IterationsPerThread; ++j)
            {
                TUniqueLock<FMutex> Lock(Mutex);
                ++Counter;
            } });
    }

    for (std::thread& Thread : Threads)
    {
        Thread.join();
    }

    EXPECT_EQ(Counter, ThreadCount * IterationsPerThread);
}

TEST_F(MutexTest, SlowLockUnlock)
{
    constexpr i32 ThreadCount = 5;
    constexpr i32 IterationsPerThread = 100;

    FMutex Mutex;
    i32 Counter = 0;
    std::vector<std::thread> Threads;
    Threads.reserve(ThreadCount);

    for (i32 i = 0; i < ThreadCount; ++i)
    {
        Threads.emplace_back([&]
                             {
            for (i32 j = 0; j < IterationsPerThread; ++j)
            {
                // Force slow path by sleeping briefly to create contention
                Mutex.Lock();
                i32 Val = Counter;
                std::this_thread::sleep_for(std::chrono::microseconds(1));
                Counter = Val + 1;
                Mutex.Unlock();
            } });
    }

    for (std::thread& Thread : Threads)
    {
        Thread.join();
    }

    EXPECT_EQ(Counter, ThreadCount * IterationsPerThread);
}
