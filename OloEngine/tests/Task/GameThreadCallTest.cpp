// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Task/GameThreadCall.h"
#include "OloEngine/Task/NamedThreads.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

// Tasks::RunOnGameThreadAndWait: work a worker hands the game thread (an asset
// pack build's loads, which create GPU resources) runs THERE, between frames,
// while the worker waits. Here the test thread plays the game thread and pumps
// its queue the way Application::Run does at the top of a frame.

namespace
{
    using namespace std::chrono_literals;
    using OloEngine::Tasks::ENamedThread;
    using OloEngine::Tasks::FNamedThreadManager;
    using OloEngine::Tasks::RunOnGameThreadAndWait;

    class GameThreadCallTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            auto& manager = FNamedThreadManager::Get();
            const ENamedThread current = manager.GetCurrentThreadIfKnown();
            ASSERT_TRUE(current == ENamedThread::Invalid || current == ENamedThread::GameThread);
            m_AttachedGameThread = current != ENamedThread::GameThread;
            if (m_AttachedGameThread)
                manager.AttachToThread(ENamedThread::GameThread);
            manager.GetQueue(ENamedThread::GameThread).ProcessAll(true);
        }

        void TearDown() override
        {
            auto& manager = FNamedThreadManager::Get();
            manager.GetQueue(ENamedThread::GameThread).ProcessAll(true);
            if (m_AttachedGameThread)
                manager.DetachFromThread(ENamedThread::GameThread);
        }

        // The game thread's frame loop: drain the queue until `done`, bounded.
        static bool PumpUntil(const std::atomic<bool>& done)
        {
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (!done.load(std::memory_order_acquire))
            {
                if (std::chrono::steady_clock::now() > deadline)
                    return false;
                FNamedThreadManager::Get().GetQueue(ENamedThread::GameThread).ProcessAll(true);
                std::this_thread::sleep_for(1ms);
            }
            return true;
        }

        static bool WaitForQueuedWork()
        {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (FNamedThreadManager::Get().GetQueue(ENamedThread::GameThread).HasPendingTasks(true))
                    return true;
                std::this_thread::yield();
            }
            return false;
        }

        bool m_AttachedGameThread = false;
    };
} // namespace

TEST_F(GameThreadCallTest, AWorkersWorkRunsOnTheGameThreadWhileTheWorkerWaits)
{
    const std::thread::id gameThread = std::this_thread::get_id();
    std::thread::id worker;
    std::optional<std::thread::id> ranOn;
    std::atomic<bool> done{ false };
    std::thread thread([&]
                       {
        worker = std::this_thread::get_id();
        ranOn = RunOnGameThreadAndWait<std::thread::id>([] { return std::this_thread::get_id(); });
        done.store(true, std::memory_order_release); });
    const bool finished = PumpUntil(done);
    thread.join();
    ASSERT_TRUE(finished) << "the worker never got its result back";
    ASSERT_TRUE(ranOn.has_value());
    EXPECT_EQ(*ranOn, gameThread) << "the work ran off the game thread";
    EXPECT_NE(*ranOn, worker);
}

TEST_F(GameThreadCallTest, TheGameThreadItselfRunsTheWorkInline)
{
    // No pump: a game-thread caller that queued its own work would wait on itself.
    const std::optional<int> result = RunOnGameThreadAndWait<int>([]
                                                                  { return 7; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 7);
}

TEST_F(GameThreadCallTest, WithNoGameThreadAttachedTheCallerRunsTheWork)
{
    // A host with no game thread (a command-line cook) has nowhere to marshal to.
    auto& manager = FNamedThreadManager::Get();
    manager.DetachFromThread(ENamedThread::GameThread);
    std::optional<std::thread::id> ranOn;
    std::thread::id worker;
    std::thread thread([&]
                       {
        worker = std::this_thread::get_id();
        ranOn = RunOnGameThreadAndWait<std::thread::id>([] { return std::this_thread::get_id(); }); });
    thread.join();
    manager.AttachToThread(ENamedThread::GameThread);
    ASSERT_TRUE(ranOn.has_value());
    EXPECT_EQ(*ranOn, worker);
}

TEST_F(GameThreadCallTest, ACancelWhileQueuedAbandonsTheWorkAndItNeverRuns)
{
    std::atomic<bool> cancel{ false };
    std::atomic<int> runs{ 0 };
    std::optional<int> result{ 1 };
    std::thread thread([&]
                       { result = RunOnGameThreadAndWait<int>([&runs]
                                                              { return ++runs; }, &cancel); });
    ASSERT_TRUE(WaitForQueuedWork());
    cancel.store(true, std::memory_order_release);
    thread.join();
    EXPECT_FALSE(result.has_value()) << "a cancelled call returned a result";
    // The game thread's next frame finds the abandoned call and skips it.
    FNamedThreadManager::Get().GetQueue(ENamedThread::GameThread).ProcessAll(true);
    EXPECT_EQ(runs.load(), 0) << "abandoned work ran anyway";
}

TEST_F(GameThreadCallTest, APostedReleaseRunsOnTheGameThreadAndTheWorkerKeepsNoReference)
{
    // What a pack build does with its temporary asset manager: the last
    // reference moves into the posted work, so the release happens there.
    const std::thread::id gameThread = std::this_thread::get_id();
    auto owned = std::make_shared<int>(1);
    const std::weak_ptr<int> watch = owned;
    std::atomic<bool> released{ false };
    std::thread::id releasedOn;
    std::thread thread([&]
                       { OloEngine::Tasks::PostToGameThread([owned = std::move(owned), &released, &releasedOn]() mutable
                                                            {
            owned.reset();
            releasedOn = std::this_thread::get_id();
            released.store(true, std::memory_order_release); }); });
    thread.join();
    EXPECT_FALSE(released.load()) << "the post ran on the worker";
    EXPECT_FALSE(watch.expired()) << "the queued work must own the reference until it runs";
    ASSERT_TRUE(PumpUntil(released));
    EXPECT_TRUE(watch.expired());
    EXPECT_EQ(releasedOn, gameThread);
}

TEST_F(GameThreadCallTest, TheWorksExceptionReachesTheCaller)
{
    std::string caught;
    std::atomic<bool> done{ false };
    std::thread thread([&]
                       {
        try
        {
            (void)RunOnGameThreadAndWait<int>([]() -> int { throw std::runtime_error("load failed"); });
        }
        catch (const std::runtime_error& e)
        {
            caught = e.what();
        }
        done.store(true, std::memory_order_release); });
    const bool finished = PumpUntil(done);
    thread.join();
    ASSERT_TRUE(finished);
    EXPECT_EQ(caught, "load failed");
}
