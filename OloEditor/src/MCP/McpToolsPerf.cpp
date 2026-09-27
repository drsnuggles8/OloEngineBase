#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpEditorLiveness.h"
#include "MCP/McpFrameCaptureWait.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpCpuScopes.h"
#include "MCP/McpPassTimings.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Renderer/Debug/CapturedFrameData.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <algorithm>
#include <string>
#include <vector>

// Performance MCP tools: olo_memory_report and the olo_perf_* family (snapshot,
// bottlenecks, frame history, frame capture, per-pass GPU timings, CPU scopes).
// Split out of the McpTools.cpp monolith (issue #357).

namespace OloEngine::MCP
{
    namespace
    {
        // ---- olo_memory_report (lock-safe) -------------------------------------
        // RendererMemoryTracker is FMutex-guarded, so it reads directly from the
        // handler thread. Server computes the per-type breakdown; raw allocations
        // never leave the process.
        ToolResult Handle_MemoryReport(IAutomationHost& /*host*/, const Json& /*args*/)
        {
            using RT = RendererMemoryTracker::ResourceType;
            static constexpr std::array<std::pair<RT, const char*>, 11> kTypes = { {
                { RT::VertexBuffer, "VertexBuffer" },
                { RT::IndexBuffer, "IndexBuffer" },
                { RT::UniformBuffer, "UniformBuffer" },
                { RT::StorageBuffer, "StorageBuffer" },
                { RT::Texture2D, "Texture2D" },
                { RT::TextureCubemap, "TextureCubemap" },
                { RT::Framebuffer, "Framebuffer" },
                { RT::Shader, "Shader" },
                { RT::RenderTarget, "RenderTarget" },
                { RT::CommandBuffer, "CommandBuffer" },
                { RT::Other, "Other" },
            } };

            const auto toMB = [](sizet bytes)
            { return std::round(static_cast<f64>(bytes) / 1048576.0 * 100.0) / 100.0; };

            auto& tracker = RendererMemoryTracker::GetInstance();
            Json byType = Json::array();
            for (const auto& [type, name] : kTypes)
            {
                const sizet bytes = tracker.GetMemoryUsage(type);
                const u32 count = tracker.GetAllocationCount(type);
                if (bytes == 0 && count == 0)
                    continue;
                byType.push_back(Json{ { "type", name },
                                       { "bytes", static_cast<u64>(bytes) },
                                       { "mb", toMB(bytes) },
                                       { "count", count } });
            }

            const sizet total = tracker.GetTotalMemoryUsage();
            const auto leaks = tracker.DetectLeaks();

            Json j;
            j["totalBytes"] = static_cast<u64>(total);
            j["totalMB"] = toMB(total);
            j["byType"] = std::move(byType);
            j["suspectedLeakCount"] = static_cast<int>(leaks.Num());
            return ToolResult::Structured(j);
        }

        const char* BottleneckTypeName(RendererProfiler::BottleneckInfo::Type type)
        {
            switch (type)
            {
                case RendererProfiler::BottleneckInfo::CPU_Bound:
                    return "CPU";
                case RendererProfiler::BottleneckInfo::GPU_Bound:
                    return "GPU";
                case RendererProfiler::BottleneckInfo::Memory_Bound:
                    return "Memory";
                case RendererProfiler::BottleneckInfo::IO_Bound:
                    return "IO";
                case RendererProfiler::BottleneckInfo::Balanced:
                    return "Balanced";
            }
            return "Unknown";
        }

