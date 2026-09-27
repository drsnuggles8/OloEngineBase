// OLO_TEST_LAYER: unit
//
// The one-shot capture wait behind olo_render_frame_breakdown and
// olo_perf_capture_frame (issue #1504). A call that stops waiting, on a timeout
// or a cancellation, must withdraw its capture before it returns; otherwise the
// capture stays armed and fires on a later frame, including the first frames of
// a scene opened afterwards. Header-only (MCP/McpFrameCaptureWait.h), driven
// through a host whose "game thread" is the calling thread and which, like
// McpServer, refuses to marshal anything once the call is cancelled.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationHost.h"
#include "MCP/McpFrameCaptureWait.h"
#include "MCP/McpServer.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>

using OloEngine::CaptureState;
using OloEngine::FrameCaptureManager;
using OloEngine::Automation::AutomationArtifact;
using OloEngine::Automation::IAutomationHost;
using OloEngine::MCP::CaptureOneFrame;
using OloEngine::MCP::EditorMcpContext;
using OloEngine::MCP::FrameCaptureWaitOutcome;

namespace
{
    // Runs marshaled jobs inline. `AfterJob` runs after the Nth marshal, to
    // stand in for game-thread frames that happen between the handler's calls.
    class InlineHost final : public IAutomationHost
    {
      public:
        [[nodiscard]] const EditorMcpContext& Context() const override
        {
            return m_Context;
        }

        [[nodiscard]] bool IsCurrentCallCancelled() const override
        {
            return Cancelled.load();
        }

        [[nodiscard]] bool PublishArtifact(AutomationArtifact /*artifact*/) override
        {
            return false;
        }

        std::atomic<bool> Cancelled{ false };
        int MarshalCount = 0;
        std::function<void(int)> AfterJob;
        // Set once the call has armed its capture and started polling.
        mutable std::atomic<bool> Polling{ false };

      protected:
        nlohmann::json MarshalReadOnMainThread(const std::function<nlohmann::json()>& readJob,
                                               std::chrono::milliseconds /*timeout*/) override
        {
            // McpServer::MarshalReadOnMainThread's contract: a cancelled call
            // never reaches the game thread again.
            if (Cancelled.load())
                throw std::runtime_error("Request cancelled before main-thread operation");
            ++MarshalCount;
            nlohmann::json result = readJob();
            if (AfterJob)
                AfterJob(MarshalCount);
            return result;
        }

        void EmitProgressUpdate(f64 /*progress*/, f64 /*total*/, const std::string& /*message*/) const override
        {
            Polling.store(true);
        }

      private:
        EditorMcpContext m_Context;
    };

    class McpFrameCaptureWaitTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            // A commit snapshots the frame-data tables. Reuse a manager an earlier
            // test brought up; only shut down what this fixture initialised.
            m_OwnsFrameData = !OloEngine::FrameDataBufferManager::IsInitialized();
            if (m_OwnsFrameData)
                OloEngine::FrameDataBufferManager::Init();
            Reset();
        }
        void TearDown() override
        {
            Reset();
            if (m_OwnsFrameData)
                OloEngine::FrameDataBufferManager::Shutdown();
        }

        static void Reset()
        {
            auto& mgr = FrameCaptureManager::GetInstance();
            mgr.StopRecording();
            mgr.ClearCaptures();
        }

        static constexpr std::chrono::milliseconds kShortTimeout{ 120 };
        bool m_OwnsFrameData = false;
    };
} // namespace

TEST_F(McpFrameCaptureWaitTest, ATimedOutCaptureIsWithdrawn)
{
    // Nothing renders, so the capture never commits.
    InlineHost host;
    const auto result = CaptureOneFrame(host, kShortTimeout);

    EXPECT_EQ(result.Outcome, FrameCaptureWaitOutcome::TimedOut);
    EXPECT_TRUE(result.Frames.IsEmpty());
    EXPECT_EQ(host.MarshalCount, 1) << "only the arm is marshaled; the withdrawal is a direct call";
    EXPECT_EQ(FrameCaptureManager::GetInstance().GetState(), CaptureState::Idle)
        << "the timed-out capture is still armed and would fire on a later frame";
}

TEST_F(McpFrameCaptureWaitTest, ACallCancelledWhileWaitingStillWithdraws)
{
    // The client cancels after the capture is armed. From then on the host
    // refuses every marshal, so a withdrawal routed through the game thread
    // would never run — the case #1504's first cut got wrong.
    InlineHost host;
    std::atomic<bool> stop{ false };
    std::thread client([&stop, &host]()
                       {
        while (!stop.load() && !host.Polling.load())
            std::this_thread::yield();
        host.Cancelled.store(true); });

    const auto result = CaptureOneFrame(host, std::chrono::seconds(10));
    stop.store(true);
    client.join();

    EXPECT_EQ(result.Outcome, FrameCaptureWaitOutcome::Cancelled);
    EXPECT_NE(result.ErrorMessage().find("Cancelled"), std::string::npos);
    EXPECT_EQ(FrameCaptureManager::GetInstance().GetState(), CaptureState::Idle)
        << "a cancelled call left its capture armed";
}

TEST_F(McpFrameCaptureWaitTest, ACaptureThatLandsIsReturned)
{
    // A stand-in game thread renders the capture frame once the call has armed
    // it and is polling (the arm itself ran inline, so it has fully returned).
    InlineHost host;
    std::atomic<bool> stop{ false };
    std::thread renderer([&stop, &host]()
                         {
        while (!stop.load() && !host.Polling.load())
            std::this_thread::yield();
        if (!stop.load())
            FrameCaptureManager::GetInstance().OnFrameEnd(11, 0.0, 0.0, 0.0); });

    const auto result = CaptureOneFrame(host, std::chrono::seconds(10));
    stop.store(true);
    renderer.join();

    EXPECT_EQ(result.Outcome, FrameCaptureWaitOutcome::Captured);
    ASSERT_FALSE(result.Frames.IsEmpty());
    EXPECT_EQ(result.Frames.Last().FrameNumber, 11u);
    EXPECT_EQ(FrameCaptureManager::GetInstance().GetState(), CaptureState::Idle);
}

TEST_F(McpFrameCaptureWaitTest, AFrameCommittedAfterTheLastPollIsKept)
{
    // The capture commits after the call stopped polling but before it
    // withdraws: the cancel finds nothing to withdraw, and the call returns the
    // frame instead of reporting a timeout. A zero timeout skips the poll loop,
    // so the commit right after the arm is exactly that window.
    InlineHost host;
    host.AfterJob = [](int marshal)
    {
        if (marshal == 1)
            FrameCaptureManager::GetInstance().OnFrameEnd(21, 0.0, 0.0, 0.0);
    };
    const auto result = CaptureOneFrame(host, std::chrono::milliseconds(0));

    EXPECT_EQ(result.Outcome, FrameCaptureWaitOutcome::Captured);
    ASSERT_FALSE(result.Frames.IsEmpty());
    EXPECT_EQ(result.Frames.Last().FrameNumber, 21u);
    EXPECT_EQ(FrameCaptureManager::GetInstance().GetState(), CaptureState::Idle);
}
