#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
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
        ToolResult Handle_MemoryReport(McpServer& /*server*/, const Json& /*args*/)
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
            j["suspectedLeakCount"] = static_cast<int>(leaks.size());
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
        ToolResult Handle_PerfSnapshot(McpServer& server, const Json& /*args*/)
        {
            Json j = server.MarshalRead([]() -> Json
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
                o["gpuMs"] = Round2(f.m_GPUTime);
                o["gpuWaitMs"] = Round2(f.m_GPUWaitTime);
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
                return o; });
            return ToolResult::Structured(j);
        }

        // ---- olo_perf_bottlenecks (main-marshaled) -----------------------------
        ToolResult Handle_PerfBottlenecks(McpServer& server, const Json& /*args*/)
        {
            Json j = server.MarshalRead([]() -> Json
                                        {
                const RendererProfiler::BottleneckInfo b = RendererProfiler::GetInstance().AnalyzeBottlenecks();
                Json o;
                o["bottleneck"] = BottleneckTypeName(b.m_Type);
                o["confidence"] = Round2(b.m_Confidence);
                o["detail"] = b.m_Description;
                o["recommendations"] = b.m_Recommendations;
                return o; });
            return ToolResult::Text(j.dump(2));
        }

        // ---- olo_perf_frame_history (main-marshaled; server downsamples) -------
        ToolResult Handle_PerfFrameHistory(McpServer& server, const Json& args)
        {
            int points = 60;
            if (args.contains("points") && args["points"].is_number_integer())
                points = static_cast<int>(std::clamp<long long>(args["points"].get<long long>(), 1, 300));

            Json j = server.MarshalRead([points]() -> Json
                                        {
                const std::vector<RendererProfiler::FrameData> hist = RendererProfiler::GetInstance().GetFrameHistoryCopy();
                Json series = Json::array();
                const std::size_t n = hist.size();
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
            return ToolResult::Text(j.dump(2));
        }

        // ---- olo_perf_capture_frame (main-marshaled) ---------------------------
        ToolResult Handle_PerfCaptureFrame(McpServer& server, const Json& args)
        {
            int topK = 10;
            if (args.contains("topK") && args["topK"].is_number_integer())
                topK = static_cast<int>(std::clamp<long long>(args["topK"].get<long long>(), 1, 50));

            // Trigger a one-frame capture on the game thread and note the capture
            // GENERATION beforehand, so we can detect the new one even when the ring
            // buffer is at capacity (deque size then stays constant as an old frame is
            // evicted for the new one, so a size comparison would never fire and the
            // tool would spuriously time out). The generation increments on every
            // commit. FrameCaptureManager is FMutex-guarded, but marshaling keeps the
            // trigger ordered with the loop.
            const Json trigger = server.MarshalRead([]() -> Json
                                                    {
                FrameCaptureManager& fcm = FrameCaptureManager::GetInstance();
                const auto beforeGen = fcm.GetCaptureGeneration();
                fcm.CaptureNextFrame();
                return Json{ { "beforeGen", beforeGen } }; });
            const auto beforeGen = trigger.value("beforeGen", static_cast<u64>(0));

            // Poll for the freshly captured frame (both accessors are thread-safe).
            std::deque<CapturedFrameData> frames;
            bool captured = false;
            int polls = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (server.IsCurrentCallCancelled())
                    return ToolResult::Error("Cancelled while waiting for the frame capture.");
                if (FrameCaptureManager::GetInstance().GetCaptureGeneration() > beforeGen)
                {
                    frames = FrameCaptureManager::GetInstance().GetCapturedFramesCopy();
                    if (!frames.empty())
                    {
                        captured = true;
                        break;
                    }
                }
                server.EmitProgress(static_cast<f64>(++polls), -1.0, "waiting for the captured frame");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!captured)
                return ToolResult::Error("Frame capture timed out (is the editor rendering the viewport?).");

            const CapturedFrameData& cap = frames.back();
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
                topDraws.push_back(Json{ { "name", draws[i]->GetDebugName() },
                                         { "type", draws[i]->GetCommandTypeString() },
                                         { "gpuMs", Round2(draws[i]->GetGpuTimeMs()) } });
            }
            o["topDrawCalls"] = std::move(topDraws);
            o["note"] = "Captured from the scene render command bucket (post-batch). GPU times come from the "
                        "renderer's timer-query pool. For the per-command / per-stage structural breakdown "
                        "(command list, draw keys, sort/batch analysis), use olo_render_frame_breakdown.";
            return ToolResult::Text(o.dump(2));
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
        ToolResult Handle_PerfPassTimings(McpServer& server, const Json& /*args*/)
        {
            Json j = server.MarshalRead([]() -> Json
                                        {
                const auto& pool = GPUPassTimerPool::GetInstance();

                std::vector<PassTimings::GpuPassEntry> gpuPasses;
                for (const auto& timing : pool.GetLastPassTimingsCopy())
                    gpuPasses.push_back(PassTimings::GpuPassEntry{ timing.Name, timing.GpuMs });

                std::vector<PassTimings::CpuPassEntry> cpuPasses;
                if (const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph())
                {
                    for (const auto& timing : graph->GetLastExecutionTimings())
                        cpuPasses.push_back(PassTimings::CpuPassEntry{ timing.NodeName, timing.CpuMs });
                }

                // See Handle_PerfSnapshot: use the last fully-completed frame
                // so frameTimeMs/cpuMs/gpuWaitMs all describe the same frame.
                const RendererProfiler::FrameData& f = RendererProfiler::GetInstance().GetLastCompletedFrameData();
                PassTimings::FrameTotals totals;
                totals.FrameTimeMs = f.m_FrameTime;
                totals.CpuMs = f.m_CPUTime;
                totals.GpuMs = f.m_GPUTime;
                totals.GpuWaitMs = f.m_GPUWaitTime;
                totals.GpuResultsAgeFrames =
                    (pool.GetLastResolvedFrameNumber() > 0 && pool.GetCurrentFrameNumber() >= pool.GetLastResolvedFrameNumber())
                        ? pool.GetCurrentFrameNumber() - pool.GetLastResolvedFrameNumber()
                        : 0;
                return PassTimings::BuildPassTimings(gpuPasses, cpuPasses, totals); });
            return ToolResult::Structured(j);
        }

        // ---- olo_perf_cpu_scopes (main-marshaled) -------------------------------
        // Per-scope CPU timings collected by PerformanceProfiler (every system in
        // Scene.cpp is wrapped in OLO_PERF_SCOPE / OLO_PERF_SCOPE_AUTO) — the same
        // data the editor's PerformanceLayer CPU Scopes table reads, exposed
        // read-only over MCP (#519, first slice). OLO_PERF_SCOPE compiles to a
        // no-op in Distribution builds, so this reports an explicit "unavailable"
        // status there rather than a misleadingly empty scope list — shaping (incl.
        // that degradation) lives in the pure McpCpuScopes.h so it unit-tests
        // without this TU.
        ToolResult Handle_PerfCpuScopes(McpServer& server, const Json& args)
        {
            u32 limit = 0; // 0 = no limit
            if (args.contains("limit") && args["limit"].is_number_integer())
                limit = static_cast<u32>(std::clamp<long long>(args["limit"].get<long long>(), 1, 1000));

#if defined(OLO_DEBUG) || defined(OLO_RELEASE)
            constexpr bool scopesCompiledIn = true;
#else
            constexpr bool scopesCompiledIn = false;
#endif

            Json j = server.MarshalRead([limit]() -> Json
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

    void RegisterPerfTools(McpServer& server)
    {
        {
            ToolDef tool;
            tool.Name = "olo_memory_report";
            tool.Toolset = "perf";
            tool.Title = "Renderer memory report";
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
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_perf_snapshot";
            tool.Toolset = "perf";
            tool.Title = "Performance snapshot";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Current-frame renderer performance: fps, frame/CPU/GPU time (ms), draw calls, instanced "
                "draw calls, triangles, state/shader/texture binds, and the ACTUAL scene render resolution "
                "(renderWidth/renderHeight — cross-check it against any olo_viewport_set_size override "
                "before trusting timings). Server-computed snapshot from the live profiler. Map a low fps "
                "back to draw calls / lack of instancing.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("fps", Schema::Number())
                                    .Prop("frameTimeMs", Schema::Number())
                                    .Prop("cpuMs", Schema::Number())
                                    .Prop("gpuMs", Schema::Number())
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
                                    .Prop("gpuWaitMs", Schema::Number().Desc("CPU ms spent blocked on the GPU frame fence — high values mean GPU-bound."))
                                    .Prop("renderWidth", Schema::Int().Min(0).Desc("Actual SceneColor render-target width in pixels. Compare against your viewport override to detect a stale/incorrect render size. Omitted when no render graph is live."))
                                    .Prop("renderHeight", Schema::Int().Min(0).Desc("Actual SceneColor render-target height in pixels."))
                                    .Required({ "fps", "frameTimeMs", "cpuMs", "gpuMs", "drawCalls", "triangles" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfSnapshot;
            server.RegisterTool(std::move(tool));
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
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfBottlenecks;
            server.RegisterTool(std::move(tool));
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
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfFrameHistory;
            server.RegisterTool(std::move(tool));
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
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfCaptureFrame;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_perf_pass_timings";
            tool.Toolset = "perf";
            tool.Title = "Per-pass GPU timings";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Whole-frame GPU time split by render-graph pass (Shadow vs Scene vs GTAO vs Bloom vs "
                "ToneMap...). Each pass carries its GPU time (always-on timestamp queries, resolved a few "
                "frames after issue) and its CPU dispatch time from the live graph. ScenePass "
                "additionally reports subPasses splitting its GPU time into DepthPrepass vs Color (no "
                "DepthPrepass sub-entry = depth prepass off; sub-pass times are inside the parent's gpuMs, "
                "not additional). Frame totals include gpuWaitMs (CPU blocked on the GPU fence AND any "
                "SwapBuffers/vsync stall — the direct GPU-bound signal); unattributedGpuMs is frame GPU time "
                "spent between/outside the timed passes. Check gpuResultsStale before trusting the numbers on "
                "very long/GPU-backlogged frames: true means the GPU fell behind far enough that a timestamp "
                "slot was dropped rather than resolved, so gpuMs/passes describe an old, possibly "
                "unrepresentative frame.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("frame", Schema::Object()
                                                       .Prop("frameTimeMs", Schema::Number())
                                                       .Prop("cpuMs", Schema::Number())
                                                       .Prop("gpuMs", Schema::Number())
                                                       .Prop("gpuWaitMs", Schema::Number()))
                                    .Prop("passes", Schema::Array(Schema::Object()
                                                                      .Prop("pass", Schema::String())
                                                                      .Prop("gpuMs", Schema::Number())
                                                                      .Prop("cpuMs", Schema::Number())
                                                                      .Prop("subPasses", Schema::Array(Schema::Object()
                                                                                                           .Prop("name", Schema::String())
                                                                                                           .Prop("gpuMs", Schema::Number()))
                                                                                             .Desc("GPU sub-pass brackets stamped inside this pass (e.g. ScenePass DepthPrepass/Color). Contained in the parent's gpuMs; absent when the pass has no sub-brackets."))))
                                    .Prop("passGpuTotalMs", Schema::Number())
                                    .Prop("unattributedGpuMs", Schema::Number())
                                    .Prop("gpuResultsAgeFrames", Schema::Int().Min(0).Desc("How many frames old the GPU numbers are (results resolve 1-3 frames after issue)."))
                                    .Prop("gpuResultsStale", Schema::Bool().Desc("True when gpuResultsAgeFrames is at or beyond the timer pool's slot count — a slot was dropped rather than resolved, so gpuMs/passes are from a stale frame."))
                                    .Required({ "frame", "passes", "passGpuTotalMs" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfPassTimings;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_perf_cpu_scopes";
            tool.Toolset = "perf";
            tool.Title = "Per-scope CPU timings";
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
            server.RegisterTool(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
