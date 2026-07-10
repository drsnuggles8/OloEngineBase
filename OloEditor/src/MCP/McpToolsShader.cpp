#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpShaderReload.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shader.h"

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
            return ToolResult::Text(j.dump(2));
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
                const auto reloadIn = [&name, &r, &allReady, &representative](ShaderLibrary& lib, std::string_view label)
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
    }
} // namespace OloEngine::MCP
