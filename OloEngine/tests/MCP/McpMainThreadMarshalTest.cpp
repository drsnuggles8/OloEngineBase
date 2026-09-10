// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationMainThreadJob.h"
#include "MCP/McpServer.h"
#include "OloEngine/Task/NamedThreads.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace
{
    using OloEngine::Automation::AutomationMainThreadJob;
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::IAutomationHost;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using OloEngine::Tasks::ENamedThread;
    using OloEngine::Tasks::FNamedThreadManager;
    using Json = OloEngine::MCP::Json;
    using namespace std::chrono_literals;

    class McpMainThreadMarshalTest : public ::testing::Test
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
            m_Server.Stop();
            auto& manager = FNamedThreadManager::Get();
            manager.GetQueue(ENamedThread::GameThread).ProcessAll(true);
            if (m_AttachedGameThread)
                manager.DetachFromThread(ENamedThread::GameThread);
        }

        bool StartServer()
        {
            // Exclusive binds and a process-varying base isolate parallel ctest
            // cases. Stride beyond contiguous Windows excluded-port blocks.
            static int anchor = 0;
            const auto base = static_cast<u32>(reinterpret_cast<std::uintptr_t>(&anchor) % 40000u);
            for (u32 attempt = 0; attempt < 40; ++attempt)
            {
                const auto port = static_cast<u16>(20000u + (base + attempt * 769u) % 40000u);
                if (m_Server.Start(port))
                    return true;
            }
            return false;
        }

        bool WaitForQueuedJob()
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

        static void DrainQueue()
        {
            FNamedThreadManager::Get().GetQueue(ENamedThread::GameThread).ProcessAll(true);
        }

        McpServer m_Server{ EditorMcpContext{} };
        bool m_AttachedGameThread = false;
    };

    TEST_F(McpMainThreadMarshalTest, StoppedServerDoesNotEnqueueAnOperation)
    {
        auto writes = std::make_shared<std::atomic<int>>(0);
        auto result = std::async(std::launch::async, [this, writes]
                                 { return m_Server.MarshalRead([writes]() -> Json
                                                               { ++*writes;
                                                                 return Json::object(); }); });
        EXPECT_THROW((void)result.get(), std::runtime_error);
        EXPECT_FALSE(FNamedThreadManager::Get().GetQueue(ENamedThread::GameThread).HasPendingTasks(true));
        DrainQueue();
        EXPECT_EQ(writes->load(), 0);
    }

    TEST_F(McpMainThreadMarshalTest, TimedOutOperationCannotRunWhenTheRealQueueDrainsLater)
    {
        ASSERT_TRUE(StartServer());
        auto writes = std::make_shared<std::atomic<int>>(0);
        auto result = std::async(std::launch::async, [this, writes]
                                 { return m_Server.MarshalRead([writes]() -> Json
                                                               { ++*writes;
                                                                 return Json{ { "written", true } }; },
                                                               1ms); });
        ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
        EXPECT_THROW((void)result.get(), std::runtime_error);
        EXPECT_TRUE(FNamedThreadManager::Get().GetQueue(ENamedThread::GameThread).HasPendingTasks(true));
        DrainQueue();
        EXPECT_EQ(writes->load(), 0);
    }

    TEST_F(McpMainThreadMarshalTest, ShutdownCancelsAPendingOperationBeforeReturningFailure)
    {
        ASSERT_TRUE(StartServer());
        auto writes = std::make_shared<std::atomic<int>>(0);
        auto result = std::async(std::launch::async, [this, writes]
                                 { return m_Server.MarshalRead([writes]() -> Json
                                                               { ++*writes;
                                                                 return Json::object(); },
                                                               10s); });
        ASSERT_TRUE(WaitForQueuedJob());
        m_Server.Stop();
        ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
        EXPECT_THROW((void)result.get(), std::runtime_error);
        DrainQueue();
        EXPECT_EQ(writes->load(), 0);
    }

    TEST_F(McpMainThreadMarshalTest, MainThreadExceptionsReachTheWaitingCaller)
    {
        ASSERT_TRUE(StartServer());
        auto result = std::async(std::launch::async, [this]
                                 { return m_Server.MarshalRead([]() -> Json
                                                               { throw std::logic_error("scene mutation failed"); },
                                                               10s); });
        ASSERT_TRUE(WaitForQueuedJob());
        DrainQueue();
        EXPECT_THROW((void)result.get(), std::logic_error);
    }

    TEST_F(McpMainThreadMarshalTest, RequestCancellationBeforeQueueDrainSuppressesTheWrite)
    {
        auto writes = std::make_shared<std::atomic<int>>(0);
        ToolDef tool;
        tool.Name = "fake_queued_write";
        tool.ProjectWrite = true;
        tool.Handler = [writes](IAutomationHost& host, const Json&) -> ToolResult
        {
            return ToolResult::Structured(host.MarshalRead([writes]() -> Json
                                                           { ++*writes;
                                                             return Json{ { "written", true } }; },
                                                           10s));
        };
        m_Server.RegisterTool(std::move(tool));
        m_Server.SetAllowWrites(true);
        ASSERT_TRUE(StartServer());
        auto result = std::async(std::launch::async, [this]
                                 { return m_Server.HandleMessage(Json{ { "jsonrpc", "2.0" },
                                                                       { "id", "queued-write" },
                                                                       { "method", "tools/call" },
                                                                       { "params", { { "name", "fake_queued_write" } } } }); });
        ASSERT_TRUE(WaitForQueuedJob());
        (void)m_Server.HandleMessage(Json{ { "jsonrpc", "2.0" },
                                           { "method", "notifications/cancelled" },
                                           { "params", { { "requestId", "queued-write" } } } });
        DrainQueue();
        const Json response = result.get();
        ASSERT_TRUE(response.contains("error")) << response.dump();
        EXPECT_EQ(response["error"]["code"], OloEngine::MCP::kRequestCancelledCode);
        EXPECT_EQ(writes->load(), 0);
    }

    // The ownership race is tested directly against the production primitive:
    // an explicit barrier guarantees execution was claimed before cancellation,
    // without betting on worker scheduling fitting inside a short timeout.
    TEST(McpMainThreadJob, AnAlreadyClaimedOperationReturnsItsActualCompletion)
    {
        std::promise<void> started;
        std::future<void> startedFuture = started.get_future();
        std::promise<void> release;
        std::shared_future<void> releaseFuture = release.get_future().share();
        AutomationMainThreadJob job([&started, releaseFuture]() -> Json
                                    { started.set_value();
                                      releaseFuture.wait();
                                      return Json{ { "written", true } }; });
        auto execution = std::async(std::launch::async, [&job]
                                    { job.Execute(); });
        const bool claimed = startedFuture.wait_for(5s) == std::future_status::ready;
        if (claimed)
        {
            EXPECT_FALSE(job.CancelPending("timeout"));
            EXPECT_EQ(job.WaitFor(0ms), std::future_status::timeout);
        }
        release.set_value();
        execution.get();
        ASSERT_TRUE(claimed);
        EXPECT_EQ(job.Get(), Json({ { "written", true } }));
    }

    TEST(McpMainThreadJob, CancelledQueueEntryReleasesItsCapturedResourcesImmediately)
    {
        auto resource = std::make_shared<int>(42);
        const std::weak_ptr<int> lifetime = resource;
        AutomationMainThreadJob job([resource]() -> Json
                                    { return Json{ { "value", *resource } }; });
        resource.reset();
        ASSERT_FALSE(lifetime.expired());
        EXPECT_TRUE(job.CancelPending("timeout"));
        EXPECT_TRUE(lifetime.expired());
        job.Execute();
        EXPECT_THROW((void)job.Get(), std::runtime_error);
    }
} // namespace
