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
            return ToolResult::Structured(j);
        }

        // ---- olo_shader_get (main-marshaled) -----------------------------------
        ToolResult Handle_ShaderGet(McpServer& server, const Json& args)
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

            const Json result = server.MarshalRead([&name, haveId, id, includeGlsl]() -> Json
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
            return ToolResult::Structured(result);
        }

        // ---- olo_shader_list (main-marshaled; GetAllShaders is unguarded) ------
        // Inventory of every registered shader so the agent can discover names/ids
        // to feed olo_shader_get / olo_shader_reload. Each entry carries a
        // `reloadable` flag (issue #607) so the list and olo_shader_reload can no
        // longer disagree: a shader is reloadable iff it is backed by a file on
        // disk, which the engine's ShaderRegistry knows for library-owned and
        // pass-owned shaders alike.
        ToolResult Handle_ShaderList(McpServer& server, const Json& /*args*/)
        {
            Json j = server.MarshalRead([]() -> Json
                                        {
                const auto& shaders = ShaderDebugger::GetInstance().GetAllShaders();
                const auto& registry = ShaderRegistry::Get();
                Json arr = Json::array();
                for (const auto& [id, info] : shaders)
                {
                    arr.push_back(Json{ { "id", id },
                                        { "name", info.m_Name },
                                        { "hasErrors", info.m_HasErrors },
                                        { "reloadable", registry.Contains(info.m_Name) },
                                        { "instructionCount", info.m_LastCompilation.m_InstructionCount } });
                }
                return Json{ { "count", static_cast<int>(arr.size()) }, { "shaders", std::move(arr) } }; });
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
            return best != nullptr ? best->m_LastCompilation.m_ErrorMessage : std::string{};
        }

        // ---- olo_shader_reload (main-marshaled; recompiles a shader from disk) --
        // The shader inner loop: edit a .glsl -> reload -> read the compile/link
        // log -> screenshot, without restarting the editor.
        //
        // Name resolution is uniform across EVERY file-backed shader (issue #607):
        //   1. the Renderer3D / Renderer2D ShaderLibrary (mirrors the editor's own
        //      "Recompile" action in ShaderEditorPanel, which reloads the shader in
        //      both libraries);
        //   2. otherwise the engine's process-wide ShaderRegistry, which every
        //      file-backed shader — including pass-owned ones like
        //      VirtualMeshGBuffer and every .comp — registers itself in from its
        //      constructor. Those used to be un-reloadable even though
        //      olo_shader_list reported them, which forced an editor restart per
        //      shader edit.
        //
        // Shader::Reload() re-reads the file and recompiles+links synchronously
        // (force-finishing any async link), so the post-reload status is
        // authoritative. GL work is main-thread-only, so it runs inside MarshalRead.
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

                // The reported status aggregates ALL reloaded copies: r.Ok is true
                // only if every copy is Ready, and the representative used for the
                // status / program-id / log is the first copy that FAILED (so a
                // failure isn't masked by a sibling that linked) — otherwise the
                // first copy.
                Ref<Shader> representative;
                bool allReady = true;
                const auto adopt = [&allReady, &representative](const Ref<Shader>& shader, bool ready)
                {
                    allReady = allReady && ready;
                    if (!representative || (!ready && representative->IsReady()))
                        representative = shader;
                };

                const auto reloadIn = [&name, &r, &adopt](ShaderLibrary& lib, std::string_view label)
                {
                    if (!lib.Exists(name))
                        return;
                    Ref<Shader> shader = lib.Get(name);
                    if (!shader)
                        return;
                    shader->Reload();
                    r.Found = true;
                    r.Libraries.emplace_back(label);
                    adopt(shader, shader->IsReady());
                };
                reloadIn(Renderer3D::GetShaderLibrary(), "Renderer3D");
                reloadIn(Renderer2D::GetShaderLibrary(), "Renderer2D");

                // Pass-owned graphics shaders (VirtualMeshGBuffer, the fluid splat
                // shaders, ...). Only consulted when no library held the name — a
                // library shader is registered here too, and reloading it twice
                // would needlessly churn its GL program.
                if (!r.Found)
                {
                    auto passOwned = ShaderRegistry::Get().FindShaders(name);
                    for (Ref<Shader>& shader : passOwned)
                    {
                        shader->Reload();
                        r.Found = true;
                        r.Libraries.emplace_back(ShaderReload::kPassOwnedLabel);
                        adopt(shader, shader->IsReady());
                    }
                }

                // Pass-owned COMPUTE shaders (GTAO/SSAO/SSR/VirtualCluster*/...).
                // ComputeShader has no ShaderCompilationStatus — IsValid() is the
                // whole contract — so map it onto Ready/Failed and flag the kind.
                if (!r.Found)
                {
                    auto computeShaders = ShaderRegistry::Get().FindComputeShaders(name);
                    if (!computeShaders.empty())
                    {
                        r.Kind = ShaderReload::ShaderKind::Compute;
                        Ref<ComputeShader> computeRep;
                        for (Ref<ComputeShader>& shader : computeShaders)
                        {
                            shader->Reload();
                            r.Found = true;
                            r.Libraries.emplace_back(ShaderReload::kPassOwnedLabel);
                            const bool valid = shader->IsValid();
                            allReady = allReady && valid;
                            if (!computeRep || (!valid && computeRep->IsValid()))
                                computeRep = shader;
                        }
                        r.Status = computeRep->IsValid() ? ShaderCompilationStatus::Ready
                                                         : ShaderCompilationStatus::Failed;
                        r.Ok = allReady;
                        r.RendererId = computeRep->GetRendererID();
                        r.Log = ReadShaderLog(name, r.RendererId);
                        return ShaderReload::ToJson(r);
                    }
                }

                if (!r.Found || !representative)
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

                // Authoritative, build-independent status (does not rely on the
                // debug-only ShaderDebugger).
                r.Status = representative->GetCompilationStatus();
                r.Ok = allReady;
                r.RendererId = representative->GetRendererID();
                r.Log = ReadShaderLog(name, r.RendererId);

                return ShaderReload::ToJson(r); });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

    } // namespace

    void RegisterShaderTools(McpServer& server)
    {
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
            tool.OutputSchema = Schema::Object()
                                    .Prop("count", Schema::Int().Min(0).Desc("Number of shaders currently in error (size of errors)."))
                                    .Prop("errors", Schema::Array(Schema::Object()
                                                                      .Prop("name", Schema::String())
                                                                      .Prop("errorMessage", Schema::String())))
                                    .Required({ "count", "errors" });
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
            server.RegisterTool(std::move(tool));
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
                "compute); it is false only for source-string shaders (boot / fallback / shader-graph).";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("count", Schema::Int().Min(0).Desc("Number of registered shaders (size of shaders)."))
                                    .Prop("shaders", Schema::Array(Schema::Object()
                                                                       .Prop("id", Schema::Int().Min(0).Desc("GL program id."))
                                                                       .Prop("name", Schema::String())
                                                                       .Prop("hasErrors", Schema::Bool())
                                                                       .Prop("reloadable", Schema::Bool().Desc("Backed by a file on disk; feed to olo_shader_reload."))
                                                                       .Prop("instructionCount", Schema::Int().Min(0))))
                                    .Required({ "count", "shaders" });
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
                "populated from the shader debugger in debug builds). Note: in a Debug build, recompiling a "
                "shader that contains a GLSL syntax error trips an engine debug assert on the main thread (same "
                "as the editor's own Recompile button) — the call then times out and can crash the editor, so "
                "reserve this for edits you expect to compile; to inspect a shader's existing errors without "
                "recompiling, use olo_shader_errors / olo_shader_get instead.";
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
                                    .Required({ "name", "found", "libraries", "kind", "status", "ok", "rendererId", "log" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderReload;
            server.RegisterTool(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