        // ---- olo_perf_snapshot (main-marshaled; profiler has no mutex) ----------
        ToolResult Handle_PerfSnapshot(IAutomationHost& host, const Json& /*args*/)
        {
            Json j = host.MarshalRead([&host]() -> Json
                                      {
                // GetLastCompletedFrameData(), not GetCurrentFrameData(): the
                // latter's FrameTime is only a live estimate carried over
                // from the previous frame, while CPUTime/GPUTime describe the
                // in-progress frame — mixing them could read as cpuMs >
                // frameTimeMs whenever frame times swing (#519). The
                // "completed" snapshot keeps every field describing the same
                // finished frame, at the cost of up to one frame of latency.
                const RendererProfiler::FrameData& f = RendererProfiler::GetInstance().GetLastCompletedFrameData();
                Json o;
                o["fps"] = f.m_FrameTime > 0.0 ? Round2(1000.0 / f.m_FrameTime) : 0.0;
                o["frameTimeMs"] = Round2(f.m_FrameTime);
                o["cpuMs"] = Round2(f.m_CPUTime);
                // The number AND its validity (#1337). A frame the timer pool
                // could not measure reaches here as null with a reason, never
                // as 0.00 ms of GPU work, which reads as the fastest frame of
                // the session.
                if (f.m_GPUTimeStatus == GpuTimingStatus::Valid)
                {
                    o["gpuMs"] = Round2(f.m_GPUTime);
                }
                else
                {
                    o["gpuMs"] = nullptr;
                }
                o["gpuStatus"] = std::string(ToString(f.m_GPUTimeStatus));
                o["gpuWaitMs"] = Round2(f.m_GPUWaitTime);
                // Split by cause. A fence wait means the GPU is behind. The
                // "present" half is the SwapBuffers span: a real vsync/present
                // wait on OpenGL, but on Vulkan the backend records and submits
                // the frame INSIDE SwapBuffers (#691), so there it contains the
                // frame's render work and is not a pacing signal.
                o["fenceWaitMs"] = Round2(f.m_FenceWaitTime);
                o["presentWaitMs"] = Round2(f.m_PresentWaitTime);
                o["drawCalls"] = f.m_DrawCalls;
                o["instancedDrawCalls"] = f.m_InstancedDrawCalls;
                o["instancesRendered"] = f.m_InstancesRendered;
                o["instancesBatched"] = f.m_InstancesBatched;
                o["triangles"] = f.m_TrianglesRendered;
                o["vertices"] = f.m_VerticesRendered;
                o["stateChanges"] = f.m_StateChanges;
                o["shaderBinds"] = f.m_ShaderBinds;
                o["textureBinds"] = f.m_TextureBinds;
                o["commandPackets"] = f.m_CommandPackets;
                o["sortingMs"] = Round2(f.m_SortingTime);
                o["cullingMs"] = Round2(f.m_CullingTime);
                // The ACTUAL scene render resolution (SceneColor target size), so a
                // reading taken at the wrong resolution is self-evident — e.g. the
                // render graph silently left at window size while a viewport
                // override claims 1920x1080 (#316), or FSR rendering below display
                // res. Omitted when no render graph is live (2D mode / no frame).
                if (const Ref<Framebuffer> sceneFB = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor))
                {
                    const auto& spec = sceneFB->GetSpecification();
                    o["renderWidth"] = spec.Width;
                    o["renderHeight"] = spec.Height;
                }
                // The DISPLAY resolution beside the render one (#1337 criterion
                // 4). They differ whenever the render scale is not 1.0, and a
                // measurement compared across two runs at different scales is
                // not a comparison at all. Both are reported so the reader does
                // not have to infer one from the other.
                if (const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph())
                {
                    o["displayWidth"] = graph->GetPhysicalWidth();
                    o["displayHeight"] = graph->GetPhysicalHeight();
                    o["renderScale"] = Round2(static_cast<f64>(graph->GetRenderScale()));
                }
                // Is the editor actually running frames at all (issue #607)? Every
                // number above describes the last COMPLETED frame, which may be
                // arbitrarily old: a minimized editor parks the loop entirely and each
                // of these counters then reports, perfectly truthfully, on a frame from
                // minutes ago. This block is what makes that visible — and it is here,
                // on the cheapest and most-called tool, so "is the loop ticking?" is one
                // call rather than a cross-tool inference.
                if (host.Context().GetEditorLiveness)
                {
                    const McpEditorLiveness liveness = host.Context().GetEditorLiveness();
                    o["liveness"] = EditorLiveness::ToJson(liveness);
                }
                return o; });
            return ToolResult::Structured(j);
        }

        // ---- olo_perf_bottlenecks (main-marshaled) -----------------------------
        ToolResult Handle_PerfBottlenecks(IAutomationHost& host, const Json& /*args*/)
        {
            Json j = host.MarshalRead([]() -> Json
                                      {
                const RendererProfiler::BottleneckInfo b = RendererProfiler::GetInstance().AnalyzeBottlenecks();
                Json o;
                o["bottleneck"] = BottleneckTypeName(b.m_Type);
                o["confidence"] = Round2(b.m_Confidence);
                o["detail"] = b.m_Description.ToStdString();
                o["recommendations"] = Json::array();
                for (const auto& recommendation : b.m_Recommendations)
                    o["recommendations"].push_back(recommendation.ToStdString());
                return o; });
            return ToolResult::Structured(j);
        }

        // ---- olo_perf_frame_history (main-marshaled; server downsamples) -------
        ToolResult Handle_PerfFrameHistory(IAutomationHost& host, const Json& args)
        {
            int points = 60;
            if (args.contains("points") && args["points"].is_number_integer())
                points = static_cast<int>(std::clamp<long long>(args["points"].get<long long>(), 1, 300));

            Json j = host.MarshalRead([points]() -> Json
                                      {
                const TArray<RendererProfiler::FrameData> hist = RendererProfiler::GetInstance().GetFrameHistoryCopy();
                Json series = Json::array();
                const std::size_t n = static_cast<sizet>(hist.Num());
                if (n > 0)
                {
                    // Ceiling division so we emit at most `points` samples (floor
                    // division would over-stride and return more than requested).
                    const auto p = static_cast<std::size_t>(points);
                    const std::size_t step = std::max<std::size_t>(1, (n + p - 1) / p);
                    for (std::size_t i = 0; i < n; i += step)
                    {
                        const auto& f = hist[i];
                        series.push_back(Json{ { "frameTimeMs", Round2(f.m_FrameTime) },
                                               { "fps", f.m_FrameTime > 0.0 ? Round2(1000.0 / f.m_FrameTime) : 0.0 },
                                               { "drawCalls", f.m_DrawCalls } });
                    }
                }
                return Json{ { "totalFrames", static_cast<u64>(n) },
                             { "returned", static_cast<int>(series.size()) },
                             { "series", std::move(series) } }; });
            return ToolResult::Structured(j);
        }

