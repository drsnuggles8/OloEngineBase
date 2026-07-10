#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"
#include "MCP/McpEventStream.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
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

    } // namespace

    void RegisterDiagnosticsTools(McpServer& server)
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
    }
} // namespace OloEngine::MCP
