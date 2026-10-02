#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpShaderReload.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderRegistry.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Shader MCP tools: olo_shader_list / olo_shader_errors / olo_shader_get and the
// olo_shader_reload inner loop. Split out of the McpTools.cpp monolith (issue #357).

namespace OloEngine::MCP
{
    namespace
    {
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

        // ---- olo_shader_errors (main-marshaled; GetAllShaders is unguarded) ----
        ToolResult Handle_ShaderErrors(IAutomationHost& host, const Json& /*args*/)
        {
            Json j = host.MarshalRead([]() -> Json
                                      {
                const auto& debugger = ShaderDebugger::GetInstance();
                // "No errors" and "this build cannot tell you" are different
                // answers and must not both render as count: 0. The registration
                // macros compile out below OLO_DEBUG, so a Release editor would
                // otherwise report a clean bill of health it never checked.
                if (!debugger.IsTracking())
                {
                    return Json{ { "available", false },
                                 { "status", ShaderDebugger::IsTrackingCompiledIn() ? "notInitialized" : "unavailableInThisBuild" },
                                 { "count", nullptr },
                                 { "errors", Json::array() },
                                 { "detail", ShaderDebugger::IsTrackingCompiledIn()
                                                 ? "Shader tracking is compiled in but ShaderDebugger::Initialize() has "
                                                   "not run in this process, so no shader has registered. Nothing was "
                                                   "checked; a zero here would be a guess, not a result."
                                                 : "Shader tracking is compiled out of this build "
                                                   "(OLO_SHADER_REGISTER is Debug-only), so shader errors cannot be "
                                                   "reported. Use a Debug editor to check shader errors." } };
                }
                const auto& shaders = debugger.GetAllShaders();
                Json arr = Json::array();
                for (const auto& [id, info] : shaders)
                {
                    if (!info.m_HasErrors && info.m_LastCompilation.m_Success)
                        continue;
                    arr.push_back(Json{ { "name", info.m_Name.ToStdString() },
                                        { "errorMessage", info.m_LastCompilation.m_ErrorMessage.ToStdString() } });
                }
                return Json{ { "available", true },
                             { "status", "ready" },
                             { "count", static_cast<int>(arr.size()) },
                             { "errors", std::move(arr) } }; });
            return ToolResult::Structured(j);
        }

