/**
 * @file ManualResetEventTest.cpp
 * @brief Unit tests for the FManualResetEvent synchronization primitive
 * 
 * Ported from UE5.7's Async/ManualResetEventTest.cpp
 * Tests cover: Notify, Wait, Reset, WaitFor timeout behavior
 */

#include <gtest/gtest.h>

#include "OloEngine/HAL/ManualResetEvent.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/MonotonicTime.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

using namespace OloEngine;

// ============================================================================
// ManualResetEvent Basic Tests
// ============================================================================

class ManualResetEventTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ManualResetEventTest, NotifyAndWait)
{
    FManualResetEvent Event;
    
    Event.Notify();
    Event.Wait();  // Should return immediately
    
    // Can wait again since it's not auto-reset
    Event.Wait();  // Should still return immediately
}

TEST_F(ManualResetEventTest, WaitForUnset)
{
    FManualResetEvent Event;
    
    auto Start = std::chrono::steady_clock::now();
    bool Result = Event.WaitFor(FMonotonicTimeSpan::FromMilliseconds(10.0));
    auto End = std::chrono::steady_clock::now();
    
    EXPECT_FALSE(Result);
    
    // Should have waited approximately 10ms
    auto ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(End - Start).count();
    EXPECT_GE(ElapsedMs, 5);
}

TEST_F(ManualResetEventTest, WaitForSet)
{
    FManualResetEvent Event;
    
    Event.Notify();
    
    auto Start = std::chrono::steady_clock::now();
    bool Result = Event.WaitFor(FMonotonicTimeSpan::FromMilliseconds(100.0));
    auto End = std::chrono::steady_clock::now();
    
    EXPECT_TRUE(Result);
    
    // Should return immediately
    auto ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(End - Start).count();
    EXPECT_LT(ElapsedMs, 50);
}

TEST_F(ManualResetEventTest, Reset)
{
    FManualResetEvent Event;
    
    Event.Notify();
    EXPECT_TRUE(Event.WaitFor(FMonotonicTimeSpan::FromMilliseconds(0)));
    
    Event.Reset();
    EXPECT_FALSE(Event.WaitFor(FMonotonicTimeSpan::FromMilliseconds(0)));
}

TEST_F(ManualResetEventTest, MultipleWaiters)
{
    constexpr i32 WaiterCount = 5;
    
    FManualResetEvent Event;
    std::atomic<i32> WokenCount{ 0 };
    std::atomic<i32> WaitingCount{ 0 };
    std::vector<std::thread> Threads;
    Threads.reserve(WaiterCount);
    
    for (i32 i = 0; i < WaiterCount; ++i)
    {
        Threads.emplace_back([&]
        {
            WaitingCount.fetch_add(1);
            Event.Wait();
            WokenCount.fetch_add(1);
        });
    }
    
    // Wait for all threads to be waiting
    while (WaitingCount.load() != WaiterCount)
    {
        std::this_thread::yield();
    }
    
    // Give threads time to enter wait
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Notify - all waiters should wake
    Event.Notify();
    
    for (std::thread& Thread : Threads)
    {
        Thread.join();
    }
    
    EXPECT_EQ(WokenCount.load(), WaiterCount);
}

TEST_F(ManualResetEventTest, NotifyBeforeWait)
{
    FManualResetEvent Event;
    
    // Notify before any waiters
    Event.Notify();
    
    std::atomic<bool> ThreadCompleted{ false };
    
    std::thread Thread([&]
    {
        Event.Wait();  // Should return immediately
        ThreadCompleted = true;
    });
    
    Thread.join();
    
    EXPECT_TRUE(ThreadCompleted.load());
}

TEST_F(ManualResetEventTest, ResetWhileWaiting)
{
    FManualResetEvent Event;
    FManualResetEvent SyncEvent;  // For synchronization
    
    std::atomic<bool> ThreadWaiting{ false };
    std::atomic<bool> ThreadWoken{ false };
    std::atomic<bool> ReadyForSecondWait{ false };
    std::atomic<bool> SecondWaitComplete{ false };
    
    std::thread Thread([&]
    {
        ThreadWaiting = true;
        Event.Wait();
        ThreadWoken = true;
        
        // Signal we're ready for second wait and wait for main thread to reset
        ReadyForSecondWait = true;
        SyncEvent.Wait();  // Wait for main thread to reset Event
        
        // Try to wait again after reset
        Event.Wait();
        SecondWaitComplete = true;
    });
    
    // Wait for thread to start waiting
    while (!ThreadWaiting.load())
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Notify to wake the first wait
    Event.Notify();
    
    // Wait for thread to be ready for second wait
    while (!ReadyForSecondWait.load())
    {
        std::this_thread::yield();
    }
    
    // Reset the event BEFORE allowing thread to proceed
    Event.Reset();
    
    // Now let thread proceed to second wait
    SyncEvent.Notify();
    
    // Give thread time to enter second wait
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Thread should be waiting again
    EXPECT_FALSE(SecondWaitComplete.load());
    
    // Notify again
    Event.Notify();
    
    Thread.join();
    
    EXPECT_TRUE(SecondWaitComplete.load());
}

