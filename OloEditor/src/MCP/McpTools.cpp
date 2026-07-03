#include "OloEnginePCH.h"
#include "MCP/McpTools.h"
#include "MCP/McpServer.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpScriptApi.h"
#include "MCP/McpFrameBreakdown.h"
#include "MCP/McpGoldenCompare.h"
#include "MCP/McpPassTimings.h"
#include "MCP/McpPhysicsExplain.h"
#include "MCP/McpRenderExplain.h"
#include "MCP/McpRenderGraphTopology.h"
#include "MCP/McpRenderOverrides.h"
#include "MCP/McpRendererSettings.h"
#include "MCP/McpGenericFieldWrite.h"
#include "MCP/McpReloadScript.h"
#include "MCP/McpSceneControl.h"
#include "MCP/McpSetCollisionLayer.h"
#include "MCP/McpShaderReload.h"
#include "MCP/McpEventStream.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Debug/CapturedFrameData.h"
#include "OloEngine/Renderer/Debug/CommandPacketDebugger.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltLayerInterface.h"
#include "OloEngine/Physics3D/PhysicsLayer.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"
#include "OloEngine/Physics3D/SceneQueries.h"
#include "OloEngine/Scripting/ScriptError.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <Jolt/Jolt.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

#include <stb_image/stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OloEngine::MCP
{
    namespace
    {
        // spdlog %l level name -> severity rank, for the olo_log_tail minLevel filter.
        int LogLevelRank(std::string_view level)
        {
            if (level == "trace")
                return 0;
            if (level == "debug")
                return 1;
            if (level == "info")
                return 2;
            if (level == "warning" || level == "warn")
                return 3;
            if (level == "error" || level == "err")
                return 4;
            if (level == "critical" || level == "fatal")
                return 5;
            return 2; // unknown -> treat as info
        }

        // ---- olo_log_tail (lock-safe) ------------------------------------------
        // Wraps Log::GetRecentLogMessages (spdlog's mutex-guarded ring-buffer sink —
        // safe from the handler thread). Parses each line's level + [tag] from the
        // "[time] [level] logger: payload" pattern to support minLevel/tag filtering.
        ToolResult Handle_LogTail(McpServer& /*server*/, const Json& arguments)
        {
            std::size_t count = 50;
            if (arguments.contains("count") && arguments["count"].is_number_integer())
                count = static_cast<std::size_t>(std::clamp<std::int64_t>(arguments["count"].get<std::int64_t>(), 1, 200));

            int minRank = 0;
            if (arguments.contains("minLevel") && arguments["minLevel"].is_string())
                minRank = LogLevelRank(arguments["minLevel"].get<std::string>());

            std::string tagFilter;
            if (arguments.contains("tag") && arguments["tag"].is_string())
                tagFilter = arguments["tag"].get<std::string>();

            const std::vector<std::string> messages = Log::Get().GetRecentLogMessages(0); // all buffered, then filter
            std::vector<std::string> matched;
            for (const auto& message : messages)
            {
                std::string line = message;
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                    line.pop_back();

                // Parse "[HH:MM:SS] [level] logger: payload".
                std::string level = "info";
                std::string payload = line;
                if (!line.empty() && line.front() == '[')
                {
                    if (const auto firstClose = line.find(']'); firstClose != std::string::npos)
                    {
                        if (const auto secondOpen = line.find('[', firstClose + 1); secondOpen != std::string::npos)
                        {
                            if (const auto secondClose = line.find(']', secondOpen + 1); secondClose != std::string::npos)
                            {
                                level = line.substr(secondOpen + 1, secondClose - secondOpen - 1);
                                const auto colon = line.find(": ", secondClose + 1);
                                payload = (colon != std::string::npos) ? line.substr(colon + 2) : line.substr(secondClose + 1);
                            }
                        }
                    }
                }

                if (LogLevelRank(level) < minRank)
                    continue;
                if (!tagFilter.empty())
                {
                    std::string tag;
                    if (!payload.empty() && payload.front() == '[')
                    {
                        if (const auto tagEnd = payload.find(']'); tagEnd != std::string::npos)
                            tag = payload.substr(1, tagEnd - 1);
                    }
                    if (tag != tagFilter)
                        continue;
                }
                matched.push_back(std::move(line));
            }

            if (matched.empty())
                return ToolResult::Text("(no matching log messages)");

            std::string out;
            const std::size_t start = matched.size() > count ? matched.size() - count : 0;
            for (std::size_t i = start; i < matched.size(); ++i)
            {
                out.append(matched[i]);
                out.push_back('\n');
            }
            return ToolResult::Text(out);
        }

        // ---- olo_scene_summary (main-marshaled) --------------------------------
        // Reads the active Scene + EnTT registry, which are NOT thread-safe, so the
        // read is marshaled onto the game thread and returns a consistent snapshot.
        ToolResult Handle_SceneSummary(McpServer& server, const Json& /*arguments*/)
        {
            Json summary = server.MarshalRead([&server]() -> Json
                                              {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                const bool isPlaying = server.Context().IsPlaying && server.Context().IsPlaying();

                j["hasActiveScene"] = static_cast<bool>(scene);
                j["isPlaying"] = isPlaying;
                if (scene)
                {
                    j["name"] = scene->GetName();
                    j["isPaused"] = scene->IsPaused();
                    // Every entity carries an IDComponent, so this view's size is the
                    // entity count without walking individual archetypes.
                    j["entityCount"] = static_cast<std::uint64_t>(scene->GetAllEntitiesWith<IDComponent>().size());
                }
                return j; });

            return ToolResult::Structured(summary);
        }

        // Parse a UUID argument that may arrive as a string (preferred — u64 exceeds
        // JSON's safe integer range) or a number. Returns false on a bad value.
        bool ParseUuid(const Json& value, u64& out)
        {
            if (value.is_string())
            {
                try
                {
                    out = std::stoull(value.get<std::string>());
                    return true;
                }
                catch (...)
                {
                    return false;
                }
            }
            if (value.is_number_unsigned())
            {
                out = value.get<u64>();
                return true;
            }
            if (value.is_number_integer())
            {
                out = static_cast<u64>(value.get<long long>());
                return true;
            }
            return false;
        }

        std::string UuidToString(UUID id)
        {
            return std::to_string(static_cast<u64>(id));
        }

        // ---- olo_scene_get_entity (main-marshaled) -----------------------------
        // Reuses SceneSerializer::SerializeEntity to dump every component of one
        // entity. Returns the component data as YAML text (the serializer already
        // exists and is authoritative) plus structured id/name/hierarchy fields.
        ToolResult Handle_SceneGetEntity(McpServer& server, const Json& args)
        {
            if (!args.contains("id"))
                return ToolResult::Error("Missing required argument 'id' (entity UUID).");
            u64 idValue = 0;
            if (!ParseUuid(args["id"], idValue))
                return ToolResult::Error("Invalid 'id': expected a UUID as a string or number.");

            Json result = server.MarshalRead([&server, idValue]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                {
                    j["error"] = "No active scene";
                    return j;
                }
                const auto entityOpt = scene->TryGetEntityWithUUID(UUID(idValue));
                if (!entityOpt)
                {
                    j["found"] = false;
                    return j;
                }
                Entity entity = *entityOpt;
                j["found"] = true;
                j["id"] = UuidToString(entity.GetUUID());
                j["name"] = entity.HasComponent<TagComponent>() ? entity.GetComponent<TagComponent>().Tag : std::string{};
                if (const UUID parent = entity.GetParentUUID(); static_cast<u64>(parent) != 0)
                    j["parent"] = UuidToString(parent);
                Json children = Json::array();
                for (const UUID child : entity.Children())
                    children.push_back(UuidToString(child));
                j["children"] = std::move(children);

                YAML::Emitter out;
                SceneSerializer::SerializeEntity(out, entity);
                j["componentsYaml"] = std::string(out.c_str());
                return j; });

            if (result.contains("error"))
                return ToolResult::Error(result["error"].get<std::string>());
            if (!result.value("found", false))
                return ToolResult::Error("No entity with that UUID in the active scene.");
            return ToolResult::Structured(result);
        }

        // ---- olo_scene_list_entities (main-marshaled) --------------------------
        // Paginated registry walk (every entity has an IDComponent). Optional
        // substring name filter. Lean entries; drill into olo_scene_get_entity for
        // full component data.
        ToolResult Handle_SceneListEntities(McpServer& server, const Json& args)
        {
            std::string namePattern;
            if (args.contains("namePattern") && args["namePattern"].is_string())
                namePattern = args["namePattern"].get<std::string>();
            int page = 0;
            int pageSize = 50;
            if (args.contains("page") && args["page"].is_number_integer())
                page = static_cast<int>(std::max<long long>(0, args["page"].get<long long>()));
            if (args.contains("pageSize") && args["pageSize"].is_number_integer())
                pageSize = static_cast<int>(std::clamp<long long>(args["pageSize"].get<long long>(), 1, 200));

            Json result = server.MarshalRead([&server, namePattern, page, pageSize]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                {
                    j["error"] = "No active scene";
                    return j;
                }

                std::vector<Entity> matches;
                for (const auto handle : scene->GetAllEntitiesWith<IDComponent>())
                {
                    Entity entity{ handle, scene.get() };
                    if (!namePattern.empty())
                    {
                        const std::string name = entity.HasComponent<TagComponent>()
                                                     ? entity.GetComponent<TagComponent>().Tag
                                                     : std::string{};
                        if (name.find(namePattern) == std::string::npos)
                            continue;
                    }
                    matches.push_back(entity);
                }

                const auto total = static_cast<int>(matches.size());
                // 64-bit to avoid int overflow when a large page is requested.
                const long long start = static_cast<long long>(page) * pageSize;
                Json entities = Json::array();
                for (long long i = start; i < total && i < start + pageSize; ++i)
                {
                    Entity entity = matches[static_cast<sizet>(i)];
                    Json e;
                    e["id"] = UuidToString(entity.GetUUID());
                    e["name"] = entity.HasComponent<TagComponent>() ? entity.GetComponent<TagComponent>().Tag : std::string{};
                    if (const UUID parent = entity.GetParentUUID(); static_cast<u64>(parent) != 0)
                        e["parent"] = UuidToString(parent);
                    e["childCount"] = static_cast<int>(entity.Children().size());
                    entities.push_back(std::move(e));
                }

                j["total"] = total;
                j["page"] = page;
                j["pageSize"] = pageSize;
                j["returned"] = static_cast<int>(entities.size());
                if (start + pageSize < total)
                    j["nextPage"] = page + 1;
                j["entities"] = std::move(entities);
                return j; });

            if (result.contains("error"))
                return ToolResult::Error(result["error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

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

        // ---- Perf / shader helpers ---------------------------------------------
        f64 Round2(f64 v)
        {
            return std::round(v * 100.0) / 100.0;
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

        const char* ShaderStageName(ShaderDebugger::ShaderStage stage)
        {
            switch (stage)
            {
                case ShaderDebugger::ShaderStage::Vertex:
                    return "vertex";
                case ShaderDebugger::ShaderStage::Fragment:
                    return "fragment";
                case ShaderDebugger::ShaderStage::Geometry:
                    return "geometry";
                case ShaderDebugger::ShaderStage::Compute:
                    return "compute";
            }
            return "unknown";
        }

        // ---- olo_perf_snapshot (main-marshaled; profiler has no mutex) ----------
        ToolResult Handle_PerfSnapshot(McpServer& server, const Json& /*args*/)
        {
            Json j = server.MarshalRead([]() -> Json
                                        {
                const RendererProfiler::FrameData& f = RendererProfiler::GetInstance().GetCurrentFrameData();
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

            // Trigger a one-frame capture on the game thread and note how many frames
            // were already retained, so we can detect the new one. FrameCaptureManager
            // is FMutex-guarded, but marshaling keeps the trigger ordered with the loop.
            const Json trigger = server.MarshalRead([]() -> Json
                                                    {
                FrameCaptureManager& fcm = FrameCaptureManager::GetInstance();
                const auto before = static_cast<u64>(fcm.GetCapturedFramesCopy().size());
                fcm.CaptureNextFrame();
                return Json{ { "before", before } }; });
            const auto before = trigger.value("before", static_cast<u64>(0));

            // Poll for the freshly captured frame (GetCapturedFramesCopy is thread-safe).
            std::deque<CapturedFrameData> frames;
            bool captured = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline)
            {
                frames = FrameCaptureManager::GetInstance().GetCapturedFramesCopy();
                if (static_cast<u64>(frames.size()) > before && !frames.empty())
                {
                    captured = true;
                    break;
                }
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

                const RendererProfiler::FrameData& f = RendererProfiler::GetInstance().GetCurrentFrameData();
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

        // Defined further down (next to the topology export handler that shares it);
        // forward-declared so the frame-breakdown handler can reuse the same
        // enum -> string mapping the topology export uses.
        const char* PassWorkTypeName(RenderGraphPassWorkType type);

        // ---- olo_render_frame_breakdown (main-marshaled) -----------------------
        // The per-command / per-pipeline-stage structural view olo_perf_capture_frame
        // omits. Same capture-then-poll trigger as Handle_PerfCaptureFrame, then the
        // freshly captured frame is shaped by the pure FrameBreakdown::BuildBreakdown
        // (JSON) or CommandPacketDebugger::BuildMarkdownReport (the Command Bucket
        // Inspector's LLM-analysis report). After capture, the live render graph is
        // read once more to attribute the captured bucket to its owning pass and place
        // it in the whole-graph command-bucket landscape (#316 Part 4). Pure read — no
        // override / mutation.
        ToolResult Handle_RenderFrameBreakdown(McpServer& server, const Json& args)
        {
            const bool explicitViewMode = args.contains("viewMode");
            const bool explicitMaxCommands = args.contains("maxCommands");

            FrameBreakdown::ViewMode requested = FrameBreakdown::ViewMode::PostBatch;
            if (explicitViewMode && args["viewMode"].is_string())
                requested = FrameBreakdown::ParseViewMode(args["viewMode"].get<std::string>());

            int maxCommands = 200;
            if (explicitMaxCommands && args["maxCommands"].is_number_integer())
                maxCommands = static_cast<int>(std::clamp<long long>(args["maxCommands"].get<long long>(), 1, 5000));

            std::string format = "json";
            if (args.contains("format") && args["format"].is_string())
            {
                format = args["format"].get<std::string>();
                if (format != "json" && format != "markdown")
                    return ToolResult::Error("format must be \"json\" or \"markdown\".");
            }

            // viewMode / maxCommands shape the JSON command list; the markdown report
            // is a fixed document that always covers all stages and every command, so
            // reject them rather than silently ignoring them.
            if (format == "markdown" && (explicitViewMode || explicitMaxCommands))
                return ToolResult::Error("viewMode and maxCommands apply to format:\"json\" only — the markdown "
                                         "report always covers all pipeline stages and every command. Omit them, "
                                         "or use format:\"json\".");

            // Trigger a one-frame capture on the game thread and note how many frames
            // were already retained, so we can detect the new one (identical to
            // Handle_PerfCaptureFrame).
            const Json trigger = server.MarshalRead([]() -> Json
                                                    {
                FrameCaptureManager& fcm = FrameCaptureManager::GetInstance();
                const auto before = static_cast<u64>(fcm.GetCapturedFramesCopy().size());
                fcm.CaptureNextFrame();
                return Json{ { "before", before } }; });
            const auto before = trigger.value("before", static_cast<u64>(0));

            std::deque<CapturedFrameData> frames;
            bool captured = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline)
            {
                frames = FrameCaptureManager::GetInstance().GetCapturedFramesCopy();
                if (static_cast<u64>(frames.size()) > before && !frames.empty())
                {
                    captured = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!captured)
                return ToolResult::Error("Frame capture timed out (is the editor rendering the viewport?).");

            const CapturedFrameData& cap = frames.back();

            if (format == "markdown")
                return ToolResult::Text(CommandPacketDebugger::BuildMarkdownReport(cap));

            // Gather the live render graph's command-bucket landscape so the
            // captured single-pass bucket can be placed in the whole-graph picture
            // (#316 Part 4). The graph is main-thread state, so the walk runs inside
            // MarshalRead; the shaping stays in the pure FrameBreakdown core. A
            // missing graph (2D mode / no frame yet) just omits the attribution.
            FrameBreakdown::GraphAttribution attribution;
            attribution.CaptureSourcePass = cap.SourcePassName;
            const Json gathered = server.MarshalRead([&attribution]() -> Json
                                                     {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "haveGraph", false } };

                const std::vector<std::string>& order = graph->GetExecutionOrder();
                std::unordered_map<std::string, int> executionIndex;
                executionIndex.reserve(order.size());
                for (int i = 0; i < static_cast<int>(order.size()); ++i)
                    executionIndex.emplace(order[i], i);

                const auto& culled = graph->GetCulledPasses();
                const std::unordered_set<std::string> culledSet(culled.begin(), culled.end());
                const std::string& finalPass = graph->GetFinalPassName();

                for (const auto& info : graph->GetNodeSubmissionInfo())
                {
                    FrameBreakdown::GraphPassInfo pass;
                    pass.Name = info.NodeName;
                    pass.WorkType = PassWorkTypeName(info.WorkType);
                    // A pass owns a command bucket iff it is a CommandBufferRenderPass
                    // (Ref::As uses dynamic_cast, so this is an exact type test).
                    pass.UsesCommandBucket = graph->GetNode<CommandBufferRenderPass>(info.NodeName) != nullptr;
                    pass.Culled = culledSet.contains(info.NodeName);
                    pass.IsFinalPass = !finalPass.empty() && info.NodeName == finalPass;
                    if (const auto it = executionIndex.find(info.NodeName); it != executionIndex.end())
                        pass.ExecutionIndex = it->second;
                    attribution.Passes.push_back(std::move(pass));
                }
                return Json{ { "haveGraph", true } }; });

            const bool haveGraph = gathered.is_object() && gathered.value("haveGraph", false);
            const Json o = FrameBreakdown::BuildBreakdown(cap, requested, maxCommands,
                                                          haveGraph ? &attribution : nullptr);
            return ToolResult::Text(o.dump(2));
        }

        // ---- olo_shader_errors (main-marshaled; GetAllShaders is unguarded) ----
        ToolResult Handle_ShaderErrors(McpServer& server, const Json& /*args*/)
        {
            Json j = server.MarshalRead([]() -> Json
                                        {
                const auto& shaders = ShaderDebugger::GetInstance().GetAllShaders();
                Json arr = Json::array();
                for (const auto& [id, info] : shaders)
                {
                    if (!info.m_HasErrors && info.m_LastCompilation.m_Success)
                        continue;
                    arr.push_back(Json{ { "name", info.m_Name },
                                        { "errorMessage", info.m_LastCompilation.m_ErrorMessage } });
                }
                return Json{ { "count", static_cast<int>(arr.size()) }, { "errors", std::move(arr) } }; });
            return ToolResult::Text(j.dump(2));
        }

        // ---- olo_shader_get (main-marshaled) -----------------------------------
        ToolResult Handle_ShaderGet(McpServer& server, const Json& args)
        {
            std::string name;
            if (args.contains("name") && args["name"].is_string())
                name = args["name"].get<std::string>();
            bool haveId = false;
            u64 id = 0;
            if (args.contains("id") && args["id"].is_number_integer())
            {
                id = static_cast<u64>(args["id"].get<long long>());
                haveId = true;
            }
            const bool includeGlsl = args.contains("includeGlsl") && args["includeGlsl"].is_boolean() && args["includeGlsl"].get<bool>();
            if (name.empty() && !haveId)
                return ToolResult::Error("Provide a shader 'name' or numeric 'id'.");

            const Json result = server.MarshalRead([&name, haveId, id, includeGlsl]() -> Json
                                                   {
                const auto& shaders = ShaderDebugger::GetInstance().GetAllShaders();
                const ShaderDebugger::ShaderInfo* found = nullptr;
                for (const auto& [sid, info] : shaders)
                {
                    if (haveId ? (sid == static_cast<u32>(id)) : (info.m_Name == name))
                    {
                        found = &info;
                        break;
                    }
                }
                if (found == nullptr)
                    return Json{ { "__error", "Shader not found." } };

                Json o;
                o["name"] = found->m_Name;
                o["filePath"] = found->m_FilePath;
                o["hasErrors"] = found->m_HasErrors;
                o["instructionCount"] = found->m_LastCompilation.m_InstructionCount;
                o["compileTimeMs"] = Round2(found->m_LastCompilation.m_CompileTimeMs);
                o["reloadCount"] = static_cast<int>(found->m_ReloadHistory.size());

                Json ubos = Json::array();
                for (const auto& b : found->m_UniformBuffers)
                    ubos.push_back(Json{ { "name", b.m_Name }, { "binding", b.m_Binding }, { "size", b.m_Size }, { "members", b.m_Members } });
                o["uniformBuffers"] = std::move(ubos);

                Json samplers = Json::array();
                for (const auto& s : found->m_Samplers)
                    samplers.push_back(Json{ { "name", s.m_Name }, { "binding", s.m_Binding }, { "type", s.m_Type } });
                o["samplers"] = std::move(samplers);

                Json uniforms = Json::array();
                for (const auto& u : found->m_Uniforms)
                    uniforms.push_back(Json{ { "name", u.m_Name }, { "location", u.m_Location }, { "size", u.m_Size } });
                o["uniforms"] = std::move(uniforms);

                if (includeGlsl)
                {
                    Json glsl = Json::object();
                    for (const auto& [stage, source] : found->m_GeneratedGLSL)
                        glsl[ShaderStageName(stage)] = source;
                    o["generatedGlsl"] = std::move(glsl);
                }
                return o; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_shader_list (main-marshaled; GetAllShaders is unguarded) ------
        // Inventory of every registered shader so the agent can discover names/ids
        // to feed olo_shader_get.
        ToolResult Handle_ShaderList(McpServer& server, const Json& /*args*/)
        {
            Json j = server.MarshalRead([]() -> Json
                                        {
                const auto& shaders = ShaderDebugger::GetInstance().GetAllShaders();
                Json arr = Json::array();
                for (const auto& [id, info] : shaders)
                {
                    arr.push_back(Json{ { "id", id },
                                        { "name", info.m_Name },
                                        { "hasErrors", info.m_HasErrors },
                                        { "instructionCount", info.m_LastCompilation.m_InstructionCount } });
                }
                return Json{ { "count", static_cast<int>(arr.size()) }, { "shaders", std::move(arr) } }; });
            return ToolResult::Text(j.dump(2));
        }

        // ---- olo_shader_reload (main-marshaled; recompiles a shader from disk) --
        // The shader inner loop: edit a .glsl -> reload -> read the compile/link
        // log -> screenshot, without restarting the editor. Mirrors the editor's
        // own "Recompile" action (ShaderEditorPanel) which reloads the shader in
        // BOTH the Renderer3D and Renderer2D libraries. Shader::Reload() re-reads
        // the file and recompiles+links synchronously (force-finishing any async
        // link), so the post-reload status is authoritative. GL work is
        // main-thread-only, so it runs inside MarshalRead.
        ToolResult Handle_ShaderReload(McpServer& server, const Json& args)
        {
            std::string name;
            if (args.contains("name") && args["name"].is_string())
                name = args["name"].get<std::string>();
            if (name.empty())
                return ToolResult::Error("Provide a shader 'name' to reload (see olo_shader_list).");

            const Json result = server.MarshalRead([name]() -> Json
                                                   {
                ShaderReload::Result r;
                r.Name = name;

                // Reload in every library that holds the name (matches the editor's
                // Recompile button). The reported status aggregates ALL reloaded
                // copies: r.Ok is true only if every copy is Ready, and the
                // representative used for the status / program-id / log is the
                // first copy that FAILED (so a failure isn't masked by a sibling
                // that linked) — otherwise the first copy.
                Ref<Shader> representative;
                bool allReady = true;
                const auto reloadIn = [&](ShaderLibrary& lib, const char* label)
                {
                    if (!lib.Exists(name))
                        return;
                    Ref<Shader> shader = lib.Get(name);
                    if (!shader)
                        return;
                    shader->Reload();
                    r.Found = true;
                    r.Libraries.emplace_back(label);
                    const bool ready = shader->IsReady();
                    allReady = allReady && ready;
                    if (!representative || (!ready && representative->IsReady()))
                        representative = shader;
                };
                reloadIn(Renderer3D::GetShaderLibrary(), "Renderer3D");
                reloadIn(Renderer2D::GetShaderLibrary(), "Renderer2D");

                if (!r.Found || !representative)
                {
                    // olo_shader_list reports every GL program the shader debugger
                    // knows about (post-process / compute shaders such as GTAO,
                    // SSAO, SSR included), but only shaders owned by the
                    // Renderer3D / Renderer2D shader libraries can be hot-reloaded
                    // by name (the rest are owned by their render pass and the
                    // engine keeps no name->Shader registry for them). List the
                    // reloadable names so the agent can pick a valid one instead of
                    // guessing from olo_shader_list.
                    std::vector<std::string> reloadable = Renderer3D::GetShaderLibrary().GetAllShaderNames();
                    const std::vector<std::string> names2D = Renderer2D::GetShaderLibrary().GetAllShaderNames();
                    reloadable.insert(reloadable.end(), names2D.begin(), names2D.end());
                    std::sort(reloadable.begin(), reloadable.end());
                    reloadable.erase(std::unique(reloadable.begin(), reloadable.end()), reloadable.end());
                    std::string list;
                    for (const auto& reloadableName : reloadable)
                    {
                        if (!list.empty())
                            list += ", ";
                        list += reloadableName;
                    }
                    return Json{ { "__error",
                                   "Shader '" + name + "' is not in a reloadable shader library. Only shaders "
                                   "managed by the Renderer3D / Renderer2D libraries can be hot-reloaded by name "
                                   "(post-process / compute shaders like GTAO, SSAO, SSR are owned by their render "
                                   "pass and are not reloadable). Reloadable shaders: " +
                                       list } };
                }

                // Authoritative, build-independent status (does not rely on the
                // debug-only ShaderDebugger). r.Ok reflects EVERY reloaded copy;
                // the status / program-id / log come from the representative (a
                // failed copy if any failed, else the first copy).
                r.Status = representative->GetCompilationStatus();
                r.Ok = allReady;
                r.RendererId = representative->GetRendererID();

                // Best-effort compile/link log via the same read path as
                // olo_shader_errors (ShaderDebugger, populated in debug builds).
                // Match by name, preferring the entry for the current program id
                // (the id changes across a reload; a failed link resets it to 0).
                const auto& shaders = ShaderDebugger::GetInstance().GetAllShaders();
                const ShaderDebugger::ShaderInfo* best = nullptr;
                for (const auto& [id, info] : shaders)
                {
                    if (info.m_Name != name)
                        continue;
                    if (id == r.RendererId)
                    {
                        best = &info;
                        break;
                    }
                    if (best == nullptr || info.m_HasErrors)
                        best = &info;
                }
                if (best != nullptr)
                    r.Log = best->m_LastCompilation.m_ErrorMessage;

                return ShaderReload::ToJson(r); });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_assets_list (main-marshaled; reads the project asset registry) -
        ToolResult Handle_AssetsList(McpServer& server, const Json& args)
        {
            std::string typeFilter;
            if (args.contains("typeFilter") && args["typeFilter"].is_string())
                typeFilter = args["typeFilter"].get<std::string>();
            int page = 0;
            int pageSize = 50;
            if (args.contains("page") && args["page"].is_number_integer())
                page = static_cast<int>(std::max<long long>(0, args["page"].get<long long>()));
            if (args.contains("pageSize") && args["pageSize"].is_number_integer())
                pageSize = static_cast<int>(std::clamp<long long>(args["pageSize"].get<long long>(), 1, 200));

            const Json result = server.MarshalRead([typeFilter, page, pageSize]() -> Json
                                                   {
                const Ref<AssetManagerBase> mgr = Project::GetAssetManager();
                if (!mgr)
                    return Json{ { "__error", "No active project / asset manager." } };

                std::vector<AssetHandle> handles;
                if (!typeFilter.empty())
                {
                    const AssetType type = AssetUtils::AssetTypeFromString(typeFilter);
                    if (type == AssetType::None)
                        return Json{ { "__error", "Unknown asset type: " + typeFilter } };
                    for (const AssetHandle h : mgr->GetAllAssetsWithType(type))
                        handles.push_back(h);
                }
                else
                {
                    std::unordered_set<u64> seen;
                    constexpr u16 kMaxType = static_cast<u16>(AssetType::CinematicSequence);
                    for (u16 ti = 1; ti <= kMaxType; ++ti)
                    {
                        for (const AssetHandle h : mgr->GetAllAssetsWithType(static_cast<AssetType>(ti)))
                        {
                            if (seen.insert(static_cast<u64>(h)).second)
                                handles.push_back(h);
                        }
                    }
                }

                std::sort(handles.begin(), handles.end(),
                          [](AssetHandle a, AssetHandle b) { return static_cast<u64>(a) < static_cast<u64>(b); });

                const auto total = static_cast<int>(handles.size());
                // 64-bit to avoid int overflow when a large page is requested.
                const long long start = static_cast<long long>(page) * pageSize;
                Json assets = Json::array();
                for (long long i = start; i < total && i < start + pageSize; ++i)
                {
                    const AssetMetadata meta = mgr->GetAssetMetadata(handles[static_cast<sizet>(i)]);
                    assets.push_back(Json{ { "handle", std::to_string(static_cast<u64>(handles[static_cast<sizet>(i)])) },
                                           { "type", AssetUtils::AssetTypeToString(meta.Type) },
                                           { "path", meta.FilePath.generic_string() },
                                           { "name", meta.FilePath.filename().string() } });
                }

                Json out;
                out["total"] = total;
                out["page"] = page;
                out["pageSize"] = pageSize;
                out["returned"] = static_cast<int>(assets.size());
                if (start + pageSize < total)
                    out["nextPage"] = page + 1;
                out["assets"] = std::move(assets);
                return out; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_assets_problems (main-marshaled; failed/missing/invalid assets) -
        ToolResult Handle_AssetsProblems(McpServer& server, const Json& /*args*/)
        {
            const Json result = server.MarshalRead([]() -> Json
                                                   {
                const Ref<AssetManagerBase> mgr = Project::GetAssetManager();
                if (!mgr)
                    return Json{ { "__error", "No active project / asset manager." } };

                std::unordered_set<u64> seen;
                Json problems = Json::array();
                constexpr u16 kMaxType = static_cast<u16>(AssetType::CinematicSequence);
                for (u16 ti = 1; ti <= kMaxType; ++ti)
                {
                    for (const AssetHandle h : mgr->GetAllAssetsWithType(static_cast<AssetType>(ti)))
                    {
                        if (!seen.insert(static_cast<u64>(h)).second)
                            continue;
                        const AssetMetadata meta = mgr->GetAssetMetadata(h);
                        if (!AssetStatusUtils::IsStatusError(meta.Status))
                            continue;
                        problems.push_back(Json{ { "handle", std::to_string(static_cast<u64>(h)) },
                                                 { "type", AssetUtils::AssetTypeToString(meta.Type) },
                                                 { "path", meta.FilePath.generic_string() },
                                                 { "status", AssetStatusUtils::AssetStatusToString(meta.Status) } });
                    }
                }
                return Json{ { "count", static_cast<int>(problems.size()) }, { "problems", std::move(problems) } }; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- Crash reports (lock-safe; files on disk) --------------------------
        std::filesystem::path CrashReportsDir()
        {
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            if (ec)
                return {};
            return cwd / "CrashReports";
        }

        ToolResult Handle_CrashList(McpServer& /*server*/, const Json& /*args*/)
        {
            const std::filesystem::path dir = CrashReportsDir();
            std::error_code ec;
            Json arr = Json::array();
            if (std::filesystem::exists(dir, ec))
            {
                for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
                {
                    if (!entry.is_regular_file() || entry.path().extension() != ".txt")
                        continue;
                    const std::string fileName = entry.path().filename().string();
                    if (fileName.rfind("crash_", 0) != 0)
                        continue;
                    std::error_code sizeEc;
                    const auto size = std::filesystem::file_size(entry.path(), sizeEc);
                    arr.push_back(Json{ { "id", fileName },
                                        { "sizeBytes", static_cast<u64>(sizeEc ? 0 : size) } });
                }
            }
            Json out;
            out["count"] = static_cast<int>(arr.size());
            out["directory"] = dir.generic_string();
            out["crashes"] = std::move(arr);
            return ToolResult::Text(out.dump(2));
        }

        ToolResult Handle_CrashGet(McpServer& /*server*/, const Json& args)
        {
            if (!args.contains("id") || !args["id"].is_string())
                return ToolResult::Error("Missing required argument 'id' (a crash report filename from olo_crash_list).");
            const std::string id = args["id"].get<std::string>();

            // Path-traversal guard: must be a bare crash_*.txt filename.
            const bool valid = id.rfind("crash_", 0) == 0 && id.size() > 4 &&
                               id.compare(id.size() - 4, 4, ".txt") == 0 &&
                               id.find('/') == std::string::npos && id.find('\\') == std::string::npos &&
                               id.find("..") == std::string::npos;
            if (!valid)
                return ToolResult::Error("Invalid crash id (expected a 'crash_*.txt' filename).");

            const std::filesystem::path path = CrashReportsDir() / id;
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
                return ToolResult::Error("Crash report not found: " + id);

            std::ifstream file(path, std::ios::binary);
            if (!file)
                return ToolResult::Error("Could not open crash report: " + id);
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();

            constexpr std::size_t kMaxBytes = 200 * 1024;
            const bool truncated = content.size() > kMaxBytes;
            if (truncated)
                content.resize(kMaxBytes);

            Json out;
            out["id"] = id;
            out["truncated"] = truncated;
            out["content"] = std::move(content);
            return ToolResult::Text(out.dump(2));
        }

        // ---- olo_script_get_api (lock-safe; reads the scripting bindings) -------
        ToolResult Handle_ScriptGetApi(McpServer& /*server*/, const Json& args)
        {
            std::string language = "csharp";
            if (args.contains("language") && args["language"].is_string())
                language = args["language"].get<std::string>();
            std::string typeFilter;
            if (args.contains("typeFilter") && args["typeFilter"].is_string())
                typeFilter = args["typeFilter"].get<std::string>();

            Json digest = BuildScriptApiDigest(language, typeFilter);
            if (digest.contains("error"))
                return ToolResult::Error(digest["error"].get<std::string>());
            return ToolResult::Text(digest.dump(2));
        }

        std::string Base64Encode(const std::vector<u8>& data)
        {
            static constexpr char kTable[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve(((data.size() + 2) / 3) * 4);
            std::size_t i = 0;
            for (; i + 3 <= data.size(); i += 3)
            {
                const u32 n = (static_cast<u32>(data[i]) << 16) | (static_cast<u32>(data[i + 1]) << 8) | data[i + 2];
                out.push_back(kTable[(n >> 18) & 0x3F]);
                out.push_back(kTable[(n >> 12) & 0x3F]);
                out.push_back(kTable[(n >> 6) & 0x3F]);
                out.push_back(kTable[n & 0x3F]);
            }
            if (const std::size_t rem = data.size() - i; rem > 0)
            {
                u32 n = static_cast<u32>(data[i]) << 16;
                if (rem == 2)
                    n |= static_cast<u32>(data[i + 1]) << 8;
                out.push_back(kTable[(n >> 18) & 0x3F]);
                out.push_back(kTable[(n >> 12) & 0x3F]);
                out.push_back(rem == 2 ? kTable[(n >> 6) & 0x3F] : '=');
                out.push_back('=');
            }
            return out;
        }

        // ---- olo_script_get_last_errors (lock-safe; script error ring buffer) --
        ToolResult Handle_ScriptGetLastErrors(McpServer& /*server*/, const Json& args)
        {
            std::size_t count = 20;
            if (args.contains("count") && args["count"].is_number_integer())
                count = static_cast<std::size_t>(std::clamp<long long>(args["count"].get<long long>(), 1, 64));

            const std::vector<ScriptError> errors = ScriptErrorBuffer::Get().GetRecent(count);
            Json arr = Json::array();
            for (const auto& error : errors)
            {
                Json j;
                j["language"] = ScriptError::LanguageString(error.Lang);
                j["scriptName"] = error.ScriptName;
                if (error.EntityId != 0)
                    j["entityId"] = std::to_string(error.EntityId);
                j["message"] = error.Message;
                if (!error.StackTrace.empty())
                    j["stackTrace"] = error.StackTrace;
                j["timestamp"] = error.Timestamp;
                arr.push_back(std::move(j));
            }

            Json out;
            out["count"] = static_cast<int>(arr.size());
            out["errors"] = std::move(arr);
            return ToolResult::Text(out.dump(2));
        }

        // ---- olo_events_tail (lock-safe; unified diagnostics event ring buffer) -

        // A unified "what just happened?" timeline backed by the engine's diagnostics
        // event ring buffer (Debug/DiagnosticsEventLog.h, mutex-guarded — safe from the
        // handler thread). Supports incremental polling via sinceId: pass back the
        // returned lastId to get only events that happened since the previous call.
        ToolResult Handle_EventsTail(McpServer& /*server*/, const Json& args)
        {
            DiagnosticEventQuery query;
            if (args.contains("count") && args["count"].is_number_integer())
                query.MaxCount = static_cast<std::size_t>(std::clamp<long long>(args["count"].get<long long>(), 1, 500));

            if (args.contains("sinceId"))
            {
                const Json& since = args["sinceId"];
                if (since.is_number_unsigned())
                    query.SinceId = since.get<u64>();
                else if (since.is_number_integer() && since.get<long long>() > 0)
                    query.SinceId = static_cast<u64>(since.get<long long>());
                else if (since.is_string())
                {
                    try
                    {
                        query.SinceId = std::stoull(since.get<std::string>());
                    }
                    catch (...)
                    {
                        return ToolResult::Error("Invalid 'sinceId': expected a non-negative integer (an event id).");
                    }
                }
            }

            if (args.contains("categories") && args["categories"].is_array())
            {
                for (const auto& entry : args["categories"])
                {
                    if (!entry.is_string())
                        continue;
                    if (DiagnosticEventCategory category; DiagnosticEvent::CategoryFromString(entry.get<std::string>(), category))
                        query.Categories.push_back(category);
                    else
                        return ToolResult::Error(
                            "Unknown category '" + entry.get<std::string>() +
                            "'. Valid: scene_load, play, stop, entity_spawn, entity_destroy, asset_reload, script_error.");
                }
            }

            // Events + cursor in one locked snapshot: reading LastId() separately would
            // race a concurrent Record and skip an event on the next sinceId poll.
            const DiagnosticEventQueryResult result = DiagnosticsEventLog::Get().QueryWithCursor(query);

            Json arr = Json::array();
            for (const auto& event : result.Events)
                arr.push_back(EventToJson(event)); // shared with the SSE push path (McpEventStream.h)

            Json out;
            out["count"] = static_cast<int>(arr.size());
            // The highest id in the buffer at snapshot time — pass it back as the next
            // call's sinceId to poll only what happened since. Consistent with the events
            // above (same lock), and stable even when no events matched the filter.
            out["lastId"] = result.LastId;
            out["events"] = std::move(arr);
            return ToolResult::Text(out.dump(2));
        }

        // ---- Camera tool helpers (Tier 0, #316) --------------------------------

        // Parse a [x, y, z] JSON array of finite numbers.
        bool ParseVec3(const Json& value, glm::vec3& out)
        {
            if (!value.is_array() || value.size() != 3)
                return false;
            for (int i = 0; i < 3; ++i)
            {
                if (!value[static_cast<sizet>(i)].is_number())
                    return false;
                const f32 v = value[static_cast<sizet>(i)].get<f32>();
                if (!std::isfinite(v))
                    return false;
                out[i] = v;
            }
            return true;
        }

        // Invert EditorCamera's orientation convention. Its forward vector is
        // (cos(pitch)·sin(yaw), -sin(pitch), -cos(pitch)·cos(yaw)), so a look
        // direction maps back to yaw = atan2(x, -z), pitch = asin(-y).
        void DirectionToYawPitch(const glm::vec3& direction, f32& yawRadians, f32& pitchRadians)
        {
            const glm::vec3 d = glm::normalize(direction);
            yawRadians = std::atan2(d.x, -d.z);
            pitchRadians = std::asin(std::clamp(-d.y, -1.0f, 1.0f));
        }

        Json PoseToJson(const McpCameraPose& pose)
        {
            Json j;
            j["position"] = Json::array({ pose.Position.x, pose.Position.y, pose.Position.z });
            j["focalPoint"] = Json::array({ pose.FocalPoint.x, pose.FocalPoint.y, pose.FocalPoint.z });
            j["forward"] = Json::array({ pose.Forward.x, pose.Forward.y, pose.Forward.z });
            j["yawDegrees"] = glm::degrees(pose.YawRadians);
            j["pitchDegrees"] = glm::degrees(pose.PitchRadians);
            j["distance"] = pose.Distance;
            j["fovDegrees"] = pose.FovDegrees;
            j["nearClip"] = pose.NearClip;
            j["farClip"] = pose.FarClip;
            j["viewportWidth"] = pose.ViewportWidth;
            j["viewportHeight"] = pose.ViewportHeight;
            return j;
        }

        // A camera placement request shared by olo_camera_set_pose, olo_camera_orbit
        // and olo_screenshot's optional camera argument.
        struct CameraRequest
        {
            bool IsOrbit = false;
            glm::vec3 EyeOrTarget{ 0.0f }; // eye (pose) or orbit target
            f32 YawRadians = 0.0f;
            f32 PitchRadians = 0.0f;
            f32 Distance = 10.0f;   // orbit only
            f32 FovDegrees = -1.0f; // <= 0 keeps the current FOV
        };

        // Parse {position, target|yaw+pitch, fov} (pose form). Returns an error
        // message, or empty on success.
        std::string ParsePoseRequest(const Json& args, CameraRequest& out)
        {
            out.IsOrbit = false;
            if (!args.contains("position") || !ParseVec3(args["position"], out.EyeOrTarget))
                return "Missing or invalid 'position': expected [x, y, z] finite numbers.";

            const bool hasTarget = args.contains("target");
            const bool hasAngles = args.contains("yaw") || args.contains("pitch");
            if (hasTarget && hasAngles)
                return "Give either 'target' or 'yaw'/'pitch', not both.";
            if (hasTarget)
            {
                glm::vec3 target{ 0.0f };
                if (!ParseVec3(args["target"], target))
                    return "Invalid 'target': expected [x, y, z] finite numbers.";
                const glm::vec3 direction = target - out.EyeOrTarget;
                if (glm::dot(direction, direction) < 1e-12f)
                    return "'target' must differ from 'position'.";
                DirectionToYawPitch(direction, out.YawRadians, out.PitchRadians);
            }
            else if (hasAngles)
            {
                if ((args.contains("yaw") && !args["yaw"].is_number()) ||
                    (args.contains("pitch") && !args["pitch"].is_number()))
                    return "Invalid 'yaw'/'pitch': expected numbers (degrees).";
                const f32 yawDeg = args.value("yaw", 0.0f);
                const f32 pitchDeg = args.value("pitch", 0.0f);
                if (!std::isfinite(yawDeg) || !std::isfinite(pitchDeg))
                    return "Invalid 'yaw'/'pitch': expected finite numbers (degrees).";
                out.YawRadians = glm::radians(yawDeg);
                out.PitchRadians = glm::radians(pitchDeg);
            }
            else
            {
                return "Missing look direction: give 'target' [x,y,z] or 'yaw'/'pitch' (degrees).";
            }

            if (args.contains("fov"))
            {
                if (!args["fov"].is_number())
                    return "Invalid 'fov': expected a number (degrees).";
                const f32 fov = args.value("fov", 0.0f);
                if (!std::isfinite(fov) || fov < 1.0f || fov > 170.0f)
                    return "Invalid 'fov': expected degrees in [1, 170].";
                out.FovDegrees = fov;
            }
            return {};
        }

        // Parse {target, yaw, pitch, distance} (orbit form).
        std::string ParseOrbitRequest(const Json& args, CameraRequest& out)
        {
            out.IsOrbit = true;
            if (!args.contains("target") || !ParseVec3(args["target"], out.EyeOrTarget))
                return "Missing or invalid 'target': expected [x, y, z] finite numbers.";
            if ((args.contains("yaw") && !args["yaw"].is_number()) ||
                (args.contains("pitch") && !args["pitch"].is_number()) ||
                (args.contains("distance") && !args["distance"].is_number()))
                return "Invalid 'yaw'/'pitch'/'distance': expected numbers.";
            const f32 yawDeg = args.value("yaw", 0.0f);
            const f32 pitchDeg = args.value("pitch", 30.0f);
            const f32 distance = args.value("distance", 10.0f);
            if (!std::isfinite(yawDeg) || !std::isfinite(pitchDeg) || !std::isfinite(distance) || distance <= 0.0f)
                return "Invalid 'yaw'/'pitch'/'distance': expected finite numbers, distance > 0.";
            out.YawRadians = glm::radians(yawDeg);
            out.PitchRadians = glm::radians(pitchDeg);
            out.Distance = distance;
            if (args.contains("fov"))
            {
                if (!args["fov"].is_number())
                    return "Invalid 'fov': expected a number (degrees).";
                const f32 fov = args.value("fov", 0.0f);
                if (!std::isfinite(fov) || fov < 1.0f || fov > 170.0f)
                    return "Invalid 'fov': expected degrees in [1, 170].";
                out.FovDegrees = fov;
            }
            return {};
        }

        bool CameraContextAvailable(const EditorMcpContext& ctx)
        {
            return ctx.GetCameraPose && ctx.SetCameraPose && ctx.OrbitCamera && ctx.RestoreCameraPose;
        }

        // Apply a CameraRequest. MUST run on the main thread (inside MarshalRead).
        void ApplyCameraRequest(const EditorMcpContext& ctx, const CameraRequest& request)
        {
            if (request.IsOrbit)
            {
                if (request.FovDegrees > 0.0f)
                    ctx.SetCameraPose(glm::vec3(0.0f), 0.0f, 0.0f, request.FovDegrees); // FOV only; pose follows
                ctx.OrbitCamera(request.EyeOrTarget, request.YawRadians, request.PitchRadians, request.Distance);
            }
            else
            {
                ctx.SetCameraPose(request.EyeOrTarget, request.YawRadians, request.PitchRadians, request.FovDegrees);
            }
        }

        // ---- olo_camera_get (main-marshaled) -----------------------------------
        ToolResult Handle_CameraGet(McpServer& server, const Json& /*args*/)
        {
            if (!server.Context().GetCameraPose)
                return ToolResult::Error("Camera control is not available in this editor build.");
            const Json pose = server.MarshalRead([&server]() -> Json
                                                 { return PoseToJson(server.Context().GetCameraPose()); });
            return ToolResult::Text(pose.dump(2));
        }

        // ---- olo_camera_set_pose (main-marshaled; mutates editor camera) -------
        ToolResult Handle_CameraSetPose(McpServer& server, const Json& args)
        {
            if (!CameraContextAvailable(server.Context()))
                return ToolResult::Error("Camera control is not available in this editor build.");
            CameraRequest request;
            if (const std::string error = ParsePoseRequest(args, request); !error.empty())
                return ToolResult::Error(error);

            const Json pose = server.MarshalRead([&server, request]() -> Json
                                                 {
                ApplyCameraRequest(server.Context(), request);
                return PoseToJson(server.Context().GetCameraPose()); });
            return ToolResult::Text(pose.dump(2));
        }

        // ---- olo_camera_orbit (main-marshaled; mutates editor camera) ----------
        ToolResult Handle_CameraOrbit(McpServer& server, const Json& args)
        {
            if (!CameraContextAvailable(server.Context()))
                return ToolResult::Error("Camera control is not available in this editor build.");
            CameraRequest request;
            if (const std::string error = ParseOrbitRequest(args, request); !error.empty())
                return ToolResult::Error(error);

            const Json pose = server.MarshalRead([&server, request]() -> Json
                                                 {
                ApplyCameraRequest(server.Context(), request);
                return PoseToJson(server.Context().GetCameraPose()); });
            return ToolResult::Text(pose.dump(2));
        }

        // ---- olo_camera_frame_entity (main-marshaled; mutates editor camera) ---
        ToolResult Handle_CameraFrameEntity(McpServer& server, const Json& args)
        {
            if (!server.Context().FrameEntity || !server.Context().GetCameraPose)
                return ToolResult::Error("Camera control is not available in this editor build.");
            if (!args.contains("id"))
                return ToolResult::Error("Missing required argument 'id' (entity UUID).");
            u64 idValue = 0;
            if (!ParseUuid(args["id"], idValue))
                return ToolResult::Error("Invalid 'id': expected a UUID as a string or number.");

            const Json result = server.MarshalRead([&server, idValue]() -> Json
                                                   {
                if (!server.Context().FrameEntity(idValue))
                    return Json{ { "__error", "No entity with that UUID in the active scene." } };
                Json j = PoseToJson(server.Context().GetCameraPose());
                j["framedEntity"] = std::to_string(idValue);
                return j; });
            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_viewport_set_size (main-marshaled; mutates viewport override) -
        ToolResult Handle_ViewportSetSize(McpServer& server, const Json& args)
        {
            if (!server.Context().SetViewportSizeOverride)
                return ToolResult::Error("Viewport control is not available in this editor build.");

            u32 width = 0;
            u32 height = 0;
            const bool reset = args.value("reset", false);
            if (!reset)
            {
                if (!args.contains("width") || !args.contains("height") ||
                    !args["width"].is_number_integer() || !args["height"].is_number_integer())
                    return ToolResult::Error("Give 'width' and 'height' (pixels), or 'reset': true.");
                width = static_cast<u32>(std::clamp<long long>(args["width"].get<long long>(), 64, 8192));
                height = static_cast<u32>(std::clamp<long long>(args["height"].get<long long>(), 64, 8192));
            }

            const Json result = server.MarshalRead([&server, width, height, reset]() -> Json
                                                   {
                server.Context().SetViewportSizeOverride(width, height);
                Json j;
                j["override"] = !reset;
                if (!reset)
                {
                    j["width"] = width;
                    j["height"] = height;
                }
                return j; });
            return ToolResult::Text(result.dump(2));
        }

        // Wait until the editor has rendered `settleFrames` frames after
        // `baseFrame` and the framebuffer is capture-ready (not throttle-skipped
        // and not mid viewport-resize transient), so a camera change is actually
        // visible in the framebuffer before a capture. Returns false on timeout.
        bool AwaitRenderedFrames(McpServer& server, u64 baseFrame, int settleFrames)
        {
            if (!server.Context().GetFrameIndex || !server.Context().IsCaptureUnready)
                return true; // older context: best effort, capture immediately
            const u64 targetFrame = baseFrame + static_cast<u64>(settleFrames);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline)
            {
                const Json state = server.MarshalRead([&server]() -> Json
                                                      { return Json{ { "frame", server.Context().GetFrameIndex() },
                                                                     { "unready", server.Context().IsCaptureUnready() } }; });
                if (state.value("frame", static_cast<u64>(0)) >= targetFrame && !state.value("unready", false))
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return false;
        }

        // ---- olo_screenshot (main-marshaled; GL readback) ----------------------
        // Captures the editor viewport framebuffer as a PNG and returns it as an MCP
        // image content block so the agent can SEE the rendered frame. With the
        // optional 'camera'/'orbit' argument it poses the editor camera first, waits
        // for the pose to be rendered, captures, and restores the prior camera —
        // multi-angle inspection without disturbing the user's viewport (#316).
        ToolResult Handle_Screenshot(McpServer& server, const Json& args)
        {
            int maxWidth = 1024;
            if (args.contains("maxWidth") && args["maxWidth"].is_number_integer())
                maxWidth = static_cast<int>(std::clamp<long long>(args["maxWidth"].get<long long>(), 16, 4096));

            if (!server.Context().CaptureViewportPng)
                return ToolResult::Error("Screenshot capture is not available in this editor build.");

            // Optional camera placement for this capture only.
            const bool hasCamera = args.contains("camera");
            const bool hasOrbit = args.contains("orbit");
            if (hasCamera && hasOrbit)
                return ToolResult::Error("Give either 'camera' or 'orbit', not both.");
            CameraRequest request;
            if (hasCamera || hasOrbit)
            {
                if (!CameraContextAvailable(server.Context()))
                    return ToolResult::Error("Camera control is not available in this editor build.");
                const std::string error = hasCamera ? ParsePoseRequest(args["camera"], request)
                                                    : ParseOrbitRequest(args["orbit"], request);
                if (!error.empty())
                    return ToolResult::Error(error);
            }

            int settleFrames = 2;
            if (args.contains("settleFrames") && args["settleFrames"].is_number_integer())
                settleFrames = static_cast<int>(std::clamp<long long>(args["settleFrames"].get<long long>(), 1, 30));

            bool posed = false;
            Json savedPose; // round-trips the prior pose through JSON for the restore job
            bool waitTimedOut = false;
            if (hasCamera || hasOrbit)
            {
                // 1. Save the user's pose and apply the requested one.
                const Json applied = server.MarshalRead([&server, request]() -> Json
                                                        {
                    const McpCameraPose prior = server.Context().GetCameraPose();
                    ApplyCameraRequest(server.Context(), request);
                    Json j;
                    j["focalPoint"] = Json::array({ prior.FocalPoint.x, prior.FocalPoint.y, prior.FocalPoint.z });
                    j["distance"] = prior.Distance;
                    j["yaw"] = prior.YawRadians;
                    j["pitch"] = prior.PitchRadians;
                    j["fov"] = prior.FovDegrees;
                    j["frame"] = server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0;
                    return j; });
                savedPose = applied;
                posed = true;
            }

            // The restore half of the save/restore contract, runnable from both
            // the success path (inside the capture job) and the error path.
            const auto restorePriorPose = [&server, &savedPose]()
            {
                McpCameraPose prior;
                prior.FocalPoint = glm::vec3{ savedPose["focalPoint"][0].get<f32>(),
                                              savedPose["focalPoint"][1].get<f32>(),
                                              savedPose["focalPoint"][2].get<f32>() };
                prior.Distance = savedPose.value("distance", 0.0f);
                prior.YawRadians = savedPose.value("yaw", 0.0f);
                prior.PitchRadians = savedPose.value("pitch", 0.0f);
                prior.FovDegrees = savedPose.value("fov", 45.0f);
                server.Context().RestoreCameraPose(prior);
            };

            Json marshaled;
            try
            {
                // 2. Wait until the new pose has actually been rendered.
                if (posed)
                    waitTimedOut = !AwaitRenderedFrames(server, savedPose.value("frame", static_cast<u64>(0)), settleFrames);

                // 3. Capture — and restore the user's camera in the same main-thread
                // job, so the restore happens exactly once whatever the capture did.
                marshaled = server.MarshalRead([&server, maxWidth, posed, &restorePriorPose]() -> Json
                                               {
                    const std::vector<u8> png = server.Context().CaptureViewportPng(maxWidth);
                    if (posed)
                        restorePriorPose();
                    if (png.empty())
                        return Json{ { "__error", "Viewport capture failed (no framebuffer or empty viewport)." } };
                    return Json{ { "b64", Base64Encode(png) }, { "bytes", static_cast<u64>(png.size()) } }; });
            }
            catch (...)
            {
                // Don't leave the user's camera stranded at the capture pose if a
                // marshal failed mid-flow. Best effort — if the editor is truly
                // stalled this will fail too, and the original exception matters more.
                if (posed)
                {
                    try
                    {
                        (void)server.MarshalRead([&restorePriorPose]() -> Json
                                                 {
                            restorePriorPose();
                            return Json{}; });
                    }
                    catch (...)
                    {
                    }
                }
                throw;
            }

            if (marshaled.is_object() && marshaled.contains("__error"))
                return ToolResult::Error(marshaled["__error"].get<std::string>());

            ToolResult result;
            result.Content = Json::array();
            if (waitTimedOut)
                result.Content.push_back(Json{ { "type", "text" },
                                               { "text", "Warning: timed out waiting for the new camera pose to render; "
                                                         "the image may show a stale frame." } });
            result.Content.push_back(Json{ { "type", "image" },
                                           { "data", marshaled["b64"] },
                                           { "mimeType", "image/png" } });
            result.IsError = false;
            return result;
        }

        // ---- Render-target capture (Part 4 of #316) ----------------------------

        const char* RGFormatName(RGResourceFormat format)
        {
            switch (format)
            {
                case RGResourceFormat::Unknown:
                    return "Unknown";
                case RGResourceFormat::R8UNorm:
                    return "R8UNorm";
                case RGResourceFormat::R32Float:
                    return "R32Float";
                case RGResourceFormat::RG16Float:
                    return "RG16Float";
                case RGResourceFormat::RGBA8UNorm:
                    return "RGBA8UNorm";
                case RGResourceFormat::RGBA16Float:
                    return "RGBA16Float";
                case RGResourceFormat::RGBA32Float:
                    return "RGBA32Float";
                case RGResourceFormat::Depth24Stencil8:
                    return "Depth24Stencil8";
                case RGResourceFormat::Depth32Float:
                    return "Depth32Float";
                case RGResourceFormat::R32Int:
                    return "R32Int";
            }
            return "Unknown";
        }

        const char* PassWorkTypeName(RenderGraphPassWorkType type)
        {
            switch (type)
            {
                case RenderGraphPassWorkType::Graphics:
                    return "Graphics";
                case RenderGraphPassWorkType::Compute:
                    return "Compute";
                case RenderGraphPassWorkType::Copy:
                    return "Copy";
            }
            return "Graphics";
        }

        // ---- olo_render_list_targets (main-marshaled) --------------------------
        ToolResult Handle_RenderListTargets(McpServer& server, const Json& /*args*/)
        {
            Json result = server.MarshalRead([]() -> Json
                                             {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                Json targets = Json::array();
                for (const auto& resource : graph->GetRegisteredResources())
                {
                    // Only capturable resources: those backed by a texture or a
                    // framebuffer. Buffers (UBO/SSBO) are not image-capturable.
                    if (!resource.TextureHandle.IsValid() && !resource.FramebufferHandle.IsValid())
                        continue;
                    Json e;
                    e["name"] = resource.Name;
                    e["kind"] = std::string(ToString(resource.Desc.Kind));
                    if (resource.Desc.Format != RGResourceFormat::Unknown)
                        e["format"] = RGFormatName(resource.Desc.Format);
                    if (resource.Desc.Width > 0 && resource.Desc.Height > 0)
                    {
                        e["width"] = resource.Desc.Width;
                        e["height"] = resource.Desc.Height;
                    }
                    if (!resource.Producers.empty())
                        e["producers"] = resource.Producers;
                    targets.push_back(std::move(e));
                }
                Json j;
                j["count"] = static_cast<int>(targets.size());
                j["targets"] = std::move(targets);
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_render_graph_topology_export (main-marshaled) -----------------
        // Read-only structured export of the live RenderGraph topology — passes,
        // execution order, pass-dependency edges, and resources with their
        // producers/consumers — so an agent can reason about the render pipeline
        // (#316 Part 4, "LLM-analysis exports"). The RenderGraph is main-thread
        // state, so the enumeration runs inside MarshalRead; the JSON / Mermaid
        // shaping is the pure, unit-tested RenderGraphTopology core.
        ToolResult Handle_RenderGraphTopologyExport(McpServer& server, const Json& args)
        {
            std::string format = "json";
            if (args.contains("format") && args["format"].is_string())
            {
                format = args["format"].get<std::string>();
                if (format != "json" && format != "mermaid")
                    return ToolResult::Error("format must be \"json\" or \"mermaid\".");
            }

            Json transport = server.MarshalRead([format]() -> Json
                                                {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                RenderGraphTopology::Snapshot snap;
                snap.FinalPass = graph->GetFinalPassName();

                const auto& culled = graph->GetCulledPasses();
                const std::unordered_set<std::string> culledSet(culled.begin(), culled.end());

                for (const auto& info : graph->GetNodeSubmissionInfo())
                {
                    RenderGraphTopology::PassInfo pass;
                    pass.Name = info.NodeName;
                    pass.WorkType = PassWorkTypeName(info.WorkType);
                    pass.DeclaresResources = info.DeclaresResources;
                    pass.AsyncComputeCandidate = info.AsyncComputeCandidate;
                    pass.Culled = culledSet.contains(info.NodeName);
                    pass.IsFinalPass = !snap.FinalPass.empty() && info.NodeName == snap.FinalPass;
                    snap.Passes.push_back(std::move(pass));
                }

                snap.ExecutionOrder = graph->GetExecutionOrder();

                for (const auto& connection : graph->GetConnections())
                    snap.Edges.push_back(RenderGraphTopology::EdgeInfo{ connection.OutputPass, connection.InputPass });

                for (const auto& resource : graph->GetRegisteredResources())
                {
                    RenderGraphTopology::ResourceInfo info;
                    info.Name = resource.Name;
                    info.Kind = std::string(ToString(resource.Desc.Kind));
                    if (resource.Desc.Format != RGResourceFormat::Unknown)
                        info.Format = RGFormatName(resource.Desc.Format);
                    info.Width = resource.Desc.Width;
                    info.Height = resource.Desc.Height;
                    info.Samples = resource.Desc.Samples;
                    info.Imported = resource.Desc.Imported;
                    info.HasExternalBacking = resource.HasExternalBacking;
                    info.Producers = resource.Producers;
                    info.Consumers = resource.Consumers;
                    snap.Resources.push_back(std::move(info));
                }

                // Mermaid is a pure transform of the snapshot, but the snapshot can
                // only be gathered on the main thread, so build the text here and
                // ferry it out under a sentinel key.
                if (format == "mermaid")
                    return Json{ { "__text", RenderGraphTopology::BuildMermaid(snap) } };
                return RenderGraphTopology::BuildJson(snap); });

            if (transport.is_object() && transport.contains("__error"))
                return ToolResult::Error(transport["__error"].get<std::string>());
            if (transport.is_object() && transport.contains("__text"))
                return ToolResult::Text(transport["__text"].get<std::string>());
            return ToolResult::Text(transport.dump(2));
        }

        // ---- olo_render_capture_target (main-marshaled; GL readback) -----------
        ToolResult Handle_RenderCaptureTarget(McpServer& server, const Json& args)
        {
            if (!args.contains("name") || !args["name"].is_string())
                return ToolResult::Error("Missing required argument 'name' (render-graph resource name; see olo_render_list_targets).");
            const std::string name = args["name"].get<std::string>();

            u32 mipLevel = 0;
            if (args.contains("mip") && args["mip"].is_number_integer())
                mipLevel = static_cast<u32>(std::clamp<long long>(args["mip"].get<long long>(), 0, 16));
            u32 faceOrLayer = 0;
            if (args.contains("face") && args["face"].is_number_integer())
                faceOrLayer = static_cast<u32>(std::clamp<long long>(args["face"].get<long long>(), 0, 64));
            int maxWidth = 1024;
            if (args.contains("maxWidth") && args["maxWidth"].is_number_integer())
                maxWidth = static_cast<int>(std::clamp<long long>(args["maxWidth"].get<long long>(), 16, 4096));

            auto normalizeMode = GPUResourceInspector::CaptureNormalizeMode::Auto;
            if (args.contains("normalize") && args["normalize"].is_boolean())
                normalizeMode = args["normalize"].get<bool>() ? GPUResourceInspector::CaptureNormalizeMode::On
                                                              : GPUResourceInspector::CaptureNormalizeMode::Off;

            Json result = server.MarshalRead([name, mipLevel, faceOrLayer, normalizeMode, maxWidth]() -> Json
                                             {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                // Resolve the name to a physical GL texture: first as a graph
                // texture (covers attachment views like SceneDepth / GBufferAlbedo
                // and imported textures like ShadowMapCSM), then as a framebuffer
                // (capture colour attachment 0, or the depth attachment for
                // depth-only targets).
                u32 textureId = Renderer3D::ResolveFrameGraphTexture(name);
                if (textureId == 0)
                {
                    if (const Ref<Framebuffer> framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(name); framebuffer)
                    {
                        bool hasColor = false;
                        for (const auto& attachment : framebuffer->GetSpecification().Attachments.Attachments)
                        {
                            if (attachment.TextureFormat != FramebufferTextureFormat::DEPTH24STENCIL8 &&
                                attachment.TextureFormat != FramebufferTextureFormat::DEPTH_COMPONENT32F &&
                                attachment.TextureFormat != FramebufferTextureFormat::None)
                            {
                                hasColor = true;
                                break;
                            }
                        }
                        textureId = hasColor ? framebuffer->GetColorAttachmentRendererID(0)
                                             : framebuffer->GetDepthAttachmentRendererID();
                    }
                }
                if (textureId == 0)
                    return Json{ { "__error", "Unknown render-graph resource '" + name +
                                                  "' (or it has no GPU backing this frame). Call olo_render_list_targets for the live list." } };

                auto capture = GPUResourceInspector::CaptureTexturePng(textureId, mipLevel, faceOrLayer, normalizeMode, maxWidth);
                if (!capture.Error.empty())
                    return Json{ { "__error", "Capture of '" + name + "' failed: " + capture.Error } };

                Json meta;
                meta["name"] = name;
                meta["width"] = capture.Width;
                meta["height"] = capture.Height;
                meta["sourceWidth"] = capture.SourceWidth;
                meta["sourceHeight"] = capture.SourceHeight;
                meta["format"] = capture.FormatName;
                meta["isDepth"] = capture.IsDepth;
                meta["normalized"] = capture.Normalized;
                if (capture.MaxValue > capture.MinValue)
                {
                    meta["minValue"] = capture.MinValue;
                    meta["maxValue"] = capture.MaxValue;
                }
                return Json{ { "meta", std::move(meta) },
                             { "b64", Base64Encode(capture.PngBytes) } }; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());

            ToolResult toolResult;
            toolResult.Content = Json::array({ Json{ { "type", "text" }, { "text", result["meta"].dump(2) } },
                                               Json{ { "type", "image" },
                                                     { "data", result["b64"] },
                                                     { "mimeType", "image/png" } } });
            toolResult.IsError = false;
            return toolResult;
        }

        // ---- olo_render_toggle_pass / olo_render_set_debug_view (#316 Part 4) ---
        // Ephemeral render-override A/B harness. Both tools mutate ONLY the
        // renderer's session-global settings (Renderer3D::GetPostProcessSettings()
        // / GetFogSettings()), never the loaded scene's own copy — so a change is
        // visible on the next rendered frame and a scene reload restores it. That
        // keeps the server read-only with respect to the project, the same boundary
        // the camera/viewport tools respect. All renderer state is main-thread-only,
        // so the work runs inside MarshalRead.

        // Canonical token for the active AO technique (reported by the toggle-pass
        // introspection so the agent can tell whether enabling SSAO vs GTAO will
        // actually apply — the two share PostProcessSettings::ActiveAOTechnique).
        const char* AOTechniqueToken(AOTechnique technique)
        {
            switch (technique)
            {
                case AOTechnique::None:
                    return "none";
                case AOTechnique::SSAO:
                    return "ssao";
                case AOTechnique::GTAO:
                    return "gtao";
            }
            return "unknown";
        }

        // Human-readable name of a rendering path, for the SSR/SSGI deferred-only
        // precondition note.
        const char* RenderingPathName(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "Forward+";
                case RenderingPath::Deferred:
                    return "Deferred";
            }
            return "Unknown";
        }

        // Map a pass token to the single bool field it flips. PostProcess* fields
        // live on PostProcessSettings; Fog* on FogSettings. Returns nullptr only if
        // a new RenderOverrides::Pass enumerator is added without a mapping here
        // (the caller surfaces that as an internal error rather than crashing).
        bool* ResolvePassField(RenderOverrides::Pass pass, PostProcessSettings& pp, FogSettings& fog)
        {
            using RenderOverrides::Pass;
            switch (pass)
            {
                case Pass::Bloom:
                    return &pp.BloomEnabled;
                case Pass::SSAO:
                    return &pp.SSAOEnabled;
                case Pass::GTAO:
                    return &pp.GTAOEnabled;
                case Pass::SSR:
                    return &pp.SSREnabled;
                case Pass::SSGI:
                    return &pp.SSGIEnabled;
                case Pass::FXAA:
                    return &pp.FXAAEnabled;
                case Pass::TAA:
                    return &pp.TAAEnabled;
                case Pass::Vignette:
                    return &pp.VignetteEnabled;
                case Pass::ChromaticAberration:
                    return &pp.ChromaticAberrationEnabled;
                case Pass::DepthOfField:
                    return &pp.DOFEnabled;
                case Pass::MotionBlur:
                    return &pp.MotionBlurEnabled;
                case Pass::ColorGrading:
                    return &pp.ColorGradingEnabled;
                case Pass::AutoExposure:
                    return &pp.AutoExposureEnabled;
                case Pass::Fog:
                    return &fog.Enabled;
                case Pass::FogScattering:
                    return &fog.EnableScattering;
                case Pass::FogVolumetric:
                    return &fog.EnableVolumetric;
                case Pass::GodRays:
                    return &fog.EnableLightShafts;
            }
            return nullptr;
        }

        ToolResult Handle_RenderTogglePass(McpServer& server, const Json& args)
        {
            using namespace RenderOverrides;

            const bool hasName = args.contains("name") && args["name"].is_string() &&
                                 !args["name"].get<std::string>().empty();

            // Introspection: no name -> list every toggleable pass with its live
            // enabled state plus the active AO technique.
            if (!hasName)
            {
                const Json result = server.MarshalRead([]() -> Json
                                                       {
                    PostProcessSettings& pp = Renderer3D::GetPostProcessSettings();
                    FogSettings& fog = Renderer3D::GetFogSettings();
                    Json passes = DescribePasses();
                    for (auto& entry : passes)
                    {
                        Pass pass{};
                        if (ParsePass(entry.at("name").get<std::string>(), pass))
                        {
                            const bool* field = ResolvePassField(pass, pp, fog);
                            entry["enabled"] = (field != nullptr) && *field;
                        }
                    }
                    Json j;
                    j["passes"] = std::move(passes);
                    j["activeAOTechnique"] = AOTechniqueToken(pp.ActiveAOTechnique);
                    return j; });
                return ToolResult::Text(result.dump(2));
            }

            const std::string name = args["name"].get<std::string>();
            Pass pass{};
            if (!ParsePass(name, pass))
                return ToolResult::Error("Unknown pass '" + name + "'. Valid passes: " + JoinTokens(PassTokens()) +
                                         ". Call olo_render_toggle_pass with no arguments to list them with their current state.");

            // 'enabled' is optional: when given, set explicitly; when omitted, flip
            // the current value (the quick A/B form).
            const bool hasEnabled = args.contains("enabled") && args["enabled"].is_boolean();
            const bool desired = hasEnabled && args["enabled"].get<bool>();

            const Json result = server.MarshalRead([pass, hasEnabled, desired]() -> Json
                                                   {
                PostProcessSettings& pp = Renderer3D::GetPostProcessSettings();
                FogSettings& fog = Renderer3D::GetFogSettings();
                bool* field = ResolvePassField(pass, pp, fog);
                if (field == nullptr)
                    return Json{ { "__error", "Internal error: pass has no field mapping." } };

                ToggleResult r;
                r.Pass = PassToken(pass);
                r.Previous = *field;
                r.Enabled = hasEnabled ? desired : !*field;
                *field = r.Enabled;
                r.Changed = r.Enabled != r.Previous;

                // Side effects + preconditions, so a freshly enabled effect actually
                // appears (otherwise an agent A/Bs a toggle and sees no change).
                if (r.Enabled)
                {
                    switch (pass)
                    {
                        case Pass::SSAO:
                            // SSAO and GTAO share ActiveAOTechnique; point it at SSAO
                            // so enabling SSAO is what renders.
                            if (pp.ActiveAOTechnique != AOTechnique::SSAO)
                            {
                                pp.ActiveAOTechnique = AOTechnique::SSAO;
                                r.Note = "Active AO technique set to SSAO so the effect is visible.";
                            }
                            break;
                        case Pass::GTAO:
                            if (pp.ActiveAOTechnique != AOTechnique::GTAO)
                            {
                                pp.ActiveAOTechnique = AOTechnique::GTAO;
                                r.Note = "Active AO technique set to GTAO so the effect is visible.";
                            }
                            break;
                        case Pass::SSR:
                        case Pass::SSGI:
                            if (Renderer3D::GetRendererSettings().Path != RenderingPath::Deferred)
                                r.Note = std::string(PassToken(pass)) +
                                         " renders only in the Deferred rendering path (current path: " +
                                         RenderingPathName(Renderer3D::GetRendererSettings().Path) + ").";
                            break;
                        case Pass::FogScattering:
                        case Pass::FogVolumetric:
                        case Pass::GodRays:
                            if (!fog.Enabled)
                                r.Note = "Fog is disabled; enable the 'fog' pass for this to take effect.";
                            break;
                        default:
                            break;
                    }
                }
                return ToJson(r); });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // Which debug view (if any) is currently on. Our tool keeps these mutually
        // exclusive, but the editor panel can set several; report the first.
        RenderOverrides::DebugView ActiveDebugView(const PostProcessSettings& pp)
        {
            using RenderOverrides::DebugView;
            if (pp.SSAODebugView)
                return DebugView::SSAO;
            if (pp.GTAODebugView)
                return DebugView::GTAO;
            if (pp.SSRDebugView)
                return DebugView::SSR;
            if (pp.SSGIDebugView)
                return DebugView::SSGI;
            return DebugView::None;
        }

        // Build the debug-view result for a (post-change or current) state: the four
        // flags from `pp`, the requested mode, and whether the backing pass is
        // actually producing the buffer this frame (with an actionable hint if not).
        RenderOverrides::DebugViewResult BuildDebugViewResult(const PostProcessSettings& pp, RenderOverrides::DebugView view)
        {
            using RenderOverrides::DebugView;
            RenderOverrides::DebugViewResult r;
            r.Mode = RenderOverrides::DebugViewToken(view);
            r.SSAODebugView = pp.SSAODebugView;
            r.GTAODebugView = pp.GTAODebugView;
            r.SSRDebugView = pp.SSRDebugView;
            r.SSGIDebugView = pp.SSGIDebugView;
            const bool deferred = Renderer3D::GetRendererSettings().Path == RenderingPath::Deferred;
            switch (view)
            {
                case DebugView::None:
                    r.PassEnabled = true;
                    break;
                case DebugView::SSAO:
                    r.PassEnabled = pp.ActiveAOTechnique == AOTechnique::SSAO && pp.SSAOEnabled;
                    if (!r.PassEnabled)
                        r.Note = "SSAO is not active; enable it with olo_render_toggle_pass { name: 'ssao' }.";
                    break;
                case DebugView::GTAO:
                    r.PassEnabled = pp.ActiveAOTechnique == AOTechnique::GTAO && pp.GTAOEnabled;
                    if (!r.PassEnabled)
                        r.Note = "GTAO is not active; enable it with olo_render_toggle_pass { name: 'gtao' }.";
                    break;
                case DebugView::SSR:
                    r.PassEnabled = pp.SSREnabled && deferred;
                    if (!r.PassEnabled)
                        r.Note = "SSR is not active; enable it with olo_render_toggle_pass { name: 'ssr' } (Deferred path only).";
                    break;
                case DebugView::SSGI:
                    r.PassEnabled = pp.SSGIEnabled && deferred;
                    if (!r.PassEnabled)
                        r.Note = "SSGI is not active; enable it with olo_render_toggle_pass { name: 'ssgi' } (Deferred path only).";
                    break;
            }
            return r;
        }

        ToolResult Handle_RenderSetDebugView(McpServer& server, const Json& args)
        {
            using namespace RenderOverrides;

            const bool hasMode = args.contains("mode") && args["mode"].is_string();
            // Accept enabled:false as an alias for mode:"none" (turn all views off).
            const bool disableViaEnabled = args.contains("enabled") && args["enabled"].is_boolean() &&
                                           !args["enabled"].get<bool>();

            // Introspection: no actionable argument -> list modes + current state.
            if (!hasMode && !disableViaEnabled)
            {
                const Json result = server.MarshalRead([]() -> Json
                                                       {
                    const PostProcessSettings& pp = Renderer3D::GetPostProcessSettings();
                    Json j;
                    j["modes"] = DescribeDebugViews();
                    j["current"] = ToJson(BuildDebugViewResult(pp, ActiveDebugView(pp)));
                    return j; });
                return ToolResult::Text(result.dump(2));
            }

            // enabled:false takes precedence over mode: it is the explicit
            // "clear all views" intent, so honour it even if a mode is also given
            // (leaving view at None) rather than letting the mode override it.
            DebugView view = DebugView::None;
            if (hasMode && !disableViaEnabled)
            {
                const std::string mode = args["mode"].get<std::string>();
                if (!ParseDebugView(mode, view))
                    return ToolResult::Error("Unknown debug view '" + mode + "'. Valid modes: " +
                                             JoinTokens(DebugViewModes()) + ".");
            }

            const Json result = server.MarshalRead([view]() -> Json
                                                   {
                PostProcessSettings& pp = Renderer3D::GetPostProcessSettings();
                // Exactly one debug view active at a time (or none).
                pp.SSAODebugView = (view == DebugView::SSAO);
                pp.GTAODebugView = (view == DebugView::GTAO);
                pp.SSRDebugView = (view == DebugView::SSR);
                pp.SSGIDebugView = (view == DebugView::SSGI);
                return ToJson(BuildDebugViewResult(pp, view)); });
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_scene_set_time_of_day / olo_scene_set_sun_angle (#316 Part 4) -
        // Ephemeral sun-direction override for lighting iteration. Like the
        // toggle/debug-view tools above, these edit ONLY a session-global renderer
        // override (Renderer3D::SetSunDirectionOverride), never the
        // ProceduralSkyComponent's serialized m_SunDirection — so the moved sun is
        // visible next frame, never saved, and resets on scene reload / play-stop /
        // server-stop / explicit clear. The sun-direction math + result shaping is
        // the pure RenderOverrides module; the handler does the renderer/scene-bound
        // work on the main thread inside MarshalRead.

        // (main thread) Finish a sun-override result: annotate it from the active
        // scene (the override only affects a procedural-sky bake) and the resulting
        // sun elevation (below the horizon bakes a dark night sky), then shape JSON.
        Json FinalizeSunOverride(McpServer& server, RenderOverrides::SunOverrideResult& r)
        {
            bool skyPresent = false;
            const Ref<Scene> scene = server.Context().GetActiveScene
                                         ? server.Context().GetActiveScene()
                                         : nullptr;
            if (scene)
            {
                auto view = scene->GetAllEntitiesWith<ProceduralSkyComponent>();
                skyPresent = view.begin() != view.end();
            }

            if (r.Active && !skyPresent)
                r.Note = "No ProceduralSkyComponent in the active scene, so this sun override has no visible "
                         "effect yet. Add a Procedural Sky to the scene to iterate time-of-day lighting.";
            else if (r.Active && RenderOverrides::SunElevationDegrees(r.Direction) < 0.0)
                r.Note = "The sun is below the horizon at this setting, so the sky bakes dark (night). Move "
                         "toward noon (hours ~12) or a positive pitch for daylight.";

            return RenderOverrides::ToJson(r);
        }

        // (main thread) Clear the ephemeral sun override and record the outcome —
        // the clear branch shared verbatim by both sun-override handlers.
        void ApplySunClear(RenderOverrides::SunOverrideResult& r)
        {
            r.Cleared = Renderer3D::HasSunDirectionOverride();
            Renderer3D::ClearSunDirectionOverride();
            r.Active = false;
            r.Source = "cleared";
        }

        // (main thread) Read the current override state into the result — the
        // no-argument introspection branch shared by both sun-override handlers.
        void ApplySunIntrospect(RenderOverrides::SunOverrideResult& r)
        {
            r.Active = Renderer3D::HasSunDirectionOverride();
            if (r.Active)
            {
                const glm::vec3& d = Renderer3D::GetSunDirectionOverride();
                r.Direction = RenderOverrides::SunVec3{ d.x, d.y, d.z };
            }
            r.Source = "current";
        }

        ToolResult Handle_SceneSetTimeOfDay(McpServer& server, const Json& args)
        {
            using namespace RenderOverrides;

            const bool wantClear = args.contains("clear") && args["clear"].is_boolean() && args["clear"].get<bool>();
            const bool hasHours = !wantClear && args.contains("hours") && args["hours"].is_number();
            double hours = 0.0;
            if (hasHours)
            {
                hours = args["hours"].get<double>();
                if (!std::isfinite(hours) || hours < 0.0 || hours > 24.0)
                    return ToolResult::Error("Invalid 'hours': expected a finite number in [0, 24] "
                                             "(0 = midnight, 6 = sunrise, 12 = noon, 18 = sunset).");
            }

            const Json result = server.MarshalRead([&server, wantClear, hasHours, hours]() -> Json
                                                   {
                SunOverrideResult r;
                if (wantClear)
                {
                    ApplySunClear(r);
                }
                else if (hasHours)
                {
                    const SunVec3 dir = SunDirectionFromTimeOfDay(hours);
                    Renderer3D::SetSunDirectionOverride(glm::vec3(
                        static_cast<f32>(dir.X), static_cast<f32>(dir.Y), static_cast<f32>(dir.Z)));
                    r.Active = true;
                    r.Direction = dir;
                    r.HasHours = true;
                    r.Hours = hours;
                    r.Source = "timeOfDay";
                }
                else
                {
                    ApplySunIntrospect(r);
                }
                return FinalizeSunOverride(server, r); });
            return ToolResult::Text(result.dump(2));
        }

        ToolResult Handle_SceneSetSunAngle(McpServer& server, const Json& args)
        {
            using namespace RenderOverrides;

            const bool wantClear = args.contains("clear") && args["clear"].is_boolean() && args["clear"].get<bool>();
            const bool hasYaw = !wantClear && args.contains("yaw") && args["yaw"].is_number();
            const bool hasPitch = !wantClear && args.contains("pitch") && args["pitch"].is_number();

            // A set needs BOTH angles — a half-specified direction is ambiguous, so
            // reject it with guidance rather than silently using a default.
            if (!wantClear && (hasYaw != hasPitch))
                return ToolResult::Error("olo_scene_set_sun_angle needs both 'yaw' (azimuth, degrees) and "
                                         "'pitch' (elevation, degrees). Provide both, pass 'clear':true to "
                                         "remove the override, or call with no arguments to read the current "
                                         "state.");

            const bool doSet = !wantClear && hasYaw && hasPitch;
            double yaw = 0.0;
            double pitch = 0.0;
            if (doSet)
            {
                yaw = args["yaw"].get<double>();
                pitch = args["pitch"].get<double>();
                if (!std::isfinite(yaw) || !std::isfinite(pitch))
                    return ToolResult::Error("Invalid 'yaw'/'pitch': expected finite numbers in degrees.");
                if (pitch < -90.0 || pitch > 90.0)
                    return ToolResult::Error("Invalid 'pitch': expected an elevation in [-90, 90] degrees "
                                             "(90 = straight up, 0 = horizon, negative = below the horizon).");
            }

            const Json result = server.MarshalRead([&server, wantClear, doSet, yaw, pitch]() -> Json
                                                   {
                SunOverrideResult r;
                if (wantClear)
                {
                    ApplySunClear(r);
                }
                else if (doSet)
                {
                    const SunVec3 dir = SunDirectionFromAngles(yaw, pitch);
                    Renderer3D::SetSunDirectionOverride(glm::vec3(
                        static_cast<f32>(dir.X), static_cast<f32>(dir.Y), static_cast<f32>(dir.Z)));
                    r.Active = true;
                    r.Direction = dir;
                    r.Source = "sunAngle";
                }
                else
                {
                    ApplySunIntrospect(r);
                }
                return FinalizeSunOverride(server, r); });
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_render_compare_golden (Part 4 of #316) ------------------------

        // Resolve a caller-supplied golden path to a safe, repo-relative location
        // under the visual-test artifact root. Rejects absolute paths and ".."
        // traversal so a server that is read-only w.r.t. the project can only ever
        // touch test artifacts (HANDOVER / #316 Tier-0 framing). On success `out`
        // is relative to the editor CWD (OloEditor/), so a bare "foo.png" lands in
        // assets/tests/visual/foo.png — the same root the suite visual tests use.
        std::string ResolveGoldenPath(const std::string& input, std::filesystem::path& out)
        {
            namespace fs = std::filesystem;
            if (input.empty())
                return "Invalid 'goldenPath': must not be empty.";
            const fs::path p = fs::path(input).lexically_normal();
            if (p.is_absolute() || p.has_root_name() || p.has_root_directory())
                return "Invalid 'goldenPath': must be a relative path (no drive letter or leading slash); "
                       "it is resolved under assets/tests/visual/.";
            for (const auto& part : p)
            {
                if (part == "..")
                    return "Invalid 'goldenPath': must not contain '..' (no directory traversal).";
            }
            const fs::path root = fs::path("assets") / "tests" / "visual";
            const fs::path rootNorm = root.lexically_normal();
            // Accept either a bare name/subpath (placed under the root) or a path
            // already rooted at assets/tests/visual.
            const auto mm = std::mismatch(rootNorm.begin(), rootNorm.end(), p.begin(), p.end());
            const bool alreadyUnderRoot = mm.first == rootNorm.end();
            out = (alreadyUnderRoot ? p : (root / p)).lexically_normal();
            // Force a .png extension so the format is unambiguous.
            if (const fs::path ext = out.extension(); ext != ".png" && ext != ".PNG")
                out += ".png";

            // Defence-in-depth against symlink escape: the lexical checks above
            // stop '..'/absolute paths, but a symlinked component inside the
            // artifact root (e.g. a symlinked assets/tests/visual) could still
            // redirect the write outside it. Resolve symlinks and confirm the
            // real path stays under assets/tests/visual/, honouring the "only
            // ever touches test artifacts" guarantee. weakly_canonical resolves
            // the existing prefix (catching a symlinked root) and handles the
            // not-yet-created golden file lexically.
            std::error_code ec;
            const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
            if (ec)
                return "Could not resolve the golden artifact root (assets/tests/visual/).";
            const fs::path canonicalOut = fs::weakly_canonical(out, ec);
            if (ec)
                return "Could not resolve 'goldenPath' to a canonical location.";
            if (const fs::path rel = canonicalOut.lexically_relative(canonicalRoot); rel.empty() || *rel.begin() == "..")
                return "Invalid 'goldenPath': resolves outside assets/tests/visual/ (possible symlink escape).";
            return {};
        }

        // ---- olo_render_compare_golden (main-marshaled; GL readback + diff) -----
        // Capture the viewport (optionally from a fixed pose), diff it against a
        // golden PNG, and return a numeric similarity + pass/fail verdict — the
        // numeric half of CLAUDE.md's "rendering changes MUST be visually verified"
        // loop. When the golden is missing (or 'rebase' is set) the capture is
        // written as the new golden instead of failing, mirroring the test suite's
        // OLOENGINE_GOLDEN_REBASE workflow. The diff math itself lives in the pure,
        // GL-free McpGoldenCompare.h so it is unit-tested headlessly and stays
        // consistent with the GoldenImageTests.cpp suite metric.
        ToolResult Handle_RenderCompareGolden(McpServer& server, const Json& args)
        {
            if (!args.contains("goldenPath") || !args["goldenPath"].is_string())
                return ToolResult::Error("Missing required argument 'goldenPath' (PNG path under assets/tests/visual/).");
            std::filesystem::path goldenPath;
            if (const std::string err = ResolveGoldenPath(args["goldenPath"].get<std::string>(), goldenPath); !err.empty())
                return ToolResult::Error(err);

            if (!server.Context().CaptureViewportPng)
                return ToolResult::Error("Screenshot capture is not available in this editor build.");

            // Optional camera placement for this capture only (same shape as olo_screenshot).
            const bool hasCamera = args.contains("camera");
            const bool hasOrbit = args.contains("orbit");
            if (hasCamera && hasOrbit)
                return ToolResult::Error("Give either 'camera' or 'orbit', not both.");
            CameraRequest request;
            if (hasCamera || hasOrbit)
            {
                if (!CameraContextAvailable(server.Context()))
                    return ToolResult::Error("Camera control is not available in this editor build.");
                const std::string error = hasCamera ? ParsePoseRequest(args["camera"], request)
                                                    : ParseOrbitRequest(args["orbit"], request);
                if (!error.empty())
                    return ToolResult::Error(error);
            }

            // Optional explicit similarity threshold in [0, 1]. Absent => the
            // suite-cascade verdict (RMSE -> SSIM) from GoldenImageTests.cpp.
            std::optional<f32> threshold;
            if (args.contains("threshold"))
            {
                if (!args["threshold"].is_number())
                    return ToolResult::Error("Invalid 'threshold': expected a number in [0, 1].");
                const f32 t = args.value("threshold", -1.0f);
                if (!std::isfinite(t) || t < 0.0f || t > 1.0f)
                    return ToolResult::Error("Invalid 'threshold': expected a finite number in [0, 1].");
                threshold = t;
            }
            const bool rebase = args.value("rebase", false);

            int settleFrames = 2;
            if (args.contains("settleFrames") && args["settleFrames"].is_number_integer())
                settleFrames = static_cast<int>(std::clamp<long long>(args["settleFrames"].get<long long>(), 1, 30));
            int maxWidth = 1024;
            if (args.contains("maxWidth") && args["maxWidth"].is_number_integer())
                maxWidth = static_cast<int>(std::clamp<long long>(args["maxWidth"].get<long long>(), 16, 4096));

            // Save the user's pose and apply the requested one (identical machinery
            // to Handle_Screenshot — the restore happens in the capture job / the
            // error path so it runs exactly once).
            bool posed = false;
            Json savedPose;
            bool waitTimedOut = false;
            if (hasCamera || hasOrbit)
            {
                const Json applied = server.MarshalRead([&server, request]() -> Json
                                                        {
                    const McpCameraPose prior = server.Context().GetCameraPose();
                    ApplyCameraRequest(server.Context(), request);
                    Json j;
                    j["focalPoint"] = Json::array({ prior.FocalPoint.x, prior.FocalPoint.y, prior.FocalPoint.z });
                    j["distance"] = prior.Distance;
                    j["yaw"] = prior.YawRadians;
                    j["pitch"] = prior.PitchRadians;
                    j["fov"] = prior.FovDegrees;
                    j["frame"] = server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0;
                    return j; });
                savedPose = applied;
                posed = true;
            }
            const auto restorePriorPose = [&server, &savedPose]()
            {
                McpCameraPose prior;
                prior.FocalPoint = glm::vec3{ savedPose["focalPoint"][0].get<f32>(),
                                              savedPose["focalPoint"][1].get<f32>(),
                                              savedPose["focalPoint"][2].get<f32>() };
                prior.Distance = savedPose.value("distance", 0.0f);
                prior.YawRadians = savedPose.value("yaw", 0.0f);
                prior.PitchRadians = savedPose.value("pitch", 0.0f);
                prior.FovDegrees = savedPose.value("fov", 45.0f);
                server.Context().RestoreCameraPose(prior);
            };

            // Capture the viewport PNG (and restore the user's camera in the same
            // main-thread job). The bytes are written into `capturedPng` by
            // reference — MarshalRead runs synchronously, so this is safe.
            std::vector<u8> capturedPng;
            try
            {
                if (posed)
                    waitTimedOut = !AwaitRenderedFrames(server, savedPose.value("frame", static_cast<u64>(0)), settleFrames);
                const Json cap = server.MarshalRead([&server, maxWidth, posed, &restorePriorPose, &capturedPng]() -> Json
                                                    {
                    capturedPng = server.Context().CaptureViewportPng(maxWidth);
                    if (posed)
                        restorePriorPose();
                    if (capturedPng.empty())
                        return Json{ { "__error", "Viewport capture failed (no framebuffer or empty viewport)." } };
                    return Json{ { "ok", true } }; });
                if (cap.is_object() && cap.contains("__error"))
                    return ToolResult::Error(cap["__error"].get<std::string>());
            }
            catch (...)
            {
                if (posed)
                {
                    try
                    {
                        (void)server.MarshalRead([&restorePriorPose]() -> Json
                                                 {
                            restorePriorPose();
                            return Json{}; });
                    }
                    catch (...)
                    {
                    }
                }
                throw;
            }

            namespace fs = std::filesystem;
            const std::string goldenPathStr = goldenPath.generic_string();
            const bool goldenExists = fs::exists(goldenPath);

            // Golden-missing / rebase: write the capture as the new golden and
            // report it (a test-artifact write under assets/tests/visual/, never
            // the user's scene/assets — see #316 Tier-0 framing).
            if (!goldenExists || rebase)
            {
                std::error_code ec;
                if (goldenPath.has_parent_path())
                    fs::create_directories(goldenPath.parent_path(), ec);
                std::ofstream f(goldenPath, std::ios::binary | std::ios::trunc);
                if (f)
                    f.write(reinterpret_cast<const char*>(capturedPng.data()), static_cast<std::streamsize>(capturedPng.size()));
                if (!f)
                    return ToolResult::Error("Failed to write golden PNG to: " + goldenPathStr);

                Json j;
                j["goldenPath"] = goldenPathStr;
                j["created"] = true;
                j["rebased"] = goldenExists; // existed AND we overwrote it
                j["bytes"] = static_cast<u64>(capturedPng.size());
                j["message"] = (goldenExists ? "Rebased golden at " : "Golden created at ") + goldenPathStr +
                               " — captured the current frame as the new baseline. Re-run the tool (without 'rebase', "
                               "same capture size) to compare against it.";
                if (waitTimedOut)
                    j["warning"] = "Timed out waiting for the new camera pose to render; the golden may be a stale frame.";

                ToolResult result;
                result.Content = Json::array({ Json{ { "type", "text" }, { "text", j.dump(2) } },
                                               Json{ { "type", "image" }, { "data", Base64Encode(capturedPng) }, { "mimeType", "image/png" } } });
                result.IsError = false;
                return result;
            }

            // Compare path: decode both PNGs to RGBA8 and run the pure diff core.
            // stb's flip flags can be left set by production asset-loading paths
            // (Model.cpp / AssetSerializer.cpp set the thread-local one), which
            // would load an image upside-down and produce a false mismatch — reset
            // both global and thread-local flags so decode orientation is
            // deterministic and matches how goldens are written (no flip). Same
            // precaution as GoldenImageTests.cpp::CompareOrBootstrap.
            ::stbi_set_flip_vertically_on_load(0);
            ::stbi_set_flip_vertically_on_load_thread(0);

            int aw = 0, ah = 0, ach = 0;
            stbi_uc* actualRaw = ::stbi_load_from_memory(capturedPng.data(), static_cast<int>(capturedPng.size()), &aw, &ah, &ach, 4);
            if (actualRaw == nullptr)
                return ToolResult::Error("Failed to decode the captured frame PNG in memory.");
            std::vector<u8> actual(actualRaw, actualRaw + (static_cast<std::size_t>(aw) * ah * 4));
            ::stbi_image_free(actualRaw);

            int gw = 0, gh = 0, gch = 0;
            stbi_uc* goldenRaw = ::stbi_load(goldenPathStr.c_str(), &gw, &gh, &gch, 4);
            if (goldenRaw == nullptr)
            {
                const char* reason = ::stbi_failure_reason();
                return ToolResult::Error("Failed to read/decode the golden PNG at " + goldenPathStr + ": " +
                                         (reason ? reason : "unknown error"));
            }
            std::vector<u8> golden(goldenRaw, goldenRaw + (static_cast<std::size_t>(gw) * gh * 4));
            ::stbi_image_free(goldenRaw);

            const GoldenCompare::CompareResult cmp =
                GoldenCompare::Compare(actual, static_cast<u32>(aw), static_cast<u32>(ah),
                                       golden, static_cast<u32>(gw), static_cast<u32>(gh), threshold);

            Json j;
            j["goldenPath"] = goldenPathStr;
            j["created"] = false;
            j["pass"] = cmp.Pass;
            j["dimensionsMatch"] = cmp.DimensionsMatch;
            j["actual"] = Json{ { "width", cmp.ActualWidth }, { "height", cmp.ActualHeight } };
            j["golden"] = Json{ { "width", cmp.GoldenWidth }, { "height", cmp.GoldenHeight } };
            if (cmp.DimensionsMatch)
            {
                j["similarity"] = cmp.Similarity;
                j["ssim"] = cmp.Ssim;
                j["rmse"] = cmp.Rmse;
                j["mse"] = cmp.Mse;
                j["threshold"] = cmp.Threshold;
                j["thresholdMode"] = cmp.ThresholdMode;
                j["mismatchPixels"] = cmp.MismatchPixels;
                j["totalPixels"] = cmp.TotalPixels;
                j["maxChannelDelta"] = cmp.MaxChannelDelta;
                j["worstPixel"] = Json{ { "x", cmp.WorstX }, { "y", cmp.WorstY } };
            }
            j["message"] = cmp.Message;
            if (waitTimedOut)
                j["warning"] = "Timed out waiting for the new camera pose to render; the comparison may use a stale frame.";

            ToolResult result;
            result.Content = Json::array({ Json{ { "type", "text" }, { "text", j.dump(2) } },
                                           Json{ { "type", "image" }, { "data", Base64Encode(capturedPng) }, { "mimeType", "image/png" } } });
            result.IsError = false;
            return result;
        }

        // ======================================================================
        // olo_physics_* — physics introspection + "explain" tools (#306 item A).
        //
        // The layer matrix is static / mutex-guarded registry data and reads
        // lock-safe from the handler thread. Everything else touches the live
        // Jolt scene + EnTT registry, so it runs inside MarshalRead on the
        // editor's main thread, exactly like the olo_scene_* tools. All are
        // strictly read-only — no body, layer, or component is ever mutated.
        // ======================================================================

        // ObjectLayers built-in count + names; user-defined layers map to object
        // layers ObjectLayers::NUM_LAYERS + layerId (see JoltLayerInterface).
        std::string ObjectLayerName(JPH::ObjectLayer layer)
        {
            switch (layer)
            {
                case ObjectLayers::NON_MOVING:
                    return "NON_MOVING";
                case ObjectLayers::MOVING:
                    return "MOVING";
                case ObjectLayers::TRIGGER:
                    return "TRIGGER";
                case ObjectLayers::CHARACTER:
                    return "CHARACTER";
                case ObjectLayers::DEBRIS:
                    return "DEBRIS";
                default:
                    break;
            }
            // layer >= NUM_LAYERS here, so the subtraction never underflows.
            const u32 userId = static_cast<u32>(layer) - ObjectLayers::NUM_LAYERS;
            if (const PhysicsLayer pl = PhysicsLayerManager::GetLayer(userId); pl.IsValid())
                return pl.m_Name;
            return "Layer#" + std::to_string(static_cast<u32>(layer));
        }

        PhysicsExplain::BodyType MapBodyType(EBodyType type)
        {
            switch (type)
            {
                case EBodyType::Dynamic:
                    return PhysicsExplain::BodyType::Dynamic;
                case EBodyType::Kinematic:
                    return PhysicsExplain::BodyType::Kinematic;
                case EBodyType::Static:
                default:
                    return PhysicsExplain::BodyType::Static;
            }
        }

        PhysicsExplain::BodyType MapBodyType(BodyType3D type)
        {
            switch (type)
            {
                case BodyType3D::Dynamic:
                    return PhysicsExplain::BodyType::Dynamic;
                case BodyType3D::Kinematic:
                    return PhysicsExplain::BodyType::Kinematic;
                case BodyType3D::Static:
                default:
                    return PhysicsExplain::BodyType::Static;
            }
        }

        EBodyType ToEBodyType(BodyType3D type)
        {
            switch (type)
            {
                case BodyType3D::Dynamic:
                    return EBodyType::Dynamic;
                case BodyType3D::Kinematic:
                    return EBodyType::Kinematic;
                case BodyType3D::Static:
                default:
                    return EBodyType::Static;
            }
        }

        bool HasAnyCollider3D(Entity entity)
        {
            return entity.HasComponent<BoxCollider3DComponent>() ||
                   entity.HasComponent<SphereCollider3DComponent>() ||
                   entity.HasComponent<CapsuleCollider3DComponent>() ||
                   entity.HasComponent<MeshCollider3DComponent>() ||
                   entity.HasComponent<ConvexMeshCollider3DComponent>() ||
                   entity.HasComponent<TriangleMeshCollider3DComponent>();
        }

        // Describe an entity's authored collision shape(s) from its collider
        // components. Most entities have exactly one; an array keeps compound
        // setups honest. Main-thread only (reads the registry).
        Json DescribeColliders(Entity entity)
        {
            const auto vec3 = [](const glm::vec3& v)
            { return Json::array({ v.x, v.y, v.z }); };

            Json arr = Json::array();
            if (entity.HasComponent<BoxCollider3DComponent>())
            {
                const auto& c = entity.GetComponent<BoxCollider3DComponent>();
                arr.push_back(Json{ { "type", "Box" }, { "halfExtents", vec3(c.m_HalfExtents) }, { "offset", vec3(c.m_Offset) } });
            }
            if (entity.HasComponent<SphereCollider3DComponent>())
            {
                const auto& c = entity.GetComponent<SphereCollider3DComponent>();
                arr.push_back(Json{ { "type", "Sphere" }, { "radius", c.m_Radius }, { "offset", vec3(c.m_Offset) } });
            }
            if (entity.HasComponent<CapsuleCollider3DComponent>())
            {
                const auto& c = entity.GetComponent<CapsuleCollider3DComponent>();
                arr.push_back(Json{ { "type", "Capsule" }, { "radius", c.m_Radius }, { "halfHeight", c.m_HalfHeight }, { "offset", vec3(c.m_Offset) } });
            }
            if (entity.HasComponent<MeshCollider3DComponent>())
            {
                const auto& c = entity.GetComponent<MeshCollider3DComponent>();
                arr.push_back(Json{ { "type", "Mesh" }, { "colliderAsset", std::to_string(static_cast<u64>(c.m_ColliderAsset)) } });
            }
            if (entity.HasComponent<ConvexMeshCollider3DComponent>())
            {
                const auto& c = entity.GetComponent<ConvexMeshCollider3DComponent>();
                arr.push_back(Json{ { "type", "ConvexMesh" }, { "colliderAsset", std::to_string(static_cast<u64>(c.m_ColliderAsset)) } });
            }
            if (entity.HasComponent<TriangleMeshCollider3DComponent>())
            {
                const auto& c = entity.GetComponent<TriangleMeshCollider3DComponent>();
                arr.push_back(Json{ { "type", "TriangleMesh" }, { "colliderAsset", std::to_string(static_cast<u64>(c.m_ColliderAsset)) } });
            }
            return arr;
        }

        std::string EntityName(Entity entity)
        {
            return entity.HasComponent<TagComponent>() ? entity.GetComponent<TagComponent>().Tag : std::string{};
        }

        // A live, initialized 3D physics scene, or nullptr. Call on the main thread.
        JoltScene* GetRunningPhysics(const Ref<Scene>& scene)
        {
            if (!scene)
                return nullptr;
            JoltScene* physics = scene->GetPhysicsScene();
            return (physics && physics->IsInitialized()) ? physics : nullptr;
        }

        // ---- olo_physics_layer_matrix (lock-safe) ------------------------------
        // Dumps the object-layer collision matrix the simulation actually uses:
        // the five built-in Jolt object layers plus every user-defined
        // PhysicsLayerManager layer, with pairwise collide/no-collide from the
        // real ObjectLayerPairFilter. Works in Edit mode too (static registry).
        ToolResult Handle_PhysicsLayerMatrix(McpServer& /*server*/, const Json& /*args*/)
        {
            // Collect the object layers in play: built-ins 0..NUM_LAYERS-1, then
            // each valid user layer at NUM_LAYERS + layerId.
            struct LayerEntry
            {
                JPH::ObjectLayer ObjectLayer;
                std::string Name;
                bool IsUser;
                u32 UserLayerId;
            };
            std::vector<LayerEntry> layers;
            for (JPH::ObjectLayer i = 0; i < ObjectLayers::NUM_LAYERS; ++i)
                layers.push_back({ i, ObjectLayerName(i), false, 0 });

            const std::vector<PhysicsLayer> userLayers = PhysicsLayerManager::GetLayers();
            for (const PhysicsLayer& pl : userLayers)
            {
                if (!pl.IsValid())
                    continue;
                layers.push_back({ static_cast<JPH::ObjectLayer>(ObjectLayers::NUM_LAYERS + pl.m_LayerID), pl.m_Name, true, pl.m_LayerID });
            }

            const ObjectLayerPairFilter& filter = JoltLayerInterface::GetObjectLayerPairFilter();

            Json objectLayers = Json::array();
            for (const LayerEntry& e : layers)
            {
                Json le{ { "objectLayer", static_cast<u32>(e.ObjectLayer) }, { "name", e.Name }, { "kind", e.IsUser ? "user" : "builtin" } };
                if (e.IsUser)
                    le["userLayerId"] = e.UserLayerId;
                objectLayers.push_back(std::move(le));
            }

            // Upper triangle (incl. self-pairs) — the matrix is symmetric.
            Json matrix = Json::array();
            for (sizet a = 0; a < layers.size(); ++a)
            {
                for (sizet b = a; b < layers.size(); ++b)
                {
                    const bool collides = filter.ShouldCollide(layers[a].ObjectLayer, layers[b].ObjectLayer);
                    matrix.push_back(Json{ { "a", layers[a].Name }, { "b", layers[b].Name }, { "collides", collides } });
                }
            }

            Json userDefined = Json::array();
            for (const PhysicsLayer& pl : userLayers)
            {
                if (!pl.IsValid())
                    continue;
                Json collidesWith = Json::array();
                for (const PhysicsLayer& other : userLayers)
                {
                    if (!other.IsValid())
                        continue;
                    if (PhysicsLayerManager::ShouldCollide(pl.m_LayerID, other.m_LayerID))
                        collidesWith.push_back(other.m_Name);
                }
                userDefined.push_back(Json{ { "id", pl.m_LayerID },
                                            { "name", pl.m_Name },
                                            { "bitValue", pl.m_BitValue },
                                            { "collidesWithSelf", pl.m_CollidesWithSelf },
                                            { "collidesWith", std::move(collidesWith) } });
            }

            Json j{ { "objectLayers", std::move(objectLayers) },
                    { "collisionMatrix", std::move(matrix) },
                    { "userDefinedLayers", std::move(userDefined) },
                    { "note",
                      "Built-in object layers and their collide rules are fixed (NON_MOVING vs NON_MOVING never "
                      "collide; MOVING collides with all; TRIGGER/CHARACTER/DEBRIS have specific rules). "
                      "User-defined layers are authored in the editor's Physics settings." } };
            return ToolResult::Text(j.dump(2));
        }

        // ---- olo_physics_list_colliders (main-marshaled) -----------------------
        // Every entity with a Rigidbody3DComponent: authored body type / layer /
        // trigger / collider shapes, plus live body state (object layer, position,
        // awake/asleep) when physics is running. Paginated like list_entities.
        ToolResult Handle_PhysicsListColliders(McpServer& server, const Json& args)
        {
            int page = 0;
            int pageSize = 50;
            if (args.contains("page") && args["page"].is_number_integer())
                page = static_cast<int>(std::max<long long>(0, args["page"].get<long long>()));
            if (args.contains("pageSize") && args["pageSize"].is_number_integer())
                pageSize = static_cast<int>(std::clamp<long long>(args["pageSize"].get<long long>(), 1, 200));

            Json result = server.MarshalRead([&server, page, pageSize]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                {
                    j["error"] = "No active scene";
                    return j;
                }
                JoltScene* physics = GetRunningPhysics(scene);
                j["physicsRunning"] = physics != nullptr;

                std::vector<Entity> bodies;
                for (const auto handle : scene->GetAllEntitiesWith<Rigidbody3DComponent>())
                    bodies.push_back(Entity{ handle, scene.get() });

                const auto total = static_cast<int>(bodies.size());
                const long long start = static_cast<long long>(page) * pageSize;
                Json colliders = Json::array();
                for (long long i = start; i < total && i < start + pageSize; ++i)
                {
                    Entity entity = bodies[static_cast<sizet>(i)];
                    const auto& rb = entity.GetComponent<Rigidbody3DComponent>();

                    Json e;
                    e["id"] = UuidToString(entity.GetUUID());
                    e["name"] = EntityName(entity);
                    e["bodyType"] = PhysicsExplain::BodyTypeName(MapBodyType(rb.m_Type));
                    e["layerId"] = rb.m_LayerID;
                    e["isTrigger"] = rb.m_IsTrigger;
                    e["disableGravity"] = rb.m_DisableGravity;
                    e["colliders"] = DescribeColliders(entity);
                    e["hasCollider"] = HasAnyCollider3D(entity);

                    if (physics)
                    {
                        if (Ref<JoltBody> body = physics->GetBody(entity); body && body->IsValid())
                        {
                            const JPH::ObjectLayer objLayer = physics->GetBodyInterface().GetObjectLayer(body->GetBodyID());
                            Json live;
                            live["bodyType"] = PhysicsExplain::BodyTypeName(MapBodyType(body->GetBodyType()));
                            live["objectLayer"] = static_cast<u32>(objLayer);
                            live["objectLayerName"] = ObjectLayerName(objLayer);
                            live["isTrigger"] = body->IsTrigger();
                            const glm::vec3 p = body->GetPosition();
                            live["position"] = Json::array({ p.x, p.y, p.z });
                            live["active"] = body->IsActive();
                            live["sleeping"] = body->IsSleeping();
                            e["live"] = std::move(live);
                        }
                        else
                        {
                            e["live"] = nullptr; // authored but no live body (creation failed / added at runtime)
                        }
                    }
                    colliders.push_back(std::move(e));
                }

                j["total"] = total;
                j["page"] = page;
                j["pageSize"] = pageSize;
                j["returned"] = static_cast<int>(colliders.size());
                if (start + pageSize < total)
                    j["nextPage"] = page + 1;
                j["colliders"] = std::move(colliders);
                return j; });

            if (result.contains("error"))
                return ToolResult::Error(result["error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_physics_contacts (main-marshaled) -----------------------------
        // The entity pairs whose bodies are touching right now, from the contact
        // listener's active-contact set (deduplicated per entity pair).
        ToolResult Handle_PhysicsContacts(McpServer& server, const Json& args)
        {
            int maxResults = 200;
            if (args.contains("maxResults") && args["maxResults"].is_number_integer())
                maxResults = static_cast<int>(std::clamp<long long>(args["maxResults"].get<long long>(), 1, 2000));

            Json result = server.MarshalRead([&server, maxResults]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                {
                    j["error"] = "No active scene";
                    return j;
                }
                JoltScene* physics = GetRunningPhysics(scene);
                j["physicsRunning"] = physics != nullptr;
                if (!physics)
                {
                    j["activeContactCount"] = 0;
                    j["contacts"] = Json::array();
                    j["note"] = "Physics is not running — enter Play mode to observe live contacts.";
                    return j;
                }

                const std::vector<std::pair<UUID, UUID>> pairs = physics->GetActiveContactPairs();
                j["activeContactCount"] = static_cast<std::uint64_t>(pairs.size());

                Json contacts = Json::array();
                const auto describe = [&scene](UUID id) -> Json
                {
                    Json e{ { "id", UuidToString(id) } };
                    if (const auto entityOpt = scene->TryGetEntityWithUUID(id))
                        e["name"] = EntityName(*entityOpt);
                    return e;
                };
                int emitted = 0;
                for (const auto& [a, b] : pairs)
                {
                    if (emitted++ >= maxResults)
                        break;
                    contacts.push_back(Json{ { "a", describe(a) }, { "b", describe(b) } });
                }
                if (static_cast<int>(pairs.size()) > maxResults)
                    j["truncated"] = true;
                j["returned"] = static_cast<int>(contacts.size());
                j["contacts"] = std::move(contacts);
                return j; });

            if (result.contains("error"))
                return ToolResult::Error(result["error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_physics_raycast (main-marshaled) ------------------------------
        // Cast a ray through the live physics world. Origin + (direction | to).
        // Returns the closest hit by default, or up to maxHits ordered hits.
        ToolResult Handle_PhysicsRaycast(McpServer& server, const Json& args)
        {
            glm::vec3 origin{ 0.0f };
            if (!args.contains("origin") || !ParseVec3(args["origin"], origin))
                return ToolResult::Error("Missing or invalid 'origin': expected [x, y, z] finite numbers.");

            glm::vec3 direction{ 0.0f, 0.0f, 1.0f };
            glm::vec3 toPoint{ 0.0f };
            const bool hasDirKey = args.contains("direction");
            const bool hasToKey = args.contains("to");
            // Require exactly one of 'direction' / 'to', and reject a present-but-
            // malformed field rather than silently falling back — keep the query
            // intent unambiguous and surface bad input instead of ignoring it.
            if (hasDirKey && hasToKey)
                return ToolResult::Error("Provide either 'direction' or 'to', not both.");
            if (!hasDirKey && !hasToKey)
                return ToolResult::Error("Provide either 'direction' [x,y,z] or 'to' [x,y,z].");
            if (hasDirKey && !ParseVec3(args["direction"], direction))
                return ToolResult::Error("Invalid 'direction': expected [x, y, z] finite numbers.");
            if (hasToKey && !ParseVec3(args["to"], toPoint))
                return ToolResult::Error("Invalid 'to': expected [x, y, z] finite numbers.");
            const bool hasTo = hasToKey;
            if (hasDirKey && glm::length(direction) <= 0.0f)
                return ToolResult::Error("'direction' must be non-zero.");

            f32 maxDistance = 500.0f;
            if (args.contains("maxDistance") && args["maxDistance"].is_number())
            {
                const f32 d = args["maxDistance"].get<f32>();
                if (std::isfinite(d) && d > 0.0f)
                    maxDistance = d;
            }

            int maxHits = 1;
            if (args.contains("maxHits") && args["maxHits"].is_number_integer())
                maxHits = static_cast<int>(std::clamp<long long>(args["maxHits"].get<long long>(), 1, 64));

            Json result = server.MarshalRead([&server, origin, direction, toPoint, hasTo, maxDistance, maxHits]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };
                JoltScene* physics = GetRunningPhysics(scene);
                if (!physics)
                    return Json{ { "__error", "Physics is not running — enter Play mode to run physics queries." } };

                RayCastInfo info = hasTo ? SceneQueryUtils::CreateRayInfo(origin, toPoint)
                                         : RayCastInfo(origin, glm::normalize(direction), maxDistance);

                const auto describeHit = [&scene](const SceneQueryHit& hit) -> Json
                {
                    Json h;
                    h["entity"] = Json{ { "id", UuidToString(hit.m_HitEntity) } };
                    if (const auto entityOpt = scene->TryGetEntityWithUUID(hit.m_HitEntity))
                        h["entity"]["name"] = EntityName(*entityOpt);
                    h["position"] = Json::array({ hit.m_Position.x, hit.m_Position.y, hit.m_Position.z });
                    h["normal"] = Json::array({ hit.m_Normal.x, hit.m_Normal.y, hit.m_Normal.z });
                    h["distance"] = hit.m_Distance;
                    return h;
                };

                Json hits = Json::array();
                if (maxHits <= 1)
                {
                    SceneQueryHit hit;
                    if (physics->CastRay(info, hit))
                        hits.push_back(describeHit(hit));
                }
                else
                {
                    std::vector<SceneQueryHit> buffer(static_cast<sizet>(maxHits));
                    const i32 count = physics->CastRayMultiple(info, buffer.data(), maxHits);
                    for (i32 k = 0; k < count; ++k)
                        hits.push_back(describeHit(buffer[static_cast<sizet>(k)]));
                }

                j["origin"] = Json::array({ info.m_Origin.x, info.m_Origin.y, info.m_Origin.z });
                j["direction"] = Json::array({ info.m_Direction.x, info.m_Direction.y, info.m_Direction.z });
                j["maxDistance"] = info.m_MaxDistance;
                j["hitCount"] = static_cast<int>(hits.size());
                j["hits"] = std::move(hits);
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_physics_overlap (main-marshaled) ------------------------------
        // Find bodies overlapping a sphere (default) or box at a world point.
        ToolResult Handle_PhysicsOverlap(McpServer& server, const Json& args)
        {
            glm::vec3 origin{ 0.0f };
            if (!args.contains("origin") || !ParseVec3(args["origin"], origin))
                return ToolResult::Error("Missing or invalid 'origin': expected [x, y, z] finite numbers.");

            // Box if halfExtents given, else sphere (radius, default 0.5). A present-
            // but-malformed halfExtents is an error, not a silent downgrade to sphere.
            glm::vec3 halfExtents{ 0.5f };
            const bool isBox = args.contains("halfExtents");
            if (isBox && !ParseVec3(args["halfExtents"], halfExtents))
                return ToolResult::Error("Invalid 'halfExtents': expected [x, y, z] finite numbers.");
            if (isBox && (halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f))
                return ToolResult::Error("Invalid 'halfExtents': all components must be positive (a box needs non-zero size).");
            f32 radius = 0.5f;
            if (args.contains("radius") && args["radius"].is_number())
            {
                const f32 r = args["radius"].get<f32>();
                if (std::isfinite(r) && r > 0.0f)
                    radius = r;
            }

            int maxHits = 32;
            if (args.contains("maxHits") && args["maxHits"].is_number_integer())
                maxHits = static_cast<int>(std::clamp<long long>(args["maxHits"].get<long long>(), 1, 256));

            Json result = server.MarshalRead([&server, origin, halfExtents, radius, isBox, maxHits]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };
                JoltScene* physics = GetRunningPhysics(scene);
                if (!physics)
                    return Json{ { "__error", "Physics is not running — enter Play mode to run physics queries." } };

                std::vector<SceneQueryHit> buffer(static_cast<sizet>(maxHits));
                i32 count = 0;
                if (isBox)
                {
                    BoxOverlapInfo info(origin, halfExtents);
                    count = physics->OverlapBox(info, buffer.data(), maxHits);
                }
                else
                {
                    SphereOverlapInfo info(origin, radius);
                    count = physics->OverlapSphere(info, buffer.data(), maxHits);
                }

                Json overlaps = Json::array();
                for (i32 k = 0; k < count; ++k)
                {
                    const SceneQueryHit& hit = buffer[static_cast<sizet>(k)];
                    Json e{ { "id", UuidToString(hit.m_HitEntity) } };
                    if (const auto entityOpt = scene->TryGetEntityWithUUID(hit.m_HitEntity))
                        e["name"] = EntityName(*entityOpt);
                    e["position"] = Json::array({ hit.m_Position.x, hit.m_Position.y, hit.m_Position.z });
                    overlaps.push_back(std::move(e));
                }

                j["shape"] = isBox ? "box" : "sphere";
                j["origin"] = Json::array({ origin.x, origin.y, origin.z });
                if (isBox)
                    j["halfExtents"] = Json::array({ halfExtents.x, halfExtents.y, halfExtents.z });
                else
                    j["radius"] = radius;
                j["overlapCount"] = static_cast<int>(overlaps.size());
                if (count >= maxHits)
                    j["truncated"] = true;
                j["overlaps"] = std::move(overlaps);
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_physics_why_no_collision (main-marshaled) ---------------------
        // The headline tool: explain why two entities are NOT colliding (the
        // "player falls through the floor" case). Gathers the collision-relevant
        // facts off the live sim, then runs the pure ExplainWhyNoCollision cascade.
        ToolResult Handle_PhysicsWhyNoCollision(McpServer& server, const Json& args)
        {
            if (!args.contains("a") || !args.contains("b"))
                return ToolResult::Error("Missing required arguments 'a' and 'b' (entity UUIDs).");
            u64 idA = 0;
            u64 idB = 0;
            if (!ParseUuid(args["a"], idA))
                return ToolResult::Error("Invalid 'a': expected a UUID as a string or number.");
            if (!ParseUuid(args["b"], idB))
                return ToolResult::Error("Invalid 'b': expected a UUID as a string or number.");

            Json result = server.MarshalRead([&server, idA, idB]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };
                JoltScene* physics = GetRunningPhysics(scene);

                PhysicsExplain::WhyNoCollisionInput in;
                in.SameEntity = (idA == idB);
                in.PhysicsRunning = physics != nullptr;

                // Gather one side's facts. Returns the live body (or null) so the
                // caller can compute the cross-body layer/bounds checks.
                Ref<JoltBody> bodyA;
                Ref<JoltBody> bodyB;
                const auto gather = [&](u64 id, PhysicsExplain::EntityPhysicsFacts& facts, Ref<JoltBody>& outBody)
                {
                    const auto entityOpt = scene->TryGetEntityWithUUID(UUID(id));
                    facts.EntityExists = entityOpt.has_value();
                    if (!facts.EntityExists)
                        return;
                    Entity entity = *entityOpt;
                    facts.HasRigidbody = entity.HasComponent<Rigidbody3DComponent>();
                    facts.HasCollider = HasAnyCollider3D(entity);

                    if (facts.HasRigidbody)
                    {
                        const auto& rb = entity.GetComponent<Rigidbody3DComponent>();
                        facts.Type = MapBodyType(rb.m_Type);
                        facts.IsTrigger = rb.m_IsTrigger;
                        facts.LayerId = rb.m_LayerID;
                        // Authored object-layer name as a fallback when no live body.
                        facts.LayerName = ObjectLayerName(JoltLayerInterface::GetObjectLayerForCollider(rb.m_LayerID, ToEBodyType(rb.m_Type), rb.m_IsTrigger));
                    }

                    if (physics)
                    {
                        if (Ref<JoltBody> body = physics->GetBody(entity); body && body->IsValid())
                        {
                            outBody = body;
                            facts.HasBody = true;
                            facts.Type = MapBodyType(body->GetBodyType());
                            facts.IsTrigger = body->IsTrigger();
                            const JPH::ObjectLayer objLayer = physics->GetBodyInterface().GetObjectLayer(body->GetBodyID());
                            facts.LayerName = ObjectLayerName(objLayer);
                        }
                    }
                };

                gather(idA, in.A, bodyA);
                gather(idB, in.B, bodyB);

                // Cross-body checks only make sense once both have live bodies.
                if (physics && bodyA && bodyB && bodyA->IsValid() && bodyB->IsValid())
                {
                    const JPH::BodyInterface& bi = physics->GetBodyInterface();
                    const JPH::ObjectLayer layerA = bi.GetObjectLayer(bodyA->GetBodyID());
                    const JPH::ObjectLayer layerB = bi.GetObjectLayer(bodyB->GetBodyID());
                    in.LayersCollide = JoltLayerInterface::GetObjectLayerPairFilter().ShouldCollide(layerA, layerB);

                    const JPH::BodyLockInterface& lockInterface = physics->GetBodyLockInterface();
                    JPH::AABox boundsA;
                    JPH::AABox boundsB;
                    bool haveA = false;
                    bool haveB = false;
                    {
                        JPH::BodyLockRead lockA(lockInterface, bodyA->GetBodyID());
                        if (lockA.Succeeded())
                        {
                            boundsA = lockA.GetBody().GetWorldSpaceBounds();
                            haveA = true;
                        }
                    }
                    {
                        JPH::BodyLockRead lockB(lockInterface, bodyB->GetBodyID());
                        if (lockB.Succeeded())
                        {
                            boundsB = lockB.GetBody().GetWorldSpaceBounds();
                            haveB = true;
                        }
                    }
                    in.BoundsOverlap = haveA && haveB && boundsA.Overlaps(boundsB);
                }

                const PhysicsExplain::WhyNoCollisionVerdict verdict = PhysicsExplain::ExplainWhyNoCollision(in);

                const auto factsJson = [](const PhysicsExplain::EntityPhysicsFacts& f) -> Json
                {
                    return Json{ { "entityExists", f.EntityExists },
                                 { "hasRigidbody", f.HasRigidbody },
                                 { "hasCollider", f.HasCollider },
                                 { "hasLiveBody", f.HasBody },
                                 { "bodyType", PhysicsExplain::BodyTypeName(f.Type) },
                                 { "isTrigger", f.IsTrigger },
                                 { "layerId", f.LayerId },
                                 { "layerName", f.LayerName } };
                };

                j["a"] = UuidToString(UUID(idA));
                j["b"] = UuidToString(UUID(idB));
                j["reasonCode"] = verdict.ReasonCode;
                j["summary"] = verdict.Summary;
                j["canCollide"] = verdict.CanCollide;
                j["checks"] = verdict.Checks;
                j["facts"] = Json{ { "a", factsJson(in.A) }, { "b", factsJson(in.B) }, { "layersCollide", in.LayersCollide }, { "boundsOverlap", in.BoundsOverlap } };
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // Keep the schema's layer cap aligned with the engine's object-layer budget.
        // A user layer id maps to the Jolt object layer ObjectLayers::NUM_LAYERS + id,
        // and the whole budget is JoltUtils::kMaxJoltLayers; the largest authored id
        // that still fits a valid slot is kMaxJoltLayers - NUM_LAYERS - 1. The shared
        // header keeps kMaxLayerId Jolt-free (it compiles into the test binary), so pin
        // it here — where the engine constants are in scope — and fail the build if the
        // budget ever changes rather than silently letting an out-of-budget layer through.
        static_assert(SetCollisionLayer::kMaxLayerId == JoltUtils::kMaxJoltLayers - ObjectLayers::NUM_LAYERS - 1,
                      "olo_set_collision_layer layer cap is out of sync with the Jolt object-layer budget");

        // ---- olo_set_collision_layer (main-marshaled; PROJECT WRITE) -----------
        // The first consented, undoable write tool (#306 item C, first slice): set an
        // entity's physics-body collision layer through the editor's undo stack, so an
        // agent can try a fix the user can Ctrl-Z. The mutation is gated at dispatch by
        // the "Allow writes" session toggle (ToolDef::ProjectWrite); the shared apply
        // logic lives in McpSetCollisionLayer.h so it is unit-tested at the dispatch
        // seam without this TU. The command is built + executed inside the MarshalRead
        // job, i.e. on the main thread, since it touches the EnTT registry and the
        // editor command stack.
        ToolResult Handle_SetCollisionLayer(McpServer& server, const Json& args)
        {
            if (!server.Context().GetActiveScene || !server.Context().GetCommandHistory)
                return ToolResult::Error("Project writes are not available in this editor build.");

            u64 entityUuid = 0;
            u32 layer = 0;
            if (const auto error = SetCollisionLayer::ParseArgs(args, entityUuid, layer))
                return ToolResult::Error(*error);

            const Json result = server.MarshalRead([&server, entityUuid, layer]() -> Json
                                                   {
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                CommandHistory* history = server.Context().GetCommandHistory
                                              ? server.Context().GetCommandHistory()
                                              : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };
                if (!history)
                    return Json{ { "__error", "No editor command history available." } };

                const SetCollisionLayer::ApplyResult applied = SetCollisionLayer::Apply(scene, *history, entityUuid, layer);
                if (!applied.Ok)
                    return Json{ { "__error", applied.Error } };
                return applied.Data; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_entity_set_field (main-marshaled; PROJECT WRITE) --------------
        // The GENERIC consented, undoable write tool (#306 item C, second slice):
        // set ANY registered component field by (component, field, value) through the
        // editor's undo stack — the catch-all successor to olo_set_collision_layer's
        // one-tool-per-field shape. Gated at dispatch by the "Allow writes" session
        // toggle (ToolDef::ProjectWrite); the shared reflect+coerce+apply core lives
        // in McpGenericFieldWrite.h so it is unit-tested at the dispatch seam without
        // this TU. The command is built + executed inside the MarshalRead job, i.e.
        // on the main thread, since it touches the EnTT registry and command stack.
        ToolResult Handle_EntitySetField(McpServer& server, const Json& args)
        {
            if (!server.Context().GetActiveScene || !server.Context().GetCommandHistory)
                return ToolResult::Error("Project writes are not available in this editor build.");

            u64 entityUuid = 0;
            std::string component;
            std::string field;
            Json value;
            if (const auto error = GenericFieldWrite::ParseArgs(args, entityUuid, component, field, value))
                return ToolResult::Error(*error);

            const Json result = server.MarshalRead([&server, entityUuid, component, field, value]() -> Json
                                                   {
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                CommandHistory* history = server.Context().GetCommandHistory
                                              ? server.Context().GetCommandHistory()
                                              : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };
                if (!history)
                    return Json{ { "__error", "No editor command history available." } };

                const GenericFieldWrite::ApplyResult applied =
                    GenericFieldWrite::Apply(scene, *history, entityUuid, component, field, value);
                if (!applied.Ok)
                    return Json{ { "__error", applied.Error } };
                return applied.Data; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_entity_list_fields (main-marshaled; read-only) ----------------
        // The discovery half of olo_entity_set_field: for one entity, list the
        // writable component fields (those the entity actually has) with their type
        // and current value, so an agent learns the exact (component, field) names +
        // value shapes before issuing a write. Read-only (not ProjectWrite).
        ToolResult Handle_EntityListFields(McpServer& server, const Json& args)
        {
            if (!server.Context().GetActiveScene)
                return ToolResult::Error("Scene reads are not available in this editor build.");

            if (!args.contains("entity"))
                return ToolResult::Error("Missing required argument 'entity' (entity UUID).");
            u64 entityUuid = 0;
            if (!ParseUuid(args["entity"], entityUuid))
                return ToolResult::Error("Invalid 'entity': expected a UUID as a string or number.");
            const std::string componentFilter =
                (args.contains("component") && args["component"].is_string()) ? args["component"].get<std::string>() : std::string();

            const Json result = server.MarshalRead([&server, entityUuid, componentFilter]() -> Json
                                                   {
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                bool entityFound = false;
                return GenericFieldWrite::ListFields(scene, entityUuid, componentFilter, entityFound); });

            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_reload_script (main-marshaled; PROJECT WRITE) -----------------
        // Reload the C# script assembly — the scripting counterpart of
        // olo_shader_reload's inner loop: build the game assembly, reload it over MCP,
        // and the editor picks up the new script code without a restart. Drives the
        // exact ScriptEngine::ReloadAssembly() path the editor's Script ▸ Reload
        // assembly (Ctrl+R) uses, via the ReloadScriptAssembly context hook (so the
        // engine/Mono call stays out of the test binary and a headless host can leave
        // it null). Gated at dispatch by the "Allow writes" session toggle
        // (ToolDef::ProjectWrite): reloading runs the user's freshly-built assembly
        // code, so it crosses the read-only line by design. The reload runs inside the
        // MarshalRead job, i.e. on the main thread, since it touches the Mono domain.
        ToolResult Handle_ReloadScript(McpServer& server, const Json&)
        {
            if (!server.Context().ReloadScriptAssembly)
                return ToolResult::Error("Script reload is not available in this editor build.");

            const Json result = server.MarshalRead([&server]() -> Json
                                                   {
                if (!server.Context().ReloadScriptAssembly)
                    return Json{ { "__error", "Script reload is not available in this editor build." } };
                const McpScriptReloadResult reloaded = server.Context().ReloadScriptAssembly();
                return ReloadScript::ToJson(reloaded); });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_renderer_settings_set (main-marshaled; PROJECT WRITE) ---------
        // Set a multi-valued, session-global renderer / post-process setting — the
        // FSR1 spatial-upscale mode, the tone-map operator, or the rendering path —
        // so an agent can verify a rendering feature LIVE at each setting over MCP
        // (the motivating case is #480's FSR1 "Spatial Upscale" dropdown, which the
        // read-only server couldn't drive). It is the ENUM-valued sibling of the
        // boolean olo_render_toggle_pass. Gated at dispatch by the "Allow writes"
        // session toggle (ToolDef::ProjectWrite): a settings write crosses the
        // read-only line, though it is session-scoped and restorable, never a
        // project mutation. Restore is restore-PRIOR-VALUE, NOT CommandHistory —
        // these are global renderer settings, not scene/ECS data, so the tool
        // reports `previousValue` and the agent reverts by setting it back (a scene
        // reload also restores them). The shared schema + parse + apply core lives
        // in McpRendererSettings.h so it is unit-tested at the dispatch seam without
        // this TU; the render-path switch's render-graph rebuild
        // (Renderer3D::ApplyRendererSettings) stays here, on the main thread.
        ToolResult Handle_RendererSettingsSet(McpServer& server, const Json& args)
        {
            using namespace RendererSettings;

            bool introspect = false;
            Setting setting{};
            i32 value = 0;
            if (const auto error = ParseArgs(args, introspect, setting, value))
                return ToolResult::Error(*error);

            // Snapshot the live renderer toggles the perf-lever settings read/write
            // (#316). Main-thread only — always called inside a MarshalRead.
            const auto snapshotLever = []() -> LeverState
            {
                LeverState lever;
                lever.DepthPrepassEnabled = Renderer3D::IsDepthPrepassEnabled();
                lever.DepthPrepassAuto = Renderer3D::ComputeSettingsDerivedDepthPrepass();
                lever.SoftShadows = Renderer3D::GetShadowMap().GetSettings().SoftShadows;
                return lever;
            };

            // Introspection: no `setting` -> list every setting with its live current
            // value and the allowed-value catalogue.
            if (introspect)
            {
                const Json result = server.MarshalRead([&snapshotLever]() -> Json
                                                       { return Describe(Renderer3D::GetPostProcessSettings(), Renderer3D::GetRendererSettings(), snapshotLever()); });
                return ToolResult::Text(result.dump(2));
            }

            const Json result = server.MarshalRead([setting, value, &snapshotLever]() -> Json
                                                   {
                PostProcessSettings& pp = Renderer3D::GetPostProcessSettings();
                // Fully qualified: `using namespace RendererSettings` is in scope, so
                // unqualified `RendererSettings` would name that MCP namespace, not the
                // engine struct.
                ::OloEngine::RendererSettings& rs = Renderer3D::GetRendererSettings();
                LeverState lever = snapshotLever();
                const ApplyResult applied = Apply(setting, value, pp, rs, lever);
                if (!applied.Ok)
                    return Json{ { "__error", applied.Error } };
                // A render-path switch changes the registered pass list, so the
                // render-graph topology must be rebuilt for the new value to render.
                if (applied.PathChanged)
                    Renderer3D::ApplyRendererSettings();
                // Push mutated lever fields back to the renderer — Apply only wrote
                // the POD snapshot (the header stays renderer-free).
                if (setting == Setting::DepthPrepass)
                {
                    Renderer3D::EnableDepthPrepass(lever.DepthPrepassEnabled);
                }
                else if (setting == Setting::SoftShadows)
                {
                    ShadowSettings shadow = Renderer3D::GetShadowMap().GetSettings();
                    shadow.SoftShadows = lever.SoftShadows;
                    Renderer3D::GetShadowMap().SetSettings(shadow);
                }
                return applied.Data; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // MarshalRead's default 5s watchdog is sized for the typical read-only tool
        // (a snapshot, a query) — nowhere near enough for a full scene load/copy at
        // stress-test entity counts (a 50k-entity YAML deserialize measured ~55-60s
        // wall time; a 100k-entity transform-only scene ~24s: see
        // docs/analysis/perf-stress-findings-2026-07.md). Below that ceiling
        // MarshalRead's CALLER gives up and returns an error while the job it
        // enqueued keeps running on the game thread regardless (nothing dequeues
        // it) — a driver that reacts to that "timeout" by immediately queuing the
        // NEXT scene's open compounds a single-threaded backlog that never
        // recovers (found running scripts/perf/run-perf-battery.ps1 against a
        // long-lived editor instance, #316 Part 5 follow-up). olo_scene_open /
        // olo_scene_play / olo_scene_stop are the only tools that legitimately need
        // more room; every other MarshalRead call site is a fast query and keeps
        // the 5s default deliberately, so slow tools genuinely hang rather than
        // silently blocking behind the queue for minutes.
        constexpr std::chrono::milliseconds kSceneControlTimeout{ 120000 };

        // ---- olo_scene_open (main-marshaled; PROJECT WRITE) --------------------
        // Open / switch the active scene over MCP — the consented-write scene switch
        // (issue #316 Part 5). Loads the requested scene file directly through the
        // editor's OpenSceneFromMcp hook, which installs it the same way the editor's
        // Open Scene menu does but WITHOUT the auto-save recovery modal (a remote
        // agent can't click it) and without the file dialog. Gated at dispatch by the
        // "Allow writes" session toggle (ToolDef::ProjectWrite): switching scenes
        // discards the current in-memory scene state, so it crosses the read-only line
        // by design. The load runs inside the MarshalRead job, i.e. on the main
        // thread, since it touches the EnTT registry / renderer settings. The shared
        // schema + path validation + result shaping live in McpSceneControl.h so they
        // are unit-tested at the dispatch seam without this TU.
        ToolResult Handle_SceneOpen(McpServer& server, const Json& args)
        {
            using namespace SceneControl;

            if (!args.contains("path") || !args["path"].is_string())
                return ToolResult::Error("Missing required argument 'path' (a .olo or .scene scene file).");
            const std::string path = args["path"].get<std::string>();
            if (const auto error = ValidateScenePath(path))
                return ToolResult::Error(*error);

            if (!server.Context().OpenSceneFromMcp)
                return ToolResult::Error("Scene open is not available in this editor build.");

            // path is captured BY VALUE: MarshalRead's caller-side wait can time out
            // (kSceneControlTimeout) while the job it enqueued is still running on
            // the game thread — nothing dequeues an abandoned job. A by-reference
            // capture of this function-local string would dangle once
            // Handle_SceneOpen's stack frame unwinds on that timeout; server is a
            // long-lived object (owned by EditorLayer for the whole session) so a
            // reference capture there is safe.
            const Json result = server.MarshalRead([&server, path]() -> Json
                                                   {
                if (!server.Context().OpenSceneFromMcp)
                    return Json{ { "__error", "Scene open is not available in this editor build." } };
                const McpSceneOpenResult opened = server.Context().OpenSceneFromMcp(path);
                return ToJson(opened); },
                                                   kSceneControlTimeout);

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_scene_play / olo_scene_stop (main-marshaled; PROJECT WRITE) ---
        // Toggle Play mode over MCP — the consented-write, fully-reversible play/stop
        // switch (issue #316 Part 5). Wraps the editor's OnScenePlay / OnSceneStop
        // (the same path the editor's Play / Stop toolbar buttons drive) through the
        // SetScenePlayState hook. Gated at dispatch by the "Allow writes" session
        // toggle (ToolDef::ProjectWrite): entering Play copies the scene and executes
        // the user's game scripts, so it crosses the read-only line — but it is
        // transient (stopping restores the authored scene, exactly like the editor).
        // olo_scene_summary reports `isPlaying`, so an agent can confirm the
        // transition took. The transition runs inside the MarshalRead job (main
        // thread), since it mutates scene state.
        ToolResult Handle_ScenePlayState(McpServer& server, bool play)
        {
            using namespace SceneControl;

            if (!server.Context().SetScenePlayState)
                return ToolResult::Error("Play-mode control is not available in this editor build.");

            const Json result = server.MarshalRead([&server, play]() -> Json
                                                   {
                if (!server.Context().SetScenePlayState)
                    return Json{ { "__error", "Play-mode control is not available in this editor build." } };
                const McpScenePlayResult r = server.Context().SetScenePlayState(play);
                return ToJson(r); },
                                                   kSceneControlTimeout);

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        ToolResult Handle_ScenePlay(McpServer& server, const Json&)
        {
            return Handle_ScenePlayState(server, /*play*/ true);
        }

        ToolResult Handle_SceneStop(McpServer& server, const Json&)
        {
            return Handle_ScenePlayState(server, /*play*/ false);
        }

        // ---- olo_render_why_not_visible (main-marshaled) -----------------------
        // The rendering counterpart of olo_physics_why_no_collision: explain why an
        // entity isn't on screen. Gathers the render-relevant facts off the live
        // scene/renderer, then runs the pure ExplainWhyNotVisible cascade. The
        // per-frame occlusion (HZB) and LOD state are NOT queryable from here, so
        // the tool reports them honestly as not-observable rather than guessing.
        ToolResult Handle_RenderWhyNotVisible(McpServer& server, const Json& args)
        {
            if (!args.contains("entity"))
                return ToolResult::Error("Missing required argument 'entity' (entity UUID).");
            u64 id = 0;
            if (!ParseUuid(args["entity"], id))
                return ToolResult::Error("Invalid 'entity': expected a UUID as a string or number.");

            Json result = server.MarshalRead([&server, id]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;

                RenderExplain::WhyNotVisibleInput in;
                in.SceneLoaded = scene != nullptr;

                // Global shader-compile hint: a broken shared mesh shader can hide
                // an otherwise correctly-configured object, but isn't attributable
                // to one entity — surfaced as a warning, not a per-entity verdict.
                int shaderErrorCount = 0;
                const auto& allShaders = ShaderDebugger::GetInstance().GetAllShaders();
                for (const auto& [sid, info] : allShaders)
                {
                    if (info.m_HasErrors || !info.m_LastCompilation.m_Success)
                        ++shaderErrorCount;
                }
                in.ShaderErrorCount = shaderErrorCount;
                in.AnyShaderHasErrors = shaderErrorCount > 0;

                RenderExplain::EntityRenderFacts& f = in.Entity;

                if (scene)
                {
                    const auto entityOpt = scene->TryGetEntityWithUUID(UUID(id));
                    f.EntityExists = entityOpt.has_value();
                    if (f.EntityExists)
                    {
                        Entity entity = *entityOpt;

                        // World-space bounding sphere (from the entity's local
                        // transform — the exact transform the renderer submits;
                        // there is no world-transform flattening in that path).
                        // Every scene entity carries a TransformComponent, but guard
                        // defensively so a transform-less entity can't crash the read.
                        const bool hasTransform = entity.HasComponent<TransformComponent>();
                        const glm::mat4 modelMatrix = hasTransform
                                                          ? entity.GetComponent<TransformComponent>().GetTransform()
                                                          : glm::mat4(1.0f);
                        BoundingSphere worldSphere;
                        const auto setBoundsFromLocal = [&](const BoundingSphere& localSphere)
                        {
                            worldSphere = localSphere.Transform(modelMatrix);
                            f.BoundsKnown = true;
                        };

                        // Resolve the entity material's OWN shader (custom-shader /
                        // shader-graph materials). Standard meshes render through a
                        // shared deferred PBR shader, so most materials carry no
                        // m_Shader and this stays unresolved (the global hint covers
                        // a broken shared shader).
                        const auto resolveMaterialShader = [&](const Material& mat)
                        {
                            const Ref<Shader>& shader = mat.GetShader();
                            if (!shader)
                                return;
                            f.HasMaterialShader = true;
                            f.MaterialShaderName = shader->GetName();
                            for (const auto& [sid, info] : allShaders)
                            {
                                if (info.m_Name == f.MaterialShaderName)
                                {
                                    f.MaterialShaderHasErrors = info.m_HasErrors || !info.m_LastCompilation.m_Success;
                                    break;
                                }
                            }
                        };

                        // Pick the primary renderable, most-specific first. Detailed
                        // geometry/visibility checks for the verified set; the rest
                        // are detected as renderable (kind label only) so a niche
                        // renderable is never mis-reported as "not renderable".
                        if (entity.HasComponent<MeshComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "MeshComponent";
                            const auto& mc = entity.GetComponent<MeshComponent>();
                            f.GeometryRequired = true;
                            f.GeometryPresent = static_cast<bool>(mc.m_MeshSource);
                            if (!f.GeometryPresent)
                                f.GeometryDetail = "the MeshComponent's MeshSource is null";
                            else
                                setBoundsFromLocal(mc.m_MeshSource->GetBoundingSphere());
                            if (entity.HasComponent<MaterialComponent>())
                                resolveMaterialShader(entity.GetComponent<MaterialComponent>().m_Material);
                        }
                        else if (entity.HasComponent<ModelComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "ModelComponent";
                            const auto& model = entity.GetComponent<ModelComponent>();
                            f.GeometryRequired = true;
                            f.GeometryPresent = model.IsLoaded();
                            if (!f.GeometryPresent)
                                f.GeometryDetail = (model.m_Model == nullptr)
                                                       ? "the ModelComponent's model is null"
                                                       : "the model has no meshes loaded";
                            else
                                setBoundsFromLocal(model.m_Model->GetBoundingSphere());
                            f.HasVisibilityFlag = true;
                            f.VisibilityFlagOn = model.m_Visible;
                            f.VisibilityFlagName = "ModelComponent.m_Visible";
                            if (entity.HasComponent<MaterialComponent>())
                                resolveMaterialShader(entity.GetComponent<MaterialComponent>().m_Material);
                        }
                        else if (entity.HasComponent<InstancedMeshComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "InstancedMeshComponent";
                            const auto& imc = entity.GetComponent<InstancedMeshComponent>();
                            f.GeometryRequired = true;
                            const bool hasMesh = static_cast<bool>(imc.MeshSource);
                            const bool hasInstances = !imc.Instances.empty() || imc.PlacementAssetHandle != 0;
                            f.GeometryPresent = hasMesh && hasInstances;
                            if (!hasMesh)
                                f.GeometryDetail = "the InstancedMeshComponent's MeshSource is null";
                            else if (!hasInstances)
                                f.GeometryDetail = "no instances (inline list empty and no placement asset)";
                            // Instances live in world space, not under the entity
                            // transform, so a combined bound isn't computed here —
                            // camera-relative checks are skipped (BoundsKnown stays false).
                            if (imc.OverrideMaterial)
                                resolveMaterialShader(*imc.OverrideMaterial);
                            else if (entity.HasComponent<MaterialComponent>())
                                resolveMaterialShader(entity.GetComponent<MaterialComponent>().m_Material);
                        }
                        else if (entity.HasComponent<SubmeshComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "SubmeshComponent";
                            const auto& sm = entity.GetComponent<SubmeshComponent>();
                            f.GeometryRequired = true;
                            f.GeometryPresent = static_cast<bool>(sm.m_Mesh);
                            if (!f.GeometryPresent)
                                f.GeometryDetail = "the SubmeshComponent's mesh is null";
                            else
                                setBoundsFromLocal(sm.m_Mesh->GetBoundingSphere());
                            f.HasVisibilityFlag = true;
                            f.VisibilityFlagOn = sm.m_Visible;
                            f.VisibilityFlagName = "SubmeshComponent.m_Visible";
                            if (entity.HasComponent<MaterialComponent>())
                                resolveMaterialShader(entity.GetComponent<MaterialComponent>().m_Material);
                        }
                        else if (entity.HasComponent<SpriteRendererComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "SpriteRendererComponent";
                            f.GeometryRequired = false; // always drawn from the transform
                            f.GeometryPresent = true;
                            // 2D renderable: culled via the 2D path, not the 3D view
                            // frustum, so camera-relative checks are left unevaluated.
                        }
                        else if (entity.HasComponent<CircleRendererComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "CircleRendererComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }
                        else if (entity.HasComponent<TextComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "TextComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }
                        else if (entity.HasComponent<WaterComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "WaterComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                            f.HasVisibilityFlag = true;
                            f.VisibilityFlagOn = entity.GetComponent<WaterComponent>().m_Enabled;
                            f.VisibilityFlagName = "WaterComponent.m_Enabled";
                        }
                        else if (entity.HasComponent<TerrainComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "TerrainComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }
                        else if (entity.HasComponent<ParticleSystemComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "ParticleSystemComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }
                        else if (entity.HasComponent<TileRendererComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "TileRendererComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }
                        else if (entity.HasComponent<EnvironmentMapComponent>())
                        {
                            // The skybox background. It is gated on m_EnableSkybox
                            // and a loaded environment map, not the entity transform.
                            f.HasRenderable = true;
                            f.RenderableKind = "EnvironmentMapComponent (skybox)";
                            f.GeometryRequired = false;
                            const auto& env = entity.GetComponent<EnvironmentMapComponent>();
                            f.GeometryPresent = static_cast<bool>(env.m_EnvironmentMap);
                            f.HasVisibilityFlag = true;
                            f.VisibilityFlagOn = env.m_EnableSkybox;
                            f.VisibilityFlagName = "EnvironmentMapComponent.m_EnableSkybox";
                        }
                        else if (entity.HasComponent<ProceduralSkyComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "ProceduralSkyComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }
                        else if (entity.HasComponent<StarNestSkyComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "StarNestSkyComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }

                        // Degenerate scale (any axis ~0) collapses the geometry to nothing.
                        if (hasTransform)
                        {
                            const glm::vec3& scale = entity.GetComponent<TransformComponent>().Scale;
                            constexpr f32 kScaleEpsilon = 1e-6f;
                            f.ScaleDegenerate = std::abs(scale.x) < kScaleEpsilon ||
                                                std::abs(scale.y) < kScaleEpsilon ||
                                                std::abs(scale.z) < kScaleEpsilon;
                        }

                        // Camera-relative checks against the editor camera. BehindCamera
                        // comes from the camera pose (robust); the frustum check uses the
                        // engine's actual view frustum from the last rendered frame.
                        if (server.Context().GetCameraPose && f.BoundsKnown)
                        {
                            const McpCameraPose pose = server.Context().GetCameraPose();
                            in.CameraKnown = true;
                            if (glm::dot(pose.Forward, pose.Forward) > 1e-12f)
                            {
                                const glm::vec3 forward = glm::normalize(pose.Forward);
                                const glm::vec3 toCenter = worldSphere.Center - pose.Position;
                                const f32 along = glm::dot(toCenter, forward);
                                f.BehindCamera = (along + worldSphere.Radius) < 0.0f;
                            }
                            BoundingSphere cullSphere = worldSphere;
                            cullSphere.Radius *= 1.3f; // match Renderer3D::IsVisibleInFrustum expansion
                            f.InFrustum = Renderer3D::GetViewFrustum().IsBoundingSphereVisible(cullSphere);
                        }
                    }
                }

                const RenderExplain::WhyNotVisibleVerdict verdict = RenderExplain::ExplainWhyNotVisible(in);

                Json facts;
                facts["entityExists"] = f.EntityExists;
                facts["hasRenderable"] = f.HasRenderable;
                facts["renderableKind"] = f.RenderableKind;
                facts["geometryRequired"] = f.GeometryRequired;
                facts["geometryPresent"] = f.GeometryPresent;
                facts["geometryDetail"] = f.GeometryDetail;
                facts["hasVisibilityFlag"] = f.HasVisibilityFlag;
                facts["visibilityFlagName"] = f.VisibilityFlagName;
                facts["visibilityFlagOn"] = f.VisibilityFlagOn;
                facts["scaleDegenerate"] = f.ScaleDegenerate;
                facts["hasMaterialShader"] = f.HasMaterialShader;
                facts["materialShaderName"] = f.MaterialShaderName;
                facts["materialShaderHasErrors"] = f.MaterialShaderHasErrors;
                facts["boundsKnown"] = f.BoundsKnown;
                facts["behindCamera"] = f.BehindCamera;
                facts["inFrustum"] = f.InFrustum;

                j["entity"] = UuidToString(UUID(id));
                j["reasonCode"] = verdict.ReasonCode;
                j["summary"] = verdict.Summary;
                j["renderableConfigOk"] = verdict.RenderableConfigOk;
                j["visible"] = verdict.Visible;
                j["checks"] = verdict.Checks;
                j["facts"] = facts;
                j["sceneLoaded"] = in.SceneLoaded;
                j["cameraKnown"] = in.CameraKnown;
                j["anyShaderHasErrors"] = in.AnyShaderHasErrors;
                j["shaderErrorCount"] = in.ShaderErrorCount;
                return j; });

            return ToolResult::Text(result.dump(2));
        }

        // ---- MCP ToolAnnotations builders (spec 2025-06-18) ---------------------
        // Behavioural hints emitted under a tool's `annotations`. Every tool here
        // sets openWorldHint:false — they all act on the local editor session, not
        // an "open world" of external systems (the spec's web-search example).

        // A tool that does not modify its environment — the signal a client uses to
        // auto-approve a diagnostic read. The frame/target capture tools count as
        // read-only: they trigger a transient one-frame diagnostic readback that
        // changes no camera, setting, file, or scene state the caller can observe.
        Json ReadOnlyAnnotations()
        {
            return Json{ { "readOnlyHint", true }, { "openWorldHint", false } };
        }

        // A tool that mutates transient editor/session state (camera pose, viewport
        // size, ephemeral render overrides, a shader recompile).
        //   idempotent  — the same arguments leave the editor in the same state
        //                 (the camera / viewport setters); omitted otherwise (a
        //                 toggle that flips, a reload that re-reads from disk).
        //   destructive — may overwrite or destroy data (e.g. rebasing a golden
        //                 PNG). When false we emit destructiveHint:false (additive
        //                 only); when true we omit it, keeping the spec default of
        //                 true. destructiveHint / idempotentHint are meaningful only
        //                 while readOnlyHint is false, which holds here.
        Json MutatingAnnotations(bool idempotent, bool destructive = false)
        {
            Json a{ { "readOnlyHint", false }, { "openWorldHint", false } };
            if (!destructive)
                a["destructiveHint"] = false;
            if (idempotent)
                a["idempotentHint"] = true;
            return a;
        }
    } // namespace

    void RegisterBuiltinTools(McpServer& server)
    {
        {
            ToolDef tool;
            tool.Name = "olo_log_tail";
            tool.Toolset = "diagnostics";
            tool.Title = "Tail engine log";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Return the most recent engine log messages from OloEditor's in-memory ring buffer "
                "(up to 200 lines). Use this to see what the engine just logged — warnings, errors, "
                "and tagged messages from asset/scene/physics/script/renderer subsystems.";
            tool.InputSchema = Schema::Object()
                                   .Prop("count", Schema::Int().Min(1).Max(200).Desc("How many of the most recent matching log lines to return (default 50)."))
                                   .Prop("minLevel", Schema::String().Enum({ "trace", "debug", "info", "warn", "error", "critical" }).Desc("Only return lines at this severity or higher."))
                                   .Prop("tag", Schema::String().Desc("Only return lines whose [Tag] matches exactly (e.g. Physics, Scene, Script)."))
                                   .NoAdditional();
            tool.MainMarshaled = false;
            tool.Handler = Handle_LogTail;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_summary";
            tool.Toolset = "scene";
            tool.Title = "Scene summary";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Summarise the active scene currently open in the editor: its name, whether the game "
                "is playing or paused, whether a scene is loaded, and the total entity count. Read "
                "directly from the live ECS on the editor's main thread (a consistent frame snapshot).";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("hasActiveScene", Schema::Bool().Desc("Whether a scene is currently loaded."))
                                    .Prop("isPlaying", Schema::Bool().Desc("Whether the game is in Play mode."))
                                    .Prop("name", Schema::String().Desc("Active scene name (only when a scene is loaded)."))
                                    .Prop("isPaused", Schema::Bool().Desc("Whether the playing scene is paused (only when a scene is loaded)."))
                                    .Prop("entityCount", Schema::Int().Min(0).Desc("Total entity count (only when a scene is loaded)."))
                                    .Required({ "hasActiveScene", "isPlaying" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneSummary;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_list_entities";
            tool.Toolset = "scene";
            tool.Title = "List scene entities";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List entities in the active scene (paginated). Each entry has the entity's UUID, name, "
                "parent UUID (if any), and child count. Optionally filter by a name substring. Use this "
                "to find an entity, then call olo_scene_get_entity with its id for full component data.";
            tool.InputSchema = Schema::Object()
                                   .Prop("namePattern", Schema::String().Desc("Case-sensitive substring to match against entity names."))
                                   .Pagination("Entities per page (default 50, max 200).")
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneListEntities;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_get_entity";
            tool.Toolset = "scene";
            tool.Title = "Get entity components";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Get the full component data of one entity by UUID, serialized from the live scene (YAML "
                "in 'componentsYaml', plus structured id/name/parent/children). Pair with "
                "olo_scene_list_entities or olo_scene_summary to obtain the UUID.";
            tool.InputSchema = Schema::Object()
                                   .Prop("id", Schema::EntityId())
                                   .Required({ "id" })
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("found", Schema::Bool().Desc("True when the entity exists (a miss is returned as isError instead)."))
                                    .Prop("id", Schema::String().Desc("Entity UUID."))
                                    .Prop("name", Schema::String().Desc("Entity tag/name (empty when it has no TagComponent)."))
                                    .Prop("parent", Schema::String().Desc("Parent entity UUID; omitted when the entity has no parent."))
                                    .Prop("children", Schema::Array(Schema::String()).Desc("Child entity UUIDs."))
                                    .Prop("componentsYaml", Schema::String().Desc("All components serialized as scene YAML."))
                                    .Required({ "found", "id", "name", "children", "componentsYaml" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneGetEntity;
            server.RegisterTool(std::move(tool));
        }

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
                "not additional). Frame totals include gpuWaitMs (CPU blocked on the GPU fence — the direct "
                "GPU-bound signal); unattributedGpuMs is frame GPU time spent between/outside the timed passes.";
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
                                    .Required({ "frame", "passes", "passGpuTotalMs" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfPassTimings;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_frame_breakdown";
            tool.Toolset = "render";
            tool.Title = "Frame command breakdown";
            // Same transient one-frame capture as olo_perf_capture_frame — read-only.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Capture the current frame and return its per-command / per-pipeline-stage structural "
                "breakdown — the granularity olo_perf_capture_frame omits. Triggers a one-frame capture of one "
                "render-graph pass's command bucket ('sourcePass', the scene render pass) and returns the "
                "pipeline stats plus the ordered command list for the chosen stage: each command's type, "
                "debug-name pass label, draw key (shader/material/depth/view-layer/render-mode), group id, "
                "execution order, static flag and GPU time, plus a command-type histogram. A 'graphAttribution' "
                "block places that bucket in the whole live render graph: every pass, which ones own a command "
                "bucket, which one is the capture source, and each pass's work type / cull state / execution "
                "order — so you can see which pass emitted the captured commands and which other passes emit "
                "commands that are not yet per-command attributed. Use format:\"markdown\" for the Command "
                "Bucket Inspector's LLM-analysis report (sort displacement, state-change deltas, batching "
                "analysis, optimization hints) instead of JSON.";
            tool.InputSchema = Schema::Object()
                                   .Prop("viewMode", Schema::String()
                                                         .Enum({ "presort", "postsort", "postbatch" })
                                                         .Desc("(json format only) Pipeline stage to list: 'presort' (submission order), 'postsort' "
                                                               "(after the radix sort), or 'postbatch' (what actually executed; default). Falls back "
                                                               "to an earlier, populated stage when the requested one is empty."))
                                   .Prop("maxCommands", Schema::Int()
                                                            .Min(1)
                                                            .Max(5000)
                                                            .Desc("(json format only) Cap on commands returned (default 200). The full count and a "
                                                                  "'truncated' flag are always reported."))
                                   .Prop("format", Schema::String()
                                                       .Enum({ "json", "markdown" })
                                                       .Desc("'json' (default): structured per-command breakdown shaped by viewMode/maxCommands. "
                                                             "'markdown': the human/LLM analysis report (covers all stages and commands)."))
                                   .NoAdditional();
            // outputSchema (#357-P2) describes the json format only; markdown returns
            // free text, which an outputSchema cannot constrain.
            tool.OutputSchema = Schema::Object()
                                    .Prop("frameNumber", Schema::Int().Min(0).Desc("Captured frame counter."))
                                    .Prop("sourcePass", Schema::String().Desc("Render-graph pass whose command bucket these commands were emitted by."))
                                    .Prop("viewMode", Schema::String().Desc("Pipeline stage actually listed (after empty-stage fallback)."))
                                    .Prop("commandCount", Schema::Int().Min(0).Desc("Total commands in the listed stage (pre-truncation)."))
                                    .Prop("returnedCommands", Schema::Int().Min(0).Desc("Commands actually included in 'commands' (<= maxCommands)."))
                                    .Prop("truncated", Schema::Bool().Desc("True when maxCommands capped the returned list."))
                                    .Prop("stats", Schema::Object().Desc("Aggregate frame stats (draw calls, state changes, sort/batch/execute ms, ...)."))
                                    .Prop("stageCounts", Schema::Object().Desc("Command counts at the preSort / postSort / postBatch stages."))
                                    .Prop("commandTypeHistogram", Schema::Object().Desc("Count of each command type over the full listed stage."))
                                    .Prop("commands", Schema::Array(Schema::Object()
                                                                        .Prop("index", Schema::Int())
                                                                        .Prop("type", Schema::String())
                                                                        .Prop("debugName", Schema::String())
                                                                        .Prop("isDraw", Schema::Bool())
                                                                        .Prop("executionOrder", Schema::Int())
                                                                        .Prop("drawKey", Schema::Object())
                                                                        .Prop("gpuMs", Schema::Number()))
                                                          .Desc("Ordered commands for the listed stage."))
                                    .Prop("graphAttribution", Schema::Object()
                                                                  .Prop("captureSourcePass", Schema::String().Desc("Pass whose bucket the per-command breakdown describes."))
                                                                  .Prop("passCount", Schema::Int().Min(0).Desc("Total passes in the live graph."))
                                                                  .Prop("commandBucketPassCount", Schema::Int().Min(0).Desc("Passes that own a command bucket."))
                                                                  .Prop("capturedPassCount", Schema::Int().Min(0).Desc("Command-bucket passes actually captured (currently 0 or 1)."))
                                                                  .Prop("passes", Schema::Array(Schema::Object()
                                                                                                    .Prop("name", Schema::String())
                                                                                                    .Prop("workType", Schema::String())
                                                                                                    .Prop("usesCommandBucket", Schema::Bool())
                                                                                                    .Prop("isCaptureSource", Schema::Bool())
                                                                                                    .Prop("culled", Schema::Bool())
                                                                                                    .Prop("isFinalPass", Schema::Bool())
                                                                                                    .Prop("executionIndex", Schema::Int()))
                                                                                      .Desc("Every pass in the live render graph."))
                                                                  .Desc("Whole-graph command-bucket landscape (omitted when no active render graph)."))
                                    .Required({ "frameNumber", "sourcePass", "commandCount", "commands" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderFrameBreakdown;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_shader_errors";
            tool.Toolset = "shader";
            tool.Title = "Shader compile errors";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Shaders that currently have compile/link errors, with the error message. Empty when all "
                "shaders compiled cleanly.";
            tool.InputSchema = Schema::EmptyObject();
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderErrors;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_shader_get";
            tool.Toolset = "shader";
            tool.Title = "Get shader details";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Details of one shader by name or numeric id: instruction count, compile time, uniforms, "
                "uniform buffers, samplers, reload count, and (with includeGlsl) the cross-compiled GLSL per stage.";
            tool.InputSchema = Schema::Object()
                                   .Prop("name", Schema::String().Desc("Shader name (as shown by olo_shader_errors / the shader debugger)."))
                                   .Prop("id", Schema::Int().Desc("GL program id (alternative to name)."))
                                   .Prop("includeGlsl", Schema::Bool().Desc("Include the cross-compiled GLSL source per stage (default false)."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderGet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_shader_list";
            tool.Toolset = "shader";
            tool.Title = "List shaders";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Inventory of all registered shaders (id, name, hasErrors, instruction count). Use it to "
                "discover a shader name/id to pass to olo_shader_get.";
            tool.InputSchema = Schema::EmptyObject();
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderList;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_shader_reload";
            tool.Toolset = "shader";
            tool.Title = "Reload shader";
            // Recompiles a shader from disk (mutates GL program state, new program
            // id each call), so not read-only and not idempotent; destroys nothing.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Reload and recompile one shader from disk by name — the shader inner loop: edit a .glsl, "
                "reload, read the compile/link log, screenshot, all without restarting the editor. Re-reads "
                "the file and recompiles+links synchronously in BOTH the Renderer3D and Renderer2D libraries "
                "(whichever hold the name). Returns the post-reload status (ready/failed/compiling/pending), "
                "the GL program id, which libraries held it, and the compile/link error log (empty on a clean "
                "reload; populated from the shader debugger in debug builds). Only shaders owned by the "
                "Renderer3D / Renderer2D libraries are reloadable (the main scene shaders); post-process / "
                "compute shaders like GTAO/SSAO/SSR are not, and asking for one returns an error that lists "
                "the reloadable names. Note: in a Debug build, recompiling a shader that contains a GLSL syntax "
                "error trips an engine debug assert on the main thread (same as the editor's own Recompile "
                "button) — the call then times out and can crash the editor, so reserve this for edits you "
                "expect to compile; to inspect a shader's existing errors without recompiling, use "
                "olo_shader_errors / olo_shader_get instead.";
            tool.InputSchema = Schema::Object()
                                   .Prop("name", Schema::String().Desc("Shader name to reload (as shown by olo_shader_list)."))
                                   .Required({ "name" })
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderReload;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_assets_list";
            tool.Toolset = "assets";
            tool.Title = "List assets";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List the project's registered assets (paginated): handle, type, project-relative path, and "
                "filename. Optionally filter by asset type (e.g. Texture2D, Mesh, Material, Scene, Script).";
            tool.InputSchema = Schema::Object()
                                   .Prop("typeFilter", Schema::String().Desc("Asset type name to filter by (e.g. 'Texture2D'). Omit for all types."))
                                   .Pagination("Assets per page (default 50, max 200).")
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_AssetsList;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_assets_problems";
            tool.Toolset = "assets";
            tool.Title = "List asset problems";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List assets that failed to load or are missing/invalid (handle, type, path, status). The "
                "first thing to check when something references an asset that isn't showing up.";
            tool.InputSchema = Schema::EmptyObject();
            tool.MainMarshaled = true;
            tool.Handler = Handle_AssetsProblems;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_crash_list";
            tool.Toolset = "diagnostics";
            tool.Title = "List crash reports";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List crash reports written by the engine (crash_<timestamp>.txt under CrashReports/). Each "
                "entry has an id and size. Use olo_crash_get to read one.";
            tool.InputSchema = Schema::EmptyObject();
            tool.MainMarshaled = false;
            tool.Handler = Handle_CrashList;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_crash_get";
            tool.Toolset = "diagnostics";
            tool.Title = "Get crash report";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Read a crash report's full text (exception, system info, last 200 log lines) by its id from "
                "olo_crash_list. Useful for an AI-summarised, shareable bug report.";
            tool.InputSchema = Schema::Object()
                                   .Prop("id", Schema::String().Desc("Crash report filename (e.g. crash_20260606_143025_123.txt) from olo_crash_list."))
                                   .Required({ "id" })
                                   .NoAdditional();
            tool.MainMarshaled = false;
            tool.Handler = Handle_CrashGet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_script_get_api";
            tool.Toolset = "scripting";
            tool.Title = "Get scripting API";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Describe the scripting API a game can call. language='csharp' lists the C# bindings "
                "(OloEngine-ScriptCore); language='lua' lists the Sol2 usertypes. Without typeFilter you get "
                "the type index; with a typeFilter substring you get matching types and their members. Use "
                "this to answer 'how do I ...' questions grounded in the actual engine API.";
            tool.InputSchema = Schema::Object()
                                   .Prop("language", Schema::String().Enum({ "csharp", "lua" }).Desc("Scripting language (default csharp)."))
                                   .Prop("typeFilter", Schema::String().Desc("Case-insensitive substring; matching types are returned with their members."))
                                   .NoAdditional();
            tool.MainMarshaled = false;
            tool.Handler = Handle_ScriptGetApi;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_script_get_last_errors";
            tool.Toolset = "scripting";
            tool.Title = "Recent script errors";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Return the most recent C# (Mono) and Lua (Sol2) script exceptions captured by the engine "
                "(message, originating script/method, entity UUID when known, timestamp). This is the #1 "
                "thing to check when a game's scripts misbehave. Empty if no script errors have occurred.";
            tool.InputSchema = Schema::Object()
                                   .Prop("count", Schema::Int().Min(1).Max(64).Desc("How many of the most recent errors to return (default 20)."))
                                   .NoAdditional();
            tool.MainMarshaled = false;
            tool.Handler = Handle_ScriptGetLastErrors;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_events_tail";
            tool.Toolset = "diagnostics";
            tool.Title = "Tail diagnostics events";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Return the unified 'what just happened?' event timeline from the engine's diagnostics "
                "ring buffer: scene loads, entering/leaving Play mode, runtime entity spawn/destroy, asset "
                "hot-reloads, and script errors — newest last, each with a monotonic 'id'. The key use is "
                "INCREMENTAL POLLING: do an action, then pass the previous call's 'lastId' as 'sinceId' to "
                "get only what happened since. Filter with 'categories'. Bulk churn (scene-copy on Play, "
                "deserialize on load) is collapsed into single scene_load/play events, not per-entity spam.";
            tool.InputSchema = Schema::Object()
                                   .Prop("count", Schema::Int().Min(1).Max(500).Desc("How many of the most recent matching events to return (default 50)."))
                                   .Prop("sinceId", Schema::Raw(Json{ { "type", Json::array({ "integer", "string" }) } })
                                                        .Min(0)
                                                        .Desc("Only return events with id greater than this. Accepts the id as a number or its string form (for large cursors beyond JSON integer precision). Pass back the previous response's 'lastId' for incremental polling."))
                                   .Prop("categories", Schema::Array(Schema::String().Enum({ "scene_load", "play", "stop", "entity_spawn", "entity_destroy", "asset_reload", "script_error" }))
                                                           .Desc("Only return events whose category is in this list. Omit for all categories."))
                                   .NoAdditional();
            tool.MainMarshaled = false;
            tool.Handler = Handle_EventsTail;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_screenshot";
            tool.Toolset = "camera";
            tool.Title = "Screenshot viewport";
            // Read-only without a pose, but with a 'camera'/'orbit' pose it moves the
            // editor camera (saving/restoring it) — a transient mutation. Flag it
            // readOnlyHint:false to be safe so a client doesn't auto-approve a call
            // that nudges the user's viewport; destroys nothing.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Capture the editor's 3D viewport as a PNG image so you can SEE the rendered frame — "
                "decisive for visual problems ('my material looks wrong', 'I can't see my object'). "
                "Returns an image content block (downscaled to maxWidth, default 1024). Optionally pose "
                "the camera for this capture only via 'camera' (explicit position + target/yaw/pitch) or "
                "'orbit' (target + yaw/pitch/distance): the prior camera is saved, the new pose is "
                "rendered ('settleFrames' frames, default 2, for TAA/temporal effects to settle), the "
                "frame is captured, and the user's camera is restored — so multiple angles can be "
                "captured without disturbing the viewport.";
            tool.InputSchema = Schema::Object()
                                   .Prop("maxWidth", Schema::Int().Min(16).Max(4096).Desc("Max output width in pixels (default 1024); aspect ratio preserved."))
                                   .Prop("camera", Schema::Object().Desc("Capture from this pose, then restore the prior camera. Same shape as olo_camera_set_pose: position [x,y,z] plus target [x,y,z] or yaw/pitch (degrees); optional fov."))
                                   .Prop("orbit", Schema::Object().Desc("Capture from this orbit pose, then restore. Same shape as olo_camera_orbit: target [x,y,z], yaw/pitch (degrees), distance; optional fov."))
                                   .Prop("settleFrames", Schema::Int().Min(1).Max(30).Desc("Frames to render at the new pose before capturing (default 2). Raise for temporal effects (TAA, fog history) to settle."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_Screenshot;
            server.RegisterTool(std::move(tool));
        }

        // ---- Tier-0 camera / viewport control (issue #316) ---------------------

        {
            ToolDef tool;
            tool.Name = "olo_camera_get";
            tool.Toolset = "camera";
            tool.Title = "Get camera pose";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Get the editor camera's current pose: position, focal point, forward vector, yaw/pitch "
                "(degrees), orbit distance, FOV, near/far clip, and the viewport size in logical pixels. "
                "Read this before moving the camera so you can put it back, or to reason about what the "
                "viewport is looking at.";
            tool.InputSchema = Schema::EmptyObject();
            tool.MainMarshaled = true;
            tool.Handler = Handle_CameraGet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_camera_set_pose";
            tool.Toolset = "camera";
            tool.Title = "Set camera pose";
            // Moves the editor camera; same args => same pose (idempotent), destroys nothing.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Move the editor camera to an explicit pose (Tier-0 inspection state; never touches the "
                "project). Give 'position' plus either 'target' (a point to look at) or 'yaw'/'pitch' in "
                "degrees; optional 'fov' (vertical, degrees). Returns the resulting pose. The viewport "
                "renders the new pose from the next frame.";
            tool.InputSchema = Schema::Object()
                                   .Prop("position", Schema::Vec3("Camera eye position [x, y, z] (world units)."))
                                   .Prop("target", Schema::Vec3("Point to look at [x, y, z]. Alternative to yaw/pitch."))
                                   .Prop("yaw", Schema::Number().Desc("Yaw in degrees (0 looks along -Z; positive turns right). Alternative to target."))
                                   .Prop("pitch", Schema::Number().Desc("Pitch in degrees (positive looks down). Alternative to target."))
                                   .Prop("fov", Schema::Number().Min(1).Max(170).Desc("Vertical field of view in degrees (omit to keep current)."))
                                   .Required({ "position" })
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_CameraSetPose;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_camera_orbit";
            tool.Toolset = "camera";
            tool.Title = "Orbit camera";
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Orbit-frame the editor camera around a world-space target point: the camera pivots at "
                "'distance' from 'target' looking at it, with 'yaw'/'pitch' in degrees (positive pitch "
                "looks down). Keeps a live orbit pivot so subsequent user orbiting feels natural. "
                "Returns the resulting pose.";
            tool.InputSchema = Schema::Object()
                                   .Prop("target", Schema::Vec3("Orbit centre [x, y, z] (world units)."))
                                   .Prop("yaw", Schema::Number().Desc("Orbit yaw in degrees (default 0)."))
                                   .Prop("pitch", Schema::Number().Desc("Orbit pitch in degrees, positive looks down (default 30)."))
                                   .Prop("distance", Schema::Number().ExclusiveMin(0).Desc("Distance from the target in world units (default 10)."))
                                   .Prop("fov", Schema::Number().Min(1).Max(170).Desc("Vertical field of view in degrees (omit to keep current)."))
                                   .Required({ "target" })
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_CameraOrbit;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_camera_frame_entity";
            tool.Toolset = "camera";
            tool.Title = "Frame entity in view";
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Point the editor camera at one entity and fit it in view (like pressing 'frame selected' "
                "in a DCC tool). Uses the entity's mesh/model/terrain bounds when available, otherwise its "
                "transform scale. Keeps the current view direction. Get the UUID from "
                "olo_scene_list_entities. Returns the resulting pose.";
            tool.InputSchema = Schema::Object()
                                   .Prop("id", Schema::EntityId())
                                   .Required({ "id" })
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_CameraFrameEntity;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_viewport_set_size";
            tool.Toolset = "camera";
            tool.Title = "Set viewport size";
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Override the editor viewport's logical render size for deterministic capture resolution "
                "(e.g. 1280x720 golden-image comparisons), independent of the panel layout. Pass "
                "'reset': true to return control to the ImGui panel size. The override persists until "
                "reset — reset it when you are done capturing.";
            tool.InputSchema = Schema::Object()
                                   .Prop("width", Schema::Int().Min(64).Max(8192).Desc("Viewport width in logical pixels."))
                                   .Prop("height", Schema::Int().Min(64).Max(8192).Desc("Viewport height in logical pixels."))
                                   .Prop("reset", Schema::Bool().Desc("true = clear the override and use the panel size again."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_ViewportSetSize;
            server.RegisterTool(std::move(tool));
        }

        // ---- Render-target capture (Part 4 of #316) -----------------------------

        {
            ToolDef tool;
            tool.Name = "olo_render_list_targets";
            tool.Toolset = "render";
            tool.Title = "List render targets";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List the render graph's live texture/framebuffer resources for the current frame — "
                "scene colour, depth, G-buffer attachments, shadow maps, AO, post-process chain stages, "
                "water/OIT buffers, etc. Each entry has the canonical resource name (pass to "
                "olo_render_capture_target), kind, format, size, and producing passes. Requires the "
                "editor to be rendering in 3D mode.";
            tool.InputSchema = Schema::EmptyObject();
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderListTargets;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_graph_topology_export";
            tool.Toolset = "render";
            tool.Title = "Export render graph topology";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Export the live render graph's topology as structured data for reasoning about the render "
                "pipeline — the passes, their topologically-sorted execution order, the pass-to-pass "
                "dependency edges, and every registered resource (texture/framebuffer/buffer) with the passes "
                "that produce and consume it. Each pass reports its work type (Graphics/Compute/Copy), whether "
                "it declares resources, whether it is an async-compute candidate, whether it was culled "
                "(unreachable from the final pass this frame), and whether it is the final/output pass. Use "
                "format:\"mermaid\" for a flowchart DAG of the pass graph instead of JSON. Read-only; requires "
                "the editor to be rendering in 3D mode.";
            tool.InputSchema = Schema::Object()
                                   .Prop("format", Schema::String()
                                                       .Enum({ "json", "mermaid" })
                                                       .Desc("'json' (default): full structured topology (passes, executionOrder, edges, resources). "
                                                             "'mermaid': a flowchart-LR DAG of the pass dependency graph."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderGraphTopologyExport;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_capture_target";
            tool.Toolset = "render";
            tool.Title = "Capture render target";
            // Reads back one render-graph texture; changes no observable state.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Read back one intermediate render target as a PNG image — THE tool for rendering-feature "
                "development: inspect depth, normals, G-buffer albedo/emissive, shadow maps, AO, bloom, or "
                "any post-process stage directly instead of guessing from the final frame. 'name' is a "
                "render-graph resource name from olo_render_list_targets (e.g. SceneColor, SceneDepth, "
                "GBufferNormal, ShadowMapCSM, AOBuffer, BloomColor). Float/HDR sources are clamped to "
                "[0,1]; depth is min-max normalised by default ('normalize' overrides). Returns metadata "
                "(format, size, value range) plus the image.";
            tool.InputSchema = Schema::Object()
                                   .Prop("name", Schema::String().Desc("Render-graph resource name (see olo_render_list_targets)."))
                                   .Prop("mip", Schema::Int().Min(0).Max(16).Desc("Mip level to capture (default 0)."))
                                   .Prop("face", Schema::Int().Min(0).Max(64).Desc("Cubemap face (0..5 = +X,-X,+Y,-Y,+Z,-Z) or texture-array layer (default 0)."))
                                   .Prop("normalize", Schema::Bool().Desc("Min-max normalise float values to [0,1] before encoding (default: true for depth, false otherwise)."))
                                   .Prop("maxWidth", Schema::Int().Min(16).Max(4096).Desc("Max output width in pixels (default 1024); aspect ratio preserved."))
                                   .Required({ "name" })
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderCaptureTarget;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_toggle_pass";
            tool.Toolset = "render";
            tool.Title = "Toggle render pass";
            // Edits ephemeral session render settings; flips state when 'enabled' is
            // omitted (so not idempotent), destroys nothing.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Flip a post-process / fog feature on or off — the rendering A/B loop: toggle off, "
                "olo_screenshot, toggle on, olo_screenshot, compare. 'name' is one of bloom, ssao, gtao, "
                "ssr, ssgi, fxaa, taa, vignette, chromaticaberration (ca), depthoffield (dof), motionblur, "
                "colorgrading, autoexposure, fog, fogscattering, fogvolumetric, godrays. 'enabled' sets the "
                "state explicitly; omit it to flip the current value. Returns the affected pass and its "
                "new/previous state. Enabling ssao/gtao also selects that AO technique (they share one "
                "slot); ssr/ssgi render only in the Deferred path and the fog sub-features need fog enabled "
                "— a 'note' flags these. The change is EPHEMERAL: it edits the renderer's session-global "
                "settings, not the scene, so it is never saved and a scene reload restores it. Call with no "
                "arguments to list every pass with its current enabled state.";
            tool.InputSchema = Schema::Object()
                                   .Prop("name", Schema::String().Desc("Pass token (e.g. 'bloom', 'ssao', 'ssr', 'fog', 'godrays'). Omit to list all passes + state."))
                                   .Prop("enabled", Schema::Bool().Desc("Desired state. Omit to toggle (flip the current value)."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderTogglePass;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_set_debug_view";
            tool.Toolset = "render";
            tool.Title = "Set render debug view";
            // Edits ephemeral session render settings; destroys nothing.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Switch the viewport to a raw intermediate buffer for AO/reflection/GI debugging. 'mode' is "
                "one of none (the normal composite), ssao, gtao, ssr, ssgi — exactly one is shown at a time; "
                "mode 'none' (or 'enabled':false) clears them all. Returns the active mode, the four "
                "*DebugView flag states, and 'passEnabled' — whether the pass that produces the chosen "
                "buffer is actually running this frame (with an actionable 'note' if not, e.g. enable SSAO "
                "first with olo_render_toggle_pass). The change is EPHEMERAL: it edits the renderer's "
                "session-global settings, not the scene, so it is never saved and a scene reload restores "
                "it. Call with no arguments to list the modes + current state.";
            tool.InputSchema = Schema::Object()
                                   .Prop("mode", Schema::String().Enum({ "none", "ssao", "gtao", "ssr", "ssgi" }).Desc("Debug view to show. 'none' clears all. Omit to list modes + state."))
                                   .Prop("enabled", Schema::Bool().Desc("Set false as an alias for mode:'none' (clear all debug views)."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderSetDebugView;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_set_time_of_day";
            tool.Toolset = "render";
            tool.Title = "Set time of day (sun)";
            // Edits an ephemeral session render override; setting the same hours
            // twice yields the same sun (idempotent), destroys nothing.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Move the procedural sky's sun to a time of day for lighting iteration — the lighting inner "
                "loop: set the hour, olo_screenshot, set another, compare. 'hours' is a 24-hour clock time "
                "in [0,24] (0 = midnight, 6 = sunrise, 12 = noon/overhead, 18 = sunset); the sun rises in "
                "the east, peaks overhead at noon, and sets in the west, dropping below the horizon at night. "
                "Returns the resulting toward-sun direction with its elevation/azimuth (a 'note' warns when "
                "the scene has no Procedural Sky to affect, or when the sun is below the horizon). The change "
                "is EPHEMERAL: it edits a session-global renderer override, NOT the ProceduralSkyComponent, "
                "so it is never saved and resets on scene reload, play-stop, server-stop, or 'clear':true. "
                "Pass 'clear':true to restore the authored sun; call with no arguments to read the current "
                "override state.";
            tool.InputSchema = Schema::Object()
                                   .Prop("hours", Schema::Number().Min(0).Max(24).Desc("Time of day on a 24-hour clock (0=midnight, 6=sunrise, 12=noon, 18=sunset). Omit to read current state."))
                                   .Prop("clear", Schema::Bool().Desc("Set true to remove the override and restore the scene's authored sun direction."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneSetTimeOfDay;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_set_sun_angle";
            tool.Toolset = "render";
            tool.Title = "Set sun angle (yaw/pitch)";
            // Edits an ephemeral session render override; same angles -> same sun
            // (idempotent), destroys nothing.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Aim the procedural sky's sun directly from a yaw/pitch pair — the precise sibling of "
                "olo_scene_set_time_of_day for lighting iteration. 'yaw' is the azimuth in degrees (measured "
                "from +Z toward +X: 0=+Z, 90=+X/east, 270=-X/west) and 'pitch' is the elevation in degrees "
                "in [-90,90] (90=straight up, 0=horizon, negative=below horizon); both are required to set. "
                "Returns the resulting toward-sun direction with its elevation/azimuth (a 'note' warns when "
                "the scene has no Procedural Sky to affect, or when the sun is below the horizon). The change "
                "is EPHEMERAL: it edits a session-global renderer override, NOT the ProceduralSkyComponent, "
                "so it is never saved and resets on scene reload, play-stop, server-stop, or 'clear':true. "
                "Pass 'clear':true to restore the authored sun; call with no arguments to read the current "
                "override state.";
            tool.InputSchema = Schema::Object()
                                   .Prop("yaw", Schema::Number().Desc("Azimuth in degrees (0=+Z, 90=+X/east, 180=-Z, 270=-X/west). Required together with 'pitch' to set."))
                                   .Prop("pitch", Schema::Number().Min(-90).Max(90).Desc("Elevation in degrees above the horizon (90=up, 0=horizon, negative=below). Required together with 'yaw' to set."))
                                   .Prop("clear", Schema::Bool().Desc("Set true to remove the override and restore the scene's authored sun direction."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneSetSunAngle;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_compare_golden";
            tool.Toolset = "render";
            tool.Title = "Compare against golden image";
            // Poses the camera (save/restore) and can WRITE/overwrite a golden PNG
            // on create or rebase — a real, potentially destructive filesystem
            // mutation, so leave destructiveHint at its spec default (true).
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false, /*destructive*/ true);
            tool.Description =
                "Capture the editor viewport and diff it against a golden PNG, returning a numeric "
                "similarity + pass/fail verdict — the numeric half of the 'rendering changes MUST be "
                "visually verified' loop: get a deterministic yes/no instead of eyeballing a screenshot. "
                "'goldenPath' is a PNG under assets/tests/visual/ (bare names land there; '..'/absolute "
                "paths are rejected). Optionally pose the camera for this capture only via 'camera' or "
                "'orbit' (same shape as olo_screenshot; the user's camera is saved and restored). If the "
                "golden does not exist (or 'rebase':true), the captured frame is WRITTEN as the new "
                "golden and the tool reports 'created' instead of failing — mirroring the test suite's "
                "OLOENGINE_GOLDEN_REBASE workflow. The verdict uses the same RMSE→SSIM metric as the "
                "GoldenImageTests suite; pass an explicit 'threshold' (min SSIM similarity in [0,1]) to "
                "override the default cascade. Use the SAME capture size when creating and comparing "
                "(set one with olo_viewport_set_size) or the dimensions will mismatch. Returns the "
                "verdict JSON plus the captured frame as an image block.";
            tool.InputSchema = Schema::Object()
                                   .Prop("goldenPath", Schema::String().Desc("Golden PNG path under assets/tests/visual/ (e.g. 'water_side.png'). A '.png' extension is added if missing. Relative only — no '..' or absolute paths."))
                                   .Prop("threshold", Schema::Number().Min(0).Max(1).Desc("Minimum SSIM similarity in [0,1] to pass (1 = identical). Omit to use the suite's RMSE→SSIM cascade verdict (the default, consistent with the golden test suite)."))
                                   .Prop("rebase", Schema::Bool().Desc("true = overwrite the golden with the current capture instead of comparing (re-baseline after a deliberate visual change). A missing golden is always created regardless."))
                                   .Prop("camera", Schema::Object().Desc("Capture from this pose, then restore the prior camera. Same shape as olo_camera_set_pose: position [x,y,z] plus target [x,y,z] or yaw/pitch (degrees); optional fov."))
                                   .Prop("orbit", Schema::Object().Desc("Capture from this orbit pose, then restore. Same shape as olo_camera_orbit: target [x,y,z], yaw/pitch (degrees), distance; optional fov."))
                                   .Prop("settleFrames", Schema::Int().Min(1).Max(30).Desc("Frames to render at the new pose before capturing (default 2). Raise for temporal effects (TAA, fog history) to settle."))
                                   .Prop("maxWidth", Schema::Int().Min(16).Max(4096).Desc("Max capture width in pixels (default 1024); aspect ratio preserved. Must match between create and compare."))
                                   .Required({ "goldenPath" })
                                   .NoAdditional();
            tool.MainMarshaled = true; // reads main-thread-only camera/viewport state (like olo_screenshot)
            tool.Handler = Handle_RenderCompareGolden;
            server.RegisterTool(std::move(tool));
        }

        // ---- Physics introspection + explain tools (#306 item A) ---------------

        {
            ToolDef tool;
            tool.Name = "olo_physics_layer_matrix";
            tool.Toolset = "physics";
            tool.Title = "Physics layer matrix";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Dump the physics collision-layer matrix the simulation actually uses: the five built-in "
                "Jolt object layers (NON_MOVING, MOVING, TRIGGER, CHARACTER, DEBRIS) plus every user-defined "
                "physics layer, with the pairwise collide/no-collide result from the real layer filter. Use "
                "this to confirm whether two layers are even allowed to collide. Works in Edit mode too.";
            tool.InputSchema = Schema::EmptyObject();
            tool.MainMarshaled = false;
            tool.Handler = Handle_PhysicsLayerMatrix;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_list_colliders";
            tool.Toolset = "physics";
            tool.Title = "List physics colliders";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List every entity with a Rigidbody3DComponent (paginated): authored body type "
                "(Static/Dynamic/Kinematic), collision layer id, trigger flag, and collider shape(s). When "
                "physics is running, also reports the live body's object layer, world position, and "
                "awake/asleep state. Pair with olo_physics_why_no_collision to debug missing collisions.";
            tool.InputSchema = Schema::Object()
                                   .Pagination("Entities per page (default 50, max 200).")
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsListColliders;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_contacts";
            tool.Toolset = "physics";
            tool.Title = "List physics contacts";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List the entity pairs whose physics bodies are touching right now (live active-contact set, "
                "deduplicated per pair). Requires Play mode. Use this to confirm a collision/trigger is "
                "actually being detected by the engine.";
            tool.InputSchema = Schema::Object()
                                   .Prop("maxResults", Schema::Int().Min(1).Max(2000).Desc("Max contact pairs to return (default 200)."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsContacts;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_raycast";
            tool.Toolset = "physics";
            tool.Title = "Physics raycast";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Cast a ray through the live physics world and return what it hits. Specify 'origin' plus "
                "either 'direction' (a vector) or 'to' (an end point). Returns the closest hit by default, or "
                "up to 'maxHits' ordered hits, each with the hit entity, world position, surface normal, and "
                "distance. Requires Play mode.";
            tool.InputSchema = Schema::Object()
                                   .Prop("origin", Schema::Array().Desc("Ray start [x, y, z]."))
                                   .Prop("direction", Schema::Array().Desc("Ray direction [x, y, z] (need not be normalised). Provide this or 'to'."))
                                   .Prop("to", Schema::Array().Desc("Ray end point [x, y, z]; sets direction and distance. Provide this or 'direction'."))
                                   .Prop("maxDistance", Schema::Number().Desc("Max ray length when using 'direction' (default 500)."))
                                   .Prop("maxHits", Schema::Int().Min(1).Max(64).Desc("Return up to N ordered hits (default 1 = closest only)."))
                                   .Required({ "origin" })
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("origin", Schema::Array(Schema::Number()).Desc("Resolved ray origin [x, y, z]."))
                                    .Prop("direction", Schema::Array(Schema::Number()).Desc("Resolved normalised ray direction [x, y, z]."))
                                    .Prop("maxDistance", Schema::Number().Desc("Resolved ray length."))
                                    .Prop("hitCount", Schema::Int().Min(0).Desc("Number of hits returned."))
                                    .Prop("hits", Schema::Array(Schema::Object()
                                                                    .Prop("entity", Schema::Object()
                                                                                        .Prop("id", Schema::String())
                                                                                        .Prop("name", Schema::String()))
                                                                    .Prop("position", Schema::Array(Schema::Number()))
                                                                    .Prop("normal", Schema::Array(Schema::Number()))
                                                                    .Prop("distance", Schema::Number()))
                                                      .Desc("Hits ordered nearest-first."))
                                    .Required({ "origin", "direction", "maxDistance", "hitCount", "hits" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsRaycast;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_overlap";
            tool.Toolset = "physics";
            tool.Title = "Physics overlap query";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Find the physics bodies overlapping a shape at a world point. Pass 'origin' plus 'radius' "
                "for a sphere (the default), or 'halfExtents' [x,y,z] for a box. Returns the overlapping "
                "entities and their positions. Requires Play mode.";
            tool.InputSchema = Schema::Object()
                                   .Prop("origin", Schema::Array().Desc("Query centre [x, y, z]."))
                                   .Prop("radius", Schema::Number().Desc("Sphere radius (default 0.5; ignored if 'halfExtents' is given)."))
                                   .Prop("halfExtents", Schema::Array().Desc("Box half-extents [x, y, z]; selects a box query instead of a sphere."))
                                   .Prop("maxHits", Schema::Int().Min(1).Max(256).Desc("Max overlapping bodies to return (default 32)."))
                                   .Required({ "origin" })
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsOverlap;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_why_no_collision";
            tool.Toolset = "physics";
            tool.Title = "Explain missing collision";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Explain why two entities are NOT colliding — the 'player falls through the floor' debugger. "
                "Given two entity UUIDs ('a' and 'b'), it checks, in order: physics running, both entities "
                "exist, both have a rigidbody + collider + live body, not both Static, their collision layers "
                "are allowed to collide, neither is a trigger, and their bounds overlap. Returns the root-cause "
                "reasonCode, a human summary, the ordered checks performed, and the raw facts for each entity.";
            tool.InputSchema = Schema::Object()
                                   .Prop("a", Schema::String().Desc("First entity UUID (string; also accepts a number)."))
                                   .Prop("b", Schema::String().Desc("Second entity UUID (string; also accepts a number)."))
                                   .Required({ "a", "b" })
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsWhyNoCollision;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_set_collision_layer";
            tool.Toolset = "physics";
            tool.Title = "Set collision layer (undoable)";
            // The first project-WRITE tool: gated behind the session "Allow writes"
            // toggle and routed through the editor undo stack. readOnlyHint:false (not
            // idempotent — each call snapshots the prior layer into a distinct undo
            // command; not destructive — fully reversible via Ctrl-Z / undo).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Set the collision layer of an entity's physics body — the undoable fix half of the "
                "olo_physics_* debugging story (e.g. after olo_physics_why_no_collision blames the layer "
                "filter). Targets the entity's Rigidbody3DComponent (or CharacterController3DComponent) "
                "'m_LayerID'. The change is applied through the editor's undo stack, so it is a single "
                "Ctrl-Z. This is a WRITE tool: it is refused unless 'Allow writes' is enabled in the "
                "editor's MCP Server panel (off by default). Discover valid layer ids with "
                "olo_physics_layer_matrix.";
            tool.InputSchema = SetCollisionLayer::InputSchema();
            tool.MainMarshaled = true;
            tool.Handler = Handle_SetCollisionLayer;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_entity_list_fields";
            tool.Toolset = "scene";
            tool.Title = "List entity writable fields";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List the writable component fields of one entity, with each field's type and current value — "
                "the read-only discovery half of olo_entity_set_field. Only components the entity actually has "
                "(and that expose writable fields) are returned, so the result is exactly what you can write "
                "right now. Pass an optional 'component' to restrict the listing. Field names match the keys "
                "shown in olo_scene_get_entity's YAML.";
            tool.InputSchema = GenericFieldWrite::ListInputSchema();
            tool.MainMarshaled = true;
            tool.Handler = Handle_EntityListFields;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_entity_set_field";
            tool.Toolset = "scene";
            tool.Title = "Set component field (undoable)";
            // The generic project-WRITE tool (#306 item C, second slice): gated behind
            // the session "Allow writes" toggle and routed through the editor undo
            // stack. readOnlyHint:false (not idempotent — each call snapshots the prior
            // value into a distinct undo command; not destructive — fully reversible
            // via Ctrl-Z / undo).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Set a single component field on an entity by (component, field, value) — the generic, "
                "undoable successor to olo_set_collision_layer that mutates ANY registered field (transform "
                "translation/scale, light color/intensity/range/shadows, sprite/circle color, camera flags, "
                "tag, ...). The value type must match the field: a boolean, a number, a string, or an array "
                "of numbers for a vector (e.g. [r,g,b] for a vec3 color). The change is applied through the "
                "editor's undo stack, so it is a single Ctrl-Z. This is a WRITE tool: it is refused unless "
                "'Allow writes' is enabled in the editor's MCP Server panel (off by default). Discover the "
                "exact writable (component, field) names + value shapes for an entity with olo_entity_list_fields.";
            tool.InputSchema = GenericFieldWrite::InputSchema();
            tool.MainMarshaled = true;
            tool.Handler = Handle_EntitySetField;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_reload_script";
            tool.Toolset = "scripting";
            tool.Title = "Reload script assembly";
            // A project-WRITE tool (#306 item C): reloading swaps in the user's
            // freshly-built C# assembly and runs its code, so it is gated behind the
            // session "Allow writes" toggle, like the other writes. readOnlyHint:false
            // (not idempotent — each reload re-reads from disk into a fresh app domain;
            // not destructive — it overwrites no project data, the source files are
            // untouched).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Reload the C# script assembly — the scripting inner loop: build the game assembly, reload it "
                "here, and the editor runs the new script code without restarting. Drives the same "
                "ScriptEngine::ReloadAssembly() path as the editor's Script menu Reload assembly (Ctrl+R), so "
                "the reload is whole-assembly (C# has no per-script granularity) and the tool takes no "
                "arguments. Returns whether scripting is available in this build, whether the reload SUCCEEDED "
                "(ok:false when the freshly-built app assembly fails to load — e.g. a compile error — see the "
                "engine log), and how many entity-script classes are registered afterwards. This is a WRITE tool: it is refused "
                "unless 'Allow writes' is enabled in the editor's MCP Server panel (off by default), because "
                "reloading executes the freshly-built assembly. If C# scripting is disabled or uninitialized "
                "the call still succeeds but reports available:false.";
            tool.InputSchema = ReloadScript::InputSchema();
            tool.MainMarshaled = true;
            tool.Handler = Handle_ReloadScript;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_renderer_settings_set";
            tool.Toolset = "render";
            tool.Title = "Set renderer / post-process setting";
            // A project-WRITE tool (#306 item C): it mutates the session-global
            // renderer settings, which crosses the read-only line, so it is gated
            // behind the "Allow writes" session toggle like the other writes.
            // readOnlyHint:false; idempotent (setting a value twice leaves the same
            // state — the enum-valued sibling of the flip-based toggle_pass); not
            // destructive (fully reversible by setting the reported previousValue).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Set a multi-valued renderer / post-process setting to verify a rendering feature LIVE at each value — "
                "the enum-valued sibling of the on/off olo_render_toggle_pass. Settings: 'upscale' (FSR1 spatial-upscale "
                "mode: off|quality|balanced|performance|ultraperformance — the #480 case), 'tonemap' (none|reinhard|aces|"
                "uncharted2), 'renderpath' (forward|forwardplus|deferred; switching rebuilds the render graph, and "
                "Deferred is required for SSR/SSGI), plus the two big perf levers (#316): 'depthprepass' (off|on|auto — "
                "forces the live depth-prepass state; 'auto' restores the settings-derived value; Forward+/Deferred "
                "derive it on for tile culling) and 'softshadows' (pcf|pcss — PCSS is the dominant ScenePass cost in "
                "shadowed scenes; A/B it in one call instead of editing shader source). Call with NO arguments to list "
                "every setting with its current value and allowed values. The change is session-global and ephemeral (a "
                "scene reload restores it); the response reports 'previousValue' so you can restore by calling again "
                "with that token — this is restore-prior-value, NOT an undo-stack entry (unlike olo_entity_set_field). "
                "This is a WRITE tool: it is refused unless 'Allow writes' is enabled in the editor's MCP Server panel "
                "(off by default).";
            tool.InputSchema = RendererSettings::InputSchema();
            tool.MainMarshaled = true;
            tool.Handler = Handle_RendererSettingsSet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_why_not_visible";
            tool.Toolset = "render";
            tool.Title = "Explain entity not visible";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Explain why an entity is NOT visible on screen — the rendering counterpart of "
                "olo_physics_why_no_collision ('why can't I see my mesh?'). Given an entity UUID, it checks, in "
                "order: a scene is loaded, the entity exists, it has a renderable component (Mesh/Model/Sprite/"
                "Circle/Text/InstancedMesh/...), its geometry asset is present, its visibility flag is on, its "
                "transform scale is non-degenerate, its material's shader compiled, and (against the editor "
                "camera) it is in front of the camera and inside the view frustum. Returns the root-cause "
                "reasonCode, a human summary, the ordered checks, and the raw facts. Note: per-frame occlusion "
                "(HZB) and LOD culling are not queryable from the editor and are reported as not-observable.";
            tool.InputSchema = Schema::Object()
                                   .Prop("entity", Schema::String().Desc("Entity UUID (string; also accepts a number)."))
                                   .Required({ "entity" })
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderWhyNotVisible;
            server.RegisterTool(std::move(tool));
        }

        // ---- olo_scene_open / olo_scene_play / olo_scene_stop (#316 Part 5) ----
        // The scriptable scene-control write tools: switch the active scene and
        // toggle Play mode over MCP. All three are ProjectWrite (gated behind the
        // "Allow writes" session toggle) and MainMarshaled (they touch the registry /
        // runtime). Registered here as a clean append-only block so they don't
        // conflict with in-flight branches editing the earlier registrations.
        {
            ToolDef tool;
            tool.Name = "olo_scene_open";
            tool.Toolset = "scene";
            tool.Title = "Open / switch scene";
            // A project-WRITE tool: switching scenes discards the current in-memory
            // scene, so it is gated behind "Allow writes". readOnlyHint:false; NOT
            // idempotent (each call reloads from disk, discarding unsaved state); not
            // destructive to the project files (it never writes — the source scene is
            // untouched, only the in-editor scene changes).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Open / switch the active scene — the scriptable scene-switch that lets an agent set up a repro "
                "without a manual project StartScene edit + editor relaunch. Give a 'path' to a .olo or .scene file "
                "(relative paths resolve against the project asset directory, e.g. \"Scenes/Sandbox.olo\"; an absolute "
                "path also works). Loads the scene directly, the same install path as the editor's File > Open Scene "
                "but WITHOUT the auto-save recovery modal (a remote agent can't click it). If Play mode is running it "
                "is stopped first. Returns whether the scene loaded (ok), the resolved path, the new scene name and "
                "entity count. This is a WRITE tool: it is refused unless 'Allow writes' is enabled in the editor's "
                "MCP Server panel (off by default).";
            tool.InputSchema = SceneControl::OpenInputSchema();
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneOpen;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_play";
            tool.Toolset = "scene";
            tool.Title = "Enter Play mode";
            // A project-WRITE tool: entering Play copies the scene and runs the user's
            // game scripts, so it is gated behind "Allow writes". readOnlyHint:false;
            // idempotent (already playing -> no-op, changed:false); not destructive
            // (fully reversible — olo_scene_stop restores the authored scene).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Enter Play mode — start the runtime simulation, the same as the editor's Play button (and the "
                "OLO_EDITOR_AUTOPLAY workaround, without a relaunch). Needed to verify anything that only runs in "
                "Play: physics, cloth/soft-body, scripts, animation. Copies the authored scene and starts the "
                "runtime; already-playing is a no-op (changed:false). Entering Play can fail if the scene has no "
                "primary camera — then ok:false and the editor stays in Edit (see the message). Confirm with "
                "olo_scene_summary's isPlaying, then olo_screenshot. Fully reversible via olo_scene_stop. This is a "
                "WRITE tool: it is refused unless 'Allow writes' is enabled in the editor's MCP Server panel (off by "
                "default).";
            tool.InputSchema = SceneControl::PlayStopInputSchema();
            tool.MainMarshaled = true;
            tool.Handler = Handle_ScenePlay;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_stop";
            tool.Toolset = "scene";
            tool.Title = "Stop Play mode";
            // A project-WRITE tool for symmetry with olo_scene_play (it ends the
            // runtime and restores the authored scene). idempotent (already stopped ->
            // no-op); not destructive (restores the pre-Play authored scene).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Stop Play mode — end the runtime simulation and restore the authored (Edit-mode) scene, the same as "
                "the editor's Stop button. Discards the transient runtime scene copy, so any runtime-only changes are "
                "dropped (exactly like the editor). Already-stopped is a no-op (changed:false). Confirm with "
                "olo_scene_summary's isPlaying. This is a WRITE tool: it is refused unless 'Allow writes' is enabled "
                "in the editor's MCP Server panel (off by default).";
            tool.InputSchema = SceneControl::PlayStopInputSchema();
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneStop;
            server.RegisterTool(std::move(tool));
        }

        // ---- Resources ---------------------------------------------------------

        {
            ResourceDef resource;
            resource.Uri = "olo://scene/current";
            resource.Name = "Current scene (YAML)";
            resource.Description = "The active scene serialized to YAML (every entity and component), read "
                                   "live from the editor on the main thread.";
            resource.MimeType = "text/yaml";
            resource.Reader = [](McpServer& s) -> std::string
            {
                const Json marshaled = s.MarshalRead([&s]() -> Json
                                                     {
                    const Ref<Scene> scene = s.Context().GetActiveScene ? s.Context().GetActiveScene() : nullptr;
                    if (!scene)
                        return Json{ { "__error", "No active scene" } };
                    SceneSerializer serializer(scene);
                    return Json(serializer.SerializeToYAML()); });
                if (marshaled.is_object() && marshaled.contains("__error"))
                    throw std::runtime_error(marshaled["__error"].get<std::string>());
                return marshaled.get<std::string>();
            };
            server.RegisterResource(std::move(resource));
        }

        {
            ResourceDef resource;
            resource.Uri = "olo://logs/recent";
            resource.Name = "Recent engine logs";
            resource.Description = "The most recent engine log messages (up to 200) from the in-memory ring buffer.";
            resource.MimeType = "text/plain";
            resource.Reader = [](McpServer& /*s*/) -> std::string
            {
                const std::vector<std::string> messages = Log::Get().GetRecentLogMessages(200);
                std::string out;
                for (const auto& message : messages)
                {
                    std::string_view line = message;
                    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                        line.remove_suffix(1);
                    out.append(line);
                    out.push_back('\n');
                }
                return out;
            };
            server.RegisterResource(std::move(resource));
        }

        // ---- Prompts (canned workflows for non-expert users) -------------------

        {
            PromptDef prompt;
            prompt.Name = "diagnose-performance";
            prompt.Title = "Diagnose performance";
            prompt.Description = "Find out why the running scene is slow and what to do about it.";
            prompt.Text =
                "Diagnose the performance of the scene currently running in OloEditor. Steps:\n"
                "1. Call olo_perf_bottlenecks to see whether the frame is CPU/GPU/Memory/IO bound.\n"
                "2. Call olo_perf_snapshot for fps, frame time, draw calls and instancing counts.\n"
                "3. If GPU-bound, call olo_perf_capture_frame to find the most expensive passes/draw calls.\n"
                "4. Call olo_memory_report if memory is implicated.\n"
                "Then give a short, prioritized list of concrete fixes, citing the specific numbers you saw.";
            server.RegisterPrompt(std::move(prompt));
        }

        {
            PromptDef prompt;
            prompt.Name = "explain-last-script-error";
            prompt.Title = "Explain my last script error";
            prompt.Description = "Explain the most recent C#/Lua script exception and how to fix it.";
            prompt.Text =
                "Explain the most recent scripting error in this OloEngine project. Steps:\n"
                "1. Call olo_script_get_last_errors to get the latest C#/Lua exceptions (message, script, entity).\n"
                "2. If an entity id is given, call olo_scene_get_entity on it to see its components.\n"
                "3. Call olo_script_get_api (matching the error's language) to check the correct API usage.\n"
                "4. Optionally call olo_log_tail for surrounding context.\n"
                "Then explain the root cause in plain terms and give a concrete fix.";
            server.RegisterPrompt(std::move(prompt));
        }

        {
            PromptDef prompt;
            prompt.Name = "why-cant-i-see-my-object";
            prompt.Title = "Why can't I see my object?";
            prompt.Description = "Figure out why an entity isn't visible in the scene.";
            prompt.Text =
                "Help figure out why an object isn't visible in the running OloEditor scene. Steps:\n"
                "1. Call olo_scene_summary to confirm a scene is loaded and whether it's playing.\n"
                "2. Call olo_scene_list_entities (optionally with a namePattern) to find the entity.\n"
                "3. Call olo_scene_get_entity on it: check its Transform (position/scale), whether it has a "
                "MeshComponent/MaterialComponent, and whether it's parented oddly.\n"
                "4. Call olo_screenshot to see the current frame, and olo_shader_errors in case its material's "
                "shader failed to compile.\n"
                "Then state the most likely reason it's not visible and how to fix it.";
            server.RegisterPrompt(std::move(prompt));
        }
    }
} // namespace OloEngine::MCP
