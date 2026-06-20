#pragma once

// Pure, engine-light structural breakdown of a captured frame for the
// olo_render_frame_breakdown MCP tool (issue #306 item D, bullet 2: surface the
// CommandPacketDebugger / FrameCaptureManager data over MCP). The MCP handler in
// McpTools.cpp triggers a one-frame capture on the editor main thread, then hands
// the resulting CapturedFrameData here to be turned into the per-command /
// per-pipeline-stage JSON that olo_perf_capture_frame deliberately omits (it only
// returns frame totals + top-K draws by GPU time).
//
// Keeping the JSON shaping in free functions that touch ONLY the already-captured
// data (CapturedFrameData + DrawKey, both deep-copied snapshots) means it
// unit-tests directly against a synthetic frame — the test binary compiles this
// header but deliberately NOT McpTools.cpp (the editor-backed handler) nor
// CommandPacketDebugger.h (which pulls in ImGui). This mirrors the sibling pattern
// of McpRenderExplain.h / McpGoldenCompare.h.
//
// Scope (HANDOVER, "don't balloon the PR"): every command FrameCaptureManager
// captures comes from SceneRenderPass's command bucket (see
// SceneRenderPass.cpp::OnPreSort/OnPostSort/OnPostBatch), so the per-command
// "named-pass" label available here is each command's debugName within that scene
// geometry bucket. Mapping commands to render-graph pass names across the whole
// graph is the unbounded alternative this task explicitly avoids.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Debug/CapturedFrameData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OloEngine::MCP::FrameBreakdown
{
    using Json = nlohmann::json;

    // Which deep-copied command list (pipeline stage) to list.
    enum class ViewMode
    {
        PreSort,  // submission order, before the radix sort
        PostSort, // after the radix sort (execution order, before batching)
        PostBatch // after batching — what actually executed
    };

    // Parse the tool's "viewMode" string. Unknown / empty -> PostBatch (the
    // "what actually ran" default that matches olo_perf_capture_frame).
    [[nodiscard]] inline ViewMode ParseViewMode(std::string_view s)
    {
        if (s == "presort" || s == "preSort" || s == "pre")
            return ViewMode::PreSort;
        if (s == "postsort" || s == "postSort" || s == "sort")
            return ViewMode::PostSort;
        return ViewMode::PostBatch;
    }

    [[nodiscard]] inline const char* ViewModeName(ViewMode m)
    {
        switch (m)
        {
            case ViewMode::PreSort:
                return "presort";
            case ViewMode::PostSort:
                return "postsort";
            case ViewMode::PostBatch:
                return "postbatch";
            default:
                return "postbatch";
        }
    }

    // Pick the command vector for the requested stage, falling back to an earlier,
    // populated stage when the requested one is empty (PostBatch -> PostSort ->
    // PreSort; PostSort -> PreSort). This matches CommandPacketDebugger's own
    // "use post-batch if not empty else post-sort" behaviour so the tool never
    // silently reports an empty list when an earlier stage has the commands. The
    // stage actually returned is written to `usedMode`.
    [[nodiscard]] inline const std::vector<CapturedCommandData>& SelectCommands(const CapturedFrameData& frame, ViewMode requested, ViewMode& usedMode)
    {
        switch (requested)
        {
            case ViewMode::PreSort:
                usedMode = ViewMode::PreSort;
                return frame.PreSortCommands;
            case ViewMode::PostSort:
                if (!frame.PostSortCommands.empty())
                {
                    usedMode = ViewMode::PostSort;
                    return frame.PostSortCommands;
                }
                usedMode = ViewMode::PreSort;
                return frame.PreSortCommands;
            case ViewMode::PostBatch:
            default:
                if (!frame.PostBatchCommands.empty())
                {
                    usedMode = ViewMode::PostBatch;
                    return frame.PostBatchCommands;
                }
                if (!frame.PostSortCommands.empty())
                {
                    usedMode = ViewMode::PostSort;
                    return frame.PostSortCommands;
                }
                usedMode = ViewMode::PreSort;
                return frame.PreSortCommands;
        }
    }

    namespace Detail
    {
        // Round to `places` decimals so the JSON stays readable (raw f64 ms /
        // GPU times would serialise with ~17 digits otherwise).
        [[nodiscard]] inline f64 Round(f64 v, int places)
        {
            const f64 scale = std::pow(10.0, places);
            return std::round(v * scale) / scale;
        }

        // One command -> its structural JSON object. All fields come from the
        // deep-copied CapturedCommandData / its DrawKey, so this is pure.
        [[nodiscard]] inline Json CommandToJson(sizet index, const CapturedCommandData& cmd)
        {
            const DrawKey& key = cmd.GetSortKey();
            return Json{
                { "index", index },
                { "type", cmd.GetCommandTypeString() },
                { "debugName", cmd.GetDebugName() },
                { "isDraw", cmd.IsDrawCommand() },
                { "executionOrder", cmd.GetExecutionOrder() },
                { "groupId", cmd.GetGroupID() },
                { "originalIndex", cmd.GetOriginalIndex() },
                { "static", cmd.IsStatic() },
                { "dependsOnPrevious", cmd.DependsOnPrevious() },
                { "drawKey", Json{ { "raw", key.GetKey() },
                                   { "viewportId", key.GetViewportID() },
                                   { "viewLayer", ToString(key.GetViewLayer()) },
                                   { "renderMode", ToString(key.GetRenderMode()) },
                                   { "shaderId", key.GetShaderID() },
                                   { "materialId", key.GetMaterialID() },
                                   { "depth", key.GetDepth() } } },
                { "gpuMs", Round(cmd.GetGpuTimeMs(), 4) }
            };
        }
    } // namespace Detail

    // Build the structured per-command / per-stage breakdown for one captured
    // frame. `requested` is the stage the caller asked for; `maxCommands` (>= 1)
    // caps the returned command array — the full count and a `truncated` flag are
    // ALWAYS reported, so the cap is never a silent truncation.
    [[nodiscard]] inline Json BuildBreakdown(const CapturedFrameData& frame, ViewMode requested, int maxCommands)
    {
        ViewMode usedMode = requested;
        const std::vector<CapturedCommandData>& commands = SelectCommands(frame, requested, usedMode);

        const sizet total = commands.size();
        const sizet limit = maxCommands < 1 ? total : std::min<sizet>(total, static_cast<sizet>(maxCommands));

        // Histogram of command types over the FULL selected stage (not just the
        // returned slice), so it stays accurate under truncation. std::map keeps
        // the keys ordered for stable output.
        std::map<std::string, u32> histogram;
        for (const auto& cmd : commands)
            ++histogram[cmd.GetCommandTypeString()];

        Json typeHistogram = Json::object();
        for (const auto& [name, count] : histogram)
            typeHistogram[name] = count;

        Json cmdArray = Json::array();
        for (sizet i = 0; i < limit; ++i)
            cmdArray.push_back(Detail::CommandToJson(i, commands[i]));

        const FrameCaptureStats& stats = frame.Stats;

        Json out;
        out["frameNumber"] = frame.FrameNumber;
        out["timestampSeconds"] = Detail::Round(frame.TimestampSeconds, 3);
        out["bucket"] = "ScenePass (scene render command bucket)";
        out["requestedViewMode"] = ViewModeName(requested);
        out["viewMode"] = ViewModeName(usedMode);
        out["stageCounts"] = Json{ { "preSort", frame.PreSortCommands.size() },
                                   { "postSort", frame.PostSortCommands.size() },
                                   { "postBatch", frame.PostBatchCommands.size() } };
        out["stats"] = Json{ { "totalCommands", stats.TotalCommands },
                             { "drawCalls", stats.DrawCalls },
                             { "batchedCommands", stats.BatchedCommands },
                             { "stateChanges", stats.StateChanges },
                             { "shaderBinds", stats.ShaderBinds },
                             { "textureBinds", stats.TextureBinds },
                             { "sortMs", Detail::Round(stats.SortTimeMs, 3) },
                             { "batchMs", Detail::Round(stats.BatchTimeMs, 3) },
                             { "executeMs", Detail::Round(stats.ExecuteTimeMs, 3) },
                             { "totalMs", Detail::Round(stats.TotalFrameTimeMs, 3) } };
        out["commandTypeHistogram"] = std::move(typeHistogram);
        out["commandCount"] = total;
        out["returnedCommands"] = limit;
        out["truncated"] = limit < total;
        out["commands"] = std::move(cmdArray);
        out["note"] = "Commands are captured from SceneRenderPass's command bucket. GPU times come from the "
                      "renderer's double-buffered timer-query pool (previous-frame readback, approximate). "
                      "'debugName' is the per-command pass/label within the scene bucket; use format:\"markdown\" "
                      "for the Command Bucket Inspector's full sort/state-change/batching analysis.";
        return out;
    }
} // namespace OloEngine::MCP::FrameBreakdown
