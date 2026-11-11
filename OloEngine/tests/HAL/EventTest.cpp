// OLO_TEST_LAYER: unit
//
// FEvent (through FEventRef): the pooled OS event behind ParallelFor's finish
// signal, the scheduler's worker events and the concurrency limiter. Windows
// backs it with a kernel event, Linux with a mutex and a condition variable,
// and the two have to agree on what a wait returns.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/HAL/Event.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
    using namespace OloEngine;
    using namespace std::chrono_literals;
} // namespace

// An auto-reset trigger releases one waiter. A second thread that takes the
// trigger first must leave the first one waiting, not hand it a failed wait: on
// Linux the first waiter woke for the trigger, found it taken, and returned
// false from a wait with no timeout.
TEST(Event, AnAutoResetWaitWithoutATimeoutNeverFails)
{
    constexpr int kRounds = 50;
    for (int round = 0; round < kRounds; ++round)
    {
        FEventRef event(EEventMode::AutoReset);
        std::atomic<bool> waiting{ false };
        std::atomic<int> result{ -1 };
        std::thread waiter(
            [&event, &waiting, &result]
            {
                waiting.store(true, std::memory_order_release);
                result.store(event->Wait() ? 1 : 0, std::memory_order_release);
            });
        while (!waiting.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(1ms); // let the waiter block

        event->Trigger();
        // Take the trigger from under the waiter where the platform allows it.
        if (event->Wait(0))
        {
            std::this_thread::sleep_for(5ms); // let the waiter run, if it was woken
            EXPECT_EQ(result.load(std::memory_order_acquire), -1) << "the wait ended although its trigger was taken";
            event->Trigger();
        }
        waiter.join();
        ASSERT_EQ(result.load(), 1) << "a wait without a timeout returned false, in round " << round;
    }
}

TEST(Event, AManualResetTriggerReleasesEveryWaiterAndStaysSet)
{
    FEventRef event(EEventMode::ManualReset);
    std::atomic<int> released{ 0 };
    std::thread first([&event, &released]
                      { released.fetch_add(event->Wait() ? 1 : 0, std::memory_order_relaxed); });
    std::thread second([&event, &released]
                       { released.fetch_add(event->Wait() ? 1 : 0, std::memory_order_relaxed); });
    event->Trigger();
    first.join();
    second.join();
    EXPECT_EQ(released.load(), 2);
    EXPECT_TRUE(event->Wait(0)) << "a manual-reset event stays set until Reset";
    event->Reset();
    EXPECT_FALSE(event->Wait(0));
}
