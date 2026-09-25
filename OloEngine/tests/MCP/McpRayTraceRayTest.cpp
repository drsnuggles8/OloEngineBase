#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpRayTraceRayTest — unit test (headless, no GL, no live editor, no device).
//
// Pins the request parsing and result shaping behind `olo_rt_trace_ray`
// (issue #607). The trace itself is Vulkan-only and its correctness is pinned
// on a device by RayTracingDevice.DeterministicRaysReportMissClosestHitTransform-
// AndAlpha (#978); what is pinnable HERE is everything a caller can observe
// without a GPU, which is exactly the part that decides whether a real answer
// can be read correctly.
//
// The three invariants this file exists for:
//
//   1. A MALFORMED RAY IS REFUSED, NOT TRACED. `0 <= tMin <= tMax`, a finite
//      origin/direction and a non-zero direction are SPEC REQUIREMENTS of
//      rayQueryInitializeEXT — violating one is undefined behaviour on the
//      device, not a miss. A tool that forwarded such a ray would produce an
//      answer that means nothing, or a device fault.
//
//   2. A MISS IS AN ANSWER. It is reported as an entry with `hit:false` and the
//      ray echoed beside it. Dropping missed rays from the array would make a
//      miss indistinguishable from a ray that was never traced — and the miss
//      case is the whole reason this tool is trustworthy.
//
//   3. `status` KEEPS THE THREE NOTHING-CAME-BACK STATES APART. "this device
//      cannot trace" (unavailable, with a reason), "not back yet" (pending) and
//      "traced, everything missed" (answered, hitCount 0) are the same bytes if
//      flattened, and need opposite fixes.
//
// Plus one arithmetic invariant: the reply echoes the NORMALIZED direction, so
// `position == origin + direction * distance` is checkable by the caller
// against the numbers it was given rather than against the numbers it sent.
// =============================================================================

#include "MCP/McpRayTraceRay.h"

#include <algorithm>
#include <limits>
#include <string>

// OLO_TEST_LAYER: unit

namespace
{
    namespace RTR = OloEngine::MCP::RayTraceRay;
    using Json = nlohmann::json;

    [[nodiscard]] Json OneRay(const Json& overrides = Json::object())
    {
        Json ray{ { "origin", { 0.0, 0.0, 5.0 } }, { "direction", { 0.0, 0.0, -1.0 } } };
        for (auto it = overrides.begin(); it != overrides.end(); ++it)
            ray[it.key()] = it.value();
        return Json{ { "rays", Json::array({ ray }) } };
    }

    [[nodiscard]] std::string ParseError(const Json& args)
    {
        RTR::Request request;
        const auto error = RTR::ParseRequest(args, request);
        EXPECT_TRUE(error.has_value()) << "expected a rejection for: " << args.dump();
        return error.value_or("");
    }

    [[nodiscard]] RTR::Request ParseOk(const Json& args)
    {
        RTR::Request request;
        const auto error = RTR::ParseRequest(args, request);
        EXPECT_FALSE(error.has_value()) << "expected acceptance, got: " << error.value_or("");
        return request;
    }

    // ---- parsing ---------------------------------------------------------

    TEST(McpRayTraceRay, ParsesAMinimalRayAndDefaultsTheRest)
    {
        const RTR::Request request = ParseOk(OneRay());
        ASSERT_EQ(request.Rays.size(), 1u);
        EXPECT_FLOAT_EQ(request.Rays[0].Origin.z, 5.0f);
        EXPECT_FLOAT_EQ(request.Rays[0].TMin, 0.0f);
        EXPECT_FLOAT_EQ(request.Rays[0].TMax, 1000.0f);
        EXPECT_FALSE(request.CullBackFaces);
        EXPECT_FALSE(request.TerminateOnFirstHit);
        EXPECT_EQ(request.InstanceMask, 255u);
    }

