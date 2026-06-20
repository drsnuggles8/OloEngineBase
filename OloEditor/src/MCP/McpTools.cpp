#include "OloEnginePCH.h"
#include "MCP/McpTools.h"
#include "MCP/McpServer.h"
#include "MCP/McpScriptApi.h"
#include "MCP/McpGoldenCompare.h"
#include "MCP/McpPhysicsExplain.h"
#include "MCP/McpRenderExplain.h"
#include "MCP/McpShaderReload.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Debug/CapturedFrameData.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
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

            return ToolResult::Text(summary.dump(2));
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
            return ToolResult::Text(result.dump(2));
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
            return ToolResult::Text(j.dump(2));
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
                return o; });
            return ToolResult::Text(j.dump(2));
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
                        "renderer's timer-query pool.";
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

        // Format an epoch-seconds timestamp as a UTC "HH:MM:SS.mmm" wall-clock string.
        // Computed with modular arithmetic to dodge the non-thread-safe / platform-split
        // gmtime APIs — the date is irrelevant for a "what just happened" timeline.
        std::string FormatEpochUtcTime(f64 epochSeconds)
        {
            if (!std::isfinite(epochSeconds) || epochSeconds <= 0.0)
                return std::string{};
            const auto total = static_cast<std::int64_t>(epochSeconds);
            const auto secOfDay = static_cast<int>(((total % 86400) + 86400) % 86400);
            const int milliseconds = static_cast<int>((epochSeconds - static_cast<f64>(total)) * 1000.0);
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d",
                          secOfDay / 3600, (secOfDay % 3600) / 60, secOfDay % 60, std::clamp(milliseconds, 0, 999));
            return std::string(buffer);
        }

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
            {
                Json j;
                j["id"] = event.Id;
                j["category"] = DiagnosticEvent::CategoryToString(event.Category);
                if (std::string time = FormatEpochUtcTime(event.Timestamp); !time.empty())
                    j["time"] = std::move(time);
                j["message"] = event.Message;
                if (event.Entity != 0)
                    j["entity"] = std::to_string(event.Entity);
                if (!event.Context.empty())
                    j["context"] = event.Context;
                arr.push_back(std::move(j));
            }

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
            return ToolResult::Text(result.dump(2));
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
    } // namespace

    void RegisterBuiltinTools(McpServer& server)
    {
        {
            ToolDef tool;
            tool.Name = "olo_log_tail";
            tool.Description =
                "Return the most recent engine log messages from OloEditor's in-memory ring buffer "
                "(up to 200 lines). Use this to see what the engine just logged — warnings, errors, "
                "and tagged messages from asset/scene/physics/script/renderer subsystems.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "count",
                      { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 }, { "description", "How many of the most recent matching log lines to return (default 50)." } } },
                    { "minLevel",
                      { { "type", "string" }, { "enum", Json::array({ "trace", "debug", "info", "warn", "error", "critical" }) }, { "description", "Only return lines at this severity or higher." } } },
                    { "tag",
                      { { "type", "string" },
                        { "description", "Only return lines whose [Tag] matches exactly (e.g. Physics, Scene, Script)." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = false;
            tool.Handler = Handle_LogTail;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_summary";
            tool.Description =
                "Summarise the active scene currently open in the editor: its name, whether the game "
                "is playing or paused, whether a scene is loaded, and the total entity count. Read "
                "directly from the live ECS on the editor's main thread (a consistent frame snapshot).";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties", Json::object() },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneSummary;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_list_entities";
            tool.Description =
                "List entities in the active scene (paginated). Each entry has the entity's UUID, name, "
                "parent UUID (if any), and child count. Optionally filter by a name substring. Use this "
                "to find an entity, then call olo_scene_get_entity with its id for full component data.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "namePattern", { { "type", "string" }, { "description", "Case-sensitive substring to match against entity names." } } },
                    { "page", { { "type", "integer" }, { "minimum", 0 }, { "description", "Zero-based page index (default 0)." } } },
                    { "pageSize", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 }, { "description", "Entities per page (default 50, max 200)." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneListEntities;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_get_entity";
            tool.Description =
                "Get the full component data of one entity by UUID, serialized from the live scene (YAML "
                "in 'componentsYaml', plus structured id/name/parent/children). Pair with "
                "olo_scene_list_entities or olo_scene_summary to obtain the UUID.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "id", { { "type", "string" }, { "description", "Entity UUID (as a string; also accepts a number)." } } } } },
                { "required", Json::array({ "id" }) },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneGetEntity;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_memory_report";
            tool.Description =
                "Renderer GPU/CPU memory usage: total bytes/MB, a per-resource-type breakdown (vertex/index/"
                "uniform/storage buffers, textures, framebuffers, shaders, render targets), and the count of "
                "suspected leaks. Read from the engine's mutex-guarded memory tracker.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties", Json::object() },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = false;
            tool.Handler = Handle_MemoryReport;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_perf_snapshot";
            tool.Description =
                "Current-frame renderer performance: fps, frame/CPU/GPU time (ms), draw calls, instanced "
                "draw calls, triangles, state/shader/texture binds. Server-computed snapshot from the live "
                "profiler. Map a low fps back to draw calls / lack of instancing.";
            tool.InputSchema = Json{ { "type", "object" }, { "properties", Json::object() }, { "additionalProperties", false } };
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfSnapshot;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_perf_bottlenecks";
            tool.Description =
                "The engine's automatic bottleneck analysis: which of CPU/GPU/Memory/IO is limiting the "
                "frame, a confidence score, a human description, and concrete recommendations.";
            tool.InputSchema = Json{ { "type", "object" }, { "properties", Json::object() }, { "additionalProperties", false } };
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfBottlenecks;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_perf_frame_history";
            tool.Description =
                "A downsampled time series of recent frames (frameTimeMs, fps, drawCalls) from the profiler's "
                "ring buffer, for spotting spikes/trends. The server downsamples to 'points' samples.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties", { { "points", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 300 }, { "description", "Number of downsampled points to return (default 60)." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfFrameHistory;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_perf_capture_frame";
            tool.Description =
                "Capture the current frame and return its breakdown: frame totals, render passes, and the "
                "top-K draw calls by GPU time. Per-pass detail requires the editor's frame-capture "
                "instrumentation; frame-level totals are always returned.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties", { { "topK", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 50 }, { "description", "How many of the most expensive draw calls to return (default 10)." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_PerfCaptureFrame;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_shader_errors";
            tool.Description =
                "Shaders that currently have compile/link errors, with the error message. Empty when all "
                "shaders compiled cleanly.";
            tool.InputSchema = Json{ { "type", "object" }, { "properties", Json::object() }, { "additionalProperties", false } };
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderErrors;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_shader_get";
            tool.Description =
                "Details of one shader by name or numeric id: instruction count, compile time, uniforms, "
                "uniform buffers, samplers, reload count, and (with includeGlsl) the cross-compiled GLSL per stage.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "name", { { "type", "string" }, { "description", "Shader name (as shown by olo_shader_errors / the shader debugger)." } } },
                    { "id", { { "type", "integer" }, { "description", "GL program id (alternative to name)." } } },
                    { "includeGlsl", { { "type", "boolean" }, { "description", "Include the cross-compiled GLSL source per stage (default false)." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderGet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_shader_list";
            tool.Description =
                "Inventory of all registered shaders (id, name, hasErrors, instruction count). Use it to "
                "discover a shader name/id to pass to olo_shader_get.";
            tool.InputSchema = Json{ { "type", "object" }, { "properties", Json::object() }, { "additionalProperties", false } };
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderList;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_shader_reload";
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
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "name", { { "type", "string" }, { "description", "Shader name to reload (as shown by olo_shader_list)." } } } } },
                { "required", Json::array({ "name" }) },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderReload;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_assets_list";
            tool.Description =
                "List the project's registered assets (paginated): handle, type, project-relative path, and "
                "filename. Optionally filter by asset type (e.g. Texture2D, Mesh, Material, Scene, Script).";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "typeFilter", { { "type", "string" }, { "description", "Asset type name to filter by (e.g. 'Texture2D'). Omit for all types." } } },
                    { "page", { { "type", "integer" }, { "minimum", 0 }, { "description", "Zero-based page index (default 0)." } } },
                    { "pageSize", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 }, { "description", "Assets per page (default 50, max 200)." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_AssetsList;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_assets_problems";
            tool.Description =
                "List assets that failed to load or are missing/invalid (handle, type, path, status). The "
                "first thing to check when something references an asset that isn't showing up.";
            tool.InputSchema = Json{ { "type", "object" }, { "properties", Json::object() }, { "additionalProperties", false } };
            tool.MainMarshaled = true;
            tool.Handler = Handle_AssetsProblems;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_crash_list";
            tool.Description =
                "List crash reports written by the engine (crash_<timestamp>.txt under CrashReports/). Each "
                "entry has an id and size. Use olo_crash_get to read one.";
            tool.InputSchema = Json{ { "type", "object" }, { "properties", Json::object() }, { "additionalProperties", false } };
            tool.MainMarshaled = false;
            tool.Handler = Handle_CrashList;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_crash_get";
            tool.Description =
                "Read a crash report's full text (exception, system info, last 200 log lines) by its id from "
                "olo_crash_list. Useful for an AI-summarised, shareable bug report.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties", { { "id", { { "type", "string" }, { "description", "Crash report filename (e.g. crash_20260606_143025_123.txt) from olo_crash_list." } } } } },
                { "required", Json::array({ "id" }) },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = false;
            tool.Handler = Handle_CrashGet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_script_get_api";
            tool.Description =
                "Describe the scripting API a game can call. language='csharp' lists the C# bindings "
                "(OloEngine-ScriptCore); language='lua' lists the Sol2 usertypes. Without typeFilter you get "
                "the type index; with a typeFilter substring you get matching types and their members. Use "
                "this to answer 'how do I ...' questions grounded in the actual engine API.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "language", { { "type", "string" }, { "enum", Json::array({ "csharp", "lua" }) }, { "description", "Scripting language (default csharp)." } } },
                    { "typeFilter", { { "type", "string" }, { "description", "Case-insensitive substring; matching types are returned with their members." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = false;
            tool.Handler = Handle_ScriptGetApi;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_script_get_last_errors";
            tool.Description =
                "Return the most recent C# (Mono) and Lua (Sol2) script exceptions captured by the engine "
                "(message, originating script/method, entity UUID when known, timestamp). This is the #1 "
                "thing to check when a game's scripts misbehave. Empty if no script errors have occurred.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "count", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 64 }, { "description", "How many of the most recent errors to return (default 20)." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = false;
            tool.Handler = Handle_ScriptGetLastErrors;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_events_tail";
            tool.Description =
                "Return the unified 'what just happened?' event timeline from the engine's diagnostics "
                "ring buffer: scene loads, entering/leaving Play mode, runtime entity spawn/destroy, asset "
                "hot-reloads, and script errors — newest last, each with a monotonic 'id'. The key use is "
                "INCREMENTAL POLLING: do an action, then pass the previous call's 'lastId' as 'sinceId' to "
                "get only what happened since. Filter with 'categories'. Bulk churn (scene-copy on Play, "
                "deserialize on load) is collapsed into single scene_load/play events, not per-entity spam.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "count",
                      { { "type", "integer" }, { "minimum", 1 }, { "maximum", 500 }, { "description", "How many of the most recent matching events to return (default 50)." } } },
                    { "sinceId",
                      { { "type", Json::array({ "integer", "string" }) }, { "minimum", 0 }, { "description", "Only return events with id greater than this. Accepts the id as a number or its string form (for large cursors beyond JSON integer precision). Pass back the previous response's 'lastId' for incremental polling." } } },
                    { "categories",
                      { { "type", "array" },
                        { "items", { { "type", "string" }, { "enum", Json::array({ "scene_load", "play", "stop", "entity_spawn", "entity_destroy", "asset_reload", "script_error" }) } } },
                        { "description", "Only return events whose category is in this list. Omit for all categories." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = false;
            tool.Handler = Handle_EventsTail;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_screenshot";
            tool.Description =
                "Capture the editor's 3D viewport as a PNG image so you can SEE the rendered frame — "
                "decisive for visual problems ('my material looks wrong', 'I can't see my object'). "
                "Returns an image content block (downscaled to maxWidth, default 1024). Optionally pose "
                "the camera for this capture only via 'camera' (explicit position + target/yaw/pitch) or "
                "'orbit' (target + yaw/pitch/distance): the prior camera is saved, the new pose is "
                "rendered ('settleFrames' frames, default 2, for TAA/temporal effects to settle), the "
                "frame is captured, and the user's camera is restored — so multiple angles can be "
                "captured without disturbing the viewport.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "maxWidth", { { "type", "integer" }, { "minimum", 16 }, { "maximum", 4096 }, { "description", "Max output width in pixels (default 1024); aspect ratio preserved." } } },
                    { "camera",
                      { { "type", "object" },
                        { "description", "Capture from this pose, then restore the prior camera. Same shape as olo_camera_set_pose: position [x,y,z] plus target [x,y,z] or yaw/pitch (degrees); optional fov." } } },
                    { "orbit",
                      { { "type", "object" },
                        { "description", "Capture from this orbit pose, then restore. Same shape as olo_camera_orbit: target [x,y,z], yaw/pitch (degrees), distance; optional fov." } } },
                    { "settleFrames", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 30 }, { "description", "Frames to render at the new pose before capturing (default 2). Raise for temporal effects (TAA, fog history) to settle." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_Screenshot;
            server.RegisterTool(std::move(tool));
        }

        // ---- Tier-0 camera / viewport control (issue #316) ---------------------

        {
            ToolDef tool;
            tool.Name = "olo_camera_get";
            tool.Description =
                "Get the editor camera's current pose: position, focal point, forward vector, yaw/pitch "
                "(degrees), orbit distance, FOV, near/far clip, and the viewport size in logical pixels. "
                "Read this before moving the camera so you can put it back, or to reason about what the "
                "viewport is looking at.";
            tool.InputSchema = Json{ { "type", "object" }, { "properties", Json::object() }, { "additionalProperties", false } };
            tool.MainMarshaled = true;
            tool.Handler = Handle_CameraGet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_camera_set_pose";
            tool.Description =
                "Move the editor camera to an explicit pose (Tier-0 inspection state; never touches the "
                "project). Give 'position' plus either 'target' (a point to look at) or 'yaw'/'pitch' in "
                "degrees; optional 'fov' (vertical, degrees). Returns the resulting pose. The viewport "
                "renders the new pose from the next frame.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "position", { { "type", "array" }, { "items", Json{ { "type", "number" } } }, { "minItems", 3 }, { "maxItems", 3 }, { "description", "Camera eye position [x, y, z] (world units)." } } },
                    { "target", { { "type", "array" }, { "items", Json{ { "type", "number" } } }, { "minItems", 3 }, { "maxItems", 3 }, { "description", "Point to look at [x, y, z]. Alternative to yaw/pitch." } } },
                    { "yaw", { { "type", "number" }, { "description", "Yaw in degrees (0 looks along -Z; positive turns right). Alternative to target." } } },
                    { "pitch", { { "type", "number" }, { "description", "Pitch in degrees (positive looks down). Alternative to target." } } },
                    { "fov", { { "type", "number" }, { "minimum", 1 }, { "maximum", 170 }, { "description", "Vertical field of view in degrees (omit to keep current)." } } } } },
                { "required", Json::array({ "position" }) },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_CameraSetPose;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_camera_orbit";
            tool.Description =
                "Orbit-frame the editor camera around a world-space target point: the camera pivots at "
                "'distance' from 'target' looking at it, with 'yaw'/'pitch' in degrees (positive pitch "
                "looks down). Keeps a live orbit pivot so subsequent user orbiting feels natural. "
                "Returns the resulting pose.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "target", { { "type", "array" }, { "items", Json{ { "type", "number" } } }, { "minItems", 3 }, { "maxItems", 3 }, { "description", "Orbit centre [x, y, z] (world units)." } } },
                    { "yaw", { { "type", "number" }, { "description", "Orbit yaw in degrees (default 0)." } } },
                    { "pitch", { { "type", "number" }, { "description", "Orbit pitch in degrees, positive looks down (default 30)." } } },
                    { "distance", { { "type", "number" }, { "exclusiveMinimum", 0 }, { "description", "Distance from the target in world units (default 10)." } } },
                    { "fov", { { "type", "number" }, { "minimum", 1 }, { "maximum", 170 }, { "description", "Vertical field of view in degrees (omit to keep current)." } } } } },
                { "required", Json::array({ "target" }) },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_CameraOrbit;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_camera_frame_entity";
            tool.Description =
                "Point the editor camera at one entity and fit it in view (like pressing 'frame selected' "
                "in a DCC tool). Uses the entity's mesh/model/terrain bounds when available, otherwise its "
                "transform scale. Keeps the current view direction. Get the UUID from "
                "olo_scene_list_entities. Returns the resulting pose.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "id", { { "type", "string" }, { "description", "Entity UUID (as a string; also accepts a number)." } } } } },
                { "required", Json::array({ "id" }) },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_CameraFrameEntity;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_viewport_set_size";
            tool.Description =
                "Override the editor viewport's logical render size for deterministic capture resolution "
                "(e.g. 1280x720 golden-image comparisons), independent of the panel layout. Pass "
                "'reset': true to return control to the ImGui panel size. The override persists until "
                "reset — reset it when you are done capturing.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "width", { { "type", "integer" }, { "minimum", 64 }, { "maximum", 8192 }, { "description", "Viewport width in logical pixels." } } },
                    { "height", { { "type", "integer" }, { "minimum", 64 }, { "maximum", 8192 }, { "description", "Viewport height in logical pixels." } } },
                    { "reset", { { "type", "boolean" }, { "description", "true = clear the override and use the panel size again." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_ViewportSetSize;
            server.RegisterTool(std::move(tool));
        }

        // ---- Render-target capture (Part 4 of #316) -----------------------------

        {
            ToolDef tool;
            tool.Name = "olo_render_list_targets";
            tool.Description =
                "List the render graph's live texture/framebuffer resources for the current frame — "
                "scene colour, depth, G-buffer attachments, shadow maps, AO, post-process chain stages, "
                "water/OIT buffers, etc. Each entry has the canonical resource name (pass to "
                "olo_render_capture_target), kind, format, size, and producing passes. Requires the "
                "editor to be rendering in 3D mode.";
            tool.InputSchema = Json{ { "type", "object" }, { "properties", Json::object() }, { "additionalProperties", false } };
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderListTargets;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_capture_target";
            tool.Description =
                "Read back one intermediate render target as a PNG image — THE tool for rendering-feature "
                "development: inspect depth, normals, G-buffer albedo/emissive, shadow maps, AO, bloom, or "
                "any post-process stage directly instead of guessing from the final frame. 'name' is a "
                "render-graph resource name from olo_render_list_targets (e.g. SceneColor, SceneDepth, "
                "GBufferNormal, ShadowMapCSM, AOBuffer, BloomColor). Float/HDR sources are clamped to "
                "[0,1]; depth is min-max normalised by default ('normalize' overrides). Returns metadata "
                "(format, size, value range) plus the image.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "name", { { "type", "string" }, { "description", "Render-graph resource name (see olo_render_list_targets)." } } },
                    { "mip", { { "type", "integer" }, { "minimum", 0 }, { "maximum", 16 }, { "description", "Mip level to capture (default 0)." } } },
                    { "face", { { "type", "integer" }, { "minimum", 0 }, { "maximum", 64 }, { "description", "Cubemap face (0..5 = +X,-X,+Y,-Y,+Z,-Z) or texture-array layer (default 0)." } } },
                    { "normalize", { { "type", "boolean" }, { "description", "Min-max normalise float values to [0,1] before encoding (default: true for depth, false otherwise)." } } },
                    { "maxWidth", { { "type", "integer" }, { "minimum", 16 }, { "maximum", 4096 }, { "description", "Max output width in pixels (default 1024); aspect ratio preserved." } } } } },
                { "required", Json::array({ "name" }) },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderCaptureTarget;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_compare_golden";
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
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "goldenPath", { { "type", "string" }, { "description", "Golden PNG path under assets/tests/visual/ (e.g. 'water_side.png'). A '.png' extension is added if missing. Relative only — no '..' or absolute paths." } } },
                    { "threshold", { { "type", "number" }, { "minimum", 0 }, { "maximum", 1 }, { "description", "Minimum SSIM similarity in [0,1] to pass (1 = identical). Omit to use the suite's RMSE→SSIM cascade verdict (the default, consistent with the golden test suite)." } } },
                    { "rebase", { { "type", "boolean" }, { "description", "true = overwrite the golden with the current capture instead of comparing (re-baseline after a deliberate visual change). A missing golden is always created regardless." } } },
                    { "camera",
                      { { "type", "object" },
                        { "description", "Capture from this pose, then restore the prior camera. Same shape as olo_camera_set_pose: position [x,y,z] plus target [x,y,z] or yaw/pitch (degrees); optional fov." } } },
                    { "orbit",
                      { { "type", "object" },
                        { "description", "Capture from this orbit pose, then restore. Same shape as olo_camera_orbit: target [x,y,z], yaw/pitch (degrees), distance; optional fov." } } },
                    { "settleFrames", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 30 }, { "description", "Frames to render at the new pose before capturing (default 2). Raise for temporal effects (TAA, fog history) to settle." } } },
                    { "maxWidth", { { "type", "integer" }, { "minimum", 16 }, { "maximum", 4096 }, { "description", "Max capture width in pixels (default 1024); aspect ratio preserved. Must match between create and compare." } } } } },
                { "required", Json::array({ "goldenPath" }) },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true; // reads main-thread-only camera/viewport state (like olo_screenshot)
            tool.Handler = Handle_RenderCompareGolden;
            server.RegisterTool(std::move(tool));
        }

        // ---- Physics introspection + explain tools (#306 item A) ---------------

        {
            ToolDef tool;
            tool.Name = "olo_physics_layer_matrix";
            tool.Description =
                "Dump the physics collision-layer matrix the simulation actually uses: the five built-in "
                "Jolt object layers (NON_MOVING, MOVING, TRIGGER, CHARACTER, DEBRIS) plus every user-defined "
                "physics layer, with the pairwise collide/no-collide result from the real layer filter. Use "
                "this to confirm whether two layers are even allowed to collide. Works in Edit mode too.";
            tool.InputSchema = Json{ { "type", "object" }, { "properties", Json::object() }, { "additionalProperties", false } };
            tool.MainMarshaled = false;
            tool.Handler = Handle_PhysicsLayerMatrix;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_list_colliders";
            tool.Description =
                "List every entity with a Rigidbody3DComponent (paginated): authored body type "
                "(Static/Dynamic/Kinematic), collision layer id, trigger flag, and collider shape(s). When "
                "physics is running, also reports the live body's object layer, world position, and "
                "awake/asleep state. Pair with olo_physics_why_no_collision to debug missing collisions.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "page", { { "type", "integer" }, { "minimum", 0 }, { "description", "Zero-based page index (default 0)." } } },
                    { "pageSize", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 }, { "description", "Entities per page (default 50, max 200)." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsListColliders;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_contacts";
            tool.Description =
                "List the entity pairs whose physics bodies are touching right now (live active-contact set, "
                "deduplicated per pair). Requires Play mode. Use this to confirm a collision/trigger is "
                "actually being detected by the engine.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "maxResults", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 2000 }, { "description", "Max contact pairs to return (default 200)." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsContacts;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_raycast";
            tool.Description =
                "Cast a ray through the live physics world and return what it hits. Specify 'origin' plus "
                "either 'direction' (a vector) or 'to' (an end point). Returns the closest hit by default, or "
                "up to 'maxHits' ordered hits, each with the hit entity, world position, surface normal, and "
                "distance. Requires Play mode.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "origin", { { "type", "array" }, { "description", "Ray start [x, y, z]." } } },
                    { "direction", { { "type", "array" }, { "description", "Ray direction [x, y, z] (need not be normalised). Provide this or 'to'." } } },
                    { "to", { { "type", "array" }, { "description", "Ray end point [x, y, z]; sets direction and distance. Provide this or 'direction'." } } },
                    { "maxDistance", { { "type", "number" }, { "description", "Max ray length when using 'direction' (default 500)." } } },
                    { "maxHits", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 64 }, { "description", "Return up to N ordered hits (default 1 = closest only)." } } } } },
                { "required", Json::array({ "origin" }) },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsRaycast;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_overlap";
            tool.Description =
                "Find the physics bodies overlapping a shape at a world point. Pass 'origin' plus 'radius' "
                "for a sphere (the default), or 'halfExtents' [x,y,z] for a box. Returns the overlapping "
                "entities and their positions. Requires Play mode.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "origin", { { "type", "array" }, { "description", "Query centre [x, y, z]." } } },
                    { "radius", { { "type", "number" }, { "description", "Sphere radius (default 0.5; ignored if 'halfExtents' is given)." } } },
                    { "halfExtents", { { "type", "array" }, { "description", "Box half-extents [x, y, z]; selects a box query instead of a sphere." } } },
                    { "maxHits", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 256 }, { "description", "Max overlapping bodies to return (default 32)." } } } } },
                { "required", Json::array({ "origin" }) },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsOverlap;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_physics_why_no_collision";
            tool.Description =
                "Explain why two entities are NOT colliding — the 'player falls through the floor' debugger. "
                "Given two entity UUIDs ('a' and 'b'), it checks, in order: physics running, both entities "
                "exist, both have a rigidbody + collider + live body, not both Static, their collision layers "
                "are allowed to collide, neither is a trigger, and their bounds overlap. Returns the root-cause "
                "reasonCode, a human summary, the ordered checks performed, and the raw facts for each entity.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "a", { { "type", "string" }, { "description", "First entity UUID (string; also accepts a number)." } } },
                    { "b", { { "type", "string" }, { "description", "Second entity UUID (string; also accepts a number)." } } } } },
                { "required", Json::array({ "a", "b" }) },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_PhysicsWhyNoCollision;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_why_not_visible";
            tool.Description =
                "Explain why an entity is NOT visible on screen — the rendering counterpart of "
                "olo_physics_why_no_collision ('why can't I see my mesh?'). Given an entity UUID, it checks, in "
                "order: a scene is loaded, the entity exists, it has a renderable component (Mesh/Model/Sprite/"
                "Circle/Text/InstancedMesh/...), its geometry asset is present, its visibility flag is on, its "
                "transform scale is non-degenerate, its material's shader compiled, and (against the editor "
                "camera) it is in front of the camera and inside the view frustum. Returns the root-cause "
                "reasonCode, a human summary, the ordered checks, and the raw facts. Note: per-frame occlusion "
                "(HZB) and LOD culling are not queryable from the editor and are reported as not-observable.";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "entity", { { "type", "string" }, { "description", "Entity UUID (string; also accepts a number)." } } } } },
                { "required", Json::array({ "entity" }) },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderWhyNotVisible;
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
