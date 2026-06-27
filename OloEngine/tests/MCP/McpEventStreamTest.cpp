// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the MCP server-push serialization seam (issue #306 item B). The
// SSE transport (GET /mcp) lives in OloEditor and needs a live socket, but the pure
// event -> MCP-notification -> SSE-frame serialization is header-only
// (MCP/McpEventStream.h), so it is exercised here directly with no editor / socket /
// agent — the same pattern as McpGoldenCompare / McpRenderExplain. We pin: the event
// JSON shape (must match olo_events_tail), the category -> log-level map, the
// JSON-RPC notification envelope, and the text/event-stream wire framing.
#include "MCP/McpEventStream.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace
{
    using OloEngine::DiagnosticEvent;
    using OloEngine::DiagnosticEventCategory;
    using Json = nlohmann::json;

    namespace MCP = OloEngine::MCP;

    // Build a fully-populated event with a fixed timestamp (2000-01-01 12:34:56.789
    // UTC = 946730096.789 epoch seconds) so the formatted time is deterministic.
    DiagnosticEvent MakeEvent(DiagnosticEventCategory category, std::string message,
                              u64 entity = 0, std::string context = {})
    {
        DiagnosticEvent e;
        e.Id = 7;
        e.Timestamp = 946730096.789;
        e.Category = category;
        e.Message = std::move(message);
        e.Entity = entity;
        e.Context = std::move(context);
        return e;
    }
} // namespace

// ---- FormatEventUtcTime ----------------------------------------------------

TEST(McpEventStreamTime, FormatsEpochAsUtcWallClock)
{
    // 946730096.789 -> 12:34:56.789 UTC (date is intentionally discarded).
    EXPECT_EQ(MCP::FormatEventUtcTime(946730096.789), "12:34:56.789");
}

TEST(McpEventStreamTime, RejectsNonFiniteAndNonPositive)
{
    EXPECT_TRUE(MCP::FormatEventUtcTime(0.0).empty());
    EXPECT_TRUE(MCP::FormatEventUtcTime(-1.0).empty());
    EXPECT_TRUE(MCP::FormatEventUtcTime(std::nan("")).empty());
    EXPECT_TRUE(MCP::FormatEventUtcTime(std::numeric_limits<f64>::infinity()).empty());
}

// ---- EventToJson (must mirror olo_events_tail's per-entry shape) ------------

TEST(McpEventStreamJson, FullEventCarriesEveryField)
{
    const Json j = MCP::EventToJson(
        MakeEvent(DiagnosticEventCategory::EntitySpawn, "Spawned entity 'Hero'", 4242u, "MyScene"));

    EXPECT_EQ(j["id"], 7u);
    EXPECT_EQ(j["category"], "entity_spawn");
    EXPECT_EQ(j["time"], "12:34:56.789");
    EXPECT_EQ(j["message"], "Spawned entity 'Hero'");
    // Entity is serialized as a STRING (UUIDs exceed JSON integer precision), exactly
    // like the poll path.
    EXPECT_EQ(j["entity"], "4242");
    EXPECT_EQ(j["context"], "MyScene");
}

TEST(McpEventStreamJson, OptionalFieldsOmittedWhenAbsent)
{
    const Json j = MCP::EventToJson(MakeEvent(DiagnosticEventCategory::Play, "Entered Play mode"));

    EXPECT_TRUE(j.contains("id"));
    EXPECT_TRUE(j.contains("category"));
    EXPECT_TRUE(j.contains("message"));
    // Entity 0 and an empty context are omitted, not emitted as 0 / "".
    EXPECT_FALSE(j.contains("entity"));
    EXPECT_FALSE(j.contains("context"));
}

TEST(McpEventStreamJson, OmitsTimeForNonFiniteTimestamp)
{
    DiagnosticEvent e = MakeEvent(DiagnosticEventCategory::Stop, "Left Play mode");
    e.Timestamp = 0.0; // unset / invalid
    const Json j = MCP::EventToJson(e);
    EXPECT_FALSE(j.contains("time"));
}

// ---- EventLogLevel ---------------------------------------------------------