    TEST(McpRayTraceRay, TheDirectionIsNormalizedSoDistanceIsAWorldDistance)
    {
        // A direction of length 5 would make the GPU's `t` a multiple of 5, and
        // the reported position would still be right while the distance was not.
        const RTR::Request request = ParseOk(OneRay(Json{ { "direction", { 0.0, 0.0, -5.0 } } }));
        ASSERT_EQ(request.Rays.size(), 1u);
        EXPECT_NEAR(glm::length(request.Rays[0].Direction), 1.0f, 1e-6f);
        EXPECT_NEAR(request.Rays[0].Direction.z, -1.0f, 1e-6f);
    }

    TEST(McpRayTraceRay, RaysIsRequiredAndMustBeANonEmptyArray)
    {
        EXPECT_NE(ParseError(Json::object()).find("rays"), std::string::npos);
        EXPECT_FALSE(ParseError(Json{ { "rays", Json::array() } }).empty());
        EXPECT_FALSE(ParseError(Json{ { "rays", "nope" } }).empty());
    }

    TEST(McpRayTraceRay, TheBatchCapIsEnforcedRatherThanTruncated)
    {
        // Truncating would report the dropped rays as misses, which is the one
        // answer this tool must never invent.
        Json rays = Json::array();
        for (u32 i = 0; i <= RTR::kMaxRays; ++i)
            rays.push_back(Json{ { "origin", { 0.0, 0.0, 5.0 } }, { "direction", { 0.0, 0.0, -1.0 } } });
        const std::string error = ParseError(Json{ { "rays", rays } });
        EXPECT_NE(error.find(std::to_string(RTR::kMaxRays)), std::string::npos) << error;
    }

    TEST(McpRayTraceRay, ANonFiniteOrZeroLengthRayIsRefusedNotTraced)
    {
        // rayQueryInitializeEXT requires finite inputs; a NaN there is
        // undefined behaviour on the device, not a miss.
        const f64 nan = std::numeric_limits<f64>::quiet_NaN();
        EXPECT_FALSE(ParseError(OneRay(Json{ { "origin", { nan, 0.0, 0.0 } } })).empty());
        EXPECT_FALSE(ParseError(OneRay(Json{ { "direction", { 0.0, 0.0, nan } } })).empty());
        EXPECT_FALSE(ParseError(OneRay(Json{ { "direction", { 0.0, 0.0, 0.0 } } })).empty());
        EXPECT_FALSE(ParseError(OneRay(Json{ { "tMax", nan } })).empty());
        EXPECT_FALSE(ParseError(OneRay(Json{ { "origin", { 0.0, 0.0 } } })).empty());
    }

    TEST(McpRayTraceRay, TheRayIntervalMustSatisfyZeroLessEqualTMinLessEqualTMax)
    {
        EXPECT_NE(ParseError(OneRay(Json{ { "tMin", -1.0 } })).find("tMin"), std::string::npos);
        EXPECT_NE(ParseError(OneRay(Json{ { "tMin", 10.0 }, { "tMax", 1.0 } })).find("tMin"), std::string::npos);
        // The boundary is legal: a degenerate but well-formed interval.
        const RTR::Request request = ParseOk(OneRay(Json{ { "tMin", 2.0 }, { "tMax", 2.0 } }));
        EXPECT_FLOAT_EQ(request.Rays[0].TMin, 2.0f);
    }

    TEST(McpRayTraceRay, AZeroInstanceMaskIsRefusedBecauseEveryRayWouldMiss)
    {
        // Honouring 0 would make every instance unhittable, so every ray would
        // come back a miss for a reason that has nothing to do with the scene —
        // the least debuggable possible answer.
        const std::string error = ParseError(Json{ { "rays", OneRay()["rays"] }, { "instanceMask", 0 } });
        EXPECT_NE(error.find("instanceMask"), std::string::npos) << error;
        EXPECT_FALSE(ParseError(Json{ { "rays", OneRay()["rays"] }, { "instanceMask", 256 } }).empty());
        const RTR::Request ok = ParseOk(Json{ { "rays", OneRay()["rays"] }, { "instanceMask", 1 } });
        EXPECT_EQ(ok.InstanceMask, 1u);
    }

