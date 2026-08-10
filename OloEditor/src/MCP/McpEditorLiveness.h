#pragma once

// Pure, engine-light shaping of the editor liveness signal (issue #607): the
// "is the editor actually running frames?" predicate, the human-readable stall
// diagnosis, and the JSON block olo_perf_snapshot / olo_screenshot /
// olo_input_inject all emit.
//
// Why this exists at all. A minimized editor is a SILENT WRONG ANSWER generator.
// Application::Run guards the entire layer-update / ImGui / render block with
// `if (!m_Minimized)`, so while the window is iconified EditorLayer::OnUpdate never
// runs: the frame counter stops, DrainMcpInputQueue never drains an injected plan,
// and every capture keeps handing back the last frame drawn before the minimize.
// But MarshalRead still works — game-thread tasks are pumped BEFORE that guard — so
// every read tool answers normally, with HTTP 200 and no error. Two screenshots
// either side of a 10 m walk came back byte-identical, which reads as "the camera
// didn't move": the exact opposite of the truth. Diagnosing it the first time took
// an out-of-band IsIconic() P/Invoke and a CPU-time delta.
//
// The predicate is deliberately based on a WALL-CLOCK gap rather than on the frame
// index, because a frame index alone cannot distinguish "stalled" from "slow"
// without a second sample: an agent would have to call twice and reason about the
// interval. MsSinceLastFrame answers it in one call, which is the whole point.
//
// Kept free of every editor/GL/window type (it touches only McpEditorLiveness, a
// plain aggregate) so it unit-tests without a window, the same split as
// McpInputInject.h / McpRenderGraphTopology.h.

#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>

namespace OloEngine::MCP::EditorLiveness
{
    using Json = nlohmann::json;

    // A frame loop is considered PARKED once this long has passed with no completed
    // frame. Even a 5 FPS editor completes one every 200 ms, and the frame-rate cap
    // (Application::m_FramePacer) never idles longer than its own budget, so the
    // threshold sits comfortably above "merely slow" and comfortably below the
    // seconds-long silence an iconified window produces. It is a diagnosis, not a
    // hard gate: the value is reported alongside the verdict so a caller can always
    // apply its own judgement.
    inline constexpr f64 kStallThresholdMs = 750.0;

    // True when the editor is running frames, so a capture is fresh and an injected
    // plan can actually drain. An unavailable signal (headless host, older context)
    // reports false — callers treat that as "unknown" via Available, never as a
    // stall, so nothing degrades in a build that cannot answer.
    [[nodiscard]] inline bool IsTicking(const McpEditorLiveness& liveness) noexcept
    {
        if (!liveness.Available)
            return false;
        if (liveness.Iconified)
            return false;
        // A non-finite / negative gap can only come from a clock that has not been
        // stamped yet (the very first frame). Treat that as ticking rather than
        // inventing a stall on startup.
        if (!std::isfinite(liveness.MsSinceLastFrame))
            return true;
        return liveness.MsSinceLastFrame < kStallThresholdMs;
    }

    // Whether a tool result derived from the rendered frame should be flagged
    // stale. Distinct from !IsTicking only in intent: an unavailable signal is
    // NOT stale (we simply do not know), whereas a known stall is.
    [[nodiscard]] inline bool IsStale(const McpEditorLiveness& liveness) noexcept
    {
        return liveness.Available && !IsTicking(liveness);
    }

    // One sentence naming the cause and the fix, or empty when nothing is wrong.
    // Written to be read by an agent that has just been told its call failed, so it
    // says what to DO, not merely what is true.
    [[nodiscard]] inline std::string StallReason(const McpEditorLiveness& liveness)
    {
        if (!liveness.Available || IsTicking(liveness))
            return {};
        if (liveness.Iconified)
        {
            return "The editor window is MINIMIZED, so its update/render loop is parked: "
                   "Application::Run skips EditorLayer::OnUpdate entirely while iconified, which means the frame "
                   "counter does not advance, the synthetic-input queue is never drained, and every capture "
                   "returns the last frame drawn before the minimize. Restore the window (the run-oloengine "
                   "skill's `driver.ps1 -Action attach` now does this automatically) and retry.";
        }
        return "The editor has not completed a frame in " +
               std::to_string(static_cast<i64>(liveness.MsSinceLastFrame)) +
               " ms, so its update/render loop is stalled or blocked (a modal dialog, a long synchronous load, "
               "or a hung frame). Nothing that depends on frames advancing — synthetic input, fresh captures — "
               "can make progress until it resumes.";
    }

    // The `liveness` block every liveness-aware tool embeds. Returns null when the
    // host cannot answer, so a caller can tell "not ticking" from "unknown".
    [[nodiscard]] inline Json ToJson(const McpEditorLiveness& liveness)
    {
        if (!liveness.Available)
            return Json(nullptr);
        Json j;
        j["ticking"] = IsTicking(liveness);
        j["frameIndex"] = liveness.FrameIndex;
        j["msSinceLastFrame"] = std::isfinite(liveness.MsSinceLastFrame)
                                    ? std::round(liveness.MsSinceLastFrame * 100.0) / 100.0
                                    : 0.0;
        j["iconified"] = liveness.Iconified;
        j["focused"] = liveness.Focused;
        j["visible"] = liveness.Visible;
        j["captureUnready"] = liveness.CaptureUnready;
        if (const std::string reason = StallReason(liveness); !reason.empty())
            j["stallReason"] = reason;
        return j;
    }

    // The reusable outputSchema fragment for that block (issue #607 conventions:
    // every tool that reports liveness describes it identically).
    [[nodiscard]] inline Schema::Node SchemaNode()
    {
        return Schema::Object()
            .Prop("ticking", Schema::Bool().Desc("False when the editor's update/render loop is parked — a minimized "
                                                 "window, or no completed frame for " +
                                                 std::to_string(static_cast<i64>(kStallThresholdMs)) +
                                                 " ms. Injected input cannot drain and captures are stale while false."))
            .Prop("frameIndex", Schema::Int().Min(0).Desc("Monotonic editor frame counter; advances only while frames run."))
            .Prop("msSinceLastFrame", Schema::Number().Min(0).Desc("Wall-clock milliseconds since the last completed editor frame."))
            .Prop("iconified", Schema::Bool().Desc("Window minimized — the update loop is skipped entirely."))
            .Prop("focused", Schema::Bool().Desc("OS keyboard focus. Injection does NOT require it (events are fed into the editor's own stream, not the OS)."))
            .Prop("visible", Schema::Bool())
            .Prop("captureUnready", Schema::Bool().Desc("Last frame skipped scene rendering, or a viewport resize transient is still settling."))
            .Prop("stallReason", Schema::String().Desc("Present only when ticking is false: what is wrong and how to fix it."))
            .Desc("Editor liveness (issue #607). Null when the host cannot report it — which means UNKNOWN, not stalled.");
    }
} // namespace OloEngine::MCP::EditorLiveness