TEST(McpEventStreamLevel, MapsCategoryToSeverity)
{
    EXPECT_STREQ(MCP::EventLogLevel(DiagnosticEventCategory::ScriptError), "error");
    EXPECT_STREQ(MCP::EventLogLevel(DiagnosticEventCategory::EntitySpawn), "debug");
    EXPECT_STREQ(MCP::EventLogLevel(DiagnosticEventCategory::EntityDestroy), "debug");
    EXPECT_STREQ(MCP::EventLogLevel(DiagnosticEventCategory::SceneLoad), "info");
    EXPECT_STREQ(MCP::EventLogLevel(DiagnosticEventCategory::Play), "info");
    EXPECT_STREQ(MCP::EventLogLevel(DiagnosticEventCategory::Stop), "info");
    EXPECT_STREQ(MCP::EventLogLevel(DiagnosticEventCategory::AssetReload), "info");
}

// Every category must map to a recognised RFC-5424 severity token (no "unknown" or
// empty), so a new category can't silently ship an invalid notification level.
TEST(McpEventStreamLevel, EveryCategoryHasAKnownLevel)
{
    constexpr DiagnosticEventCategory all[] = {
        DiagnosticEventCategory::SceneLoad,
        DiagnosticEventCategory::Play,
        DiagnosticEventCategory::Stop,
        DiagnosticEventCategory::EntitySpawn,
        DiagnosticEventCategory::EntityDestroy,
        DiagnosticEventCategory::AssetReload,
        DiagnosticEventCategory::ScriptError,
    };
    static_assert(std::size(all) == OloEngine::kDiagnosticEventCategoryCount,
                  "update this list when DiagnosticEventCategory changes");
    const std::vector<std::string> valid = { "debug", "info", "notice", "warning",
                                             "error", "critical", "alert", "emergency" };
    for (const DiagnosticEventCategory c : all)
    {
        const std::string level = MCP::EventLogLevel(c);
        EXPECT_NE(std::find(valid.begin(), valid.end(), level), valid.end()) << "level: " << level;
    }
}

// ---- MakeEventNotification (JSON-RPC envelope) -----------------------------

TEST(McpEventStreamNotification, IsAJsonRpcMessageNotification)
{
    const Json n = MCP::MakeEventNotification(
        MakeEvent(DiagnosticEventCategory::ScriptError, "NullReferenceException", 99u, "Player.cs"));

    EXPECT_EQ(n["jsonrpc"], "2.0");
    EXPECT_EQ(n["method"], "notifications/message");
    // A notification carries NO id — the client never replies.
    EXPECT_FALSE(n.contains("id"));

    const Json& params = n["params"];
    EXPECT_EQ(params["level"], "error");
    EXPECT_EQ(params["logger"], "olo.events");
    // The structured event rides under params.data and matches EventToJson exactly.
    EXPECT_EQ(params["data"], MCP::EventToJson(
                                  MakeEvent(DiagnosticEventCategory::ScriptError, "NullReferenceException", 99u, "Player.cs")));
}

// ---- FormatSseEvent (text/event-stream wire framing) -----------------------

TEST(McpEventStreamSse, FramesIdAndSingleLineData)
{
    const Json n = MCP::MakeEventNotification(MakeEvent(DiagnosticEventCategory::Play, "Entered Play mode"));
    const std::string frame = MCP::FormatSseEvent(312u, n);

    // id line carries the monotonic event id (for Last-Event-ID resume).
    EXPECT_TRUE(frame.starts_with("id: 312\n")) << frame;
    // terminated by the mandatory blank line.
    EXPECT_TRUE(frame.ends_with("\n\n")) << frame;

    // Extract the data payload and confirm it is a SINGLE line (compact dump) — a raw
    // newline inside would split the SSE data field and corrupt the frame.
    const std::string marker = "data: ";
    const auto dataStart = frame.find(marker);
    ASSERT_NE(dataStart, std::string::npos);
    const auto payloadStart = dataStart + marker.size();
    const auto payloadEnd = frame.find("\n\n", payloadStart);
    ASSERT_NE(payloadEnd, std::string::npos);
    const std::string payload = frame.substr(payloadStart, payloadEnd - payloadStart);
    EXPECT_EQ(payload.find('\n'), std::string::npos) << "data field must be a single line";

    // The payload round-trips back to the original notification.
    EXPECT_EQ(Json::parse(payload), n);
}

TEST(McpEventStreamSse, CommentLineIsIgnorableHeartbeat)
{
    const std::string hb = MCP::FormatSseComment("keep-alive");
    EXPECT_EQ(hb, ": keep-alive\n\n");
    // An SSE comment starts with ':' so clients ignore it.
    EXPECT_TRUE(hb.starts_with(":"));
    EXPECT_TRUE(hb.ends_with("\n\n"));
}
