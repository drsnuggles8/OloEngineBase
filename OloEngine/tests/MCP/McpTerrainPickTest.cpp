#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpTerrainPick.h"

#include <limits>
#include <utility>

// OLO_TEST_LAYER: unit

namespace
{
    namespace Pick = OloEngine::MCP::TerrainPick;
    using Json = nlohmann::json;

    Json Pixel(f64 x = 20.0, f64 y = 30.0, i32 width = 640, i32 height = 480)
    {
        return Json{ { "viewportPixel", { { "coordinate", Json::array({ x, y }) }, { "width", width }, { "height", height } } } };
    }

    Json WorldRay(Json direction = Json::array({ 0.0, -2.0, 0.0 }))
    {
        return Json{ { "worldRay", { { "origin", Json::array({ 1.0, 2.0, 3.0 }) }, { "direction", std::move(direction) } } } };
    }
} // namespace

TEST(McpTerrainPick, ParsesEachRaySourceAndRequiresExactlyOne)
{
    Pick::Request request;
    ASSERT_FALSE(Pick::ParseRequest(Pixel(), request).has_value());
    EXPECT_EQ(request.Source, Pick::RaySource::ViewportPixel);
    EXPECT_EQ(request.ViewportDimensions, glm::uvec2(640u, 480u));

    ASSERT_FALSE(Pick::ParseRequest(Json{ { "viewportNormalized", Json::array({ 0.25, 0.75 }) } }, request).has_value());
    EXPECT_EQ(request.Source, Pick::RaySource::ViewportNormalized);
    EXPECT_EQ(request.Coordinate, glm::vec2(0.25f, 0.75f));

    ASSERT_FALSE(Pick::ParseRequest(WorldRay(), request).has_value());
    EXPECT_EQ(request.Source, Pick::RaySource::WorldRay);
    EXPECT_FLOAT_EQ(request.Ray.Direction.y, -1.0f);
    EXPECT_FLOAT_EQ(request.Ray.MaxDistance, 2000.0f);

    EXPECT_TRUE(Pick::ParseRequest(Json::object(), request).has_value());
    Json two = Pixel();
    two["viewportNormalized"] = Json::array({ 0.5, 0.5 });
    EXPECT_TRUE(Pick::ParseRequest(two, request).has_value());
}

TEST(McpTerrainPick, ValidatesViewportCoordinatesAndDimensions)
{
    Pick::Request request;
    EXPECT_TRUE(Pick::ParseRequest(Pixel(-1.0, 2.0), request).has_value());
    EXPECT_TRUE(Pick::ParseRequest(Pixel(640.0, 2.0), request).has_value());
    EXPECT_TRUE(Pick::ParseRequest(Pixel(2.0, 480.0), request).has_value());
    EXPECT_TRUE(Pick::ParseRequest(Pixel(2.0, 2.0, 0, 480), request).has_value());
    EXPECT_TRUE(Pick::ParseRequest(Pixel(2.0, 2.0, 640, -1), request).has_value());
    EXPECT_TRUE(Pick::ParseRequest(Json{ { "viewportNormalized", Json::array({ 1.01, 0.5 }) } }, request).has_value());
    EXPECT_FALSE(Pick::ParseRequest(Json{ { "viewportNormalized", Json::array({ 0.0, 1.0 }) } }, request).has_value());
}

TEST(McpTerrainPick, RejectsNonFiniteAndZeroWorldRays)
{
    Pick::Request request;
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();
    const f64 infinity = std::numeric_limits<f64>::infinity();
    EXPECT_TRUE(Pick::ParseRequest(WorldRay(Json::array({ 0.0, 0.0, 0.0 })), request).has_value());
    EXPECT_TRUE(Pick::ParseRequest(WorldRay(Json::array({ nan, -1.0, 0.0 })), request).has_value());

    Json badOrigin = WorldRay();
    badOrigin["worldRay"]["origin"][1] = infinity;
    EXPECT_TRUE(Pick::ParseRequest(badOrigin, request).has_value());

    Json badDistance = WorldRay();
    badDistance["worldRay"]["maxDistance"] = 0.0;
    EXPECT_TRUE(Pick::ParseRequest(badDistance, request).has_value());
    badDistance["worldRay"]["maxDistance"] = infinity;
    EXPECT_TRUE(Pick::ParseRequest(badDistance, request).has_value());
}

