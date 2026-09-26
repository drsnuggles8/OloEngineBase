#pragma once

// The one-shot frame capture that olo_render_frame_breakdown and
// olo_perf_capture_frame wait on (issue #1504).
//
// Both tools arm FrameCaptureManager on the game thread and poll for the
// committed frame. Before this helper each did that by hand, and neither
// disarmed on a timeout or a cancellation, so the capture stayed armed and
// fired on whatever frame came next, including the first frames of a scene
// opened afterwards. The rule this helper owns: a call that stops waiting
// withdraws its capture, on the game thread, before it returns.

#include "Automation/AutomationHost.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Renderer/Debug/CapturedFrameData.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <exception>
#include <string>
#include <thread>

namespace OloEngine::MCP
{
    enum class FrameCaptureWaitOutcome : u8
    {
        Captured,
        TimedOut,
        Cancelled
    };

    struct FrameCaptureWaitResult
    {
        FrameCaptureWaitOutcome Outcome = FrameCaptureWaitOutcome::TimedOut;
        // The captured-frame ring at the moment the capture landed; the new frame
        // is Frames.Last(). Empty unless Outcome == Captured.
        TArray<CapturedFrameData> Frames;
        // False only when the withdrawal itself could not run (the game thread did
        // not pick up the marshaled job), so the capture may still be armed.
        bool Disarmed = true;

        [[nodiscard]] bool Captured() const
        {
            return Outcome == FrameCaptureWaitOutcome::Captured;
        }

        // The tool error for a capture that did not land.
        [[nodiscard]] std::string ErrorMessage() const
        {
            std::string message = Outcome == FrameCaptureWaitOutcome::Cancelled
                                      ? "Cancelled while waiting for the frame capture."
                                      : "Frame capture timed out (is the editor rendering the viewport?).";
            if (!Disarmed)
                message += " The pending capture could not be withdrawn (the game thread did not respond); "
                           "it may fire on a later frame.";
            return message;
        }
    };

    // Arms a one-shot capture and waits up to `timeout` for it to commit. A new
    // capture is detected by the capture generation, not the ring size: at
    // capacity a commit evicts the oldest frame and the size does not change.
    inline FrameCaptureWaitResult CaptureOneFrame(Automation::IAutomationHost& host,
                                                  std::chrono::milliseconds timeout = std::chrono::seconds(3))
    {
        const nlohmann::json trigger = host.MarshalRead([]() -> nlohmann::json
                                              {
            FrameCaptureManager& fcm = FrameCaptureManager::GetInstance();
            const auto beforeGen = fcm.GetCaptureGeneration();
            fcm.CaptureNextFrame();
            return nlohmann::json{ { "beforeGen", beforeGen } }; });
        const auto beforeGen = trigger.value("beforeGen", static_cast<u64>(0));

        FrameCaptureWaitResult result;
        const auto takeFramesIfCommitted = [&result, beforeGen]()
        {
            if (FrameCaptureManager::GetInstance().GetCaptureGeneration() <= beforeGen)
                return false;
            result.Frames = FrameCaptureManager::GetInstance().GetCapturedFramesCopy();
            return !result.Frames.IsEmpty();
        };

        int polls = 0;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (host.IsCurrentCallCancelled())
            {
                result.Outcome = FrameCaptureWaitOutcome::Cancelled;
                break;
            }
            if (takeFramesIfCommitted())
            {
                result.Outcome = FrameCaptureWaitOutcome::Captured;
                return result;
            }
            host.EmitProgress(static_cast<f64>(++polls), -1.0, "waiting for the captured frame");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // Stopped waiting: withdraw the capture on the game thread, which owns the
        // pending frame. If it committed in the meantime (the cancel finds nothing
        // to withdraw) and the caller was not cancelled, the frame is still good.
        try
        {
            const nlohmann::json cancel = host.MarshalRead([]() -> nlohmann::json
                                                 { return nlohmann::json{ { "withdrawn", FrameCaptureManager::GetInstance().CancelCapture() } }; });
            if (!cancel.value("withdrawn", false) && result.Outcome == FrameCaptureWaitOutcome::TimedOut &&
                takeFramesIfCommitted())
                result.Outcome = FrameCaptureWaitOutcome::Captured;
        }
        catch (const std::exception&)
        {
            result.Disarmed = false;
        }
        if (!result.Captured())
            result.Frames.Empty();
        return result;
    }
} // namespace OloEngine::MCP
