// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpLightmapBake.h"

#include <limits>

namespace
{
    namespace Bake = OloEngine::MCP::LightmapBake;
    using Json = Bake::Json;
} // namespace

TEST(McpLightmapBake, SchemasAreClosedAndExposeStableResponseFields)
{
    const Json input = Bake::InputSchema();
    EXPECT_EQ(input["additionalProperties"], false);
    EXPECT_EQ(input["required"], Json::array({ "mode" }));
    EXPECT_EQ(input["properties"]["mode"]["enum"], Json::array({ "start", "poll", "blocking" }));
    EXPECT_EQ(input["properties"]["operationId"]["minLength"], 1);

    const Json output = Bake::OutputSchema();
    EXPECT_FALSE(output.contains("additionalProperties"));
    EXPECT_EQ(output["required"],
              Json::array({ "operationId", "status", "progress", "error", "result" }));
    EXPECT_EQ(output["properties"]["progress"]["minimum"], 0);
    EXPECT_EQ(output["properties"]["progress"]["maximum"], 1);

    const Json& result = output["properties"]["result"]["oneOf"][0];
    EXPECT_EQ(result["additionalProperties"], false);
    EXPECT_EQ(result["required"],
              Json::array({ "bakedEntityCount", "skippedEntityCount", "saveRequested", "saved" }));
}

TEST(McpLightmapBake, ParsesAsyncPollAndBlockingModes)
{
    Bake::Request request;
    ASSERT_FALSE(Bake::ParseRequest(Json{ { "mode", "start" }, { "save", true } }, request).has_value());
    EXPECT_EQ(request.RequestMode, Bake::Mode::Start);
    EXPECT_TRUE(request.Save);
    EXPECT_TRUE(request.OperationId.empty());

    ASSERT_FALSE(Bake::ParseRequest(Json{ { "mode", "poll" }, { "operationId", "lightmap-bake-41" } }, request).has_value());
    EXPECT_EQ(request.RequestMode, Bake::Mode::Poll);
    EXPECT_EQ(request.OperationId, "lightmap-bake-41");
    EXPECT_FALSE(request.Save);

    ASSERT_FALSE(Bake::ParseRequest(Json{ { "mode", "blocking" } }, request).has_value());
    EXPECT_EQ(request.RequestMode, Bake::Mode::Blocking);
    EXPECT_FALSE(request.Save);
    EXPECT_TRUE(request.OperationId.empty());
}

TEST(McpLightmapBake, RejectsUnknownAndModeSpecificArguments)
{
    Bake::Request request;
    EXPECT_TRUE(Bake::ParseRequest(Json::array(), request).has_value());
    EXPECT_TRUE(Bake::ParseRequest(Json{ { "mode", "start" }, { "extra", 1 } }, request).has_value());
    EXPECT_TRUE(Bake::ParseRequest(Json{ { "mode", "poll" } }, request).has_value());
    EXPECT_TRUE(Bake::ParseRequest(Json{ { "mode", "poll" }, { "operationId", "op" }, { "save", false } }, request).has_value());
    EXPECT_TRUE(Bake::ParseRequest(Json{ { "mode", "blocking" }, { "operationId", "op" } }, request).has_value());
    EXPECT_TRUE(Bake::ParseRequest(Json{ { "mode", "start" }, { "save", 1 } }, request).has_value());
    EXPECT_TRUE(Bake::ParseRequest(Json{ { "mode", "poll" }, { "operationId", "" } }, request).has_value());
}

TEST(McpLightmapBake, OperationIdsAreDeterministicAndStable)
{
    EXPECT_EQ(Bake::MakeOperationId(0), "lightmap-bake-0");
    EXPECT_EQ(Bake::MakeOperationId(42), "lightmap-bake-42");
    EXPECT_EQ(Bake::MakeOperationId(42), Bake::MakeOperationId(42));
}

TEST(McpLightmapBake, ShapesRunningAndCompletedResponsesDeterministically)
{
    const Json running = Bake::BuildResponse(Bake::Snapshot{ .OperationId = "lightmap-bake-7",
                                                             .State = Bake::Status::Running,
                                                             .Progress = 0.25 });
    EXPECT_EQ(running,
              (Json{ { "operationId", "lightmap-bake-7" },
                     { "status", "running" },
                     { "progress", 0.25 },
                     { "error", nullptr },
                     { "result", nullptr } }));

    const Bake::Result result{ .BakedEntityCount = 3,
                               .SkippedEntityCount = 1,
                               .SaveRequested = true,
                               .Saved = true };
    const Json succeeded = Bake::BuildResponse(Bake::Snapshot{ .OperationId = "lightmap-bake-7",
                                                               .State = Bake::Status::Succeeded,
                                                               .Progress = 0.8,
                                                               .CompletedResult = result });
    EXPECT_EQ(succeeded["status"], "succeeded");
    EXPECT_EQ(succeeded["progress"], 1.0);
    EXPECT_TRUE(succeeded["error"].is_null());
    EXPECT_EQ(succeeded["result"],
              (Json{ { "bakedEntityCount", 3 },
                     { "skippedEntityCount", 1 },
                     { "saveRequested", true },
                     { "saved", true } }));
}

TEST(McpLightmapBake, ShapesFailuresAndSanitizesProgress)
{
    const Json failed = Bake::BuildResponse(Bake::Snapshot{ .OperationId = "lightmap-bake-9",
                                                            .State = Bake::Status::Failed,
                                                            .Progress = std::numeric_limits<double>::quiet_NaN(),
                                                            .Error = "Atlas packing failed",
                                                            .CompletedResult = Bake::Result{} });
    EXPECT_EQ(failed["status"], "failed");
    EXPECT_EQ(failed["progress"], 0.0);
    EXPECT_EQ(failed["error"], "Atlas packing failed");
    EXPECT_TRUE(failed["result"].is_null());

    const Json clamped = Bake::BuildResponse(Bake::Snapshot{ .OperationId = "lightmap-bake-10",
                                                             .State = Bake::Status::Queued,
                                                             .Progress = 2.0 });
    EXPECT_EQ(clamped["progress"], 1.0);
}
