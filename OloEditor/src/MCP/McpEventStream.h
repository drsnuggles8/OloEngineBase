#pragma once

// Pure, transport-agnostic serialization for the MCP server-push event stream
// (issue #306 item B — the "live push" half of olo_events_tail).
//
// `GET /mcp` opens a persistent `text/event-stream` (Server-Sent Events) and the
// engine's diagnostics ring buffer (Debug/DiagnosticsEventLog.h) is bridged onto it:
// every newly-recorded DiagnosticEvent is pushed to connected agents as an MCP
// JSON-RPC notification (`notifications/message`), framed per the SSE wire format.
//
// This header holds ONLY the serialization — turning a DiagnosticEvent into its
// JSON-RPC notification object and into the SSE frame bytes. It is deliberately
// httplib-free and editor-free so:
//   * the OloEditor SSE transport (MCP/McpServer.cpp) and the poll-based
//     olo_events_tail tool (MCP/McpTools.cpp) share ONE event-JSON shape — a push
//     consumer and a poll consumer see byte-identical event fields, and
//   * the logic is unit-testable in OloEngine/tests/MCP without binding a socket or
//     constructing httplib types (mirrors the McpDispatch / McpGoldenCompare seams).

#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace OloEngine::MCP
{
    using Json = nlohmann::json;

    // Format an epoch-seconds timestamp as a UTC "HH:MM:SS.mmm" wall-clock string.
    // Computed with modular arithmetic to dodge the non-thread-safe / platform-split
    // gmtime APIs — the date is irrelevant for a "what just happened" timeline. Empty
    // for a non-finite / non-positive timestamp. (Shared with olo_events_tail so the
    // stream and the poll tool stamp time identically.)
    [[nodiscard]] inline std::string FormatEventUtcTime(f64 epochSeconds)
    {
        if (!std::isfinite(epochSeconds) || epochSeconds <= 0.0)
            return std::string{};
        const auto total = static_cast<std::int64_t>(epochSeconds);
        const auto secOfDay = static_cast<int>(((total % 86400) + 86400) % 86400);
        int milliseconds = static_cast<int>((epochSeconds - static_cast<f64>(total)) * 1000.0);
        if (milliseconds < 0)
            milliseconds = 0;
        else if (milliseconds > 999)
            milliseconds = 999;
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d",
                      secOfDay / 3600, (secOfDay % 3600) / 60, secOfDay % 60, milliseconds);
        return std::string(buffer);
    }

    // One diagnostics event as a JSON object — the SAME shape olo_events_tail emits
    // per array entry (id, category, time, message, entity, context), so a push
    // subscriber and an incremental poller see identical records. Optional fields
    // (time / entity / context) are omitted when absent, exactly like the poll path.
    [[nodiscard]] inline Json EventToJson(const DiagnosticEvent& event)
    {
        Json j;
        j["id"] = event.Id;
        j["category"] = DiagnosticEvent::CategoryToString(event.Category);
        if (std::string time = FormatEventUtcTime(event.Timestamp); !time.empty())
            j["time"] = std::move(time);
        j["message"] = event.Message;
        if (event.Entity != 0)
            j["entity"] = std::to_string(event.Entity);
        if (!event.Context.empty())
            j["context"] = event.Context;
        return j;
    }

    // Map an event category to an MCP logging severity (RFC-5424 token, the
    // `notifications/message` `level` field): a script error is an "error", the
    // chatty per-entity spawn/destroy are "debug", and the rest are "info". Lets a
    // generic MCP client filter / colour the pushed events by severity.
    [[nodiscard]] inline const char* EventLogLevel(DiagnosticEventCategory category)
    {
        switch (category)
        {
            case DiagnosticEventCategory::ScriptError:
                return "error";
            case DiagnosticEventCategory::EntitySpawn:
            case DiagnosticEventCategory::EntityDestroy:
                return "debug";
            case DiagnosticEventCategory::SceneLoad:
            case DiagnosticEventCategory::Play:
            case DiagnosticEventCategory::Stop:
            case DiagnosticEventCategory::AssetReload:
                return "info";
        }
        return "info";
    }

    // The full MCP JSON-RPC notification for one event: the spec's logging
    // notification (`notifications/message`) carrying the structured event under
    // `params.data`, with a severity `level` and a `logger` namespace. It is a
    // notification (no `id`), so the client never replies. Spec-compliant means a
    // generic MCP client surfaces it without bespoke handling; clients that ignore
    // logging simply drop it.
    [[nodiscard]] inline Json MakeEventNotification(const DiagnosticEvent& event)
    {
        return Json{ { "jsonrpc", "2.0" },
                     { "method", "notifications/message" },
                     { "params", { { "level", EventLogLevel(event.Category) }, { "logger", "olo.events" }, { "data", EventToJson(event) } } } };
    }

    // Frame a payload as one SSE event block: an `id:` line carrying the monotonic
    // event id (so a reconnecting client can resume via the `Last-Event-ID` header)
    // and a single `data:` line, terminated by the mandatory blank line. The payload
    // is dumped compact so it never contains a newline that would split the data
    // field across SSE lines.
    [[nodiscard]] inline std::string FormatSseEvent(u64 eventId, const Json& payload)
    {
        std::string out;
        out += "id: ";
        out += std::to_string(eventId);
        out += '\n';
        out += "data: ";
        out += payload.dump();
        out += "\n\n";
        return out;
    }

    // An SSE comment line (a line starting with ':'). Clients ignore it; used as a
    // keep-alive heartbeat that also probes whether the socket is still writable.
    [[nodiscard]] inline std::string FormatSseComment(std::string_view text)
    {
        std::string out;
        out += ": ";
        out += text;
        out += "\n\n";
        return out;
    }
} // namespace OloEngine::MCP
