/**
 * @file ParkingLotTest.cpp
 * @brief Unit tests for the ParkingLot synchronization primitive
 *
 * Ported from UE5.7's Async/ParkingLotTest.cpp
 * Tests cover: Wait, WaitFor, WaitUntil, WakeOne, FIFO ordering
 */

#include <gtest/gtest.h>

#include "OloEngine/HAL/ParkingLot.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/MonotonicTime.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

using namespace OloEngine;

// ============================================================================
// ParkingLot Basic Tests
// ============================================================================

class ParkingLotTest : public ::testing::Test
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ParkingLotTest, CanWaitReturnsFalse)
{
    i32 Value = 0;
    i32 CanWaitCount = 0;
    i32 BeforeWaitCount = 0;

    ParkingLot::FWaitState State = ParkingLot::Wait(&Value, [&CanWaitCount]
                                                    { ++CanWaitCount; return false; }, [&BeforeWaitCount]
                                                    { ++BeforeWaitCount; });

    EXPECT_EQ(CanWaitCount, 1);
    EXPECT_EQ(BeforeWaitCount, 0);
    EXPECT_FALSE(State.bDidWait);
    EXPECT_FALSE(State.bDidWake);
    EXPECT_EQ(State.WakeToken, 0u);
}

TEST_F(ParkingLotTest, WaitForWithTimeout)
{
    i32 Value = 0;
    i32 CanWaitCount = 0;
    i32 BeforeWaitCount = 0;

    ParkingLot::FWaitState State = ParkingLot::WaitFor(&Value, [&CanWaitCount]
                                                       { ++CanWaitCount; return true; }, [&BeforeWaitCount]
                                                       { ++BeforeWaitCount; }, FMonotonicTimeSpan::FromMilliseconds(1.0));

    EXPECT_EQ(CanWaitCount, 1);
    EXPECT_EQ(BeforeWaitCount, 1);
    EXPECT_TRUE(State.bDidWait);
    EXPECT_FALSE(State.bDidWake);
    EXPECT_EQ(State.WakeToken, 0u);
}

TEST_F(ParkingLotTest, WaitUntilWithTimeout)
{
    i32 Value = 0;
    i32 CanWaitCount = 0;
    i32 BeforeWaitCount = 0;

    ParkingLot::FWaitState State = ParkingLot::WaitUntil(&Value, [&CanWaitCount]
                                                         { ++CanWaitCount; return true; }, [&BeforeWaitCount]
                                                         { ++BeforeWaitCount; }, FMonotonicTimePoint::Now() + FMonotonicTimeSpan::FromMilliseconds(1.0));

    EXPECT_EQ(CanWaitCount, 1);
    EXPECT_EQ(BeforeWaitCount, 1);
    EXPECT_TRUE(State.bDidWait);
    EXPECT_FALSE(State.bDidWake);
    EXPECT_EQ(State.WakeToken, 0u);
}

TEST_F(ParkingLotTest, FIFOOrderingAndWakeToken)
{
    constexpr i32 TaskCount = 5;
    std::vector<std::thread> Threads;
    Threads.reserve(TaskCount);

    std::atomic<i32> WaitCount{ 0 };
    ParkingLot::FWaitState WaitStates[TaskCount];

    // Launch tasks that wait on the address of WaitCount
    for (i32 Index = 0; Index < TaskCount; ++Index)
    {
        Threads.emplace_back([&WaitCount, OutState = &WaitStates[Index]]
                             {
            i32 CanWaitCount = 0;
            i32 BeforeWaitCount = 0;
            *OutState = ParkingLot::Wait(&WaitCount,
                [&CanWaitCount] { ++CanWaitCount; return true; },
                [&BeforeWaitCount, &WaitCount] { ++BeforeWaitCount; WaitCount.fetch_add(1); });
            WaitCount.fetch_sub(1);
            EXPECT_EQ(CanWaitCount, 1);
            EXPECT_EQ(BeforeWaitCount, 1); });

        // Spin until the task is waiting
        while (WaitCount.load() != Index + 1)
        {
            std::this_thread::yield();
        }
    }

    // Wake each task with a sequence number, with an extra wake call that has no thread to wake
    u64 Sequence = 0;
    for (i32 Index = 0; Index <= TaskCount; ++Index)
    {
        i32 WakeCount = 0;
        ParkingLot::WakeOne(&WaitCount, [&WakeCount, &Sequence, Index](ParkingLot::FWakeState WakeState) -> u64
                            {
            ++WakeCount;
            EXPECT_EQ(WakeState.bDidWake, (Index < TaskCount));
            EXPECT_EQ(WakeState.bHasWaitingThreads, (Index + 1 < TaskCount));
            return ++Sequence; });
        // The callback must be invoked exactly once
        EXPECT_EQ(WakeCount, 1);
    }

    // Spin until the tasks are complete
    while (WaitCount.load() != 0)
    {
        std::this_thread::yield();
    }

    // Verify that tasks woke in FIFO order
    for (i32 Index = 0; Index < TaskCount; ++Index)
    {
        const ParkingLot::FWaitState& WaitState = WaitStates[Index];
        EXPECT_TRUE(WaitState.bDidWait);
        EXPECT_TRUE(WaitState.bDidWake);
        EXPECT_EQ(WaitState.WakeToken, static_cast<u64>(Index + 1));
    }

    // Wait for the threads to exit
    for (std::thread& Thread : Threads)
    {
        Thread.join();
    }
}

TEST_F(ParkingLotTest, WakeAll)
{
    constexpr i32 TaskCount = 5;
    std::vector<std::thread> Threads;
    Threads.reserve(TaskCount);

    std::atomic<i32> WaitCount{ 0 };
    std::atomic<i32> WokenCount{ 0 };

    // Launch tasks that wait on the address of WaitCount
    for (i32 Index = 0; Index < TaskCount; ++Index)
    {
        Threads.emplace_back([&WaitCount, &WokenCount]
                             {
            ParkingLot::Wait(&WaitCount,
                [] { return true; },
                [&WaitCount] { WaitCount.fetch_add(1); });
            WokenCount.fetch_add(1); });
    }

    // Spin until all tasks are waiting
    while (WaitCount.load() != TaskCount)
    {
        std::this_thread::yield();
    }

    // Wake all tasks at once
    ParkingLot::WakeAll(&WaitCount);

    // Wait for all threads
    for (std::thread& Thread : Threads)
    {
        Thread.join();
    }

    EXPECT_EQ(WokenCount.load(), TaskCount);
}