TEST(McpTerrainPick, PendingAndUnavailableAreNotShapedAsMisses)
{
    Pick::Snapshot pending;
    pending.State = Pick::Status::Pending;
    pending.RayId = 17u;
    pending.Input.Source = Pick::RaySource::ViewportNormalized;
    pending.Input.Coordinate = { 0.5f, 0.25f };
    pending.ResolvedWorldRay = Pick::WorldRay{ { 1.0f, 2.0f, 3.0f }, { 0.0f, -1.0f, 0.0f }, 2000.0f };
    const Json pendingJson = Pick::BuildResult(pending);
    EXPECT_EQ(pendingJson["status"], "pending");
    EXPECT_EQ(pendingJson["rayId"], 17u);
    EXPECT_TRUE(pendingJson.contains("worldRay"));
    EXPECT_FALSE(pendingJson.contains("hit"));

    Pick::Snapshot unavailable = pending;
    unavailable.State = Pick::Status::Unavailable;
    unavailable.UnavailableReason = "No built terrain is active.";
    const Json unavailableJson = Pick::BuildResult(unavailable);
    EXPECT_EQ(unavailableJson["status"], "unavailable");
    EXPECT_EQ(unavailableJson["reason"], "No built terrain is active.");
    EXPECT_FALSE(unavailableJson.contains("hit"));
}

TEST(McpTerrainPick, AnsweredHitAndMissExposeLatencyAndOptionalPositions)
{
    Pick::Snapshot hit;
    hit.State = Pick::Status::Answered;
    hit.RayId = 42u;
    hit.Input.Source = Pick::RaySource::WorldRay;
    hit.Input.Ray = Pick::WorldRay{ { 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 10.0f };
    hit.ResolvedWorldRay = hit.Input.Ray;
    hit.Hit = true;
    hit.WorldHit = glm::vec3(0.0f, 1.0f, 0.0f);
    hit.LocalHit = glm::vec3(4.0f, 1.0f, 7.0f);
    hit.LatencyFrames = 3u;
    const Json hitJson = Pick::BuildResult(hit);
    EXPECT_EQ(hitJson["status"], "answered");
    EXPECT_EQ(hitJson["hit"], true);
    EXPECT_EQ(hitJson["latencyFrames"], 3u);
    EXPECT_EQ(hitJson["worldHit"], Json::array({ 0.0f, 1.0f, 0.0f }));
    EXPECT_EQ(hitJson["localHit"], Json::array({ 4.0f, 1.0f, 7.0f }));

    Pick::Snapshot miss = hit;
    miss.Hit = false;
    miss.WorldHit.reset();
    miss.LocalHit.reset();
    const Json missJson = Pick::BuildResult(miss);
    EXPECT_EQ(missJson["hit"], false);
    EXPECT_FALSE(missJson.contains("worldHit"));
    EXPECT_FALSE(missJson.contains("localHit"));
}

TEST(McpTerrainPick, OverflowFlagsRemainVisibleOnAnAnsweredResult)
{
    Pick::Snapshot snapshot;
    snapshot.State = Pick::Status::Answered;
    snapshot.OverflowFlags = Pick::kOverflowNodes | Pick::kOverflowMarch;
    const Json result = Pick::BuildResult(snapshot);
    EXPECT_EQ(result["overflow"]["any"], true);
    EXPECT_EQ(result["overflow"]["nodes"], true);
    EXPECT_EQ(result["overflow"]["candidates"], false);
    EXPECT_EQ(result["overflow"]["march"], true);
    EXPECT_EQ(result["overflow"]["rawFlags"], 5u);
}

TEST(McpTerrainPick, InputSchemaNamesAllThreeExclusiveSources)
{
    const Json schema = Pick::InputSchema();
    EXPECT_EQ(schema["additionalProperties"], false);
    EXPECT_TRUE(schema["properties"].contains("viewportPixel"));
    EXPECT_TRUE(schema["properties"].contains("viewportNormalized"));
    EXPECT_TRUE(schema["properties"].contains("worldRay"));
}