        // ---- olo_perf_capture_frame (main-marshaled) ---------------------------
        ToolResult Handle_PerfCaptureFrame(IAutomationHost& host, const Json& args)
        {
            int topK = 10;
            if (args.contains("topK") && args["topK"].is_number_integer())
                topK = static_cast<int>(std::clamp<long long>(args["topK"].get<long long>(), 1, 50));

            // Arm a one-frame capture and wait for it; a timeout or a cancel
            // withdraws it again, so it cannot fire on a later frame (#1504).
            const FrameCaptureWaitResult wait = CaptureOneFrame(host);
            if (!wait.Captured())
                return ToolResult::Error(wait.ErrorMessage());

            const CapturedFrameData& cap = wait.Frames.Last();
            Json o;
            o["frameNumber"] = cap.FrameNumber;
            o["stats"] = Json{ { "drawCalls", cap.Stats.DrawCalls },
                               { "totalCommands", cap.Stats.TotalCommands },
                               { "batchedCommands", cap.Stats.BatchedCommands },
                               { "stateChanges", cap.Stats.StateChanges },
                               { "shaderBinds", cap.Stats.ShaderBinds },
                               { "textureBinds", cap.Stats.TextureBinds },
                               { "sortMs", Round2(cap.Stats.SortTimeMs) },
                               { "batchMs", Round2(cap.Stats.BatchTimeMs) },
                               { "executeMs", Round2(cap.Stats.ExecuteTimeMs) },
                               { "totalMs", Round2(cap.Stats.TotalFrameTimeMs) } };

            // Top-K draw commands by GPU time (post-batch = what actually executed).
            std::vector<const CapturedCommandData*> draws;
            for (const auto& cmd : cap.PostBatchCommands)
            {
                if (cmd.IsDrawCommand())
                    draws.push_back(&cmd);
            }
            std::sort(draws.begin(), draws.end(),
                      [](const CapturedCommandData* a, const CapturedCommandData* b)
                      { return a->GetGpuTimeMs() > b->GetGpuTimeMs(); });

            Json topDraws = Json::array();
            for (std::size_t i = 0; i < draws.size() && i < static_cast<std::size_t>(topK); ++i)
            {
                topDraws.push_back(Json{ { "name", draws[i]->GetDebugName().ToStdString() },
                                         { "type", draws[i]->GetCommandTypeString() },
                                         { "gpuMs", Round2(draws[i]->GetGpuTimeMs()) } });
            }
            o["topDrawCalls"] = std::move(topDraws);
            o["note"] = "Captured from the scene render command bucket (post-batch). GPU times come from the "
                        "renderer's timer-query pool. For the per-command / per-stage structural breakdown "
                        "(command list, draw keys, sort/batch analysis), use olo_render_frame_breakdown.";
            return ToolResult::Structured(o);
        }

        // McpPassTimings.h duplicates the pool's slot count as a Jolt/engine-free
        // constant so it stays unit-testable without pulling in GPUPassTimerPool.h;
        // pin the two together here — where both are in scope — so a future ring
        // resize fails the build instead of silently desyncing the staleness flag.
        static_assert(PassTimings::kGpuResultsStaleThreshold == GPUPassTimerPool::kSlotCount,
                      "olo_perf_pass_timings staleness threshold is out of sync with GPUPassTimerPool's slot count");

