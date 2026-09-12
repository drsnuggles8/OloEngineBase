#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "OloEngine/Core/CVar.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"
#include "MCP/McpEventStream.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Diagnostics MCP tools: olo_log_tail, olo_crash_list / olo_crash_get, and the
// unified olo_events_tail timeline. Split out of the McpTools.cpp monolith
// (issue #357); RegisterBuiltinTools composes every domain's registration.

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
        ToolResult Handle_LogTail(IAutomationHost& /*host*/, const Json& arguments)
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

        // ---- Crash reports (lock-safe; files on disk) --------------------------
        std::filesystem::path CrashReportsDir()
        {
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            if (ec)
                return {};
            return cwd / "CrashReports";
        }

        // ---- olo_debug_levers (lock-free; reads the registry's own atomics) -----
        //
        // The question this answers is "why is this session behaving oddly?".
        // Before the registry existed there was no way to ask it: the levers
        // were 21 independent environment reads, so an editor launched hours ago
        // with OLO_RG_POISON_TRANSIENTS set looked identical to one without.
        [[nodiscard("the JSON-serialized lever kind must be used")]] std::string_view LeverKindName(Levers::LeverKind kind)
        {
            using enum Levers::LeverKind;
            switch (kind)
            {
                case Toggle:
                    return "toggle";
                case Exact:
                    return "exactToggle";
                case Tristate:
                    return "tristate";
                case Integer:
                    return "integer";
                case Number:
                    return "number";
                case Text:
                    return "text";
                default:
                    // Not a missing enumerator — every one is handled above.
                    // This is the "kind holds a value outside the enum's
                    // defined range" case (e.g. read through a stale/raw cast).
                    return "unknown";
            }
        }

        ToolResult Handle_DebugLevers(IAutomationHost& /*host*/, const Json& args)
        {
            const bool activeOnly = args.value("activeOnly", false);

            Json levers = Json::array();
            u32 activeCount = 0;
            for (const Levers::LeverInfo& lever : Levers::Snapshot())
            {
                if (!lever.IsDefault)
                {
                    ++activeCount;
                }
                else if (activeOnly)
                {
                    continue;
                }
                else
                {
                    // Default AND the caller wants everything — fall through
                    // and include it below.
                }

                Json entry;
                entry["name"] = std::string(lever.Name);
                entry["help"] = std::string(lever.Help);
                entry["kind"] = std::string(LeverKindName(lever.Kind));
                entry["value"] = lever.Value;
                entry["isDefault"] = lever.IsDefault;
                entry["source"] = lever.FromEnvironment ? "environment" : "code";
                levers.push_back(std::move(entry));
            }

            Json out;
            out["count"] = levers.size();
            out["activeCount"] = activeCount;
            out["summary"] = Levers::ActiveSummary();
            out["levers"] = std::move(levers);
            return ToolResult::Structured(out);
        }

        // ---- olo_cvar_set (main-marshaled; session WRITE) -----------------------
        //
        // The generalisation of olo_render_debug_set's two special cases (issue
        // #821): every registered console variable, by name, against a running
        // editor. olo_render_debug_set stays because it does more than set a
        // value — it hands back the poison colour map and settles two frames —
        // but the transient-pool eviction it used to own is now a change
        // callback, so this tool flips the aliasing lever just as correctly.
        // Type names come from CVars::CVarTypeName, not a local copy: this
        // tool's schema `enum` and the editor console must not be free to drift.

        ToolResult Handle_CVarSet(IAutomationHost& host, const Json& args)
        {
            if (!args.contains("name") || !args["name"].is_string())
                return ToolResult::Error("Missing 'name': the console variable to set.");
            if (!args.contains("value") || !args["value"].is_string())
                return ToolResult::Error("Missing 'value': the new value, as a string.");

            const std::string name = args["name"].get<std::string>();
            const std::string value = args["value"].get<std::string>();

            // Marshaled to the main thread: the write itself is thread-safe, but
            // the observers run from the game thread's dispatch and a caller
            // reading the result back wants the two in a defined order.
            const Json result = host.MarshalRead([&name, &value]() -> Json
                                                 {
                const CVars::SetResult set = CVars::SetFromString(name, value);
                if (!set.Ok)
                    return Json{ { "__error", set.Error } };

                const std::optional<CVars::CVarInfo> info = CVars::Find(name);
                Json out{
                    { "name", info ? std::string(info->Name) : name },
                    { "value", set.NewValue },
                    { "previous", set.OldValue },
                    { "changed", set.Changed },
                    { "restoreWith", set.OldValue },
                    // The note has to match what actually happened. A no-op
                    // write schedules no notification at all, so promising one
                    // would send a caller looking for an effect that is not
                    // coming.
                    { "note", set.Changed
                                  ? "Observers run at the top of the next frame; a value read back "
                                    "immediately is already the new one, but the frame you are looking "
                                    "at was rendered under the old one."
                                  : "No change: the variable already held this value, so no observer "
                                    "runs and nothing about the rendered frame will differ." },
                };
                if (info)
                {
                    out["type"] = std::string(CVars::CVarTypeName(info->Type));
                    out["isDefault"] = info->IsDefault;
                    out["help"] = std::string(info->Help);
                }
                return out; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        ToolResult Handle_CrashList(IAutomationHost& /*host*/, const Json& /*args*/)
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
            return ToolResult::Structured(out);
        }

        ToolResult Handle_CrashGet(IAutomationHost& /*host*/, const Json& args)
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

            // Bound the read BEFORE loading into memory so a pathologically large
            // report can't exhaust it: read at most kMaxBytes, plus one probe byte to
            // detect (and report) truncation. Slurping file.rdbuf() first would defeat
            // the cap by allocating the whole file regardless.
            constexpr std::size_t kMaxBytes = 200 * 1024;
            std::string content(kMaxBytes, '\0');
            file.read(content.data(), static_cast<std::streamsize>(kMaxBytes));
            content.resize(static_cast<std::size_t>(file.gcount()));
            const bool truncated = file.peek() != std::char_traits<char>::eof();

            Json out;
            out["id"] = id;
            out["truncated"] = truncated;
            out["content"] = std::move(content);
            return ToolResult::Structured(out);
        }

        // ---- olo_events_tail / olo_events_wait (lock-safe; the diagnostics ring) -

        // The comma-separated list of every category token, for error messages.
        std::string CategoryTokenList()
        {
            std::string list;
            for (const auto token : DiagnosticEvent::AllCategoryTokens())
            {
                if (!list.empty())
                    list += ", ";
                list += token;
            }
            return list;
        }

        // The arguments olo_events_tail and olo_events_wait share: `count`,
        // `sinceId` (number or decimal string) and `categories`. Returns an error
        // message, or empty on success. `hasSinceId` reports whether the caller
        // supplied a cursor at all, which the wait command turns into "from now".
        [[nodiscard]] std::string ParseEventQueryArgs(const Json& args, DiagnosticEventQuery& query, bool& hasSinceId)
        {
            hasSinceId = false;
            if (args.contains("count") && args["count"].is_number_integer())
                query.MaxCount = static_cast<std::size_t>(std::clamp<long long>(args["count"].get<long long>(), 1, 500));

            if (args.contains("sinceId"))
            {
                // A cursor that is present but malformed is REFUSED, never
                // reinterpreted: "-1" read as 0 would turn a wait for what happens
                // next into a dump of the whole ring, and stoull would read "-1" as
                // ULLONG_MAX (a wait that can never match) and "12abc" as 12.
                const Json& since = args["sinceId"];
                constexpr const char* kBadCursor =
                    "Invalid 'sinceId': expected a non-negative integer (an event id) or its decimal string form.";
                if (since.is_number_unsigned())
                {
                    query.SinceId = since.get<u64>();
                }
                else if (since.is_number_integer())
                {
                    // A signed integer (how a C++ int reaches nlohmann) is fine
                    // when it is not negative.
                    const auto signedId = since.get<long long>();
                    if (signedId < 0)
                        return kBadCursor;
                    query.SinceId = static_cast<u64>(signedId);
                }
                else if (since.is_string())
                {
                    const std::string text = since.get<std::string>();
                    u64 parsed = 0;
                    const char* const first = text.data();
                    const char* const last = first + text.size();
                    const auto [ptr, ec] = std::from_chars(first, last, parsed);
                    if (text.empty() || ec != std::errc{} || ptr != last)
                        return kBadCursor;
                    query.SinceId = parsed;
                }
                else
                {
                    return kBadCursor;
                }
                hasSinceId = true;
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
                        return "Unknown category '" + entry.get<std::string>() + "'. Valid: " + CategoryTokenList() + ".";
                }
            }
            return {};
        }

        // The shared result shape: `events` (the per-entry shape McpEventStream.h
        // guarantees the push carriers use too), `count`, the `lastId` cursor and
        // the `dropped` gap.
        Json EventQueryResultJson(const DiagnosticEventQueryResult& result)
        {
            Json arr = Json::array();
            for (const auto& event : result.Events)
                arr.push_back(EventToJson(event)); // shared with the SSE push path (McpEventStream.h)

            Json out;
            out["count"] = static_cast<int>(arr.size());
            // The highest id in the buffer at snapshot time — pass it back as the next
            // call's sinceId to poll only what happened since. Consistent with the events
            // above (same lock), and stable even when no events matched the filter.
            out["lastId"] = result.LastId;
            // How many records above sinceId were evicted before this call could
            // return them (#1131). Non-zero means the caller's history has a hole
            // it must not paper over.
            out["dropped"] = result.Dropped;
            out["events"] = std::move(arr);
            return out;
        }

        // A unified "what just happened?" timeline backed by the engine's diagnostics
        // event ring buffer (Debug/DiagnosticsEventLog.h, mutex-guarded — safe from the
        // handler thread). Supports incremental polling via sinceId: pass back the
        // returned lastId to get only events that happened since the previous call.
        ToolResult Handle_EventsTail(IAutomationHost& /*host*/, const Json& args)
        {
            DiagnosticEventQuery query;
            bool hasSinceId = false;
            if (const std::string error = ParseEventQueryArgs(args, query, hasSinceId); !error.empty())
                return ToolResult::Error(error);

            // Events + cursor in one locked snapshot: reading LastId() separately would
            // race a concurrent Record and skip an event on the next sinceId poll.
            return ToolResult::Structured(EventQueryResultJson(DiagnosticsEventLog::Get().QueryWithCursor(query)));
        }

        // The long-poll subscription (#1131): the same query as olo_events_tail,
        // but the call BLOCKS on the ring's condition variable until a matching
        // event exists, `waitMs` elapses, or the call is cancelled. Runs on the
        // handler thread and never touches the game thread, so it cannot stall a
        // frame however long it waits. A caller that passes no sinceId waits for
        // events NEWER than the call — the "what happens next" question — and gets
        // the cursor back either way.
        ToolResult Handle_EventsWait(IAutomationHost& host, const Json& args)
        {
            DiagnosticEventQuery query;
            bool hasSinceId = false;
            if (const std::string error = ParseEventQueryArgs(args, query, hasSinceId); !error.empty())
                return ToolResult::Error(error);
            if (!hasSinceId)
                query.SinceId = DiagnosticsEventLog::Get().LastId();

            // The cap keeps the OLDEST matches, not the newest as a tail does: a
            // subscription promises "everything after your cursor, in order", so
            // when a burst outgrows `count` the caller gets its head and a cursor
            // that stops at the last record delivered — never a cursor past
            // records it was not shown.
            const std::size_t count = args.contains("count") ? query.MaxCount : 100;
            query.MaxCount = 0;

            long long waitMs = 10000;
            if (args.contains("waitMs") && args["waitMs"].is_number_integer())
                waitMs = std::clamp<long long>(args["waitMs"].get<long long>(), 0, 60000);

            DiagnosticEventQueryResult result = DiagnosticsEventLog::Get().WaitWithCursor(
                query, std::chrono::milliseconds(waitMs), [&host]
                { return host.IsCurrentCallCancelled(); });
            if (result.Events.size() > count)
            {
                result.Events.resize(count);
                result.LastId = result.Events.back().Id;
            }

            Json out = EventQueryResultJson(result);
            const bool cancelled = host.IsCurrentCallCancelled();
            out["cancelled"] = cancelled;
            out["timedOut"] = result.Events.empty() && !cancelled;
            return ToolResult::Structured(out);
        }

        // The schema every event entry shares between the two commands; the same
        // shape the push carriers emit (McpEventStream.h).
        Schema::Node EventEntrySchema()
        {
            return Schema::Object()
                .Prop("id", Schema::Int().Min(0))
                .Prop("category", Schema::String().EnumFrom(DiagnosticEvent::AllCategoryTokens()))
                .Prop("time", Schema::String().Desc("UTC wall-clock HH:MM:SS.mmm; omitted when the event carries no timestamp."))
                .Prop("message", Schema::String())
                .Prop("entity", Schema::String().Desc("Entity UUID as a decimal string (u64 — beyond JSON integer precision); omitted when the event has no entity."))
                .Prop("context", Schema::String().Desc("Scene name / asset path / script name; omitted when empty."))
                .Prop("data", Schema::Object().Desc("Structured payload, present for the automation-bus categories only. scene_save: {scene, path, changed}; scene_dirty: {scene, dirty}; asset_import: {asset, type, handle, source}; compile_finished: {kind, target, ok, errors, warnings, seconds}; command_completed: {command, ok, durationMs, projectWrite, toolset}. Identities only — never a command's arguments or result."));
        }

        Schema::Node CategoriesFilterSchema()
        {
            return Schema::Array(Schema::String().EnumFrom(DiagnosticEvent::AllCategoryTokens()))
                .Desc("Only return events whose category is in this list. Omit for all categories.");
        }

        Schema::Node SinceIdSchema(const char* description)
        {
            return Schema::Raw(Json{ { "type", Json::array({ "integer", "string" }) } }).Min(0).Desc(description);
        }

    } // namespace

    void RegisterDiagnosticsTools(AutomationRegistry& registry)
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
            // No outputSchema: raw spdlog lines are free text, which an outputSchema cannot constrain.
            tool.MainMarshaled = false;
            tool.Handler = Handle_LogTail;
            registry.Register(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_debug_levers";
            tool.Toolset = "diagnostics";
            tool.Title = "List debug levers";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "The engine's debug/diagnostic levers and their current values — transient poisoning, "
                "aliasing, bindless routing, the threading and terrain-LOD bisection switches, and the "
                "task-graph tuning knobs. Each seeds from an environment variable of the same name and "
                "all but the read-only text ones are settable at runtime by name with olo_cvar_set "
                "(olo_render_debug_set stays the richer tool for the two transient instruments). Call "
                "this FIRST when a session renders or performs unlike a clean one: a lever left on is "
                "invisible otherwise, and explains a whole class of 'it only misbehaves on this machine'.";
            tool.InputSchema = Schema::Object()
                                   .Prop("activeOnly", Schema::Bool().Desc("Only return levers that are NOT at their default (default false)."))
                                   .NoAdditional();
            tool.OutputSchema =
                Schema::Object()
                    .Prop("count", Schema::Int().Min(0).Desc("Number of levers returned (size of levers)."))
                    .Prop("activeCount", Schema::Int().Min(0).Desc("How many levers are not at their default — counts ALL levers regardless of activeOnly."))
                    .Prop("summary", Schema::String().Desc("The non-default levers as one line, exactly as the startup log prints it. Empty when everything is default."))
                    .Prop("levers", Schema::Array(Schema::Object()
                                                      .Prop("name", Schema::String().Desc("The environment variable that seeds it."))
                                                      .Prop("help", Schema::String())
                                                      .Prop("kind", Schema::String().Enum({ "toggle", "exactToggle", "tristate", "integer", "number", "text" }))
                                                      .Prop("value", Schema::String().Desc("Rendered current value; 'unset' when the lever has none."))
                                                      .Prop("isDefault", Schema::Bool().Desc("True when the lever is doing nothing."))
                                                      .Prop("source", Schema::String().Enum({ "environment", "code" }).Desc("'code' means something overrode the environment seed at runtime — a C++ setter, --set NAME=VALUE, the editor console, or olo_cvar_set."))))
                    .Required({ "count", "activeCount", "summary", "levers" });
            tool.MainMarshaled = false;
            tool.Handler = Handle_DebugLevers;
            registry.Register(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_cvar_set";
            tool.Toolset = "diagnostics";
            tool.Title = "Set a console variable";
            // Flips a session-global engine switch — the same read-only line
            // olo_render_debug_set crosses, so the same gate. Reversible via the
            // reported 'restoreWith'.
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Set any of the engine's console variables BY NAME against the running editor, instead of "
                "exporting an environment variable and restarting — which loses whatever repro you were "
                "looking at. The names, current values, types and help are exactly what olo_debug_levers "
                "lists, so call that first. Values are given as strings: a boolean takes on/off (also "
                "1/0, true/false, yes/no); a tristate additionally takes 'unset', which means 'leave the "
                "hardware-derived default alone' and is NOT the same as off; an int or float also takes "
                "'unset' to clear it. Out-of-range and non-finite numbers are refused with the reason, "
                "not silently clamped. A few variables are read-only (they are consumed once at init, so "
                "a later write would be accepted and ignored) and are refused explicitly. Subsystems that "
                "cached the value are re-notified at the top of the next frame, so the frame you captured "
                "a moment ago was still rendered under the old value — take a fresh screenshot with "
                "olo_screenshot { forceFrame: true }. For the two transient-corruption instruments "
                "specifically, olo_render_debug_set is still the better tool: it also returns the "
                "resource->poison-colour map and settles two frames for you. This is a WRITE tool: "
                "refused unless 'Allow writes' is enabled in the editor's MCP Server panel (off by default).";
            tool.InputSchema = Schema::Object()
                                   .Prop("name", Schema::String().Desc("The console variable, e.g. OLO_RG_POISON_TRANSIENTS. Case-insensitive."))
                                   .Prop("value", Schema::String().Desc("The new value as a string — see the description for what each type accepts."))
                                   .Required({ "name", "value" })
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("name", Schema::String().Desc("The registered name, in its canonical casing."))
                                    .Prop("value", Schema::String().Desc("Rendered value after the call."))
                                    .Prop("previous", Schema::String().Desc("Rendered value before the call."))
                                    .Prop("changed", Schema::Bool().Desc("False when the value was already what you asked for."))
                                    .Prop("restoreWith", Schema::String().Desc("Pass this back as 'value' to restore."))
                                    .Prop("type", Schema::String().Enum({ "bool", "tristate", "int", "float", "string" }))
                                    .Prop("isDefault", Schema::Bool().Desc("True when the variable is now doing nothing."))
                                    .Prop("help", Schema::String())
                                    .Prop("note", Schema::String().Desc("When the change reaches subsystems that cached it."))
                                    .Required({ "name", "value", "previous", "changed", "restoreWith" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_CVarSet;
            registry.Register(std::move(tool));
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
            tool.OutputSchema = Schema::Object()
                                    .Prop("count", Schema::Int().Min(0))
                                    .Prop("directory", Schema::String().Desc("Absolute path of the CrashReports directory scanned."))
                                    .Prop("crashes", Schema::Array(Schema::Object()
                                                                       .Prop("id", Schema::String().Desc("Filename — the id olo_crash_get takes."))
                                                                       .Prop("sizeBytes", Schema::Int().Min(0).Desc("File size in bytes (0 when the size could not be read)."))))
                                    .Required({ "count", "directory", "crashes" });
            tool.MainMarshaled = false;
            tool.Handler = Handle_CrashList;
            registry.Register(std::move(tool));
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
            tool.OutputSchema = Schema::Object()
                                    .Prop("id", Schema::String().Desc("The crash report filename that was read (echoes the request)."))
                                    .Prop("truncated", Schema::Bool().Desc("True when the file exceeded the 200 KiB read cap and content is a prefix."))
                                    .Prop("content", Schema::String().Desc("Raw crash-report text (exception, system info, last 200 log lines), at most 200 KiB."))
                                    .Required({ "id", "truncated", "content" });
            tool.MainMarshaled = false;
            tool.Handler = Handle_CrashGet;
            registry.Register(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_events_tail";
            tool.Toolset = "diagnostics";
            tool.Title = "Tail diagnostics events";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Return the unified 'what just happened?' event timeline from the engine's diagnostics "
                "ring buffer: scene loads and saves, unsaved-changes edges, entering/leaving Play mode, "
                "runtime entity spawn/destroy, asset imports and hot-reloads, script errors, build / script / "
                "shader compiles finishing, and automation commands completing — newest last, each with a "
                "monotonic 'id'. The key use is INCREMENTAL POLLING: do an action, then pass the previous "
                "call's 'lastId' as 'sinceId' to get only what happened since; 'dropped' > 0 means the cursor "
                "fell behind the 512-record window and that many records are gone. Filter with 'categories'. "
                "To BLOCK for the next event instead of polling, use olo_events_wait. Bulk churn (scene-copy on "
                "Play, deserialize on load) is collapsed into single scene_load/play events, not per-entity spam.";
            tool.InputSchema = Schema::Object()
                                   .Prop("count", Schema::Int().Min(1).Max(500).Desc("How many of the most recent matching events to return (default 50)."))
                                   .Prop("sinceId", SinceIdSchema("Only return events with id greater than this. Accepts the id as a number or its string form (for large cursors beyond JSON integer precision). Pass back the previous response's 'lastId' for incremental polling."))
                                   .Prop("categories", CategoriesFilterSchema())
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("count", Schema::Int().Min(0))
                                    .Prop("lastId", Schema::Int().Min(0).Desc("Highest event id in the buffer at snapshot time (0 when nothing was ever recorded) — pass back as the next call's sinceId; valid even when no events matched."))
                                    .Prop("dropped", Schema::Int().Min(0).Desc("Records above sinceId that were evicted from the ring before this call; 0 unless the cursor fell behind the 512-record window."))
                                    .Prop("events", Schema::Array(EventEntrySchema()).Desc("Matching events, oldest first, newest last."))
                                    .Required({ "count", "lastId", "dropped", "events" });
            tool.MainMarshaled = false;
            tool.Handler = Handle_EventsTail;
            registry.Register(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_events_wait";
            tool.Toolset = "diagnostics";
            tool.Title = "Wait for diagnostics events";
            tool.Annotations = ReadOnlyAnnotations();
            // THE SUBSCRIPTION COMMAND of the automation event bus (#1131). A read at
            // the authority of any read-only command — see AutomationEvents.h for
            // why that is sound (identity-only payloads) — and deliberately NOT a
            // completion-event source itself: EmitsCompletionEvent excludes
            // read-only commands, or two waiting agents would wake each other
            // forever.
            tool.Description =
                "BLOCK until the next matching diagnostics event, then return it — the subscription half of "
                "olo_events_tail, for an agent that would otherwise poll. Returns as soon as at least one event "
                "with id > 'sinceId' matches 'categories' (all such, oldest first, up to 'count'), or after "
                "'waitMs' with count 0 and timedOut true. Omit 'sinceId' to wait for events NEWER than this call; "
                "to not miss one that lands between your action and this call, take 'lastId' from a call made "
                "BEFORE the action and pass it as 'sinceId'. The response's 'lastId' is always the cursor for the "
                "next call. Runs on the handler thread only, so waiting never stalls the editor; cancel the call "
                "to stop early. 'dropped' > 0 means the cursor fell behind the 512-record window.";
            tool.InputSchema = Schema::Object()
                                   .Prop("sinceId", SinceIdSchema("Wait for events with id greater than this. Omit to wait for events newer than the call. Accepts a number or its decimal string form."))
                                   .Prop("categories", CategoriesFilterSchema())
                                   .Prop("waitMs", Schema::Int().Min(0).Max(60000).Desc("How long to wait for a match before returning empty (default 10000, max 60000). 0 returns immediately, like olo_events_tail."))
                                   .Prop("count", Schema::Int().Min(1).Max(500).Desc("Cap on how many matching events to return (default 100). Keeps the OLDEST matches and moves 'lastId' back to the last one returned, so nothing is skipped."))
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("count", Schema::Int().Min(0))
                                    .Prop("lastId", Schema::Int().Min(0).Desc("The cursor for the next call — valid whether or not anything matched. When 'count' truncated a burst it is the id of the LAST EVENT RETURNED, so resuming from it delivers the rest."))
                                    .Prop("dropped", Schema::Int().Min(0).Desc("Records above sinceId evicted before this call could return them; 0 unless the cursor fell behind the 512-record window."))
                                    .Prop("timedOut", Schema::Bool().Desc("True when waitMs elapsed with no match."))
                                    .Prop("cancelled", Schema::Bool().Desc("True when the call was cancelled before a match."))
                                    .Prop("events", Schema::Array(EventEntrySchema()).Desc("Matching events, oldest first, newest last."))
                                    .Required({ "count", "lastId", "dropped", "timedOut", "cancelled", "events" });
            tool.MainMarshaled = false;
            tool.Handler = Handle_EventsWait;
            registry.Register(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
