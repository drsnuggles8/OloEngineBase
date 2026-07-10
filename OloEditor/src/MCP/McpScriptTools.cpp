#include "OloEnginePCH.h"
#include "MCP/McpScriptTools.h"
#include "MCP/McpServer.h"
#include "MCP/McpToolsCommon.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Project/Project.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <lua.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Implementation of script-defined MCP tools (issue #357, Lua v1). The design
// and threat model live in docs/adr/0005-mcp-script-tools-lua-sandbox.md; the
// short version:
//   * scripts are USER-AUTHORED PROJECT FILES (the network can only call the
//     tools they register, never define new ones),
//   * they run in a dedicated sol::state with no io/os/debug/package and the
//     file/code loaders stripped from base,
//   * their only engine window is the read-only olo.call_tool bridge, so every
//     script tool is read-only by construction, and
//   * a debug-hook watchdog bounds each call's wall-clock (and honours MCP
//     cancellation), so a runaway script cannot pin a dispatch worker.

namespace OloEngine::MCP
{
    namespace
    {
        // Everything the registered handlers share: the sandboxed state, the
        // mutex serializing entry into it (handlers run on concurrent httplib
        // worker threads; sol::state is not thread-safe), and the per-call
        // watchdog deadline the debug hook checks. Handlers keep it alive via
        // shared_ptr, so its lifetime tracks the tools that captured it (the
        // server's tool vector), not the LoadScriptTools call.
        struct ScriptToolsRuntime
        {
            std::recursive_mutex Mutex; // recursive: olo.call_tool may re-enter Lua via conversions
            sol::state Lua;
            std::chrono::milliseconds Budget{ 10000 };
            std::chrono::steady_clock::time_point CallDeadline{};
            // The server owning the current call, for the olo.call_tool bridge
            // and the cancellation poll. Set for the duration of each handler
            // call, under Mutex. Never dangles: handlers (and thus this
            // runtime) are owned by the server's tool vector.
            McpServer* CurrentServer = nullptr;
            // The load-time registration window. RegisterMcpTool routes through
            // these (owned by this runtime, nulled the instant loading ends)
            // instead of capturing the LoadScriptTools stack locals by reference:
            // a script that stashes a `local reg = RegisterMcpTool` alias and calls
            // it from a later handler would otherwise write through a dangling
            // pointer to the destroyed report. Non-null only while the load loop runs.
            McpServer* LoadServer = nullptr;
            McpScriptToolsReport* LoadReport = nullptr;
        };

        // ---- Lua <-> JSON conversion -------------------------------------------

        constexpr int kMaxConversionDepth = 16;

        // True if `table` reads as a JSON array: every key is an integer and the
        // keys are exactly 1..n. An empty table converts as an OBJECT (the
        // common case for "no arguments" / "empty result" shapes).
        bool IsSequence(const sol::table& table)
        {
            std::size_t count = 0;
            for (const auto& [key, value] : table)
            {
                (void)value;
                if (key.get_type() != sol::type::number || !key.is<std::int64_t>())
                    return false;
                ++count;
            }
            if (count == 0)
                return false;
            for (std::size_t i = 1; i <= count; ++i)
            {
                if (!table[i].valid())
                    return false;
            }
            return true;
        }

