/**
 * @file MutexLockRateTest.cpp
 * @brief Lock rate benchmarks for OloEngine mutex implementations
 * 
 * Ported from UE5.7's Tests/Async/MutexLockRateTest.cpp
 * Tests: FMutex, FRecursiveMutex, FSharedMutex, FSharedRecursiveMutex, FExternalMutex
 * 
 * These are benchmarks rather than unit tests - they measure lock/unlock throughput
 * across varying thread counts. Run with --gtest_filter=*LockRate* for benchmark output.
 */

#include <gtest/gtest.h>

#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/RecursiveMutex.h"
#include "OloEngine/Threading/SharedMutex.h"
#include "OloEngine/Threading/SharedRecursiveMutex.h"
#include "OloEngine/Threading/ExternalMutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "OloEngine/Threading/SharedLock.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Task/LowLevelTask.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Base.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <iomanip>

using namespace OloEngine;
using namespace OloEngine::LowLevelTasks;

// ============================================================================
// Test Utilities
// ============================================================================

template <typename BodyType>
static double TestConcurrency(const i32 TaskCount, BodyType&& Body)
{
    constexpr i32 MaxTaskCount = 256;
    OLO_CORE_ASSERT(TaskCount <= MaxTaskCount);

    LowLevelTasks::FTask Tasks[MaxTaskCount];
    std::atomic<i32> StartCount = TaskCount;
    std::atomic<i32> EndCount = TaskCount;

    const auto TaskBody = [&Body, &StartCount, &EndCount](i32 TaskIndex)
    {
        StartCount.fetch_sub(1, std::memory_order_relaxed);
        while (StartCount.load(std::memory_order_relaxed) > 0)
        {
            // Spin wait for all threads to be ready
        }
        Body(TaskIndex);
        EndCount.fetch_sub(1, std::memory_order_relaxed);
    };

    for (i32 TaskIndex = 1; TaskIndex < TaskCount; ++TaskIndex)
    {
        Tasks[TaskIndex].Init("LockRateTask", LowLevelTasks::ETaskPriority::Normal, 
            [&TaskBody, TaskIndex]
            {
                TaskBody(TaskIndex);
            }
        );
        OLO_CORE_VERIFY(LowLevelTasks::TryLaunch(Tasks[TaskIndex]));
    }

    while (StartCount.load(std::memory_order_relaxed) > 1)
    {
        // Spin wait
    }

    const auto StartTime = std::chrono::high_resolution_clock::now();
    StartCount.fetch_sub(1, std::memory_order_relaxed);
    Body(0);
    while (EndCount.load(std::memory_order_relaxed) > 1)
    {
        // Spin wait
    }
    const auto EndTime = std::chrono::high_resolution_clock::now();

    for (auto& Task : MakeArrayView(Tasks, TaskCount))
    {
        while (!Task.IsCompleted())
        {
            // Spin wait
        }
    }

    return std::chrono::duration<double>(EndTime - StartTime).count();
}

template <typename LockType>
static void TestLockRate(LockType& Mutex, i32 LockTarget, i32 IterationCount)
{
    std::cout << std::setw(8) << "Threads" 
              << std::setw(14) << "LockRate" 
              << std::setw(14) << "Mean" 
              << std::setw(14) << "StdDev" 
              << std::endl;

    struct FIteration
    {
        i64 LockRate = 0;
        i64 LockCount = 0;
        TArray<i64> LockCountByThread;
    };
    TArray<FIteration> Iterations;

    const i32 ThreadLimit = static_cast<i32>(FScheduler::Get().GetNumWorkers());
    
    for (i32 ThreadCount = 1; ThreadCount <= ThreadLimit; ++ThreadCount)
    {
        Iterations.Reset(IterationCount);
        
        for (i32 Iteration = 0; Iteration < IterationCount; ++Iteration)
        {
            TArray<i64> LockCountByThread;
            LockCountByThread.SetNumZeroed(ThreadCount);
            std::atomic<bool> bStop = false;
            std::atomic<i64> LockCount = 0;
            
            const double Duration = TestConcurrency(ThreadCount, 
                [&Mutex, &LockCount, &LockCountByThread, LockTarget, &bStop](i32 ThreadIndex)
                {
                    i64 ThreadLockCount = 0;
                    while (!bStop.load(std::memory_order_relaxed))
                    {
                        Mutex.Lock();
                        Mutex.Unlock();
                        if (++ThreadLockCount >= LockTarget && ThreadIndex == 0)
                        {
                            bStop.store(true, std::memory_order_relaxed);
                        }
                    }
                    LockCount.fetch_add(ThreadLockCount, std::memory_order_relaxed);
                    LockCountByThread[ThreadIndex] = ThreadLockCount;
                }
            );
            
            FIteration NewIter;
            NewIter.LockRate = static_cast<i64>(LockCount / Duration);
            NewIter.LockCount = LockCount;
            NewIter.LockCountByThread = MoveTemp(LockCountByThread);
            Iterations.Add(MoveTemp(NewIter));
        }

        // Sort by lock rate and take the best iteration
        // Using raw pointers since TArray iterator doesn't support operator-
        std::sort(Iterations.GetData(), Iterations.GetData() + Iterations.Num(), 
            [](const FIteration& A, const FIteration& B) { return A.LockRate < B.LockRate; }
        );
        
        const FIteration& BestIteration = Iterations.Last();
        const i64 LockRate = BestIteration.LockRate;
        const i64 LockCountMean = BestIteration.LockCount / ThreadCount;
        
        // Calculate standard deviation
        i64 SumSquaredDiff = 0;
        for (i64 ThreadLockCount : BestIteration.LockCountByThread)
        {
            i64 Diff = ThreadLockCount - LockCountMean;
            SumSquaredDiff += Diff * Diff;
        }
        const i64 LockCountStdDev = static_cast<i64>(std::sqrt(static_cast<double>(SumSquaredDiff) / ThreadCount));

        std::cout << std::setw(8) << ThreadCount
                  << std::setw(14) << LockRate
                  << std::setw(14) << LockCountMean
                  << std::setw(14) << LockCountStdDev
                  << std::endl;
    }
}

