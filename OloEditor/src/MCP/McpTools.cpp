#include "MCP/McpTools.h"
#include "MCP/McpServer.h"
#include "MCP/McpScriptApi.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Scripting/ScriptError.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OloEngine::MCP
{
    namespace
    {
        // ---- olo_log_tail (lock-safe) ------------------------------------------
        // Wraps Log::GetRecentLogMessages, which reads spdlog's mutex-guarded
        // ring-buffer sink — safe to call straight from the handler thread.
        ToolResult Handle_LogTail(McpServer& /*server*/, const Json& arguments)
        {
            std::size_t count = 50;
            if (arguments.contains("count") && arguments["count"].is_number_integer())
            {
                const auto requested = arguments["count"].get<std::int64_t>();
                count = static_cast<std::size_t>(std::clamp<std::int64_t>(requested, 1, 200));
            }

            const std::vector<std::string> messages = Log::Get().GetRecentLogMessages(count);
            if (messages.empty())
                return ToolResult::Text("(no log messages buffered yet)");

            // The ring-buffer entries are already pattern-formatted (and usually end
            // with a newline). Normalise to exactly one '\n' between lines.
            std::string out;
            for (const auto& message : messages)
            {
                std::string_view line = message;
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                    line.remove_suffix(1);
                out.append(line);
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
                const int start = page * pageSize;
                Json entities = Json::array();
                for (int i = start; i < total && i < start + pageSize; ++i)
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
                    const std::size_t step = std::max<std::size_t>(1, n / static_cast<std::size_t>(points));
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

            const Json result = server.MarshalRead([topK]() -> Json
                                                   {
                RendererProfiler& profiler = RendererProfiler::GetInstance();
                profiler.CaptureFrame("MCP capture");
                const std::vector<RendererProfiler::CapturedFrame>& captures = profiler.GetCapturedFrames();
                if (captures.empty())
                    return Json{ { "__error", "No frame could be captured." } };

                const RendererProfiler::CapturedFrame& cap = captures.back();
                Json o;
                o["frameNumber"] = cap.m_FrameNumber;
                o["drawCalls"] = cap.m_FrameData.m_DrawCalls;
                o["triangles"] = cap.m_FrameData.m_TrianglesRendered;
                o["gpuMs"] = Round2(cap.m_FrameData.m_GPUTime);
                o["cpuMs"] = Round2(cap.m_FrameData.m_CPUTime);

                Json passes = Json::array();
                std::vector<const RendererProfiler::DrawCallInfo*> allDraws;
                for (const auto& pass : cap.m_RenderPasses)
                {
                    passes.push_back(Json{ { "name", pass.m_Name },
                                           { "durationMs", Round2(pass.m_Duration) },
                                           { "drawCalls", pass.m_DrawCallCount } });
                    for (const auto& draw : pass.m_DrawCalls)
                        allDraws.push_back(&draw);
                }
                std::sort(allDraws.begin(), allDraws.end(),
                          [](const RendererProfiler::DrawCallInfo* a, const RendererProfiler::DrawCallInfo* b)
                          { return a->m_GPUTime > b->m_GPUTime; });

                Json topDraws = Json::array();
                for (std::size_t i = 0; i < allDraws.size() && i < static_cast<std::size_t>(topK); ++i)
                {
                    const auto* d = allDraws[i];
                    topDraws.push_back(Json{ { "name", d->m_Name },
                                             { "shader", d->m_ShaderName },
                                             { "gpuMs", Round2(d->m_GPUTime) },
                                             { "triangles", d->m_IndexCount / 3 } });
                }
                o["passes"] = std::move(passes);
                o["topDrawCalls"] = std::move(topDraws);
                if (cap.m_RenderPasses.empty())
                    o["note"] = "Per-pass / per-draw-call detail is only recorded while the editor's frame-capture "
                                "instrumentation is active; the frame-level totals above are always valid.";
                return o; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
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
                const int start = page * pageSize;
                Json assets = Json::array();
                for (int i = start; i < total && i < start + pageSize; ++i)
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

        // ---- olo_screenshot (main-marshaled; GL readback) ----------------------
        // Captures the editor viewport framebuffer as a PNG and returns it as an MCP
        // image content block so the agent can SEE the rendered frame.
        ToolResult Handle_Screenshot(McpServer& server, const Json& args)
        {
            int maxWidth = 1024;
            if (args.contains("maxWidth") && args["maxWidth"].is_number_integer())
                maxWidth = static_cast<int>(std::clamp<long long>(args["maxWidth"].get<long long>(), 16, 4096));

            if (!server.Context().CaptureViewportPng)
                return ToolResult::Error("Screenshot capture is not available in this editor build.");

            const Json marshaled = server.MarshalRead([&server, maxWidth]() -> Json
                                                      {
                const std::vector<u8> png = server.Context().CaptureViewportPng(maxWidth);
                if (png.empty())
                    return Json{ { "__error", "Viewport capture failed (no framebuffer or empty viewport)." } };
                return Json{ { "b64", Base64Encode(png) }, { "bytes", static_cast<u64>(png.size()) } }; });

            if (marshaled.is_object() && marshaled.contains("__error"))
                return ToolResult::Error(marshaled["__error"].get<std::string>());

            ToolResult result;
            result.Content = Json::array({ Json{ { "type", "image" },
                                                 { "data", marshaled["b64"] },
                                                 { "mimeType", "image/png" } } });
            result.IsError = false;
            return result;
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
                      { { "type", "integer" },
                        { "minimum", 1 },
                        { "maximum", 200 },
                        { "description", "How many of the most recent log lines to return (default 50)." } } } } },
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
                "Returns an image content block (downscaled to maxWidth, default 1024).";
            tool.InputSchema = Json{
                { "type", "object" },
                { "properties",
                  { { "maxWidth", { { "type", "integer" }, { "minimum", 16 }, { "maximum", 4096 }, { "description", "Max output width in pixels (default 1024); aspect ratio preserved." } } } } },
                { "additionalProperties", false }
            };
            tool.MainMarshaled = true;
            tool.Handler = Handle_Screenshot;
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
