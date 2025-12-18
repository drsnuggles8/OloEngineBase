/**
 * @file ExternalMutexTest.cpp
 * @brief Unit tests for TExternalMutex synchronization primitive
 * 
 * Ported from UE5.7's Async/ExternalMutexTest.cpp
 * Tests cover: IsLocked, TryLock, Lock, Unlock with external state
 */

#include <gtest/gtest.h>

#include "OloEngine/Threading/ExternalMutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "OloEngine/Core/Base.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

using namespace OloEngine;

// ============================================================================
// ExternalMutex Basic Tests
// ============================================================================

namespace
{

// Test params for TExternalMutex (in anonymous namespace to avoid ODR issues)
struct FExternalMutexTestParams
{
    constexpr static u8 IsLockedFlag = 1;
    constexpr static u8 MayHaveWaitingLockFlag = 2;
};

} // anonymous namespace

class ExternalMutexTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
    
    static constexpr i32 TaskCount = 5;
};

TEST_F(ExternalMutexTest, IsLockedAndTryLock)
{
    std::vector<std::thread> Threads;
    Threads.reserve(TaskCount);
    std::atomic<u32> TasksComplete = 0;
    const u8 ThirdBit = 1 << 2;
    // Only the 2 LSBs of this should change.
    std::atomic<u8> ExternalState = ThirdBit;

    TExternalMutex<FExternalMutexTestParams> MainMutex(ExternalState);
    MainMutex.Lock();

    // Launch tasks that wait on the locked Mutex.
    for (i32 Index = 0; Index < TaskCount; ++Index)
    {
        Threads.emplace_back([&TasksComplete, &ExternalState, ThirdBit]
        {
            TExternalMutex<FExternalMutexTestParams> Mutex(ExternalState);
            while (!Mutex.TryLock()) // spin on attempting to acquire the lock
            {
                std::this_thread::yield();
            }
            EXPECT_TRUE(Mutex.IsLocked());
            EXPECT_FALSE(Mutex.TryLock());
            EXPECT_TRUE((ExternalState.load() & ThirdBit) != 0);
            TasksComplete++;
            Mutex.Unlock();
        });
    }

    MainMutex.Unlock();

    // spin while waiting for tasks to complete
    while (TasksComplete.load() != TaskCount)
    {
        std::this_thread::yield();
    }

    // Wait for the threads to exit.
    for (std::thread& Thread : Threads)
    {
        Thread.join();
    }

    EXPECT_EQ(ExternalState.load(), ThirdBit);
}

TEST_F(ExternalMutexTest, WithUniqueLockSlowPath)
{
    std::vector<std::thread> Threads;
    Threads.reserve(TaskCount);
    std::atomic<u32> TasksComplete = 0;
    const u8 ThirdBit = 1 << 2;
    // Only the 2 LSBs of this should change.
    std::atomic<u8> ExternalState = ThirdBit;

    TExternalMutex<FExternalMutexTestParams> MainMutex(ExternalState);
    MainMutex.Lock();

    // Launch tasks that wait on the locked Mutex.
    for (i32 Index = 0; Index < TaskCount; ++Index)
    {
        Threads.emplace_back([&TasksComplete, &ExternalState, ThirdBit]
        {
            TExternalMutex<FExternalMutexTestParams> Mutex(ExternalState);
            TUniqueLock Lock(Mutex);
            EXPECT_TRUE(Mutex.IsLocked());
            EXPECT_TRUE((ExternalState.load() & ThirdBit) != 0);
            TasksComplete++;
        });
    }

    MainMutex.Unlock();

    // spin while waiting for tasks to complete
    while (TasksComplete.load() != TaskCount)
    {
        std::this_thread::yield();
    }

    // Wait for the threads to exit.
    for (std::thread& Thread : Threads)
    {
        Thread.join();
    }

    EXPECT_EQ(ExternalState.load(), ThirdBit);
}

TEST_F(ExternalMutexTest, MutualExclusion)
{
    constexpr i32 IterationsPerThread = 100;
    
    const u8 ThirdBit = 1 << 2;
    std::atomic<u8> ExternalState = ThirdBit;
    i32 Counter = 0;
    std::vector<std::thread> Threads;
    Threads.reserve(TaskCount);
    
    for (i32 i = 0; i < TaskCount; ++i)
    {
        Threads.emplace_back([&Counter, &ExternalState, ThirdBit]
        {
            for (i32 j = 0; j < IterationsPerThread; ++j)
            {
                TExternalMutex<FExternalMutexTestParams> Mutex(ExternalState);
                Mutex.Lock();
                
                // Verify the third bit is preserved
                EXPECT_TRUE((ExternalState.load() & ThirdBit) != 0);
                
                i32 Val = Counter;
                std::this_thread::yield(); // Give chance for race conditions
                Counter = Val + 1;
                
                Mutex.Unlock();
            }
        });
    }
    
    for (std::thread& Thread : Threads)
    {
        Thread.join();
    }
    
    EXPECT_EQ(Counter, TaskCount * IterationsPerThread);
    EXPECT_EQ(ExternalState.load(), ThirdBit);
}

TEST_F(ExternalMutexTest, StatePreservation)
{
    // Test that the mutex only modifies the specified bits
    // and preserves other state in the atomic
    
    std::atomic<u8> ExternalState = 0b11111100;  // All bits except lock bits set
    
    TExternalMutex<FExternalMutexTestParams> Mutex(ExternalState);
    
    EXPECT_FALSE(Mutex.IsLocked());
    
    Mutex.Lock();
    EXPECT_TRUE(Mutex.IsLocked());
    // Other bits should be preserved
    EXPECT_EQ((ExternalState.load() & 0b11111100), 0b11111100);
    
    Mutex.Unlock();
    EXPECT_FALSE(Mutex.IsLocked());
    // State should return to original (with no lock bits)
    EXPECT_EQ(ExternalState.load(), 0b11111100);
}

TEST_F(ExternalMutexTest, MultipleMutexesSameState)
{
    // Test that multiple TExternalMutex instances referencing the same state work correctly
    std::atomic<u8> ExternalState = 0;
    
    TExternalMutex<FExternalMutexTestParams> Mutex1(ExternalState);
    TExternalMutex<FExternalMutexTestParams> Mutex2(ExternalState);
    
    EXPECT_FALSE(Mutex1.IsLocked());
    EXPECT_FALSE(Mutex2.IsLocked());
    
    Mutex1.Lock();
    
    EXPECT_TRUE(Mutex1.IsLocked());
    EXPECT_TRUE(Mutex2.IsLocked());  // Both see the same state
    EXPECT_FALSE(Mutex2.TryLock());  // Can't lock again via different instance
    
    Mutex1.Unlock();
    
    EXPECT_FALSE(Mutex1.IsLocked());
    EXPECT_FALSE(Mutex2.IsLocked());
    
    // Now lock via Mutex2
    Mutex2.Lock();
    EXPECT_TRUE(Mutex1.IsLocked());
    Mutex2.Unlock();
}