    TEST(McpRayTraceRay, FlagsParseAndAreTypeChecked)
    {
        const RTR::Request request = ParseOk(Json{ { "rays", OneRay()["rays"] },
                                                   { "cullBackFaces", true },
                                                   { "terminateOnFirstHit", true } });
        EXPECT_TRUE(request.CullBackFaces);
        EXPECT_TRUE(request.TerminateOnFirstHit);
        EXPECT_FALSE(ParseError(Json{ { "rays", OneRay()["rays"] }, { "cullBackFaces", "yes" } }).empty());
    }

    // ---- shaping ---------------------------------------------------------

    [[nodiscard]] RTR::Snapshot TwoRaySnapshot()
    {
        RTR::Snapshot snapshot;
        snapshot.State = RTR::Status::Answered;
        snapshot.BatchId = 7;
        snapshot.LatencyFrames = 2;
        snapshot.Input.Rays = {
            RTR::Ray{ glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.001f, 100.0f },
            RTR::Ray{ glm::vec3(50.0f, 50.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.001f, 100.0f },
        };
        RTR::Hit hit;
        hit.IsHit = true;
        hit.Distance = 4.0f;
        hit.Position = glm::vec3(0.0f, 0.0f, 1.0f);
        hit.Barycentrics = glm::vec2(0.25f, 0.25f);
        hit.InstanceSlot = 3;
        hit.PrimitiveIndex = 11;
        hit.MaterialSlot = 2;
        hit.GeometrySlot = 1;
        hit.UV = glm::vec2(0.25f, 0.25f);
        hit.WorldNormal = glm::vec3(0.0f, 0.0f, 1.0f);
        hit.WindingSign = -1.0f;
        snapshot.Hits = { hit, RTR::Hit{} }; // ray 1 missed
        return snapshot;
    }

    TEST(McpRayTraceRay, AMissIsAnEntryWithHitFalseNotAnAbsentOne)
    {
        const Json result = RTR::BuildResult(TwoRaySnapshot());
        ASSERT_TRUE(result.contains("rays"));
        ASSERT_EQ(result["rays"].size(), 2u) << "a missed ray must still have an entry: " << result.dump(2);
        EXPECT_TRUE(result["rays"][0]["hit"].get<bool>());
        EXPECT_FALSE(result["rays"][1]["hit"].get<bool>());
        // The ray is echoed beside the miss, so the caller can see WHAT missed.
        EXPECT_TRUE(result["rays"][1].contains("ray"));
        EXPECT_EQ(result["hitCount"].get<u32>(), 1u);
        EXPECT_EQ(result["missCount"].get<u32>(), 1u);
        EXPECT_EQ(result["rayCount"].get<u32>(), 2u);
    }

    TEST(McpRayTraceRay, AMissCarriesNoHitFieldsAtAll)
    {
        // A miss reporting distance 0 and instanceSlot 0 would read as a hit on
        // instance 0 at the ray origin to anything that checked the fields
        // before the flag.
        const Json miss = RTR::BuildResult(TwoRaySnapshot())["rays"][1];
        for (const char* key : { "distance", "position", "barycentrics", "instanceSlot", "primitiveIndex",
                                 "materialSlot", "geometrySlot", "uv", "worldNormal", "windingSign" })
        {
            EXPECT_FALSE(miss.contains(key)) << "a miss must not carry '" << key << "': " << miss.dump(2);
        }
    }

    TEST(McpRayTraceRay, AllThreeBarycentricsAreReportedAndSumToOne)
    {
        // The GPU reports (b1, b2); b0 = 1 - b1 - b2. Making the caller do that
        // subtraction is how a vertex order gets misread.
        const Json hit = RTR::BuildResult(TwoRaySnapshot())["rays"][0];
        ASSERT_EQ(hit["barycentrics"].size(), 3u);
        const f32 b0 = hit["barycentrics"][0].get<f32>();
        const f32 b1 = hit["barycentrics"][1].get<f32>();
        const f32 b2 = hit["barycentrics"][2].get<f32>();
        EXPECT_NEAR(b0 + b1 + b2, 1.0f, 1e-5f);
        EXPECT_NEAR(b1, 0.25f, 1e-6f);
        EXPECT_NEAR(b2, 0.25f, 1e-6f);
    }

    TEST(McpRayTraceRay, AHitCarriesEverySlotTheCallerNeedsToCrossCheckTheScene)
    {
        const Json hit = RTR::BuildResult(TwoRaySnapshot())["rays"][0];
        EXPECT_EQ(hit["instanceSlot"].get<u32>(), 3u);
        EXPECT_EQ(hit["primitiveIndex"].get<u32>(), 11u);
        EXPECT_EQ(hit["materialSlot"].get<u32>(), 2u);
        EXPECT_EQ(hit["geometrySlot"].get<u32>(), 1u);
        EXPECT_NEAR(hit["distance"].get<f32>(), 4.0f, 1e-6f);
        // A mirrored instance basis is reported, not silently normalised away.
        EXPECT_NEAR(hit["windingSign"].get<f32>(), -1.0f, 1e-6f);
    }

    TEST(McpRayTraceRay, PositionAgreesWithTheEchoedRayAndDistance)
    {
        // The caller checks the arithmetic against the numbers it was GIVEN,
        // which is only possible because the echoed direction is the normalized
        // one the trace actually used.
        const Json hit = RTR::BuildResult(TwoRaySnapshot())["rays"][0];
        const f32 distance = hit["distance"].get<f32>();
        for (int axis = 0; axis < 3; ++axis)
        {
            const f32 expected =
                hit["ray"]["origin"][axis].get<f32>() + hit["ray"]["direction"][axis].get<f32>() * distance;
            EXPECT_NEAR(hit["position"][axis].get<f32>(), expected, 1e-5f) << "axis " << axis;
        }
    }

    TEST(McpRayTraceRay, UnavailableCarriesAReasonAndNoRays)
    {
        RTR::Snapshot snapshot;
        snapshot.State = RTR::Status::Unavailable;
        snapshot.UnavailableReason = "This device/backend has no hardware ray tracing.";
        snapshot.Input.Rays = { RTR::Ray{} };
        const Json result = RTR::BuildResult(snapshot);
        EXPECT_EQ(result["status"].get<std::string>(), "unavailable");
        EXPECT_NE(result["reason"].get<std::string>().find("ray tracing"), std::string::npos);
        // No hits array at all: an empty one would read as "traced, all missed".
        EXPECT_FALSE(result.contains("rays"));
        EXPECT_FALSE(result.contains("hitCount"));
    }

    TEST(McpRayTraceRay, UnavailableAlwaysHasSomeReasonEvenIfNobodySetOne)
    {
        RTR::Snapshot snapshot;
        snapshot.State = RTR::Status::Unavailable;
        const Json result = RTR::BuildResult(snapshot);
        EXPECT_FALSE(result["reason"].get<std::string>().empty());
    }

    TEST(McpRayTraceRay, PendingIsDistinctFromAnsweredWithZeroHits)
    {
        RTR::Snapshot pending;
        pending.State = RTR::Status::Pending;
        pending.BatchId = 4;
        pending.Input.Rays = { RTR::Ray{} };
        const Json pendingJson = RTR::BuildResult(pending);
        EXPECT_EQ(pendingJson["status"].get<std::string>(), "pending");
        EXPECT_FALSE(pendingJson.contains("rays"));
        EXPECT_FALSE(pendingJson.contains("hitCount"));

        RTR::Snapshot allMissed;
        allMissed.State = RTR::Status::Answered;
        allMissed.BatchId = 4;
        allMissed.Input.Rays = { RTR::Ray{} };
        allMissed.Hits = { RTR::Hit{} };
        const Json answeredJson = RTR::BuildResult(allMissed);
        EXPECT_EQ(answeredJson["status"].get<std::string>(), "answered");
        EXPECT_EQ(answeredJson["hitCount"].get<u32>(), 0u);
        EXPECT_EQ(answeredJson["rays"].size(), 1u);
    }

    TEST(McpRayTraceRay, TheBatchIdAndFlagsAreEchoedInEveryStatus)
    {
        // The batch id is how a caller knows the hits belong to ITS rays rather
        // than to a trace the ring retired in between.
        for (const RTR::Status state : { RTR::Status::Unavailable, RTR::Status::Pending, RTR::Status::Answered })
        {
            RTR::Snapshot snapshot;
            snapshot.State = state;
            snapshot.BatchId = 99;
            snapshot.Input.InstanceMask = 2;
            snapshot.Input.TerminateOnFirstHit = true;
            const Json result = RTR::BuildResult(snapshot);
            EXPECT_EQ(result["batchId"].get<u32>(), 99u);
            EXPECT_EQ(result["flags"]["instanceMask"].get<u32>(), 2u);
            EXPECT_TRUE(result["flags"]["terminateOnFirstHit"].get<bool>());
        }
    }

    TEST(McpRayTraceRay, AShortHitsArrayIsReadAsMissesRatherThanOutOfBounds)
    {
        // Defensive: the probe always fills one hit per ray, but this shaping
        // runs on data that crossed a GPU readback, and an indexed read past
        // the end would be a crash in a diagnostic tool.
        RTR::Snapshot snapshot;
        snapshot.State = RTR::Status::Answered;
        snapshot.Input.Rays = { RTR::Ray{}, RTR::Ray{}, RTR::Ray{} };
        snapshot.Hits = { RTR::Hit{} };
        const Json result = RTR::BuildResult(snapshot);
        ASSERT_EQ(result["rays"].size(), 3u);
        EXPECT_EQ(result["missCount"].get<u32>(), 3u);
    }

    // ---- camera ray sources (#607) ----------------------------------------
    //
    // A camera ray is not deterministic — it moves with the pose — so the
    // sources never mix, every reply names its source, and a camera reply
    // carries the resolved world ray as `replay` so it can be re-traced with the
    // camera out of the loop.

    TEST(McpRayTraceRay, AViewportSourceParsesToOneUnresolvedCameraRay)
    {
        const RTR::Request pixel = ParseOk(Json{
            { "viewportPixel", { { "coordinate", { 320.0, 180.0 } }, { "width", 640 }, { "height", 360 } } },
            { "cullBackFaces", true } });
        EXPECT_EQ(pixel.Source, OloEngine::MCP::ViewportRay::RaySource::ViewportPixel);
        EXPECT_TRUE(pixel.Rays.empty()) << "the handler resolves the ray through the camera, not the parser";
        EXPECT_TRUE(pixel.CullBackFaces) << "flags still apply to a camera ray";

        const RTR::Request normalized = ParseOk(Json{ { "viewportNormalized", { 0.25, 0.75 } } });
        EXPECT_EQ(normalized.Source, OloEngine::MCP::ViewportRay::RaySource::ViewportNormalized);

        EXPECT_EQ(ParseOk(OneRay()).Source, OloEngine::MCP::ViewportRay::RaySource::WorldRay);
    }

    TEST(McpRayTraceRay, WorldAndCameraRaysNeverShareARequest)
    {
        Json mixed = OneRay();
        mixed["viewportNormalized"] = Json::array({ 0.5, 0.5 });
        const std::string error = ParseError(mixed);
        EXPECT_NE(error.find("exactly one"), std::string::npos) << error;

        Json both{ { "viewportNormalized", { 0.5, 0.5 } },
                   { "viewportPixel", { { "coordinate", { 1.0, 1.0 } }, { "width", 4 }, { "height", 4 } } } };
        EXPECT_FALSE(ParseError(both).empty());
    }

    TEST(McpRayTraceRay, EveryReplyNamesItsRaySource)
    {
        for (const RTR::Status state : { RTR::Status::Unavailable, RTR::Status::Pending, RTR::Status::Answered })
        {
            RTR::Snapshot world;
            world.State = state;
            EXPECT_EQ(RTR::BuildResult(world)["raySource"], "worldRay");
            EXPECT_FALSE(RTR::BuildResult(world).contains("replay")) << "a world ray is its own replay";

            RTR::Snapshot camera;
            camera.State = state;
            camera.Input = ParseOk(Json{ { "viewportNormalized", { 0.5, 0.5 } } });
            EXPECT_EQ(RTR::BuildResult(camera)["raySource"], "viewportNormalized");
            EXPECT_EQ(RTR::BuildResult(camera)["viewport"]["source"], "viewportNormalized");
        }
    }

    TEST(McpRayTraceRay, ACameraReplyCarriesTheResolvedRayAsReadyToSendArgs)
    {
        RTR::Snapshot snapshot;
        snapshot.State = RTR::Status::Answered;
        snapshot.Input = ParseOk(Json{ { "viewportNormalized", { 0.5, 0.5 } }, { "instanceMask", 3 } });
        snapshot.Input.Rays = { RTR::Ray{ { 0.0f, 1.0f, 9.9f }, { 0.0f, 0.0f, -1.0f }, 0.0f, 99.9f } };
        RTR::Hit hit;
        hit.IsHit = true;
        hit.Distance = 9.9f;
        hit.InstanceSlot = 4;
        snapshot.Hits = { hit };

        const Json result = RTR::BuildResult(snapshot);
        ASSERT_TRUE(result.contains("replay")) << result.dump(2);
        const Json& replay = result["replay"];
        // The replay IS the traced ray: the same bytes the reply echoes per entry.
        ASSERT_EQ(replay["rays"].size(), 1u);
        EXPECT_EQ(replay["rays"][0], result["rays"][0]["ray"]);
        EXPECT_EQ(replay["instanceMask"].get<u32>(), 3u) << "the flags travel with the replay";

        // Pasting it back is a valid world-ray request, and parses to the same ray.
        const RTR::Request replayed = ParseOk(replay);
        EXPECT_EQ(replayed.Source, OloEngine::MCP::ViewportRay::RaySource::WorldRay);
        ASSERT_EQ(replayed.Rays.size(), 1u);
        EXPECT_FLOAT_EQ(replayed.Rays[0].Origin.z, 9.9f);
        EXPECT_FLOAT_EQ(replayed.Rays[0].TMax, 99.9f);
        EXPECT_EQ(replayed.InstanceMask, 3u);
    }

    TEST(McpRayTraceRay, AnUnavailableCameraReplyStillNamesTheRayItWouldHaveTraced)
    {
        RTR::Snapshot resolved;
        resolved.State = RTR::Status::Unavailable;
        resolved.UnavailableReason = "no hardware ray tracing";
        resolved.Input = ParseOk(Json{ { "viewportNormalized", { 0.5, 0.5 } } });
        resolved.Input.Rays = { RTR::Ray{} };
        EXPECT_TRUE(RTR::BuildResult(resolved).contains("replay"));

        // Refused before resolution (Play mode): no ray, so no replay to offer.
        RTR::Snapshot refused = resolved;
        refused.Input.Rays.clear();
        const Json result = RTR::BuildResult(refused);
        EXPECT_FALSE(result.contains("replay"));
        EXPECT_EQ(result["reason"], "no hardware ray tracing");
    }

    TEST(McpRayTraceRay, TheSchemasAdvertiseTheSameCapTheParserEnforces)
    {
        const Json schema = RTR::InputSchema();
        EXPECT_EQ(schema["properties"]["rays"]["maxItems"].get<u32>(), RTR::kMaxRays);
        EXPECT_EQ(schema["properties"]["rays"]["minItems"].get<u32>(), 1u);
        // instanceMask 0 is out of the advertised range as well as refused by
        // the parser — the schema and the guard must not disagree.
        EXPECT_EQ(schema["properties"]["instanceMask"]["minimum"].get<u32>(), 1u);
        EXPECT_EQ(schema["properties"]["instanceMask"]["maximum"].get<u32>(), 255u);

        // 'rays' is one of three exclusive sources now, so the schema gate must
        // not require it — or a viewport request is refused before the parser runs.
        EXPECT_TRUE(schema["properties"].contains("viewportPixel"));
        EXPECT_TRUE(schema["properties"].contains("viewportNormalized"));
        if (schema.contains("required"))
        {
            for (const Json& key : schema["required"])
                EXPECT_NE(key, "rays");
        }
        const Json output = RTR::OutputSchema();
        EXPECT_NE(std::find(output["required"].begin(), output["required"].end(), "raySource"), output["required"].end());
    }
} // namespace
