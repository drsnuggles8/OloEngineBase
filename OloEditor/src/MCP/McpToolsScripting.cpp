#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpReloadScript.h"
#include "MCP/McpScriptApi.h"
#include "OloEngine/Scripting/ScriptError.h"

#include <string>
#include <vector>

// Scripting MCP tools: olo_script_get_api, olo_script_get_last_errors, and the
// consented olo_reload_script assembly reload. Split out of the McpTools.cpp
// monolith (issue #357).

namespace OloEngine::MCP
{
    namespace
    {
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
            return ToolResult::Structured(digest);
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
            return ToolResult::Structured(out);
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
            return ToolResult::Structured(result);
        }

    } // namespace

    void RegisterScriptingTools(McpServer& server)
    {
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
            tool.OutputSchema = Schema::Object()
                                    .Prop("language", Schema::String().Enum({ "csharp", "lua" }))
                                    .Prop("engineVersion", Schema::String())
                                    .Prop("typeCount", Schema::Int().Min(0).Desc("Total types discovered, before typeFilter matching."))
                                    .Prop("types", Schema::Array(Schema::Object()
                                                                     .Prop("name", Schema::String())
                                                                     .Prop("kind", Schema::String().Desc("csharp only: class/struct/enum/interface."))
                                                                     .Prop("file", Schema::String().Desc("csharp only: declaring .cs file name."))
                                                                     .Prop("members", Schema::Array(Schema::String()).Desc("csharp filter mode only: public member declaration lines."))
                                                                     .Prop("registration", Schema::String().Desc("lua filter mode only: raw Sol2 registration block (capped)."))
                                                                     .Prop("truncated", Schema::Bool().Desc("lua filter mode only: true when the registration block hit the cap."))
                                                                     .Required({ "name" }))
                                                       .Desc("Element shape varies by language and typeFilter mode; only 'name' is always present."))
                                    .Prop("note", Schema::String().Desc("Index mode only (no typeFilter): hint to pass typeFilter."))
                                    .Prop("matched", Schema::Int().Min(0).Desc("Filter mode only: number of matching types."))
                                    .Required({ "language", "engineVersion", "typeCount", "types" });
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
            tool.OutputSchema = Schema::Object()
                                    .Prop("count", Schema::Int().Min(0).Desc("Number of entries in 'errors'."))
                                    .Prop("errors", Schema::Array(Schema::Object()
                                                                      .Prop("language", Schema::String().Enum({ "csharp", "lua" }))
                                                                      .Prop("scriptName", Schema::String())
                                                                      .Prop("entityId", Schema::String().Desc("Entity UUID (decimal); omitted when unknown."))
                                                                      .Prop("message", Schema::String())
                                                                      .Prop("stackTrace", Schema::String().Desc("Omitted when empty or folded into message."))
                                                                      .Prop("timestamp", Schema::Number().Desc("Unix epoch seconds.")))
                                                        .Desc("Oldest-first."))
                                    .Required({ "count", "errors" });
            tool.MainMarshaled = false;
            tool.Handler = Handle_ScriptGetLastErrors;
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
            tool.OutputSchema = Schema::Object()
                                    .Prop("language", Schema::String().Desc("Always \"csharp\" today."))
                                    .Prop("available", Schema::Bool().Desc("C# scripting initialized in this build; false is a clean result, not an error."))
                                    .Prop("ok", Schema::Bool().Desc("Whether the assembly reload succeeded."))
                                    .Prop("scriptClassCount", Schema::Int().Min(0).Desc("Entity-script classes registered after the reload; non-zero signals the app assembly loaded."))
                                    .Prop("message", Schema::String())
                                    .Required({ "language", "available", "ok", "scriptClassCount", "message" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_ReloadScript;
            server.RegisterTool(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
