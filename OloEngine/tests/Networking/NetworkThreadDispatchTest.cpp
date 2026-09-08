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

// The dispatched task can outlive this frame. Both waits below are bounded, and on a
// loaded runner the bound expires before the network thread gets to the task -- the
// assertion then fails, the frame is destroyed, and the task writes into it. ASan
// reported that as a stack-use-after-return here on the hosted arm. So the shared state
// is owned by a shared_ptr captured BY VALUE, and outlives whichever of the two runs last.
namespace
{
    struct FDispatchProbe
    {
        std::atomic<bool> Executed{ false };
        std::atomic<std::thread::id> ExecutionThreadId{};
    };
} // namespace

TEST_F(NetworkThreadDispatchTest, EnqueueNetworkThreadTask)
{
    auto probe = std::make_shared<FDispatchProbe>();

    // ExecutionThreadId is published BEFORE Executed: the wait below keys on Executed
    // and then reads ExecutionThreadId, so the other order lets it read an unwritten id.
    EnqueueNetworkThreadTask([probe]()
                             {
        probe->ExecutionThreadId.store(std::this_thread::get_id(), std::memory_order_release);
        probe->Executed.store(true, std::memory_order_release); }, "TestTask");

    // Wait for the network thread to process the task (up to 2 seconds)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!probe->Executed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(probe->Executed.load(std::memory_order_acquire))
        << "Task was not executed on the network thread within timeout";

    // Verify the task ran on a different thread (the network thread, not the test/game thread)
    EXPECT_NE(probe->ExecutionThreadId.load(std::memory_order_acquire), std::this_thread::get_id())
        << "Task should have executed on the network thread, not the calling thread";
}

TEST_F(NetworkThreadDispatchTest, EnqueueGameThreadFromNetwork)
{
    // Same shared-ownership reason as the test above, one level deeper: the outer task
    // runs on the network thread and the inner one on the game thread, and the only
    // bound on either is the fixed sleep below.
    auto probe = std::make_shared<FDispatchProbe>();

    // From the network thread, dispatch a callback to the game thread
    EnqueueNetworkThreadTask([probe]()
                             { EnqueueGameThreadTask([probe]()
                                                     { probe->Executed.store(true, std::memory_order_release); }, "GameThreadCallback"); }, "NetworkToGameBridge");

    // Give the network thread time to enqueue the game-thread callback
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Process pending game-thread tasks (simulates what Application::Run does each frame)
    auto& queue = FNamedThreadManager::Get().GetQueue(ENamedThread::GameThread);
    queue.ProcessAll(true);

    EXPECT_TRUE(probe->Executed.load(std::memory_order_acquire))
        << "Game-thread callback dispatched from NetworkThread was not executed during ProcessTasks";
}
