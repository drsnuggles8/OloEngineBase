#include "OloEnginePCH.h"
#include "MCP/McpTools.h"
#include "MCP/McpServer.h"
#include "MCP/McpScriptApi.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Debug/CapturedFrameData.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scripting/ScriptError.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
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
