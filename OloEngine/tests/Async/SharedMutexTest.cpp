/**
 * @file SharedMutexTest.cpp
 * @brief Unit tests for the FSharedMutex (reader-writer lock) synchronization primitive
 * 
 * Ported from UE5.7's Async/SharedMutexTest.cpp
 * Tests cover: Lock, LockShared, TryLock, TryLockShared, concurrent readers
 */

#include <gtest/gtest.h>

#include "OloEngine/Threading/SharedMutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "OloEngine/Core/Base.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

using namespace OloEngine;

// ============================================================================
// SharedMutex Basic Tests
// ============================================================================

class SharedMutexTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(SharedMutexTest, SingleThreadExclusive)
{
    FSharedMutex Mutex;
    
    Mutex.Lock();
    EXPECT_TRUE(Mutex.IsLocked());
    Mutex.Unlock();
    
    EXPECT_FALSE(Mutex.IsLocked());
}

TEST_F(SharedMutexTest, SingleThreadShared)
{
    FSharedMutex Mutex;
    
    Mutex.LockShared();
    EXPECT_FALSE(Mutex.IsLocked());
    EXPECT_TRUE(Mutex.IsLockedShared());
    Mutex.UnlockShared();
}

TEST_F(SharedMutexTest, MultipleReaders)
{
    FSharedMutex Mutex;
    
    // Multiple readers should be allowed
    Mutex.LockShared();
    Mutex.LockShared();
    Mutex.LockShared();
    
    EXPECT_FALSE(Mutex.IsLocked());
    EXPECT_TRUE(Mutex.IsLockedShared());
    
    Mutex.UnlockShared();
    Mutex.UnlockShared();
    Mutex.UnlockShared();
}

TEST_F(SharedMutexTest, TryLockWhenUnlocked)
{
    FSharedMutex Mutex;
    
    EXPECT_TRUE(Mutex.TryLock());
    Mutex.Unlock();
}

TEST_F(SharedMutexTest, TryLockWhenExclusiveLocked)
{
    FSharedMutex Mutex;
    
    Mutex.Lock();
    
    std::atomic<bool> TryLockResult{ true };
    std::thread Thread([&]
    {
        TryLockResult = Mutex.TryLock();
    });
    
    Thread.join();
    
    EXPECT_FALSE(TryLockResult.load());
    Mutex.Unlock();
}

TEST_F(SharedMutexTest, TryLockWhenSharedLocked)
{
    FSharedMutex Mutex;
    
    Mutex.LockShared();
    
    std::atomic<bool> TryLockResult{ true };
    std::thread Thread([&]
    {
        TryLockResult = Mutex.TryLock();
    });
    
    Thread.join();
    
    EXPECT_FALSE(TryLockResult.load());
    Mutex.UnlockShared();
}

TEST_F(SharedMutexTest, TryLockSharedWhenExclusiveLocked)
{
    FSharedMutex Mutex;
    
    Mutex.Lock();
    
    std::atomic<bool> TryLockResult{ true };
    std::thread Thread([&]
    {
        TryLockResult = Mutex.TryLockShared();
    });
    
    Thread.join();
    
    EXPECT_FALSE(TryLockResult.load());
    Mutex.Unlock();
}

TEST_F(SharedMutexTest, ConcurrentReaders)
{
    constexpr i32 ReaderCount = 10;
    
    FSharedMutex Mutex;
    std::atomic<i32> CurrentReaderCount{ 0 };
    std::atomic<i32> MaxConcurrentReaders{ 0 };
    std::vector<std::thread> Threads;
    Threads.reserve(ReaderCount);
    
    for (i32 i = 0; i < ReaderCount; ++i)
    {
        Threads.emplace_back([&]
        {
            Mutex.LockShared();
            
            i32 Current = CurrentReaderCount.fetch_add(1) + 1;
            
            // Update max concurrent readers
            i32 Max = MaxConcurrentReaders.load();
            while (Current > Max && !MaxConcurrentReaders.compare_exchange_weak(Max, Current))
            {
                // Retry
            }
            
            // Hold the lock for a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
            CurrentReaderCount.fetch_sub(1);
            
            Mutex.UnlockShared();
        });
    }
    
    for (std::thread& Thread : Threads)
    {
        Thread.join();
    }
    
    // Multiple readers should have been concurrent
    EXPECT_GT(MaxConcurrentReaders.load(), 1);
}