        // ---- olo_perf_pass_timings (main-marshaled) -----------------------------
        // Per-render-graph-pass GPU/CPU times: GPU from the always-on
        // GPUPassTimerPool (GL_TIMESTAMP pairs around each executed pass, resolved
        // 1-3 frames after issue), CPU from the live graph's last execution
        // timings, frame totals from the profiler. Shaping lives in the pure
        // McpPassTimings.h so it unit-tests without this TU.
        ToolResult Handle_PerfPassTimings(IAutomationHost& host, const Json& /*args*/)
        {
            Json j = host.MarshalRead([]() -> Json
                                      {
                const auto& pool = GPUPassTimerPool::GetInstance();

                // ONE snapshot, so the pass list, the frame identity and the age
                // all describe the same frame. Reading them through separate
                // calls could pair a pass list with an age from the next one.
                const GPUPassTimerPool::FrameTimings gpuFrame = pool.GetLastFrameTimings();

                std::vector<PassTimings::GpuPassEntry> gpuPasses;
                gpuPasses.reserve(static_cast<sizet>(gpuFrame.Passes.Num()));
                for (const auto& timing : gpuFrame.Passes)
                    gpuPasses.push_back(PassTimings::GpuPassEntry{ timing.Name.ToStdString(), timing.Sample,
                                                                   timing.IsSubPass, timing.ParentName.ToStdString() });

                std::vector<PassTimings::CpuPassEntry> cpuPasses;
                if (const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph())
                {
                    for (const auto& timing : graph->GetLastExecutionTimings())
                        cpuPasses.push_back(PassTimings::CpuPassEntry{ timing.NodeName.ToStdString(), timing.CpuMs });
                }

                // See Handle_PerfSnapshot: use the last fully-completed frame
                // so frameTimeMs/cpuMs/gpuWaitMs all describe the same frame.
                const RendererProfiler::FrameData& f = RendererProfiler::GetInstance().GetLastCompletedFrameData();
                PassTimings::FrameTotals totals;
                totals.FrameTimeMs = f.m_FrameTime;
                totals.CpuMs = f.m_CPUTime;
                // The whole-frame GPU time comes from the SAME snapshot as the
                // passes, not from the profiler's copy of it: the profiler's is
                // written at BeginFrame and could be one frame apart from this
                // read (#1337 criterion 4 — the frame IDs have to agree).
                totals.Gpu = gpuFrame.Frame;
                totals.GpuWaitMs = f.m_GPUWaitTime;
                totals.FenceWaitMs = f.m_FenceWaitTime;
                totals.PresentWaitMs = f.m_PresentWaitTime;
                // Parallel command recorder telemetry (#806): the profiler pulled
                // it at that frame's EndFrame(), so it describes the same frame
                // as the totals above.
                const RendererAPI::ParallelRecordingFrameStats& pr = f.m_ParallelRecording;
                totals.ParallelRecording.Regions = pr.Regions;
                totals.ParallelRecording.InlineRegions = pr.InlineRegions;
                totals.ParallelRecording.SecondariesExecuted = pr.SecondariesExecuted;
                totals.ParallelRecording.MergeConflicts = pr.MergeConflicts;
                totals.ParallelRecording.DeclinedGroups = pr.DeclinedGroups;
                totals.ParallelRecording.WorkerRecordMs = pr.WorkerRecordMs;
                totals.ParallelRecording.RegionWallMs = pr.RegionWallMs;
                totals.ParallelRecording.JoinWaitMs = pr.JoinWaitMs;
                for (const auto& region : pr.RegionTimings)
                {
                    std::vector<std::string> itemPassNames;
                    itemPassNames.reserve(static_cast<sizet>(region.ItemPassNames.Num()));
                    for (const auto& name : region.ItemPassNames)
                        itemPassNames.push_back(name.ToStdString());
                    totals.ParallelRecording.RegionTimings.push_back({ region.PassName.ToStdString(), region.Parallel,
                        region.WorkerRecordMs, region.RegionWallMs, region.JoinWaitMs,
                        std::vector<f64>(region.ItemRecordMs.begin(), region.ItemRecordMs.end()), std::move(itemPassNames),
                        region.SelectionSeedMs, region.AttachmentPrepareMs, region.SampledImagePrepareMs, region.PipelineLookupMs,
                        region.FrontendPrepareMs });
                }
                // Async compute queue telemetry (#808), pulled from the same
                // completed-frame record so it describes the same frame.
                const RendererAPI::AsyncComputeFrameStats& ac = f.m_AsyncCompute;
                totals.AsyncCompute.BatchesOnComputeQueue = ac.BatchesOnComputeQueue;
                totals.AsyncCompute.BatchesDeclined = ac.BatchesDeclined;
                totals.AsyncCompute.OwnershipTransfers = ac.OwnershipTransfers;
                totals.AsyncCompute.ComputeSubmits = ac.ComputeSubmits;
                totals.AsyncCompute.DeclineReason = std::string(ac.DeclineReason);
                totals.GpuResultsAgeFrames = gpuFrame.AgeFrames;
                totals.GpuMeasurementFrameId = gpuFrame.FrameNumber;
                totals.CurrentFrameId = gpuFrame.CurrentFrameNumber;
                totals.GpuDroppedSlots = gpuFrame.DroppedSlots;
                totals.GpuUnstampedFrames = gpuFrame.UnstampedFrames;
                return PassTimings::BuildPassTimings(gpuPasses, cpuPasses, totals); });
            return ToolResult::Structured(j);
        }