        // Convert a Lua value to JSON. Throws std::runtime_error (caught by the
        // handler wrapper) on unconvertible values — functions, userdata,
        // non-string/integer keys, cycles deeper than the cap — instead of
        // silently mangling the result.
        Json LuaToJson(const sol::object& value, int depth)
        {
            if (depth > kMaxConversionDepth)
                throw std::runtime_error("Lua value nests deeper than 16 levels (a cycle?)");

            switch (value.get_type())
            {
                case sol::type::lua_nil:
                    return Json(nullptr);
                case sol::type::boolean:
                    return Json(value.as<bool>());
                case sol::type::number:
                {
                    // Preserve integers (Lua 5.4 has a real integer subtype) so
                    // ids/counts don't grow ".0" suffixes in JSON.
                    const sol::object& obj = value;
                    lua_State* state = obj.lua_state();
                    obj.push(state);
                    const bool isInteger = lua_isinteger(state, -1) != 0;
                    lua_pop(state, 1);
                    if (isInteger)
                        return Json(value.as<std::int64_t>());
                    return Json(value.as<double>());
                }
                case sol::type::string:
                    return Json(value.as<std::string>());
                case sol::type::table:
                {
                    const sol::table table = value.as<sol::table>();
                    if (IsSequence(table))
                    {
                        Json array = Json::array();
                        const std::size_t n = table.size();
                        for (std::size_t i = 1; i <= n; ++i)
                            array.push_back(LuaToJson(table[i], depth + 1));
                        return array;
                    }
                    Json object = Json::object();
                    for (const auto& [key, entry] : table)
                    {
                        if (key.get_type() != sol::type::string)
                            throw std::runtime_error("table keys must be strings (or 1..n integers for an array)");
                        object[key.as<std::string>()] = LuaToJson(entry, depth + 1);
                    }
                    return object;
                }
                default:
                    throw std::runtime_error("value of type '" + std::string(sol::type_name(value.lua_state(), value.get_type())) +
                                             "' cannot be converted to JSON");
            }
        }

