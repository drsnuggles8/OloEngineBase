/**
 * @file ParkingLotTest.cpp
 * @brief Unit tests for the ParkingLot synchronization primitive
 *
 * Ported from UE5.7's Async/ParkingLotTest.cpp
 * Tests cover: Wait, WaitFor, WaitUntil, WakeOne, FIFO ordering
 */

#include "OloEnginePCH.h"
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

// A waker clears the woken thread's wait address, then notifies its event. A
// timed wait whose timeout fires between the two finds itself dequeued and must
// take that notify before it returns. It used to return at once, and the notify
// then arrived during the thread's next, untimed wait, which returned as woken
// although nobody had woken it, with its node still queued. The test hook holds
// the waker in that gap for longer than the timed wait's timeout.
TEST_F(ParkingLotTest, ATimedWaitTimingOutWhileItsWakerIsMidWakeLeavesNoWakeBehind)
{
    static std::atomic<int> s_HeldWakes{ 0 };
    s_HeldWakes.store(0, std::memory_order_relaxed); // static, so reset for --gtest_repeat
    struct FHook
    {
        FHook()
        {
            ParkingLot::Private::GBetweenClearAndNotifyForTest.store(
                []
                {
                    s_HeldWakes.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                },
                std::memory_order_release);
        }
        ~FHook()
        {
            ParkingLot::Private::GBetweenClearAndNotifyForTest.store(nullptr, std::memory_order_release);
        }
    };

    i32 First = 0;
    i32 Second = 0;
    std::atomic<bool> bParkingOnSecond{ false };
    std::atomic<bool> bReturnedFromSecond{ false };
    std::thread Waiter(
        [&]
        {
            ParkingLot::WaitFor(&First, []
                                { return true; }, [] {}, FMonotonicTimeSpan::FromMilliseconds(100.0));
            bParkingOnSecond.store(true, std::memory_order_release);
            ParkingLot::Wait(&Second, []
                             { return true; }, [] {});
            bReturnedFromSecond.store(true, std::memory_order_release);
        });

    // Wake the timed waiter as soon as it is queued; the hook then holds this
    // thread for 300 ms between the clear and the notify, past the 100 ms timeout.
    bool bWoke = false;
    {
        FHook Hook;
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
        while (!bWoke && std::chrono::steady_clock::now() < Deadline)
        {
            bWoke = ParkingLot::WakeOne(&First).bDidWake;
        }
    }

    while (!bParkingOnSecond.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool bReturnedUnwoken = bReturnedFromSecond.load(std::memory_order_acquire);

    while (!bReturnedFromSecond.load(std::memory_order_acquire))
    {
        ParkingLot::WakeOne(&Second);
        std::this_thread::yield();
    }
    Waiter.join();

    ASSERT_TRUE(bWoke) << "the waiter was not queued within 80 ms, so the gap was never tested";
    EXPECT_EQ(s_HeldWakes.load(), 1);
    EXPECT_FALSE(bReturnedUnwoken) << "the untimed wait returned although nobody woke it: the timed wait left its wake behind";
}
