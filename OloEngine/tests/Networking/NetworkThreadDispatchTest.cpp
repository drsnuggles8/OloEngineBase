#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Networking/Core/NetworkManager.h"

using namespace OloEngine;
using namespace OloEngine::Tasks;

// NetworkThread dispatch tests require a live NetworkManager (which starts
// the network thread and attaches it to the named thread system).

class NetworkThreadDispatchTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Attach the game thread so we can process tasks dispatched back to it
        FNamedThreadManager::Get().AttachToThread(ENamedThread::GameThread);

        ASSERT_TRUE(NetworkManager::Init());
    }

    void TearDown() override
    {
        NetworkManager::Shutdown();
        FNamedThreadManager::Get().DetachFromThread(ENamedThread::GameThread);
    }
};

TEST_F(NetworkThreadDispatchTest, EnqueueNetworkThreadTask)
{
    std::atomic<bool> executed{ false };
    std::atomic<std::thread::id> executionThreadId{};

    EnqueueNetworkThreadTask([&]()
    {
        executed.store(true, std::memory_order_release);
        executionThreadId.store(std::this_thread::get_id(), std::memory_order_release);
    }, "TestTask");

    // Wait for the network thread to process the task (up to 2 seconds)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!executed.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(executed.load(std::memory_order_acquire))
        << "Task was not executed on the network thread within timeout";

    // Verify the task ran on a different thread (the network thread, not the test/game thread)
    EXPECT_NE(executionThreadId.load(std::memory_order_acquire), std::this_thread::get_id())
        << "Task should have executed on the network thread, not the calling thread";
}

TEST_F(NetworkThreadDispatchTest, EnqueueGameThreadFromNetwork)
{
    std::atomic<bool> gameThreadTaskExecuted{ false };

    // From the network thread, dispatch a callback to the game thread
    EnqueueNetworkThreadTask([&]()
    {
        EnqueueGameThreadTask([&]()
        {
            gameThreadTaskExecuted.store(true, std::memory_order_release);
        }, "GameThreadCallback");
    }, "NetworkToGameBridge");

    // Give the network thread time to enqueue the game-thread callback
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Process pending game-thread tasks (simulates what Application::Run does each frame)
    auto& queue = FNamedThreadManager::Get().GetQueue(ENamedThread::GameThread);
    queue.ProcessAll(true);

    EXPECT_TRUE(gameThreadTaskExecuted.load(std::memory_order_acquire))
        << "Game-thread callback dispatched from NetworkThread was not executed during ProcessTasks";
}