        sol::object JsonToLua(sol::state_view lua, const Json& value)
        {
            switch (value.type())
            {
                case Json::value_t::null:
                    return sol::make_object(lua, sol::lua_nil);
                case Json::value_t::boolean:
                    return sol::make_object(lua, value.get<bool>());
                case Json::value_t::number_integer:
                    return sol::make_object(lua, value.get<std::int64_t>());
                case Json::value_t::number_unsigned:
                {
                    // Lua 5.4 integers are signed 64-bit: a value above INT64_MAX
                    // would wrap to a negative integer (e.g. a large UUID/count read
                    // back as a bogus negative). Keep it exact as a Lua integer when
                    // it fits, and fall back to a double above that (JSON's own
                    // number model) so the magnitude is preserved rather than corrupted.
                    const std::uint64_t u = value.get<std::uint64_t>();
                    if (u <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                        return sol::make_object(lua, static_cast<std::int64_t>(u));
                    return sol::make_object(lua, static_cast<double>(u));
                }
                case Json::value_t::number_float:
                    return sol::make_object(lua, value.get<double>());
                case Json::value_t::string:
                    return sol::make_object(lua, value.get<std::string>());
                case Json::value_t::array:
                {
                    sol::table table = lua.create_table(static_cast<int>(value.size()), 0);
                    int index = 1;
                    for (const auto& entry : value)
                        table[index++] = JsonToLua(lua, entry);
                    return table;
                }
                case Json::value_t::object:
                {
                    sol::table table = lua.create_table(0, static_cast<int>(value.size()));
                    for (const auto& [key, entry] : value.items())
                        table[key] = JsonToLua(lua, entry);
                    return table;
                }
                default:
                    return sol::make_object(lua, sol::lua_nil);
            }
        }

        // ---- watchdog ------------------------------------------------------------

        // The debug hook fires every kWatchdogInstructionInterval Lua
        // instructions while a handler runs and errors the script out once the
        // per-call deadline passes or the MCP call is cancelled
        // (notifications/cancelled). It only fires while LUA code executes — a
        // script blocked inside a native olo.call_tool is bounded by that
        // tool's own MarshalRead timeout instead. A pcall inside the script can
        // catch one watchdog error, but the hook keeps firing, so a
        // catch-and-continue loop still cannot run to completion. (Containment
        // for accidents, not for adversaries — see the ADR's threat model.)
        constexpr int kWatchdogInstructionInterval = 50000;

        // The runtime the CURRENTLY EXECUTING handler put in charge of its Lua
        // state; read by the C hook, which gets no closure. One slot per thread
        // is enough: the runtime mutex means at most one handler runs per state,
        // and each handler runs start-to-finish on one worker thread.
        thread_local ScriptToolsRuntime* t_HookRuntime = nullptr;

        void WatchdogHook(lua_State* state, lua_Debug* /*debug*/)
        {
            ScriptToolsRuntime* runtime = t_HookRuntime;
            if (runtime == nullptr)
                return;
            if (runtime->CurrentServer != nullptr && runtime->CurrentServer->IsCurrentCallCancelled())
                luaL_error(state, "tool call cancelled by the client");
            if (std::chrono::steady_clock::now() >= runtime->CallDeadline)
                luaL_error(state, "script tool exceeded its %d ms time budget",
                           static_cast<int>(runtime->Budget.count()));
        }

        // RAII: arm the watchdog hook + deadline for one handler call (or, with a
        // null server, for a top-level script load). Nests correctly: olo.call_tool
        // can re-enter Lua and drive an inner script tool, whose handler arms a
        // second scope. The prior deadline / server / hook-runtime are saved and
        // restored so the OUTER call's timeout + cancellation enforcement resumes
        // once the inner scope unwinds — instead of the inner dtor unconditionally
        // clearing them and leaving the rest of the outer call running unguarded.
        class WatchdogScope
        {
          public:
            WatchdogScope(ScriptToolsRuntime& runtime, McpServer* server)
                : m_Runtime(runtime),
                  m_PriorHookRuntime(t_HookRuntime),
                  m_PriorDeadline(runtime.CallDeadline),
                  m_PriorServer(runtime.CurrentServer)
            {
                m_Runtime.CallDeadline = std::chrono::steady_clock::now() + m_Runtime.Budget;
                if (server != nullptr)
                    m_Runtime.CurrentServer = server;
                t_HookRuntime = &m_Runtime;
                lua_sethook(m_Runtime.Lua.lua_state(), &WatchdogHook, LUA_MASKCOUNT, kWatchdogInstructionInterval);
            }
            ~WatchdogScope()
            {
                t_HookRuntime = m_PriorHookRuntime;
                m_Runtime.CurrentServer = m_PriorServer;
                m_Runtime.CallDeadline = m_PriorDeadline;
                // Only fully disarm when there is no outer scope to hand back to;
                // otherwise leave the hook armed so the restored outer deadline/server
                // keep being enforced for the remainder of that call.
                if (m_PriorHookRuntime == nullptr)
                    lua_sethook(m_Runtime.Lua.lua_state(), nullptr, 0, 0);
            }
            WatchdogScope(const WatchdogScope&) = delete;
            WatchdogScope& operator=(const WatchdogScope&) = delete;

          private:
            ScriptToolsRuntime& m_Runtime;
            ScriptToolsRuntime* m_PriorHookRuntime;
            std::chrono::steady_clock::time_point m_PriorDeadline;
            McpServer* m_PriorServer;
        };

        // ---- sandbox -------------------------------------------------------------

        // Reserved namespace for script tools: never collides with the native
        // olo_* tools, so a script cannot shadow or spoof a built-in (the P5a
        // RegisterTool validation still applies on top).
        constexpr std::string_view kScriptToolPrefix = "script_";

        bool IsValidScriptToolName(std::string_view name)
        {
            if (name.size() <= kScriptToolPrefix.size() || name.size() > 128)
                return false;
            if (name.substr(0, kScriptToolPrefix.size()) != kScriptToolPrefix)
                return false;
            return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(kScriptToolPrefix.size()), name.end(),
                               [](char c)
                               { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
        }

        // Open the restricted library set and strip the escape hatches. base
        // brings pcall/pairs/type/tostring/... but also the file/code loaders —
        // dofile/loadfile read the filesystem and load compiles caller-supplied
        // chunks; all are removed (ADR: capability-stripped, not audited).
        void BuildSandbox(sol::state& lua)
        {
            lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
            lua["dofile"] = sol::lua_nil;
            lua["loadfile"] = sol::lua_nil;
            lua["load"] = sol::lua_nil;
            lua["require"] = sol::lua_nil;
            // print() goes to the engine log instead of stdout, so scripts can
            // trace without gaining an I/O channel.
            lua["print"] = [](sol::variadic_args args)
            {
                std::string line;
                for (const auto& arg : args)
                {
                    if (!line.empty())
                        line += '\t';
                    line += luaL_tolstring(arg.lua_state(), arg.stack_index(), nullptr);
                    lua_pop(arg.lua_state(), 1);
                }
                OLO_CORE_INFO("[McpScriptTool] {}", line);
            };
        }

        // The olo.* bridge — the script's ONLY window into the engine.
        void BindBridge(sol::state& lua, const std::shared_ptr<ScriptToolsRuntime>& runtime)
        {
            sol::table olo = lua.create_named_table("olo");
            olo["log"] = [](const std::string& message)
            { OLO_CORE_INFO("[McpScriptTool] {}", message); };

            // olo.call_tool(name, args?) -> (result, nil) | (nil, errorMessage).
            // Routes through the same argument validation dispatch applies, then
            // the tool's handler directly — legal because we are already ON a
            // dispatch worker thread (the outer script tool's handler), so
            // MarshalRead inside the inner tool behaves normally. ProjectWrite
            // tools are rejected outright: script tools stay read-only by
            // construction (the ADR's one-sentence security argument).
            olo["call_tool"] = [runtime](const std::string& name, sol::optional<sol::table> args)
                -> std::tuple<sol::object, sol::object>
            {
                sol::state_view lua(runtime->Lua.lua_state());
                const auto fail = [&lua](const std::string& message)
                {
                    return std::tuple<sol::object, sol::object>(sol::make_object(lua, sol::lua_nil),
                                                                sol::make_object(lua, message));
                };

                McpServer* server = runtime->CurrentServer;
                if (server == nullptr)
                    return fail("olo.call_tool is only available while a tool call is executing");

                const ToolDef* target = nullptr;
                for (const ToolDef& tool : server->Tools())
                {
                    if (tool.Name == name)
                    {
                        target = &tool;
                        break;
                    }
                }
                if (target == nullptr)
                    return fail("unknown tool: " + name);
                if (target->ProjectWrite)
                    return fail("tool '" + name + "' mutates the project; script tools are read-only (v1)");

                Json arguments = Json::object();
                if (args.has_value())
                {
                    try
                    {
                        arguments = LuaToJson(args.value(), 0);
                    }
                    catch (const std::exception& e)
                    {
                        return fail(std::string("arguments not convertible to JSON: ") + e.what());
                    }
                    if (!arguments.is_object())
                        return fail("arguments must be a table with string keys");
                }
                if (const auto error = McpServer::ValidateArguments(target->InputSchema, arguments))
                    return fail("invalid arguments for '" + name + "': " + *error);

                ToolResult result;
                try
                {
                    result = target->Handler(*server, arguments);
                }
                catch (const std::exception& e)
                {
                    return fail(std::string("tool failed: ") + e.what());
                }
                if (result.IsError)
                {
                    std::string message = "tool returned an error";
                    if (result.Content.is_array() && !result.Content.empty() && result.Content[0].contains("text"))
                        message = result.Content[0]["text"].get<std::string>();
                    return fail(message);
                }

                // Prefer the typed result; fall back to the first text block.
                Json payload;
                if (!result.StructuredContent.is_null())
                    payload = result.StructuredContent;
                else if (result.Content.is_array() && !result.Content.empty() && result.Content[0].contains("text"))
                    payload = result.Content[0]["text"];
                return { JsonToLua(lua, payload), sol::make_object(lua, sol::lua_nil) };
            };
        }

        // Wrap one registered Lua handler as a ToolHandler. Serializes on the
        // runtime mutex, arms the watchdog, converts args in, result out, and
        // turns every Lua error into a clean isError result.
        ToolHandler MakeScriptHandler(std::shared_ptr<ScriptToolsRuntime> runtime, sol::protected_function handler)
        {
            return [runtime = std::move(runtime), handler = std::move(handler)](McpServer& server,
                                                                                const Json& arguments) -> ToolResult
            {
                std::lock_guard lock(runtime->Mutex);
                WatchdogScope watchdog(*runtime, &server);

                // Build the state_view from the raw lua_State* rather than passing the
                // sol::state (which derives from sol::state_view) by value — the latter
                // is a derived->base slice. Semantically identical (both wrap the same
                // state), but explicit and slice-free.
                sol::protected_function_result result =
                    handler(JsonToLua(sol::state_view(runtime->Lua.lua_state()), arguments));
                if (!result.valid())
                {
                    const sol::error error = result;
                    return ToolResult::Error(std::string("Lua tool error: ") + error.what());
                }

                try
                {
                    const sol::object value = result;
                    if (value.get_type() == sol::type::lua_nil)
                        return ToolResult::Text("(no result)");
                    if (value.get_type() == sol::type::string)
                        return ToolResult::Text(value.as<std::string>());
                    const Json json = LuaToJson(value, 0);
                    if (json.is_object())
                        return ToolResult::Structured(json);
                    return ToolResult::Text(json.dump(2));
                }
                catch (const std::exception& e)
                {
                    return ToolResult::Error(std::string("Lua tool returned an unconvertible value: ") + e.what());
                }
            };
        }
    } // namespace

    McpScriptToolsReport LoadScriptTools(McpServer& server, const std::filesystem::path& directory,
                                         std::chrono::milliseconds budget)
    {
        McpScriptToolsReport report;

        // Replace semantics: a rescan (panel restart) drops the previous scripts'
        // tools first, so edits and deletions take effect — not just additions.
        // Legal here because loading only ever happens while the server is stopped.
        server.UnregisterScriptTools();

        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec))
            return report; // no script-tools directory in this project — clean no-op.

        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".lua")
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end()); // deterministic registration order

        if (files.empty())
            return report;

        auto runtime = std::make_shared<ScriptToolsRuntime>();
        runtime->Budget = budget;
        BuildSandbox(runtime->Lua);
        BindBridge(runtime->Lua, runtime);

        // The registration entry point scripts call at load time. Reaches the
        // server + report through the runtime's load-window pointers (set just
        // below, nulled the instant loading ends) rather than capturing the
        // LoadScriptTools stack locals by reference — a script that retains an
        // alias (`local reg = RegisterMcpTool`) and invokes it from a later handler
        // would otherwise write through a dangling `&report`. Outside the load
        // window both pointers are null and the call is rejected safely, which also
        // enforces the ADR's load-at-start rule (the tool vector stays immutable
        // once the server runs) beyond just nil-ing the global alias.
        runtime->Lua["RegisterMcpTool"] = [runtime](const sol::table& def)
        {
            McpServer* serverPtr = runtime->LoadServer;
            McpScriptToolsReport* reportPtr = runtime->LoadReport;
            if (serverPtr == nullptr || reportPtr == nullptr)
            {
                OLO_CORE_WARN("[MCP] RegisterMcpTool called outside the load window (a retained alias?); ignored.");
                return;
            }
            McpServer& server = *serverPtr;
            McpScriptToolsReport& report = *reportPtr;

            const auto reject = [reportPtr](const std::string& why)
            {
                ++reportPtr->Failures;
                reportPtr->Messages.push_back("RegisterMcpTool rejected: " + why);
                OLO_CORE_WARN("[MCP] {}", reportPtr->Messages.back());
            };

            const sol::optional<std::string> name = def["name"];
            if (!name.has_value() || !IsValidScriptToolName(*name))
            {
                reject("'name' must match script_[a-z0-9_]+ (got " +
                       (name.has_value() ? "'" + *name + "'" : std::string("none")) + ")");
                return;
            }
            for (const ToolDef& existing : server.Tools())
            {
                if (existing.Name == *name)
                {
                    reject("a tool named '" + *name + "' already exists");
                    return;
                }
            }
            const sol::optional<sol::protected_function> handler = def["handler"];
            if (!handler.has_value())
            {
                reject("'" + *name + "' has no 'handler' function");
                return;
            }
            const sol::optional<std::string> description = def["description"];

            ToolDef tool;
            tool.Name = *name;
            tool.Title = def["title"].get_or(std::string{});
            tool.Description = description.value_or("Script-defined tool (no description).");
            tool.Toolset = def["toolset"].get_or(std::string("script"));
            tool.Annotations = ReadOnlyAnnotations(); // read-only by construction (see bridge)
            tool.ProjectWrite = false;
            tool.MainMarshaled = false;

            if (const sol::optional<sol::table> schema = def["schema"]; schema.has_value())
            {
                Json converted;
                try
                {
                    converted = LuaToJson(schema.value(), 0);
                }
                catch (const std::exception& e)
                {
                    reject("'" + *name + "' schema is not convertible to JSON: " + e.what());
                    return;
                }
                if (!converted.is_object())
                {
                    reject("'" + *name + "' schema must be a table with string keys (a JSON Schema object)");
                    return;
                }
                tool.InputSchema = converted;
            }
            else
            {
                tool.InputSchema = Json{ { "type", "object" } };
            }

            tool.Handler = MakeScriptHandler(runtime, handler.value());
            tool.ScriptOwned = true; // replaced wholesale on the next rescan
            server.RegisterTool(std::move(tool));
            ++report.ToolsRegistered;
        };

        // Open the load window: RegisterMcpTool routes through these pointers.
        runtime->LoadServer = &server;
        runtime->LoadReport = &report;

        for (const std::filesystem::path& file : files)
        {
            // Bound each file's TOP-LEVEL execution with the same watchdog handler
            // calls get: a script that runs a non-terminating loop at file scope
            // (e.g. `while true do end` before/instead of RegisterMcpTool) would
            // otherwise hang the load — which runs on the editor UI thread at
            // server start — with no deadline to break out. A null server means no
            // cancellation source (there is no MCP call in flight during load), so
            // only the time budget applies here.
            const sol::protected_function_result result = [&]
            {
                WatchdogScope loadWatchdog(*runtime, nullptr);
                return runtime->Lua.safe_script_file(file.string(), sol::script_pass_on_error);
            }();
            if (result.valid())
            {
                ++report.FilesLoaded;
            }
            else
            {
                ++report.Failures;
                const sol::error error = result;
                report.Messages.push_back("script '" + file.filename().string() + "' failed: " + error.what());
                OLO_CORE_WARN("[MCP] {}", report.Messages.back());
            }
        }

        // Loading is over: close the window so scripts cannot register tools from
        // inside a later handler call (the server is about to Start and the tool
        // vector must stay immutable). A retained RegisterMcpTool alias now hits the
        // null-pointer guard, and the global is nil'd for a clear error at the call site.
        runtime->LoadServer = nullptr;
        runtime->LoadReport = nullptr;
        runtime->Lua["RegisterMcpTool"] = sol::lua_nil;

        OLO_CORE_INFO("[MCP] Script tools: {} registered from {} file(s) under {} ({} failure(s)).",
                      report.ToolsRegistered, report.FilesLoaded, directory.string(), report.Failures);
        return report;
    }

    std::filesystem::path DefaultScriptToolsDirectory()
    {
        if (!Project::GetActive())
            return {};
        return Project::GetAssetDirectory() / "McpTools";
    }
} // namespace OloEngine::MCP