        // ---- olo_shader_get (main-marshaled) -----------------------------------
        ToolResult Handle_ShaderGet(IAutomationHost& host, const Json& args)
        {
            std::string name;
            if (args.contains("name") && args["name"].is_string())
                name = args["name"].get<std::string>();
            bool haveId = false;
            u32 id = 0;
            if (args.contains("id") && args["id"].is_number_integer())
            {
                // Shader ids are u32; validate before narrowing so a negative or
                // out-of-range value fails cleanly instead of wrapping into a
                // different (or matching-by-accident) id.
                const long long rawId = args["id"].get<long long>();
                if (rawId < 0 || rawId > static_cast<long long>(std::numeric_limits<u32>::max()))
                    return ToolResult::Error("Invalid 'id': expected a non-negative shader id within 32-bit range.");
                id = static_cast<u32>(rawId);
                haveId = true;
            }
            const bool includeGlsl = args.contains("includeGlsl") && args["includeGlsl"].is_boolean() && args["includeGlsl"].get<bool>();
            if (name.empty() && !haveId)
                return ToolResult::Error("Provide a shader 'name' or numeric 'id'.");

            const Json result = host.MarshalRead([&name, haveId, id, includeGlsl]() -> Json
                                                 {
                const auto& shaders = ShaderDebugger::GetInstance().GetAllShaders();
                const ShaderDebugger::ShaderInfo* found = nullptr;
                for (const auto& [sid, info] : shaders)
                {
                    if (haveId ? (sid == id) : (info.m_Name == name))
                    {
                        found = &info;
                        break;
                    }
                }
                if (found == nullptr)
                    return Json{ { "__error", "Shader not found." } };

                Json o;
                o["name"] = found->m_Name.ToStdString();
                o["filePath"] = found->m_FilePath.ToStdString();
                o["hasErrors"] = found->m_HasErrors;
                o["instructionCount"] = found->m_LastCompilation.m_InstructionCount;
                o["compileTimeMs"] = Round2(found->m_LastCompilation.m_CompileTimeMs);
                o["reloadCount"] = static_cast<int>(found->m_ReloadHistory.Num());

                Json ubos = Json::array();
                for (const auto& b : found->m_UniformBuffers)
                {
                    Json members = Json::array();
                    for (const auto& member : b.m_Members)
                        members.push_back(member.ToStdString());
                    ubos.push_back(Json{ { "name", b.m_Name.ToStdString() }, { "binding", b.m_Binding }, { "size", b.m_Size }, { "members", std::move(members) } });
                }
                o["uniformBuffers"] = std::move(ubos);

                Json samplers = Json::array();
                for (const auto& s : found->m_Samplers)
                    samplers.push_back(Json{ { "name", s.m_Name.ToStdString() }, { "binding", s.m_Binding }, { "type", s.m_Type.ToStdString() } });
                o["samplers"] = std::move(samplers);

                Json uniforms = Json::array();
                for (const auto& u : found->m_Uniforms)
                    uniforms.push_back(Json{ { "name", u.m_Name.ToStdString() }, { "location", u.m_Location }, { "size", u.m_Size } });
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
            return ToolResult::Structured(result);
        }

        // ---- olo_shader_list (main-marshaled; GetAllShaders is unguarded) ------
        // Inventory of every registered shader so the agent can discover names/ids
        // to feed olo_shader_get / olo_shader_reload. Each entry carries a
        // `reloadable` flag (issue #607) so the list and olo_shader_reload can no
        // longer disagree: a shader is reloadable iff it is backed by a file on
        // disk, which the engine's ShaderRegistry knows for library-owned and
        // pass-owned shaders alike.
        ToolResult Handle_ShaderList(IAutomationHost& host, const Json& /*args*/)
        {
            Json j = host.MarshalRead([]() -> Json
                                      {
                const auto& debugger = ShaderDebugger::GetInstance();
                // Same distinction as olo_shader_errors: an empty list here means
                // "this build does not track shaders", not "there are none", and a
                // caller that reads it as the latter concludes shader hot reload is
                // unavailable when it is simply unlistable.
                if (!debugger.IsTracking())
                {
                    return Json{ { "available", false },
                                 { "status", ShaderDebugger::IsTrackingCompiledIn() ? "notInitialized" : "unavailableInThisBuild" },
                                 { "count", nullptr },
                                 { "shaders", Json::array() },
                                 { "detail", ShaderDebugger::IsTrackingCompiledIn()
                                                 ? "Shader tracking is compiled in but ShaderDebugger::Initialize() has "
                                                   "not run in this process, so nothing has registered. The shaders "
                                                   "exist and render; they cannot be enumerated here."
                                                 : "Shader tracking is compiled out of this build "
                                                   "(OLO_SHADER_REGISTER is Debug-only). The shaders exist and render; "
                                                   "they cannot be enumerated here. Use a Debug editor to list or "
                                                   "hot-reload them by name." } };
                }
                const auto& shaders = debugger.GetAllShaders();
                const auto& registry = ShaderRegistry::Get();
                Json arr = Json::array();
                for (const auto& [id, info] : shaders)
                {
                    arr.push_back(Json{ { "id", id },
                                        { "name", info.m_Name.ToStdString() },
                                        { "hasErrors", info.m_HasErrors },
                                        { "reloadable", registry.Contains(info.m_Name.ToStdString()) },
                                        { "instructionCount", info.m_LastCompilation.m_InstructionCount } });
                }
                return Json{ { "available", true },
                             { "status", "ready" },
                             { "count", static_cast<int>(arr.size()) },
                             { "shaders", std::move(arr) } }; });
            return ToolResult::Structured(j);
        }

        // Best-effort compile/link log via the same read path as
        // olo_shader_errors (ShaderDebugger, populated in debug builds). Match by
        // name, preferring the entry for the current program id (the id changes
        // across a reload; a failed link resets it to 0).
        std::string ReadShaderLog(const std::string& name, u32 rendererId)
        {
            const auto& shaders = ShaderDebugger::GetInstance().GetAllShaders();
            const ShaderDebugger::ShaderInfo* best = nullptr;
            for (const auto& [id, info] : shaders)
            {
                if (info.m_Name != name)
                    continue;
                if (id == rendererId)
                {
                    best = &info;
                    break;
                }
                if (best == nullptr || info.m_HasErrors)
                    best = &info;
            }
            return best != nullptr ? best->m_LastCompilation.m_ErrorMessage.ToStdString() : std::string{};
        }

        // ---- olo_shader_reload (main-marshaled; recompiles a shader from disk) --
        // The shader inner loop: edit a .glsl -> reload -> read the compile/link
        // log -> screenshot, without restarting the editor.
        //
        // Name resolution is uniform across EVERY file-backed shader (issue #607):
        //   1. the Renderer3D / Renderer2D ShaderLibrary (mirrors the editor's own
        //      "Recompile" action in ShaderEditorPanel, which reloads the shader in
        //      both libraries);
        //   2. AND the engine's process-wide ShaderRegistry, which every
        //      file-backed shader (including pass-owned ones like
        //      VirtualMeshGBuffer and every .comp) registers itself in from its
        //      constructor, on both backends. Consulted even when a library held
        //      the name: a pass that creates its own copy of a library shader
        //      draws with that copy, and reloading only the library one changed
        //      nothing on screen. Copies are de-duplicated by address, so the
        //      library copy (registered too) is not reloaded twice.
        //
        // A copy reloaded only if Reload() RETURNED true; IsReady()/IsValid() is
        // an additional check on top, never the verdict on its own. A backend may
        // keep the previous program and its status after a failed compile (Vulkan
        // does), so the status alone reported a broken edit as `ready` (#607).
        // The return value is the contract (#1131).
        //
        // On a backend with a PSO cache (Vulkan) a successful module rebuild is
        // not yet a visible change: the reload invalidates every pipeline built
        // from the old modules and the next draw or dispatch rebuilds them. So
        // there the handler renders a few frames and reports `ready` only once a
        // pipeline was rebuilt (ShaderReload::ResolveStatus). GL work is
        // main-thread-only, so every touch of a shader runs inside MarshalRead.

        // Shared between the reload job and the post-settle job. The Refs must
        // be released on the main thread: a render-path switch in between could
        // make ours the last reference, and a shader destructor deletes GPU
        // objects. ReleaseOnMainThread is the only way they are dropped.
        struct ReloadState
        {
            ShaderReload::Result Result;
            ShaderCompilationStatus ModuleStatus = ShaderCompilationStatus::Ready;
            std::vector<Ref<Shader>> Copies;
            std::vector<Ref<ComputeShader>> ComputeCopies;
            u64 BaseFrame = 0;
        };

        // (main thread) Sum the pipeline state of every reloaded copy.
        template<typename TFn>
        void ForEachPipelineState(const ReloadState& state, TFn&& fn)
        {
            for (const Ref<Shader>& copy : state.Copies)
                fn(copy->GetPipelineState());
            for (const Ref<ComputeShader>& copy : state.ComputeCopies)
                fn(copy->GetPipelineState());
        }

        // Drop the Refs on the main thread. If the editor is so stalled that even
        // this cannot be marshaled, the Refs are deliberately leaked: a leaked
        // shader costs memory, a shader destroyed on this thread deletes GL/Vulkan
        // objects off the render thread.
        void ReleaseOnMainThread(IAutomationHost& host, const std::shared_ptr<ReloadState>& state)
        {
            try
            {
                (void)host.MarshalRead([state]() -> Json
                                       {
                    state->Copies.clear();
                    state->ComputeCopies.clear();
                    return Json::object(); });
            }
            catch (...)
            {
                auto* leaked = new ReloadState();
                leaked->Copies = std::move(state->Copies);
                leaked->ComputeCopies = std::move(state->ComputeCopies);
                (void)leaked;
            }
        }

        ToolResult Handle_ShaderReload(IAutomationHost& host, const Json& args)
        {
            std::string name;
            if (args.contains("name") && args["name"].is_string())
                name = args["name"].get<std::string>();
            if (name.empty())
                return ToolResult::Error("Provide a shader 'name' to reload (see olo_shader_list).");

            // Frames a PSO backend gets to rebuild the invalidated pipelines.
            constexpr int kPipelineSettleFrames = 3;

            auto state = std::make_shared<ReloadState>();
            const auto reloadJob = [name, state, &host]() -> Json
            {
                ShaderReload::Result& r = state->Result;
                r.Name = name;

                // The reported status aggregates ALL reloaded copies: r.Ok is true
                // only if every copy reloaded, and the representative used for the
                // status / program-id / log is the first copy that FAILED (so a
                // failure isn't masked by a sibling that linked) — otherwise the
                // first copy.
                Ref<Shader> representative;
                bool representativeReloaded = true;
                bool allReady = true;
                const auto reload = [&r, &representative, &representativeReloaded, &allReady, &state](Ref<Shader> shader, std::string_view label)
                {
                    if (std::ranges::any_of(state->Copies, [&shader](const Ref<Shader>& done)
                                            { return done.Raw() == shader.Raw(); }))
                        return;
                    const bool reloaded = shader->Reload() && shader->IsReady();
                    r.Found = true;
                    r.Libraries.emplace_back(label);
                    state->Copies.push_back(shader);
                    allReady = allReady && reloaded;
                    if (!representative || (!reloaded && representativeReloaded))
                    {
                        representative = shader;
                        representativeReloaded = reloaded;
                    }
                };
                const auto reloadIn = [&name, &reload](ShaderLibrary& lib, std::string_view label)
                {
                    if (!lib.Exists(name))
                        return;
                    if (Ref<Shader> shader = lib.Get(name))
                        reload(shader, label);
                };
                reloadIn(Renderer3D::GetShaderLibrary(), "Renderer3D");
                reloadIn(Renderer2D::GetShaderLibrary(), "Renderer2D");
                for (const Ref<Shader>& shader : ShaderRegistry::Get().FindShaders(name))
                    reload(shader, ShaderReload::kPassOwnedLabel);

                constexpr const char* kKeptPreviousNote =
                    "The reload failed and the previous program is still live; the compiler error is in the editor log.";

                // Pass-owned COMPUTE shaders (GTAO/SSAO/SSR/VirtualCluster*/...).
                // ComputeShader has no ShaderCompilationStatus, so Reload()'s result
                // (and IsValid()) map onto Ready/Failed, and the kind is flagged.
                if (!r.Found)
                {
                    Ref<ComputeShader> computeRep;
                    bool computeRepReloaded = true;
                    for (Ref<ComputeShader>& shader : ShaderRegistry::Get().FindComputeShaders(name))
                    {
                        const bool reloaded = shader->Reload() && shader->IsValid();
                        r.Found = true;
                        r.Kind = ShaderReload::ShaderKind::Compute;
                        r.Libraries.emplace_back(ShaderReload::kPassOwnedLabel);
                        state->ComputeCopies.push_back(shader);
                        allReady = allReady && reloaded;
                        if (!computeRep || (!reloaded && computeRepReloaded))
                        {
                            computeRep = shader;
                            computeRepReloaded = reloaded;
                        }
                    }
                    if (computeRep)
                    {
                        state->ModuleStatus = computeRepReloaded ? ShaderCompilationStatus::Ready : ShaderCompilationStatus::Failed;
                        if (!computeRepReloaded && computeRep->IsValid())
                            r.Note = kKeptPreviousNote;
                        r.RendererId = computeRep->GetRendererID();
                    }
                }

                if (!r.Found)
                {
                    // Every file-backed shader registers itself, so this list is
                    // the complete set of reloadable names — a shader that appears
                    // in olo_shader_list but not here is a source-string shader
                    // (boot / fallback / shader-graph), which has no file on disk
                    // to reload from.
                    const std::vector<std::string> reloadable = ShaderRegistry::Get().GetAllNames();
                    std::string list;
                    for (const auto& reloadableName : reloadable)
                    {
                        if (!list.empty())
                            list += ", ";
                        list += reloadableName;
                    }
                    return Json{ { "__error",
                                   "Shader '" + name + "' is not reloadable: no shader by that name is backed by a "
                                   "file on disk (source-string shaders such as the boot / fallback / shader-graph "
                                   "programs cannot be reloaded). Reloadable shaders: " +
                                       list } };
                }

                if (representative)
                {
                    // Authoritative, build-independent status (does not rely on the
                    // debug-only ShaderDebugger).
                    state->ModuleStatus = representativeReloaded ? representative->GetCompilationStatus()
                                                                 : ShaderCompilationStatus::Failed;
                    if (!representativeReloaded && representative->IsReady())
                        r.Note = kKeptPreviousNote;
                    r.RendererId = representative->GetRendererID();
                }
                r.Status = state->ModuleStatus;
                r.Ok = allReady;
                r.Log = ReadShaderLog(name, r.RendererId);

                // Only after a successful reload: a failed one invalidated nothing,
                // and the counts left from the previous reload would mislead.
                if (allReady)
                {
                    ForEachPipelineState(*state, [&r](const ShaderPipelineState& pipelines)
                                         {
                        r.PipelineState.Tracked = r.PipelineState.Tracked || pipelines.Tracked;
                        r.PipelineState.Invalidated += pipelines.InvalidatedByLastReload; });
                }
                const bool settle = allReady && r.PipelineState.Tracked && r.PipelineState.Invalidated > 0;
                if (!settle)
                {
                    state->Copies.clear(); // released here, on the main thread
                    state->ComputeCopies.clear();
                }
                else if (host.Context().GetFrameIndex)
                {
                    state->BaseFrame = host.Context().GetFrameIndex();
                }
                return Json{ { "settle", settle } }; };

            // The job stores Refs in `state` before it can fail. If the marshal
            // throws (the job threw, or the editor never picked it up), `state`
            // would otherwise unwind here with ours possibly the last reference.
            Json first;
            try
            {
                first = host.MarshalRead(reloadJob);
            }
            catch (...)
            {
                ReleaseOnMainThread(host, state);
                throw;
            }

            if (first.is_object() && first.contains("__error"))
                return ToolResult::Error(first["__error"].get<std::string>());

            if (first.value("settle", false))
            {
                ShaderReload::Pipelines& pipelines = state->Result.PipelineState;
                bool frameRendered = false;
                try
                {
                    // A host that cannot report frames cannot be waited on: say no
                    // frame was waited rather than claim three were.
                    if (host.Context().GetFrameIndex && host.Context().IsCaptureUnready)
                    {
                        frameRendered = AwaitRenderedFrames(host, state->BaseFrame, kPipelineSettleFrames);
                        pipelines.SettleFrames = kPipelineSettleFrames;
                    }
                    if (host.IsCurrentCallCancelled())
                    {
                        ReleaseOnMainThread(host, state);
                        return ToolResult::Error("Cancelled while waiting for the reloaded pipelines to rebuild; the "
                                                 "modules did reload.");
                    }
                    pipelines.FrameRendered = frameRendered;
                    (void)host.MarshalRead([state]() -> Json
                                           {
                        ShaderReload::Pipelines& p = state->Result.PipelineState;
                        ForEachPipelineState(*state, [&p](const ShaderPipelineState& after)
                                             {
                            p.Live += after.Live;
                            if (after.CreationFailed && !p.CreationFailed)
                            {
                                p.CreationFailed = true;
                                p.CreationFailure = after.CreationFailure.ToStdString();
                            } });
                        state->Copies.clear();
                        state->ComputeCopies.clear();
                        return Json::object(); });
                }
                catch (...)
                {
                    ReleaseOnMainThread(host, state);
                    throw;
                }
            }

            ShaderReload::Result& r = state->Result;
            const ShaderReload::Resolution resolution = ShaderReload::ResolveStatus(state->ModuleStatus, r.PipelineState);
            r.Status = resolution.Status;
            r.Ok = r.Ok && resolution.Status == ShaderCompilationStatus::Ready;
            if (!resolution.Note.empty())
                r.Note = resolution.Note;
            return ToolResult::Structured(ShaderReload::ToJson(r));
        }

    } // namespace

    // Composed by olo_project_validate (#1130): the same ShaderDebugger sweep the
    // Shader editor panel renders from, through the standalone command's handler.
    ToolResult CollectShaderProblems(IAutomationHost& host)
    {
        return Handle_ShaderErrors(host, Json::object());
    }

    void RegisterShaderTools(AutomationRegistry& registry)
    {
        {
            ToolDef tool;
            tool.Name = "olo_shader_errors";
            tool.Toolset = "shader";
            tool.Title = "Shader compile errors";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Shaders that currently have compile/link errors, with the error message. CHECK `available` "
                "FIRST: shader tracking is compiled out below OLO_DEBUG, so a Release or Dist editor cannot "
                "answer this at all and returns available:false with a null count. An empty errors list is only "
                "evidence of a clean build when available is true — otherwise it means nobody looked.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("available", Schema::Bool().Desc("False when this build does not track shaders; count is then null."))
                                    .Prop("status", Schema::String().Enum({ "ready", "notInitialized", "unavailableInThisBuild" }))
                                    .Prop("count", Schema::Raw(Json{ { "type", Json::array({ "integer", "null" }) }, { "minimum", 0 } }))
                                    .Prop("errors", Schema::Array(Schema::Object()
                                                                      .Prop("name", Schema::String())
                                                                      .Prop("errorMessage", Schema::String())))
                                    .Prop("detail", Schema::String())
                                    .Required({ "available", "status", "count", "errors" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderErrors;
            registry.Register(std::move(tool));
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
            tool.OutputSchema = Schema::Object()
                                    .Prop("name", Schema::String())
                                    .Prop("filePath", Schema::String())
                                    .Prop("hasErrors", Schema::Bool())
                                    .Prop("instructionCount", Schema::Int().Min(0).Desc("Estimated from the SPIR-V binary."))
                                    .Prop("compileTimeMs", Schema::Number().Desc("Rounded to 2 decimals."))
                                    .Prop("reloadCount", Schema::Int().Min(0))
                                    .Prop("uniformBuffers", Schema::Array(Schema::Object()
                                                                              .Prop("name", Schema::String())
                                                                              .Prop("binding", Schema::Int())
                                                                              .Prop("size", Schema::Int())
                                                                              .Prop("members", Schema::Array(Schema::String()))))
                                    .Prop("samplers", Schema::Array(Schema::Object()
                                                                        .Prop("name", Schema::String())
                                                                        .Prop("binding", Schema::Int())
                                                                        .Prop("type", Schema::String())))
                                    .Prop("uniforms", Schema::Array(Schema::Object()
                                                                        .Prop("name", Schema::String())
                                                                        .Prop("location", Schema::Int())
                                                                        .Prop("size", Schema::Int())))
                                    .Prop("generatedGlsl", Schema::Object().Desc("Stage token (vertex/fragment/geometry/compute) -> cross-compiled GLSL source string. Only present when includeGlsl=true."))
                                    .Required({ "name", "filePath", "hasErrors", "instructionCount", "compileTimeMs", "reloadCount", "uniformBuffers", "samplers", "uniforms" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderGet;
            registry.Register(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_shader_list";
            tool.Toolset = "shader";
            tool.Title = "List shaders";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Inventory of all registered shaders (id, name, hasErrors, reloadable, instruction count). Use "
                "it to discover a shader name/id to pass to olo_shader_get / olo_shader_reload. 'reloadable' is "
                "true when the shader is backed by a file on disk (library- AND pass-owned shaders, including "
                "compute); it is false only for source-string shaders (boot / fallback / shader-graph). CHECK "
                "`available` FIRST: the tracking this reads is compiled out below OLO_DEBUG, so a Release or Dist "
                "editor returns available:false and an empty list even though the shaders exist and render. An "
                "empty list there means unlistable, not absent. olo_shader_reload still works there: it resolves a "
                "name through ShaderLibrary and the pass-owned shaders, not through this tracking, so a name you "
                "already know can be reloaded — you just cannot discover names here.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("available", Schema::Bool().Desc("False when this build does not track shaders; count is then null."))
                                    .Prop("status", Schema::String().Enum({ "ready", "notInitialized", "unavailableInThisBuild" }))
                                    .Prop("detail", Schema::String())
                                    .Prop("count", Schema::Raw(Json{ { "type", Json::array({ "integer", "null" }) }, { "minimum", 0 } }))
                                    .Prop("shaders", Schema::Array(Schema::Object()
                                                                       .Prop("id", Schema::Int().Min(0).Desc("GL program id."))
                                                                       .Prop("name", Schema::String())
                                                                       .Prop("hasErrors", Schema::Bool())
                                                                       .Prop("reloadable", Schema::Bool().Desc("Backed by a file on disk; feed to olo_shader_reload."))
                                                                       .Prop("instructionCount", Schema::Int().Min(0))))
                                    .Required({ "available", "status", "count", "shaders" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderList;
            registry.Register(std::move(tool));
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
                "Reload and recompile one shader from disk by name — the shader inner loop: edit a .glsl or "
                ".comp, reload, read the compile/link log, screenshot, all without restarting the editor. "
                "Re-reads the file and recompiles+links synchronously. EVERY file-backed shader is reloadable: "
                "the Renderer3D / Renderer2D library shaders (reloaded in both libraries when both hold the "
                "name, matching the editor's Recompile button) AND pass-owned shaders such as "
                "VirtualMeshGBuffer / VirtualVisibilityResolve and the compute shaders (GTAO, SSAO, SSR, "
                "VirtualCluster*, FluidSmooth, ...), which resolve through the engine's ShaderRegistry. Use the "
                "'reloadable' flag from olo_shader_list; only source-string shaders (boot / fallback / "
                "shader-graph) have no file to reload from, and asking for one returns an error listing the "
                "reloadable names. Returns the post-reload status (ready/failed/compiling/pending), whether it "
                "was a graphics or compute program ('kind'), the GL program id, who owned it ('libraries': "
                "Renderer3D / Renderer2D / PassOwned), and the compile/link error log (empty on a clean reload; "
                "populated from the shader debugger in debug builds). A GLSL compile or link error returns "
                "status 'failed' with the log and leaves the editor running (#568); only a malformed #type "
                "directive still trips a debug assert. On Vulkan a module rebuild is not yet visible: every "
                "pipeline built from the old modules is invalidated and the next draw rebuilds it, so the tool "
                "renders a few frames and answers 'ready' only once a pipeline was rebuilt ('pipelines' says how "
                "many), 'failed' when pipeline creation failed, and 'pending' with a 'note' when no draw used the "
                "shader (it may belong to the other render path). Every copy is reloaded, including a pass's own "
                "copy of a library shader. To inspect a shader's existing errors without recompiling, use "
                "olo_shader_errors / olo_shader_get instead.";
            tool.InputSchema = Schema::Object()
                                   .Prop("name", Schema::String().Desc("Shader name to reload (as shown by olo_shader_list)."))
                                   .Required({ "name" })
                                   .NoAdditional();
            // Contract shaped by ShaderReload::ToJson (McpShaderReload.h), pinned by McpShaderReloadTest.
            tool.OutputSchema = Schema::Object()
                                    .Prop("name", Schema::String().Desc("The requested shader name (echoed)."))
                                    .Prop("found", Schema::Bool().Desc("Always true on a success response (a non-reloadable name is returned as isError instead)."))
                                    .Prop("libraries", Schema::Array(Schema::String()).Desc("Owners that held it: Renderer3D / Renderer2D / PassOwned."))
                                    .Prop("kind", Schema::String().Enum({ "graphics", "compute" }))
                                    .Prop("status", Schema::String().Enum({ "pending", "compiling", "ready", "failed", "unknown" }).Desc("Post-reload status of the primary copy."))
                                    .Prop("ok", Schema::Bool().Desc("True only when every reloaded copy is ready/valid."))
                                    .Prop("rendererId", Schema::Int().Min(0).Desc("Current GL program id of the primary copy; 0 when a link failed."))
                                    .Prop("log", Schema::String().Desc("Compile/link error log; empty on a clean reload (best-effort, debug builds)."))
                                    .Prop("pipelines", Schema::Object()
                                                           .Prop("invalidated", Schema::Int().Min(0).Desc("Pipelines the reload invalidated."))
                                                           .Prop("rebuilt", Schema::Int().Min(0).Desc("Pipelines built from the new modules since."))
                                                           .Prop("creationFailed", Schema::Bool())
                                                           .Prop("settleFrames", Schema::Int().Min(0).Desc("Frames waited for a draw to rebuild them."))
                                                           .Prop("frameRendered", Schema::Bool().Desc("False when no frame rendered during that wait."))
                                                           .Desc("Vulkan only (a PSO-cache backend); absent on OpenGL, which relinks the program in place."))
                                    .Prop("note", Schema::String().Desc("Why the status is what it is, when the pipelines decided it."))
                                    .Required({ "name", "found", "libraries", "kind", "status", "ok", "rendererId", "log" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderReload;
            registry.Register(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