// ============================================================================
// Lock Rate Test Fixture
// ============================================================================

class MutexLockRateTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        // Start workers for concurrent testing
        FScheduler::Get().StartWorkers();
    }

    static void TearDownTestSuite()
    {
        FScheduler::Get().StopWorkers();
    }

    // Benchmark configuration - reduced for automated testing
    // Run manually with larger values for detailed benchmarks
    static constexpr i32 LockTarget = 8192;      // Locks before stopping
    static constexpr i32 IterationCount = 4;     // Iterations per thread count
};

// ============================================================================
// Lock Rate Benchmarks
// ============================================================================

TEST_F(MutexLockRateTest, DISABLED_MutexLockRate)
{
    std::cout << "\n=== FMutex Lock Rate ===" << std::endl;
    FMutex Mutex;
    TestLockRate(Mutex, LockTarget, IterationCount);
}

TEST_F(MutexLockRateTest, DISABLED_RecursiveMutexLockRate)
{
    std::cout << "\n=== FRecursiveMutex Lock Rate ===" << std::endl;
    FRecursiveMutex Mutex;
    TestLockRate(Mutex, LockTarget, IterationCount);
}

TEST_F(MutexLockRateTest, DISABLED_SharedMutexLockRate)
{
    std::cout << "\n=== FSharedMutex Lock Rate ===" << std::endl;
    FSharedMutex Mutex;
    TestLockRate(Mutex, LockTarget, IterationCount);
}

TEST_F(MutexLockRateTest, DISABLED_SharedRecursiveMutexLockRate)
{
    std::cout << "\n=== FSharedRecursiveMutex Lock Rate ===" << std::endl;
    FSharedRecursiveMutex Mutex;
    TestLockRate(Mutex, LockTarget, IterationCount);
}

namespace
{

// External mutex with test params (in anonymous namespace to avoid ODR issues)
// Uses bits 7 and 6 for testing that the mutex works with any bit positions
struct FExternalMutexLockRateTestParams
{
    constexpr static u8 IsLockedFlag = 1 << 7;
    constexpr static u8 MayHaveWaitingLockFlag = 1 << 6;
};

} // anonymous namespace

TEST_F(MutexLockRateTest, DISABLED_ExternalMutexLockRate)
{
    std::cout << "\n=== TExternalMutex Lock Rate ===" << std::endl;
    std::atomic<u8> State{0};
    TExternalMutex<FExternalMutexLockRateTestParams> Mutex(State);
    TestLockRate(Mutex, LockTarget, IterationCount);
}

// ============================================================================
// Enabled Smoke Test (runs by default to verify the framework works)
// ============================================================================

TEST_F(MutexLockRateTest, SmokeTest)
{
    // Quick smoke test to verify the benchmark infrastructure works
    FMutex Mutex;
    constexpr i32 QuickLockTarget = 1000;
    constexpr i32 QuickIterations = 1;
    
    std::atomic<i64> TotalLocks{0};
    std::atomic<bool> bStop{false};
    
    const double Duration = TestConcurrency(2, 
        [&Mutex, &TotalLocks, &bStop, QuickLockTarget](i32 ThreadIndex)
        {
            i64 Count = 0;
            while (!bStop.load(std::memory_order_relaxed))
            {
                Mutex.Lock();
                Mutex.Unlock();
                if (++Count >= QuickLockTarget && ThreadIndex == 0)
                {
                    bStop.store(true, std::memory_order_relaxed);
                }
            }
            TotalLocks.fetch_add(Count, std::memory_order_relaxed);
        }
    );
    
    // Just verify we completed without issues
    EXPECT_GT(TotalLocks.load(), 0);
    EXPECT_GT(Duration, 0.0);
}

