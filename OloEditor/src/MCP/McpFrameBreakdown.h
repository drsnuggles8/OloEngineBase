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
// Graph attribution + full per-pass capture (issue #316 Part 4 / issue #463).
// FrameCaptureManager now captures EVERY command-bucket pass that runs in the
// frame, not just SceneRenderPass's: each pass (Scene, Water, Foliage, Decal,
// ForwardOverlay) registers itself and snapshots its own bucket, and the commit
// is a single central hook in Renderer3D::EndScene AFTER the whole graph executes
// (relocated out of SceneRenderPass::OnFrameEnd, which used to commit mid-graph
// before the other passes ran). The captured frame therefore carries one
// CapturedPassData per command-bucket pass (CapturedFrameData::Passes); the
// top-level lists remain the SOURCE pass (SourcePassName, the scene pass) for
// backward compatibility. BuildBreakdown emits a `passBreakdowns` array — one
// per-command list per pass, each tagged with its graph pass name. To place those
// buckets in the whole-graph picture, the MCP handler also gathers a
// GraphAttribution off the live RenderGraph (every pass, which own a command
// bucket, which is the capture source, culled/final/work-type) and passes it here;
// when capture covered every command-bucket pass that ran,
// graphAttribution.capturedPassCount == graphAttribution.commandBucketPassCount.
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
#include <set>
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

    // Pick the command vector for the requested stage out of one bucket's three
    // stage lists, falling back to an earlier, populated stage when the requested
    // one is empty (PostBatch -> PostSort -> PreSort; PostSort -> PreSort). This
    // matches CommandPacketDebugger's own "use post-batch if not empty else
    // post-sort" behaviour so the tool never silently reports an empty list when an
    // earlier stage has the commands. The stage actually returned is written to
    // `usedMode`. Used by both the top-level (source-pass) breakdown and each
    // per-pass entry (CapturedPassData has the same three stage lists).
    [[nodiscard]] inline const std::vector<CapturedCommandData>& SelectStage(
        const std::vector<CapturedCommandData>& preSort,
        const std::vector<CapturedCommandData>& postSort,
        const std::vector<CapturedCommandData>& postBatch,
        ViewMode requested, ViewMode& usedMode)
    {
        switch (requested)
        {
            case ViewMode::PreSort:
                usedMode = ViewMode::PreSort;
                return preSort;
            case ViewMode::PostSort:
                if (!postSort.empty())
                {
                    usedMode = ViewMode::PostSort;
                    return postSort;
                }
                usedMode = ViewMode::PreSort;
                return preSort;
            case ViewMode::PostBatch:
            default:
                if (!postBatch.empty())
                {
                    usedMode = ViewMode::PostBatch;
                    return postBatch;
                }
                if (!postSort.empty())
                {
                    usedMode = ViewMode::PostSort;
                    return postSort;
                }
                usedMode = ViewMode::PreSort;
                return preSort;
        }
    }

    // Convenience overload selecting from a frame's top-level (source-pass) lists.
    [[nodiscard]] inline const std::vector<CapturedCommandData>& SelectCommands(const CapturedFrameData& frame, ViewMode requested, ViewMode& usedMode)
    {
        return SelectStage(frame.PreSortCommands, frame.PostSortCommands, frame.PostBatchCommands, requested, usedMode);
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

    // The whole-graph command-bucket landscape joined to a captured frame. Every
    // command-bucket pass that ran is now captured per-command (see
    // CapturedFrameData::Passes / `passBreakdowns`); CaptureSourcePass names the one
    // whose bucket is ALSO the top-level view. The full pass list lets the breakdown
    // flag which graph passes were captured and compute capturedPassCount /
    // commandBucketPassCount.
    struct GraphAttribution
    {
        std::vector<GraphPassInfo> Passes;
        std::string CaptureSourcePass; // graph pass whose bucket is the top-level (source) per-command breakdown
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

    // Shape one command bucket's three stage lists into the common per-bucket JSON
    // fields (requestedViewMode, viewMode, stageCounts, commandTypeHistogram,
    // commandCount, returnedCommands, truncated, commands). Shared by the top-level
    // (source-pass) breakdown and every per-pass entry so both views are identical
    // in shape. The histogram covers the FULL selected stage (not just the returned
    // slice) so it stays accurate under truncation; `commandCount` is always the
    // full count and `truncated` flags the cap, so the maxCommands limit is never a
    // silent truncation.
    [[nodiscard]] inline Json ShapeBucket(const std::vector<CapturedCommandData>& preSort,
                                          const std::vector<CapturedCommandData>& postSort,
                                          const std::vector<CapturedCommandData>& postBatch,
                                          ViewMode requested, int maxCommands)
    {
        ViewMode usedMode = requested;
        const std::vector<CapturedCommandData>& commands = SelectStage(preSort, postSort, postBatch, requested, usedMode);

        const sizet total = commands.size();
        const sizet limit = maxCommands < 1 ? total : std::min<sizet>(total, static_cast<sizet>(maxCommands));

        std::map<std::string, u32> histogram;
        for (const auto& cmd : commands)
            ++histogram[cmd.GetCommandTypeString()];

        Json typeHistogram = Json::object();
        for (const auto& [name, count] : histogram)
            typeHistogram[name] = count;

        Json cmdArray = Json::array();
        for (sizet i = 0; i < limit; ++i)
            cmdArray.push_back(Detail::CommandToJson(i, commands[i]));

        Json out;
        out["requestedViewMode"] = ViewModeName(requested);
        out["viewMode"] = ViewModeName(usedMode);
        out["stageCounts"] = Json{ { "preSort", preSort.size() },
                                   { "postSort", postSort.size() },
                                   { "postBatch", postBatch.size() } };
        out["commandTypeHistogram"] = std::move(typeHistogram);
        out["commandCount"] = total;
        out["returnedCommands"] = limit;
        out["truncated"] = limit < total;
        out["commands"] = std::move(cmdArray);
        return out;
    }

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
        // Shape the top-level (source / scene pass) bucket — backward-compatible
        // with the single-pass breakdown.
        Json out = ShapeBucket(frame.PreSortCommands, frame.PostSortCommands, frame.PostBatchCommands,
                               requested, maxCommands);

        const FrameCaptureStats& stats = frame.Stats;

        // The pass these top-level commands were emitted by. Prefer the name the
        // capture recorded on the frame; fall back to the attribution's source
        // (same value, kept for callers that only populate the attribution).
        std::string sourcePass = frame.SourcePassName;
        if (sourcePass.empty() && attribution != nullptr)
            sourcePass = attribution->CaptureSourcePass;

        out["frameNumber"] = frame.FrameNumber;
        out["timestampSeconds"] = Detail::Round(frame.TimestampSeconds, 3);
        out["sourcePass"] = sourcePass;
        out["bucket"] = sourcePass.empty() ? std::string("scene render command bucket")
                                           : sourcePass + " command bucket";
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

        // Per-pass command breakdown (issue #463 / #316 Part 4). One entry per
        // command-bucket pass that executed this frame (Scene, Water, Foliage,
        // Decal, ForwardOverlay), each tagged with its graph pass name and shaped
        // identically to the top-level bucket. The capture is no longer limited to
        // the single scene-pass bucket. Empty for a legacy single-pass capture.
        if (!frame.Passes.empty())
        {
            Json passBreakdowns = Json::array();
            for (const auto& pass : frame.Passes)
            {
                Json entry = ShapeBucket(pass.PreSortCommands, pass.PostSortCommands, pass.PostBatchCommands,
                                         requested, maxCommands);
                entry["name"] = pass.PassName;
                entry["isCaptureSource"] = !sourcePass.empty() && pass.PassName == sourcePass;
                passBreakdowns.push_back(std::move(entry));
            }
            out["passBreakdowns"] = std::move(passBreakdowns);
        }

        // Names of the passes actually captured this frame — used to flag each
        // graph pass below as captured / not, and to size capturedPassCount.
        std::set<std::string> capturedPassNames;
        for (const auto& pass : frame.Passes)
            capturedPassNames.insert(pass.PassName);

        // capturedPassCount is the number of per-pass command captures this frame.
        // Falls back to the legacy single-pass count (0/1) for an old-style frame
        // that has no per-pass entries.
        const u32 capturedPassCount = frame.Passes.empty()
                                          ? (sourcePass.empty() ? 0u : 1u)
                                          : static_cast<u32>(frame.Passes.size());

        // Graph-wide command-bucket attribution: place the captured per-pass
        // buckets in the context of the whole render graph (issue #316 Part 4).
        if (attribution != nullptr)
        {
            // Command-bucket passes that RAN this frame (non-culled). This is the
            // denominator the capture is expected to cover: with full per-pass
            // capture, capturedPassCount == commandBucketPassCount for a frame in
            // which every command-bucket pass executed. Culled passes own a bucket
            // but emit nothing, so they are excluded from the running count.
            u32 bucketPassCount = 0;
            Json passes = Json::array();
            for (const auto& p : attribution->Passes)
            {
                if (p.UsesCommandBucket && !p.Culled)
                    ++bucketPassCount;
                passes.push_back(Json{ { "name", p.Name },
                                       { "workType", p.WorkType },
                                       { "usesCommandBucket", p.UsesCommandBucket },
                                       { "isCaptureSource", !sourcePass.empty() && p.Name == sourcePass },
                                       { "captured", capturedPassNames.contains(p.Name) },
                                       { "culled", p.Culled },
                                       { "isFinalPass", p.IsFinalPass },
                                       { "executionIndex", p.ExecutionIndex } });
            }

            Json graph;
            graph["captureSourcePass"] = sourcePass;
            graph["passCount"] = static_cast<u32>(attribution->Passes.size());
            graph["commandBucketPassCount"] = bucketPassCount;
            graph["capturedPassCount"] = capturedPassCount;
            graph["passes"] = std::move(passes);
            graph["note"] = "Every pass in the live render graph. 'usesCommandBucket' passes emit sortable draw "
                            "commands; each that ran this frame had its bucket captured and broken down per-command "
                            "in 'passBreakdowns' (flagged here with 'captured'). 'isCaptureSource' marks the pass "
                            "whose bucket is ALSO the top-level 'commands'/'stats' view (the scene pass). "
                            "'commandBucketPassCount' counts command-bucket passes that RAN (non-culled); when "
                            "capture covered them all, capturedPassCount == commandBucketPassCount. "
                            "'executionIndex' is the topological run order; 'culled' passes were skipped this frame.";
            out["graphAttribution"] = std::move(graph);
        }

        out["note"] = "Top-level 'commands'/'stats' describe the source pass's bucket ('sourcePass', the scene "
                      "pass). 'passBreakdowns' lists EVERY command-bucket pass that ran this frame (Scene, Water, "
                      "Foliage, Decal, ForwardOverlay) with its own per-command list, each tagged by pass name. "
                      "GPU times come from the renderer's double-buffered timer-query pool (previous-frame "
                      "readback, approximate) and are populated for the source pass only. See 'graphAttribution' "
                      "for where these buckets sit in the whole graph. Use format:\"markdown\" for the Command "
                      "Bucket Inspector's full sort/state-change/batching analysis.";
        return out;
    }
} // namespace OloEngine::MCP::FrameBreakdown