        // ---- olo_perf_cpu_scopes (main-marshaled) -------------------------------
        // Per-scope CPU timings collected by PerformanceProfiler (every system in
        // Scene.cpp is wrapped in OLO_PERF_SCOPE / OLO_PERF_SCOPE_AUTO) — the same
        // data the editor's PerformanceLayer CPU Scopes table reads, exposed
        // read-only over MCP (#519). OLO_PERF_SCOPE compiles to a
        // no-op in Distribution builds, so this reports an explicit "unavailable"
        // status there rather than a misleadingly empty scope list — shaping (incl.
        // that degradation) lives in the pure McpCpuScopes.h so it unit-tests
        // without this TU.
        ToolResult Handle_PerfCpuScopes(IAutomationHost& host, const Json& args)
        {
            u32 limit = 0; // 0 = no limit
            if (args.contains("limit") && args["limit"].is_number_integer())
                limit = static_cast<u32>(std::clamp<long long>(args["limit"].get<long long>(), 1, 1000));

#if defined(OLO_DEBUG) || defined(OLO_RELEASE)
            constexpr bool scopesCompiledIn = true;
#else
            constexpr bool scopesCompiledIn = false;
#endif

            Json j = host.MarshalRead([limit]() -> Json
                                      {
                std::vector<CpuScopes::ScopeEntry> entries;
                if (Application* app = Application::TryGet())
                {
                    const auto& data = app->GetProfilerPreviousFrameData();
                    entries.reserve(data.size());
                    for (const auto& [name, perFrame] : data)
                        entries.push_back(CpuScopes::ScopeEntry{ name, perFrame.Time, perFrame.Samples });
                }
                return CpuScopes::BuildCpuScopes(entries, scopesCompiledIn, limit); });
            return ToolResult::Structured(j);
        }

    } // namespace

    void RegisterPerfTools(AutomationRegistry& registry)
    {
        {
            ToolDef tool;
            tool.Name = "olo_memory_report";
            tool.Toolset = "perf";
            tool.Title = "Renderer memory report";
            // The per-resource-type breakdown is the editor's own memory table —
            // a human reads it as rows, not as JSON.
            tool.DualAudienceContent = true;
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Renderer GPU/CPU memory usage: total bytes/MB, a per-resource-type breakdown (vertex/index/"
                "uniform/storage buffers, textures, framebuffers, shaders, render targets), and the count of "
                "suspected leaks. Read from the engine's mutex-guarded memory tracker.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("totalBytes", Schema::Int().Min(0).Desc("Total tracked renderer memory, bytes."))
                                    .Prop("totalMB", Schema::Number().Desc("Total tracked renderer memory, MB."))
                                    .Prop("byType", Schema::Array(Schema::Object()
                                                                      .Prop("type", Schema::String())
                                                                      .Prop("bytes", Schema::Int().Min(0))
                                                                      .Prop("mb", Schema::Number())
                                                                      .Prop("count", Schema::Int().Min(0)))
                                                        .Desc("Per-resource-type breakdown; only non-empty types are listed."))
                                    .Prop("suspectedLeakCount", Schema::Int().Min(0).Desc("Number of suspected leaks detected."))
                                    .Required({ "totalBytes", "totalMB", "byType", "suspectedLeakCount" });
            tool.MainMarshaled = false;
            tool.Handler = Handle_MemoryReport;
            registry.Register(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_perf_snapshot";
            tool.Toolset = "perf";
            tool.Title = "Performance snapshot";
            // ~18 frame counters: a human scans them as an aligned table, the
            // model wants the object.
            tool.DualAudienceContent = true;
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Current-frame renderer performance: fps, frame/CPU/GPU time (ms), draw calls, instanced "
                "draw calls, triangles, state/shader/texture binds, and the ACTUAL scene render resolution "
                "(renderWidth/renderHeight — cross-check it against any olo_viewport_set_size override "
                "before trusting timings). Server-computed snapshot from the live profiler. Map a low fps "
                "back to draw calls / lack of instancing.\n\n"
                "ALSO THE LIVENESS PROBE. Every counter here describes the last COMPLETED frame, which can be "
                "arbitrarily old — a minimized editor parks its update/render loop entirely and then reports "
                "these numbers, truthfully, about a frame from minutes ago while input injection silently "
                "never drains and screenshots go stale. The 'liveness' block answers 'is the editor actually "
                "running frames?' in ONE call: check liveness.ticking (with frameIndex, msSinceLastFrame, "
                "iconified, focused) before trusting any other tool's view of the frame.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("fps", Schema::Number())
                                    .Prop("frameTimeMs", Schema::Number())
                                    .Prop("cpuMs", Schema::Number())
                                    .Prop("gpuMs", Schema::NullableNumber().Desc("Null when unmeasured; see gpuStatus. Never 0 for an unmeasured frame."))
                                    .Prop("gpuStatus", Schema::String().Desc("valid|pending|dropped|notStamped|outOfOrder|notTimed|unavailable."))
                                    .Prop("drawCalls", Schema::Int().Min(0))
                                    .Prop("instancedDrawCalls", Schema::Int().Min(0))
                                    .Prop("instancesRendered", Schema::Int().Min(0))
                                    .Prop("instancesBatched", Schema::Int().Min(0))
                                    .Prop("triangles", Schema::Int().Min(0))
                                    .Prop("vertices", Schema::Int().Min(0))
                                    .Prop("stateChanges", Schema::Int().Min(0))
                                    .Prop("shaderBinds", Schema::Int().Min(0))
                                    .Prop("textureBinds", Schema::Int().Min(0))
                                    .Prop("commandPackets", Schema::Int().Min(0))
                                    .Prop("sortingMs", Schema::Number())
                                    .Prop("cullingMs", Schema::Number())
                                    .Prop("gpuWaitMs", Schema::Number().Desc("fenceWaitMs + presentWaitMs."))
                                    .Prop("fenceWaitMs", Schema::Number().Desc("Blocked on the frame fence: the GPU is behind."))
                                    .Prop("presentWaitMs", Schema::Number().Desc("CPU time inside SwapBuffers. OpenGL: the present/vsync wait. Vulkan: the backend renders the frame inside SwapBuffers (#691), so this contains the frame's render work and is NOT a pacing signal; read fenceWaitMs instead."))
                                    .Prop("renderWidth", Schema::Int().Min(0).Desc("Actual SceneColor render-target width. Compare against a viewport override to spot a stale render size. Omitted when no graph is live."))
                                    .Prop("renderHeight", Schema::Int().Min(0).Desc("Actual SceneColor render-target height in pixels."))
                                    .Prop("displayWidth", Schema::Int().Min(0).Desc("Presented framebuffer width; differs from renderWidth when renderScale < 1."))
                                    .Prop("displayHeight", Schema::Int().Min(0))
                                    .Prop("renderScale", Schema::Number().Desc("Render scale in force, [0.25, 1.0]."))
                                    .Prop("liveness", EditorLiveness::SchemaNode())
                                    .Required({ "fps", "frameTimeMs", "cpuMs", "gpuMs", "drawCalls", "triangles" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfSnapshot;
            registry.Register(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_perf_bottlenecks";
            tool.Toolset = "perf";
            tool.Title = "Bottleneck analysis";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "The engine's automatic bottleneck analysis: which of CPU/GPU/Memory/IO is limiting the "
                "frame, a confidence score, a human description, and concrete recommendations.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("bottleneck", Schema::String().Enum({ "CPU", "GPU", "Memory", "IO", "Balanced", "Unknown" }))
                                    .Prop("confidence", Schema::Number().Desc("Diagnosis confidence, 0-1."))
                                    .Prop("detail", Schema::String())
                                    .Prop("recommendations", Schema::Array(Schema::String()))
                                    .Required({ "bottleneck", "confidence", "detail", "recommendations" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfBottlenecks;
            registry.Register(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_perf_frame_history";
            tool.Toolset = "perf";
            tool.Title = "Frame-time history";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "A downsampled time series of recent frames (frameTimeMs, fps, drawCalls) from the profiler's "
                "ring buffer, for spotting spikes/trends. The server downsamples to 'points' samples.";
            tool.InputSchema = Schema::Object()
                                   .Prop("points", Schema::Int().Min(1).Max(300).Desc("Number of downsampled points to return (default 60)."))
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("totalFrames", Schema::Int().Min(0).Desc("Frames in the profiler ring buffer, before downsampling."))
                                    .Prop("returned", Schema::Int().Min(0).Desc("Samples actually emitted after downsampling."))
                                    .Prop("series", Schema::Array(Schema::Object()
                                                                      .Prop("frameTimeMs", Schema::Number())
                                                                      .Prop("fps", Schema::Number())
                                                                      .Prop("drawCalls", Schema::Int().Min(0)))
                                                        .Desc("Downsampled samples, oldest first; empty when no frame history exists yet."))
                                    .Required({ "totalFrames", "returned", "series" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfFrameHistory;
            registry.Register(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_perf_capture_frame";
            tool.Toolset = "perf";
            tool.Title = "Capture frame profile";
            // Triggers a one-frame diagnostic capture but observes-only — no
            // camera/setting/file/scene change the caller can see (see builder note).
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Capture the current frame and return its breakdown: frame totals, render passes, and the "
                "top-K draw calls by GPU time. Per-pass detail requires the editor's frame-capture "
                "instrumentation; frame-level totals are always returned.";
            tool.InputSchema = Schema::Object()
                                   .Prop("topK", Schema::Int().Min(1).Max(50).Desc("How many of the most expensive draw calls to return (default 10)."))
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("frameNumber", Schema::Int().Min(0))
                                    .Prop("stats", Schema::Object()
                                                       .Prop("drawCalls", Schema::Int().Min(0))
                                                       .Prop("totalCommands", Schema::Int().Min(0))
                                                       .Prop("batchedCommands", Schema::Int().Min(0))
                                                       .Prop("stateChanges", Schema::Int().Min(0))
                                                       .Prop("shaderBinds", Schema::Int().Min(0))
                                                       .Prop("textureBinds", Schema::Int().Min(0))
                                                       .Prop("sortMs", Schema::Number())
                                                       .Prop("batchMs", Schema::Number())
                                                       .Prop("executeMs", Schema::Number())
                                                       .Prop("totalMs", Schema::Number()))
                                    .Prop("topDrawCalls", Schema::Array(Schema::Object()
                                                                            .Prop("name", Schema::String())
                                                                            .Prop("type", Schema::String())
                                                                            .Prop("gpuMs", Schema::Number()))
                                                              .Desc("At most topK post-batch draw commands, sorted by GPU time descending."))
                                    .Prop("note", Schema::String().Desc("Fixed provenance note pointing at olo_render_frame_breakdown for the per-command breakdown."))
                                    .Required({ "frameNumber", "stats", "topDrawCalls", "note" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfCaptureFrame;
            registry.Register(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_perf_pass_timings";
            tool.Toolset = "perf";
            tool.Title = "Per-pass GPU timings";
            // A per-pass gpuMs/cpuMs table is THE "where did the frame go" read.
            tool.DualAudienceContent = true;
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Whole-frame GPU time split by render-graph pass (Shadow vs Scene vs GTAO vs Bloom vs "
                "ToneMap...). Each pass carries its GPU time (always-on timestamp queries, resolved a few "
                "frames after issue) and its CPU dispatch time from the live graph. ScenePass "
                "additionally reports subPasses splitting its GPU time into DepthPrepass vs Color (no "
                "DepthPrepass sub-entry = depth prepass off; sub-pass times are inside the parent's gpuMs, "
                "not additional). EVERY gpuMs is null-or-a-number with a gpuStatus beside it (issue #1337): "
                "null means no measurement exists and gpuStatus says which of pending/dropped/notStamped/"
                "outOfOrder/notTimed/unavailable applied — it never means the pass was free. passGpuTotalMs "
                "sums only the measured top-level passes, so check passGpuTotalIsComplete (and "
                "unmeasuredPasses) before treating it as the frame's pass time; unattributedGpuMs is frame "
                "GPU time spent between/outside the timed passes and is null unless the frame span and every "
                "pass were measured. gpuMeasurementFrameId vs currentFrameId say which frame each half "
                "describes. Frame totals split the GPU-bound signal three ways: gpuWaitMs (the sum), "
                "fenceWaitMs (blocked on the frame fence — the GPU is behind) and presentWaitMs (the "
                "SwapBuffers span: a present/vsync wait on OpenGL, but on Vulkan the frame is recorded and "
                "submitted inside SwapBuffers, so there it contains the render work and is not a pacing "
                "signal — use recordingBreakdown.elapsedRecordingWallMs). Check gpuResultsStale before trusting the "
                "numbers on very long/GPU-backlogged frames: true means the GPU fell behind far enough that a "
                "timestamp slot was dropped rather than resolved, so the numbers describe an old, possibly "
                "unrepresentative frame; gpuDroppedSlots counts how many frames were lost that way. "
                "parallelRecording is the parallel command recorder's telemetry "
                "for the same frame (issue #806): regions (RecordParallel calls that forked) vs inlineRegions, "
                "secondariesExecuted, workerRecordMs (summed per-item record time) vs regionWallMs "
                "(fork-to-join wall time); all zero on OpenGL, whose facade default reports nothing. Vulkan reports item and region timings for both inline and parallel execution. "
                "mergeConflicts > 0 means two items transitioned the same subresource "
                "differently - a bug in the pass that forked, never a driver condition.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("frame", Schema::Object()
                                                       .Prop("frameTimeMs", Schema::Number())
                                                       .Prop("cpuMs", Schema::Number())
                                                       .Prop("gpuMs", Schema::NullableNumber().Desc("Whole-frame GPU time, or null when no measurement exists — see gpuStatus. Never 0 for an unmeasured frame (#1337)."))
                                                       .Prop("gpuStatus", Schema::String().Desc("valid | pending | dropped | notStamped | outOfOrder | notTimed | unavailable."))
                                                       .Prop("gpuWaitMs", Schema::Number().Desc("fenceWaitMs + presentWaitMs — the combined GPU-bound signal."))
                                                       .Prop("fenceWaitMs", Schema::Number().Desc("CPU blocked on the frame fence: the GPU is behind."))
                                                       .Prop("presentWaitMs", Schema::Number().Desc("CPU time inside SwapBuffers. A present/vsync wait on OpenGL; on Vulkan it contains the frame's recording and submit (#691), so it is not a pacing signal."))
                                                       .Prop("gpuMeasurementFrameId", Schema::Int().Min(0).Desc("Frame the GPU numbers describe."))
                                                       .Prop("currentFrameId", Schema::Int().Min(0).Desc("Frame the CPU numbers describe; normally 1-3 ahead of gpuMeasurementFrameId.")))
                                    .Prop("parallelRecording", Schema::Object()
                                                                   .Prop("regions", Schema::Int().Min(0).Desc("RecordParallel calls that forked onto task workers this frame."))
                                                                   .Prop("inlineRegions", Schema::Int().Min(0).Desc("RecordParallel calls that ran inline on the render thread (backend unsupported, declined, or fewer than 2 items)."))
                                                                   .Prop("secondariesExecuted", Schema::Int().Min(0).Desc("Secondary command buffers executed into the primary at the join."))
                                                                   .Prop("mergeConflicts", Schema::Int().Min(0).Desc("Subresources two items transitioned differently (ADR 0011 amendment (92) rule 5). Any non-zero value is a bug in the pass that forked."))
                                                                   .Prop("workerRecordMs", Schema::Number().Desc("Sum of per-item recording time, including caller items and inline regions."))
                                                                   .Prop("regionWallMs", Schema::Number().Desc("Sum of fork-to-join wall time on the render thread."))
                                                                   .Prop("joinWaitMs", Schema::Number().Desc("Time after the caller's last item until the parallel loop returned, including scheduler/join bookkeeping."))
                                                                   .Prop("regionTimings", Schema::Array(Schema::Object()
                                                                                                            .Prop("pass", Schema::String())
                                                                                                            .Prop("parallel", Schema::Bool())
                                                                                                            .Prop("workerRecordMs", Schema::Number())
                                                                                                            .Prop("regionWallMs", Schema::Number())
                                                                                                            .Prop("joinWaitMs", Schema::Number())
                                                                                                            .Prop("itemRecordMs", Schema::Array(Schema::Number()))
                                                                                                            .Prop("itemPassNames", Schema::Array(Schema::String()))
                                                                                                            .Prop("selectionSeedMs", Schema::Number())
                                                                                                            .Prop("attachmentPrepareMs", Schema::Number())
                                                                                                            .Prop("sampledImagePrepareMs", Schema::Number())
                                                                                                            .Prop("pipelineLookupMs", Schema::Number())))
                                                                   .Desc("Parallel recorder telemetry for the same frame as `frame`; Vulkan reports both inline and parallel regions in execution order. OpenGL reports zeros."))
                                    .Prop("passes", Schema::Array(Schema::Object()
                                                                      .Prop("pass", Schema::String())
                                                                      .Prop("gpuMs", Schema::NullableNumber().Desc("This pass's GPU time, or null when it was not measured — see gpuStatus."))
                                                                      .Prop("gpuStatus", Schema::String().Desc("valid | pending | dropped | notStamped | outOfOrder | notTimed | unavailable."))
                                                                      .Prop("cpuMs", Schema::Number())
                                                                      .Prop("subPasses", Schema::Array(Schema::Object()
                                                                                                           .Prop("name", Schema::String())
                                                                                                           .Prop("gpuMs", Schema::NullableNumber())
                                                                                                           .Prop("gpuStatus", Schema::String()))
                                                                                             .Desc("GPU sub-pass brackets stamped inside this pass (e.g. ScenePass DepthPrepass/Color). Contained in the parent's gpuMs; absent when the pass has no sub-brackets."))))
                                    .Prop("recordingBreakdown", Schema::Object()
                                                                    .Prop("elapsedRecordingWallMs", Schema::Number().Desc("ELAPSED: fork-to-join wall time on the render thread."))
                                                                    .Prop("summedWorkerCpuMs", Schema::Number().Desc("A SUM across workers, NOT elapsed time. Legitimately exceeds elapsedRecordingWallMs when work ran concurrently."))
                                                                    .Prop("joinWaitMs", Schema::Number().Desc("ELAPSED, inside the wall time: waiting for the last worker."))
                                                                    .Prop("summedCpuPrepareMs", Schema::Number().Desc("A SUM: caller-side setup per region. Zero unless OLO_VK_RECORDING_COSTS=1."))
                                                                    .Prop("fenceWaitMs", Schema::Number().Desc("ELAPSED: CPU blocked on the frame fence."))
                                                                    .Prop("presentWaitMs", Schema::Number().Desc("ELAPSED: CPU time inside SwapBuffers. Vsync on OpenGL; on Vulkan it overlaps elapsedRecordingWallMs because the frame renders inside SwapBuffers."))
                                                                    .Prop("gpuExecutionMs", Schema::NullableNumber().Desc("ELAPSED on the GPU timeline, or null when unmeasured."))
                                                                    .Prop("gpuExecutionStatus", Schema::String())
                                                                    .Prop("note", Schema::String())
                                                                    .Desc("The seven distinct frame measurements, each labelled ELAPSED or SUM so they are not added together (#1337 criterion 2)."))
                                    .Prop("passGpuTotalMs", Schema::Number().Desc("Sum of the MEASURED top-level passes. A lower bound unless passGpuTotalIsComplete."))
                                    .Prop("unmeasuredPasses", Schema::Int().Min(0).Desc("Passes that carried no GPU measurement, so are missing from passGpuTotalMs."))
                                    .Prop("passGpuTotalIsComplete", Schema::Bool().Desc("True when every pass contributed to passGpuTotalMs."))
                                    .Prop("unattributedGpuMs", Schema::NullableNumber().Desc("Frame GPU time outside the timed passes, or null when the frame span or any pass was unmeasured (the subtraction would attribute the missing passes' time to it)."))
                                    .Prop("gpuResultsAgeFrames", Schema::Int().Min(0).Desc("How many frames old the GPU numbers are (results resolve 1-3 frames after issue)."))
                                    .Prop("gpuDroppedSlots", Schema::Int().Min(0).Desc("Timestamp slots discarded since startup because the GPU fell more than a ring behind. Each is a frame never measured."))
                                    .Prop("gpuUnstampedFrames", Schema::Int().Min(0).Desc("Frames the backend declined to stamp at all. A different fault from gpuDroppedSlots: the instrument is not working, rather than the GPU being behind."))
                                    .Prop("gpuResultsStatus", Schema::String().Desc("The frame sample's status, repeated at top level as the one word to branch on."))
                                    .Prop("gpuResultsStale", Schema::Bool().Desc("True when gpuResultsAgeFrames is at or beyond the timer pool's slot count — a slot was dropped rather than resolved, so gpuMs/passes are from a stale frame."))
                                    .Required({ "frame", "passes", "passGpuTotalMs", "parallelRecording" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfPassTimings;
            registry.Register(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_perf_cpu_scopes";
            tool.Toolset = "perf";
            tool.Title = "Per-scope CPU timings";
            // Literally the editor's PerformanceLayer CPU Scopes table — the
            // strongest possible evidence a human wants it back as a table.
            tool.DualAudienceContent = true;
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Per-scope CPU time from PerformanceProfiler — every system in Scene.cpp (and other "
                "OLO_PERF_SCOPE / OLO_PERF_SCOPE_AUTO call sites) reports how long it took last frame, sorted "
                "descending by time. Mirrors the editor's PerformanceLayer CPU Scopes table. OLO_PERF_SCOPE is "
                "compiled out entirely in Distribution builds; this reports status \"unavailable\" there instead "
                "of an empty list. status \"ok_no_data\" means the build supports scope timing but nothing was "
                "recorded last frame (e.g. queried before the first frame completed).";
            tool.InputSchema = Schema::Object()
                                   .Prop("limit", Schema::Int().Min(1).Max(1000).Desc("Max scopes to return, sorted by time descending (default: all)."))
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("status", Schema::String().Enum({ "ok", "ok_no_data", "unavailable" }))
                                    .Prop("note", Schema::String())
                                    .Prop("scopes", Schema::Array(Schema::Object()
                                                                      .Prop("name", Schema::String())
                                                                      .Prop("timeMs", Schema::Number())
                                                                      .Prop("samples", Schema::Int().Min(0))))
                                    .Prop("totalTimeMs", Schema::Number())
                                    .Prop("scopeCount", Schema::Int().Min(0).Desc("Total distinct scopes recorded last frame, before `limit` truncation."))
                                    .Required({ "status", "note", "scopes", "totalTimeMs", "scopeCount" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfCpuScopes;
            registry.Register(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
