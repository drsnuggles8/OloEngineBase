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
// Graph attribution (issue #316 Part 4, "graph-wide command-bucket attribution").
// Every command FrameCaptureManager captures still comes from a SINGLE pass's
// command bucket — SceneRenderPass's — because that pass alone drives the capture
// state machine (it both begins the pending frame at OnPreSort and commits it at
// OnFrameEnd, before the other command-bucket passes run). The frame records which
// pass that was (CapturedFrameData::SourcePassName), so BuildBreakdown attributes
// every captured command to a real render-graph pass rather than a hard-coded
// label. To place that single bucket in the whole-graph picture, the MCP handler
// also gathers a GraphAttribution off the live RenderGraph (every pass, which ones
// own a command bucket, which is the capture source, culled/final/work-type) and
// passes it here. Per-command capture of the OTHER command-bucket passes (water /
// foliage / decal / forward-overlay) requires relocating the capture commit out of
// SceneRenderPass into a central frame-end hook — the unbounded alternative this
// slice defers (tracked as a follow-up); those passes are enumerated here so the
// gap is visible, not silently scoped away.
//
// GraphAttribution is a plain, engine-free input struct (the handler pre-resolves
// every graph enum to a string / bool), so this header still unit-tests against a
// synthetic frame + synthetic graph with no GL / RenderGraph dependency.

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

    // One render-graph pass as seen by the command-attribution view. The MCP
    // handler fills this off the live RenderGraph, pre-resolving WorkType to a
    // string so this header stays free of the RenderGraphPassWorkType enum.
    struct GraphPassInfo
    {
        std::string Name;
        std::string WorkType = "Graphics"; // "Graphics" | "Compute" | "Copy"
        bool UsesCommandBucket = false;    // pass owns a CommandBucket (emits sortable draw commands)
        bool Culled = false;               // unreachable from the final pass this frame
        bool IsFinalPass = false;          // the graph's designated final/output pass
        int ExecutionIndex = -1;           // 0-based position in the topological run order; -1 if not scheduled
    };

    // The whole-graph command-bucket landscape joined to a captured frame. The
    // capture today covers exactly one pass's bucket (CaptureSourcePass); the other
    // command-bucket passes are enumerated so an agent sees which passes emit draw
    // commands and which are not yet per-command attributed.
    struct GraphAttribution
    {
        std::vector<GraphPassInfo> Passes;
        std::string CaptureSourcePass; // graph pass whose bucket the per-command breakdown describes
    };

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
    //
    // `attribution` (optional) is the live-graph command-bucket landscape gathered
    // by the MCP handler. When supplied, a `graphAttribution` block enumerates every
    // graph pass and flags the capture source; when null (e.g. unit tests, or no
    // active graph), only the frame-recorded `sourcePass` is reported.
    [[nodiscard]] inline Json BuildBreakdown(const CapturedFrameData& frame, ViewMode requested, int maxCommands,
                                             const GraphAttribution* attribution = nullptr)
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

        // The pass these captured commands were emitted by. Prefer the name the
        // capture recorded on the frame; fall back to the attribution's source
        // (same value, kept for callers that only populate the attribution).
        std::string sourcePass = frame.SourcePassName;
        if (sourcePass.empty() && attribution != nullptr)
            sourcePass = attribution->CaptureSourcePass;

        Json out;
        out["frameNumber"] = frame.FrameNumber;
        out["timestampSeconds"] = Detail::Round(frame.TimestampSeconds, 3);
        out["sourcePass"] = sourcePass;
        out["bucket"] = sourcePass.empty() ? std::string("scene render command bucket")
                                           : sourcePass + " command bucket";
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

        // Graph-wide command-bucket attribution: place the captured single-pass
        // bucket in the context of the whole render graph (issue #316 Part 4).
        if (attribution != nullptr)
        {
            u32 bucketPassCount = 0;
            Json passes = Json::array();
            for (const auto& p : attribution->Passes)
            {
                if (p.UsesCommandBucket)
                    ++bucketPassCount;
                passes.push_back(Json{ { "name", p.Name },
                                       { "workType", p.WorkType },
                                       { "usesCommandBucket", p.UsesCommandBucket },
                                       { "isCaptureSource", !sourcePass.empty() && p.Name == sourcePass },
                                       { "culled", p.Culled },
                                       { "isFinalPass", p.IsFinalPass },
                                       { "executionIndex", p.ExecutionIndex } });
            }

            Json graph;
            graph["captureSourcePass"] = sourcePass;
            graph["passCount"] = static_cast<u32>(attribution->Passes.size());
            graph["commandBucketPassCount"] = bucketPassCount;
            graph["capturedPassCount"] = sourcePass.empty() ? 0u : 1u;
            graph["passes"] = std::move(passes);
            graph["note"] = "Every pass in the live render graph. 'usesCommandBucket' passes emit sortable draw "
                            "commands; exactly one of them ('isCaptureSource' / captureSourcePass) had its bucket "
                            "captured and broken down per-command above. The other command-bucket passes are "
                            "listed but not yet per-command attributed — full per-pass capture is deferred "
                            "(it needs the capture commit moved out of SceneRenderPass into a frame-end hook). "
                            "'executionIndex' is the topological run order; 'culled' passes were skipped this frame.";
            out["graphAttribution"] = std::move(graph);
        }

        out["note"] = "Per-command breakdown of one render-graph pass's command bucket ('sourcePass'). GPU times "
                      "come from the renderer's double-buffered timer-query pool (previous-frame readback, "
                      "approximate). 'debugName' is the per-command label within that bucket. See "
                      "'graphAttribution' for where this bucket sits in the whole graph and which other passes "
                      "emit commands. Use format:\"markdown\" for the Command Bucket Inspector's full "
                      "sort/state-change/batching analysis.";
        return out;
    }
} // namespace OloEngine::MCP::FrameBreakdown
