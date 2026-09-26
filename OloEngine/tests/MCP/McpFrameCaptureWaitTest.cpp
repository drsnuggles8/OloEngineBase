// OLO_TEST_LAYER: unit
//
// The one-shot capture wait behind olo_render_frame_breakdown and
// olo_perf_capture_frame (issue #1504). A call that stops waiting, on a timeout
// or a cancellation, must withdraw its capture before it returns; otherwise the
// capture stays armed and fires on a later frame, including the first frames of
// a scene opened afterwards. Header-only (MCP/McpFrameCaptureWait.h), driven
// through a host whose "game thread" is the calling thread.

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
    // Runs marshaled jobs inline. `BeforeJob` runs ahead of the Nth marshal, to
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
            return Cancelled;
        }

        [[nodiscard]] bool PublishArtifact(AutomationArtifact /*artifact*/) override
        {
            return false;
        }

        bool Cancelled = false;
        int MarshalCount = 0;
        int ThrowOnMarshal = -1; // 1-based; -1 = never
        std::function<void(int)> BeforeJob;
        // Set once the call has armed its capture and started polling.
        mutable std::atomic<bool> Polling{ false };

      protected:
        nlohmann::json MarshalReadOnMainThread(const std::function<nlohmann::json()>& readJob,
                                               std::chrono::milliseconds /*timeout*/) override
        {
            ++MarshalCount;
            if (MarshalCount == ThrowOnMarshal)
                throw std::runtime_error("game thread did not pick up the job");
            if (BeforeJob)
                BeforeJob(MarshalCount);
            return readJob();
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
    EXPECT_TRUE(result.Disarmed);
    EXPECT_TRUE(result.Frames.IsEmpty());
    EXPECT_EQ(host.MarshalCount, 2) << "one marshal to arm, one to withdraw";
    EXPECT_EQ(FrameCaptureManager::GetInstance().GetState(), CaptureState::Idle)
        << "the timed-out capture is still armed and would fire on a later frame";
}

TEST_F(McpFrameCaptureWaitTest, ACancelledCaptureIsWithdrawn)
{
    InlineHost host;
    host.Cancelled = true;
    const auto result = CaptureOneFrame(host, std::chrono::seconds(10));

    EXPECT_EQ(result.Outcome, FrameCaptureWaitOutcome::Cancelled);
    EXPECT_NE(result.ErrorMessage().find("Cancelled"), std::string::npos);
    EXPECT_EQ(FrameCaptureManager::GetInstance().GetState(), CaptureState::Idle);
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
    EXPECT_EQ(host.MarshalCount, 1) << "a capture that landed has nothing to withdraw";
    EXPECT_EQ(FrameCaptureManager::GetInstance().GetState(), CaptureState::Idle);
}

TEST_F(McpFrameCaptureWaitTest, AFrameCommittedJustBeforeTheWithdrawalIsKept)
{
    // The capture commits on the game thread after the last poll but before the
    // withdrawal job runs: the cancel finds nothing to withdraw, and the call
    // returns the frame instead of reporting a timeout.
    InlineHost host;
    host.BeforeJob = [](int marshal)
    {
        if (marshal == 2)
            FrameCaptureManager::GetInstance().OnFrameEnd(21, 0.0, 0.0, 0.0);
    };
    const auto result = CaptureOneFrame(host, kShortTimeout);

    EXPECT_EQ(result.Outcome, FrameCaptureWaitOutcome::Captured);
    ASSERT_FALSE(result.Frames.IsEmpty());
    EXPECT_EQ(result.Frames.Last().FrameNumber, 21u);
    EXPECT_EQ(FrameCaptureManager::GetInstance().GetState(), CaptureState::Idle);
}

TEST_F(McpFrameCaptureWaitTest, AWithdrawalThatCannotRunIsReported)
{
    InlineHost host;
    host.ThrowOnMarshal = 2;
    const auto result = CaptureOneFrame(host, kShortTimeout);

    EXPECT_EQ(result.Outcome, FrameCaptureWaitOutcome::TimedOut);
    EXPECT_FALSE(result.Disarmed);
    EXPECT_NE(result.ErrorMessage().find("could not be withdrawn"), std::string::npos)
        << "a capture that may still be armed must be reported, not hidden: " << result.ErrorMessage();

    // Clean up the capture the failed withdrawal left armed.
    FrameCaptureManager::GetInstance().CancelCapture();
}