TEST_F(SharedMutexTest, ExclusiveBlocksReaders)
{
    FSharedMutex Mutex;
    std::atomic<bool> ReaderStarted{ false };
    std::atomic<bool> ReaderAcquired{ false };
    
    Mutex.Lock();
    
    std::thread ReaderThread([&]
    {
        ReaderStarted = true;
        Mutex.LockShared();
        ReaderAcquired = true;
        Mutex.UnlockShared();
    });
    
    // Wait for reader to start
    while (!ReaderStarted.load())
    {
        std::this_thread::yield();
    }
    
    // Give reader time to attempt lock
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Reader should be blocked
    EXPECT_FALSE(ReaderAcquired.load());
    
    Mutex.Unlock();
    
    ReaderThread.join();
    
    EXPECT_TRUE(ReaderAcquired.load());
}

TEST_F(SharedMutexTest, ReaderWriterInterleaving)
{
    constexpr i32 IterationCount = 100;
    
    FSharedMutex Mutex;
    i32 SharedData = 0;
    std::atomic<bool> Running{ true };
    std::atomic<i32> ReaderErrors{ 0 };
    
    // Writer thread (uses exclusive lock)
    std::thread Writer([&]
    {
        for (i32 i = 0; i < IterationCount; ++i)
        {
            Mutex.Lock();
            SharedData = i * 2;  // Always even
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            SharedData = i * 2 + 1;  // Now odd
            Mutex.Unlock();
        }
        Running = false;
    });
    
    // Reader threads (use shared lock)
    std::vector<std::thread> Readers;
    for (i32 i = 0; i < 3; ++i)
    {
        Readers.emplace_back([&]
        {
            while (Running.load())
            {
                Mutex.LockShared();
                i32 Value = SharedData;
                // Value should always be complete (odd) after first write
                // or 0 before any writes
                if (Value != 0 && Value % 2 == 0)
                {
                    ReaderErrors.fetch_add(1);
                }
                Mutex.UnlockShared();
                std::this_thread::yield();
            }
        });
    }
    
    Writer.join();
    
    for (std::thread& Reader : Readers)
    {
        Reader.join();
    }
    
    EXPECT_EQ(ReaderErrors.load(), 0);
    EXPECT_EQ(SharedData, (IterationCount - 1) * 2 + 1);
}

TEST_F(SharedMutexTest, ScopedSharedLock)
{
    FSharedMutex Mutex;
    
    {
        TSharedLock<FSharedMutex> ReadLock(Mutex);
        EXPECT_FALSE(Mutex.IsLocked());
        EXPECT_FALSE(Mutex.TryLock());
    }
    
    EXPECT_TRUE(Mutex.TryLock());
    Mutex.Unlock();
}

TEST_F(SharedMutexTest, ScopedExclusiveLock)
{
    FSharedMutex Mutex;
    
    {
        TUniqueLock<FSharedMutex> WriteLock(Mutex);
        EXPECT_TRUE(Mutex.IsLocked());
    }
    
    EXPECT_FALSE(Mutex.IsLocked());
}

TEST_F(SharedMutexTest, StressTest)
{
    constexpr i32 ThreadCount = 8;
    constexpr i32 OperationsPerThread = 1000;
    
    FSharedMutex Mutex;
    i32 Counter = 0;
    std::vector<std::thread> Threads;
    Threads.reserve(ThreadCount);
    
    for (i32 i = 0; i < ThreadCount; ++i)
    {
        Threads.emplace_back([&, threadId = i]
        {
            for (i32 j = 0; j < OperationsPerThread; ++j)
            {
                if (j % 10 == 0)  // 10% writes
                {
                    Mutex.Lock();
                    ++Counter;
                    Mutex.Unlock();
                }
                else  // 90% reads
                {
                    Mutex.LockShared();
                    volatile i32 Val = Counter;  // Just read
                    (void)Val;
                    Mutex.UnlockShared();
                }
            }
        });
    }
    
    for (std::thread& Thread : Threads)
    {
        Thread.join();
    }
    
    // Expected writes: ThreadCount * OperationsPerThread * 0.1
    EXPECT_EQ(Counter, ThreadCount * (OperationsPerThread / 10));
}

