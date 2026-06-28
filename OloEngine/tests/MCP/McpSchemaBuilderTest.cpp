// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the MCP JSON-Schema builder DSL (issue #357 P4). The builder
// (MCP/McpSchemaBuilder.h) is header-only and renderer/editor-free, so it is
// exercised here directly with no editor / socket / agent — the same pattern as
// McpEventStream / McpGoldenCompare / McpRenderExplain (RegisterBuiltinTools, which
// pulls the whole renderer/physics stack, is deliberately NOT linked into the test
// binary). Two things are pinned:
//   1. The primitive shapes each factory/modifier emits.
//   2. Equivalence: for a representative cross-section of the migrated tools, the
//      builder reproduces the OLD hand-written `nlohmann::json` literal *byte for
//      byte* on the wire. Each case copies the original literal verbatim and asserts
//      the builder output is `==` to it AND `.dump()`-identical, so the schema MCP
//      clients receive across the migration is provably unchanged.
#include "MCP/McpSchemaBuilder.h"

#include <nlohmann/json.hpp>

#include <string>

namespace
{
    namespace Schema = OloEngine::MCP::Schema;
    using Json = nlohmann::json;

    // Assert two schemas are equal both as JSON values and on the wire. The
    // value-equality catches a structural difference; the dump-equality additionally
    // catches an int-vs-float divergence (json(1) == json(1.0), but "1" != "1.0"),
    // which is exactly the byte-identity guarantee the migration must hold.
    void ExpectSameSchema(const Json& builder, const Json& literal)
    {
        EXPECT_EQ(builder, literal);
        EXPECT_EQ(builder.dump(), literal.dump());
    }
} // namespace

// ---- primitive factories / modifiers ---------------------------------------

TEST(McpSchemaBuilder, ScalarPrimitives)
{
    ExpectSameSchema(Schema::Int(), Json{ { "type", "integer" } });
    ExpectSameSchema(Schema::Number(), Json{ { "type", "number" } });
    ExpectSameSchema(Schema::Bool(), Json{ { "type", "boolean" } });
    ExpectSameSchema(Schema::String(), Json{ { "type", "string" } });
    ExpectSameSchema(Schema::Array(), Json{ { "type", "array" } });
}

TEST(McpSchemaBuilder, IntegerBoundsStayIntegralOnTheWire)
{
    // The migration's central wire-identity risk: a bound stored as a float would
    // dump "1.0" where the literal dumps "1". Min/Max take i64, so it stays "1".
    const Json built = Schema::Int().Min(1).Max(200);
    EXPECT_NE(built.dump().find("\"minimum\":1"), std::string::npos);
    EXPECT_EQ(built.dump().find("\"minimum\":1.0"), std::string::npos);
    EXPECT_TRUE(built["minimum"].is_number_integer());
    EXPECT_TRUE(built["maximum"].is_number_integer());
}

TEST(McpSchemaBuilder, StringEnumAndDescription)
{
    ExpectSameSchema(Schema::String().Enum({ "a", "b", "c" }).Desc("pick one"),
                     Json{ { "type", "string" }, { "enum", Json::array({ "a", "b", "c" }) }, { "description", "pick one" } });
}

TEST(McpSchemaBuilder, ArrayWithItems)
{
    ExpectSameSchema(Schema::Array(Schema::Number()),
                     Json{ { "type", "array" }, { "items", Json{ { "type", "number" } } } });
}

TEST(McpSchemaBuilder, Vec3Helper)
{
    ExpectSameSchema(Schema::Vec3("xyz"),
                     Json{ { "type", "array" }, { "items", Json{ { "type", "number" } } }, { "minItems", 3 }, { "maxItems", 3 }, { "description", "xyz" } });
}

TEST(McpSchemaBuilder, EntityIdHelperDefaultDescription)
{
    ExpectSameSchema(Schema::EntityId(),
                     Json{ { "type", "string" }, { "description", "Entity UUID (as a string; also accepts a number)." } });
}

TEST(McpSchemaBuilder, EmptyObjectIsAClosedNoArgSchema)
{
    ExpectSameSchema(Schema::EmptyObject(),
                     Json{ { "type", "object" }, { "properties", Json::object() }, { "additionalProperties", false } });
}

TEST(McpSchemaBuilder, OpaqueObjectKeepsNoPropertiesKey)
{
    // An object described only by Desc() (the screenshot 'camera'/'orbit' sub-args)
    // must NOT acquire a `properties` key — only Prop() creates one.
    ExpectSameSchema(Schema::Object().Desc("free-form"),
                     Json{ { "type", "object" }, { "description", "free-form" } });
}

TEST(McpSchemaBuilder, NestedObjectOmitsAdditionalPropertiesUntilClosed)
{
    ExpectSameSchema(Schema::Object().Prop("id", Schema::String()),
                     Json{ { "type", "object" }, { "properties", { { "id", { { "type", "string" } } } } } });
}

// ---- equivalence with the migrated production literals ----------------------
//
// Each `literal` below is the exact hand-written schema McpTools.cpp carried before
// P4; each `built` is the builder expression that replaced it. They must match.

TEST(McpSchemaBuilderEquivalence, LogTailInput)
{
    const Json built = Schema::Object()
                           .Prop("count", Schema::Int().Min(1).Max(200).Desc("How many of the most recent matching log lines to return (default 50)."))
                           .Prop("minLevel", Schema::String().Enum({ "trace", "debug", "info", "warn", "error", "critical" }).Desc("Only return lines at this severity or higher."))
                           .Prop("tag", Schema::String().Desc("Only return lines whose [Tag] matches exactly (e.g. Physics, Scene, Script)."))
                           .NoAdditional();
    const Json literal = Json{
        { "type", "object" },
        { "properties",
          { { "count",
              { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 }, { "description", "How many of the most recent matching log lines to return (default 50)." } } },
            { "minLevel",
              { { "type", "string" }, { "enum", Json::array({ "trace", "debug", "info", "warn", "error", "critical" }) }, { "description", "Only return lines at this severity or higher." } } },
            { "tag",
              { { "type", "string" },
                { "description", "Only return lines whose [Tag] matches exactly (e.g. Physics, Scene, Script)." } } } } },
        { "additionalProperties", false }
    };
    ExpectSameSchema(built, literal);
}

TEST(McpSchemaBuilderEquivalence, SceneSummaryOutput)
{
    const Json built = Schema::Object()
                           .Prop("hasActiveScene", Schema::Bool().Desc("Whether a scene is currently loaded."))
                           .Prop("isPlaying", Schema::Bool().Desc("Whether the game is in Play mode."))
                           .Prop("name", Schema::String().Desc("Active scene name (only when a scene is loaded)."))
                           .Prop("isPaused", Schema::Bool().Desc("Whether the playing scene is paused (only when a scene is loaded)."))
                           .Prop("entityCount", Schema::Int().Min(0).Desc("Total entity count (only when a scene is loaded)."))
                           .Required({ "hasActiveScene", "isPlaying" });
    const Json literal = Json{
        { "type", "object" },
        { "properties",
          { { "hasActiveScene", { { "type", "boolean" }, { "description", "Whether a scene is currently loaded." } } },
            { "isPlaying", { { "type", "boolean" }, { "description", "Whether the game is in Play mode." } } },
            { "name", { { "type", "string" }, { "description", "Active scene name (only when a scene is loaded)." } } },
            { "isPaused", { { "type", "boolean" }, { "description", "Whether the playing scene is paused (only when a scene is loaded)." } } },
            { "entityCount", { { "type", "integer" }, { "minimum", 0 }, { "description", "Total entity count (only when a scene is loaded)." } } } } },
        { "required", Json::array({ "hasActiveScene", "isPlaying" }) }
    };
    ExpectSameSchema(built, literal);
}

TEST(McpSchemaBuilderEquivalence, SceneListEntitiesInputPagination)
{
    const Json built = Schema::Object()
                           .Prop("namePattern", Schema::String().Desc("Case-sensitive substring to match against entity names."))
                           .Pagination("Entities per page (default 50, max 200).")
                           .NoAdditional();
    const Json literal = Json{
        { "type", "object" },
        { "properties",
          { { "namePattern", { { "type", "string" }, { "description", "Case-sensitive substring to match against entity names." } } },
            { "page", { { "type", "integer" }, { "minimum", 0 }, { "description", "Zero-based page index (default 0)." } } },
            { "pageSize", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 }, { "description", "Entities per page (default 50, max 200)." } } } } },
        { "additionalProperties", false }
    };
    ExpectSameSchema(built, literal);
}

TEST(McpSchemaBuilderEquivalence, SceneGetEntityInputAndOutput)
{
    const Json builtIn = Schema::Object().Prop("id", Schema::EntityId()).Required({ "id" }).NoAdditional();
    const Json literalIn = Json{
        { "type", "object" },
        { "properties",
          { { "id", { { "type", "string" }, { "description", "Entity UUID (as a string; also accepts a number)." } } } } },
        { "required", Json::array({ "id" }) },
        { "additionalProperties", false }
    };
    ExpectSameSchema(builtIn, literalIn);

    const Json builtOut = Schema::Object()
                              .Prop("found", Schema::Bool().Desc("True when the entity exists (a miss is returned as isError instead)."))
                              .Prop("id", Schema::String().Desc("Entity UUID."))
                              .Prop("name", Schema::String().Desc("Entity tag/name (empty when it has no TagComponent)."))
                              .Prop("parent", Schema::String().Desc("Parent entity UUID; omitted when the entity has no parent."))
                              .Prop("children", Schema::Array(Schema::String()).Desc("Child entity UUIDs."))
                              .Prop("componentsYaml", Schema::String().Desc("All components serialized as scene YAML."))
                              .Required({ "found", "id", "name", "children", "componentsYaml" });
    const Json literalOut = Json{
        { "type", "object" },
        { "properties",
          { { "found", { { "type", "boolean" }, { "description", "True when the entity exists (a miss is returned as isError instead)." } } },
            { "id", { { "type", "string" }, { "description", "Entity UUID." } } },
            { "name", { { "type", "string" }, { "description", "Entity tag/name (empty when it has no TagComponent)." } } },
            { "parent", { { "type", "string" }, { "description", "Parent entity UUID; omitted when the entity has no parent." } } },
            { "children", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "Child entity UUIDs." } } },
            { "componentsYaml", { { "type", "string" }, { "description", "All components serialized as scene YAML." } } } } },
        { "required", Json::array({ "found", "id", "name", "children", "componentsYaml" }) }
    };
    ExpectSameSchema(builtOut, literalOut);
}

TEST(McpSchemaBuilderEquivalence, MemoryReportOutputNested)
{
    const Json built = Schema::Object()
                           .Prop("totalBytes", Schema::Int().Min(0).Desc("Total tracked renderer memory, bytes."))
                           .Prop("totalMB", Schema::Number().Desc("Total tracked renderer memory, MB."))
                           .Prop("byType", Schema::Array(Schema::Object()
                                                             .Prop("type", Schema::String())
                                                             .Prop("bytes", Schema::Int().Min(0))
                                                             .Prop("mb", Schema::Number())
                                                             .Prop("count", Schema::Int().Min(0)))
                                               .Desc("Per-resource-type breakdown; only non-empty types are listed."))
                           .Prop("suspectedLeakCount", Schema::Int().Min(0).Desc("Number of suspected leaks detected."))
                           .Required({ "totalBytes", "totalMB", "byType", "suspectedLeakCount" });
    const Json literal = Json{
        { "type", "object" },
        { "properties",
          { { "totalBytes", { { "type", "integer" }, { "minimum", 0 }, { "description", "Total tracked renderer memory, bytes." } } },
            { "totalMB", { { "type", "number" }, { "description", "Total tracked renderer memory, MB." } } },
            { "byType",
              { { "type", "array" },
                { "description", "Per-resource-type breakdown; only non-empty types are listed." },
                { "items",
                  { { "type", "object" },
                    { "properties",
                      { { "type", { { "type", "string" } } },
                        { "bytes", { { "type", "integer" }, { "minimum", 0 } } },
                        { "mb", { { "type", "number" } } },
                        { "count", { { "type", "integer" }, { "minimum", 0 } } } } } } } } },
            { "suspectedLeakCount", { { "type", "integer" }, { "minimum", 0 }, { "description", "Number of suspected leaks detected." } } } } },
        { "required", Json::array({ "totalBytes", "totalMB", "byType", "suspectedLeakCount" }) }
    };
    ExpectSameSchema(built, literal);
}

TEST(McpSchemaBuilderEquivalence, CameraSetPoseInputVec3AndRequired)
{
    const Json built = Schema::Object()
                           .Prop("position", Schema::Vec3("Camera eye position [x, y, z] (world units)."))
                           .Prop("target", Schema::Vec3("Point to look at [x, y, z]. Alternative to yaw/pitch."))
                           .Prop("yaw", Schema::Number().Desc("Yaw in degrees (0 looks along -Z; positive turns right). Alternative to target."))
                           .Prop("pitch", Schema::Number().Desc("Pitch in degrees (positive looks down). Alternative to target."))
                           .Prop("fov", Schema::Number().Min(1).Max(170).Desc("Vertical field of view in degrees (omit to keep current)."))
                           .Required({ "position" })
                           .NoAdditional();
    const Json literal = Json{
        { "type", "object" },
        { "properties",
          { { "position", { { "type", "array" }, { "items", Json{ { "type", "number" } } }, { "minItems", 3 }, { "maxItems", 3 }, { "description", "Camera eye position [x, y, z] (world units)." } } },
            { "target", { { "type", "array" }, { "items", Json{ { "type", "number" } } }, { "minItems", 3 }, { "maxItems", 3 }, { "description", "Point to look at [x, y, z]. Alternative to yaw/pitch." } } },
            { "yaw", { { "type", "number" }, { "description", "Yaw in degrees (0 looks along -Z; positive turns right). Alternative to target." } } },
            { "pitch", { { "type", "number" }, { "description", "Pitch in degrees (positive looks down). Alternative to target." } } },
            { "fov", { { "type", "number" }, { "minimum", 1 }, { "maximum", 170 }, { "description", "Vertical field of view in degrees (omit to keep current)." } } } } },
        { "required", Json::array({ "position" }) },
        { "additionalProperties", false }
    };
    ExpectSameSchema(built, literal);
}

TEST(McpSchemaBuilderEquivalence, CameraOrbitInputExclusiveMinimum)
{
    const Json built = Schema::Object()
                           .Prop("target", Schema::Vec3("Orbit centre [x, y, z] (world units)."))
                           .Prop("yaw", Schema::Number().Desc("Orbit yaw in degrees (default 0)."))
                           .Prop("pitch", Schema::Number().Desc("Orbit pitch in degrees, positive looks down (default 30)."))
                           .Prop("distance", Schema::Number().ExclusiveMin(0).Desc("Distance from the target in world units (default 10)."))
                           .Prop("fov", Schema::Number().Min(1).Max(170).Desc("Vertical field of view in degrees (omit to keep current)."))
                           .Required({ "target" })
                           .NoAdditional();
    const Json literal = Json{
        { "type", "object" },
        { "properties",
          { { "target", { { "type", "array" }, { "items", Json{ { "type", "number" } } }, { "minItems", 3 }, { "maxItems", 3 }, { "description", "Orbit centre [x, y, z] (world units)." } } },
            { "yaw", { { "type", "number" }, { "description", "Orbit yaw in degrees (default 0)." } } },
            { "pitch", { { "type", "number" }, { "description", "Orbit pitch in degrees, positive looks down (default 30)." } } },
            { "distance", { { "type", "number" }, { "exclusiveMinimum", 0 }, { "description", "Distance from the target in world units (default 10)." } } },
            { "fov", { { "type", "number" }, { "minimum", 1 }, { "maximum", 170 }, { "description", "Vertical field of view in degrees (omit to keep current)." } } } } },
        { "required", Json::array({ "target" }) },
        { "additionalProperties", false }
    };
    ExpectSameSchema(built, literal);
}

TEST(McpSchemaBuilderEquivalence, ScreenshotInputOpaqueSubObjects)
{
    const Json built = Schema::Object()
                           .Prop("maxWidth", Schema::Int().Min(16).Max(4096).Desc("Max output width in pixels (default 1024); aspect ratio preserved."))
                           .Prop("camera", Schema::Object().Desc("Capture from this pose, then restore the prior camera. Same shape as olo_camera_set_pose: position [x,y,z] plus target [x,y,z] or yaw/pitch (degrees); optional fov."))
                           .Prop("orbit", Schema::Object().Desc("Capture from this orbit pose, then restore. Same shape as olo_camera_orbit: target [x,y,z], yaw/pitch (degrees), distance; optional fov."))
                           .Prop("settleFrames", Schema::Int().Min(1).Max(30).Desc("Frames to render at the new pose before capturing (default 2). Raise for temporal effects (TAA, fog history) to settle."))
                           .NoAdditional();
    const Json literal = Json{
        { "type", "object" },
        { "properties",
          { { "maxWidth", { { "type", "integer" }, { "minimum", 16 }, { "maximum", 4096 }, { "description", "Max output width in pixels (default 1024); aspect ratio preserved." } } },
            { "camera",
              { { "type", "object" },
                { "description", "Capture from this pose, then restore the prior camera. Same shape as olo_camera_set_pose: position [x,y,z] plus target [x,y,z] or yaw/pitch (degrees); optional fov." } } },
            { "orbit",
              { { "type", "object" },
                { "description", "Capture from this orbit pose, then restore. Same shape as olo_camera_orbit: target [x,y,z], yaw/pitch (degrees), distance; optional fov." } } },
            { "settleFrames", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 30 }, { "description", "Frames to render at the new pose before capturing (default 2). Raise for temporal effects (TAA, fog history) to settle." } } } } },
        { "additionalProperties", false }
    };
    ExpectSameSchema(built, literal);
}

TEST(McpSchemaBuilderEquivalence, EventsTailInputUnionTypeAndEnumArray)
{
    const Json built = Schema::Object()
                           .Prop("count", Schema::Int().Min(1).Max(500).Desc("How many of the most recent matching events to return (default 50)."))
                           .Prop("sinceId", Schema::Raw(Json{ { "type", Json::array({ "integer", "string" }) } })
                                                .Min(0)
                                                .Desc("Only return events with id greater than this. Accepts the id as a number or its string form (for large cursors beyond JSON integer precision). Pass back the previous response's 'lastId' for incremental polling."))
                           .Prop("categories", Schema::Array(Schema::String().Enum({ "scene_load", "play", "stop", "entity_spawn", "entity_destroy", "asset_reload", "script_error" }))
                                                   .Desc("Only return events whose category is in this list. Omit for all categories."))
                           .NoAdditional();
    const Json literal = Json{
        { "type", "object" },
        { "properties",
          { { "count",
              { { "type", "integer" }, { "minimum", 1 }, { "maximum", 500 }, { "description", "How many of the most recent matching events to return (default 50)." } } },
            { "sinceId",
              { { "type", Json::array({ "integer", "string" }) }, { "minimum", 0 }, { "description", "Only return events with id greater than this. Accepts the id as a number or its string form (for large cursors beyond JSON integer precision). Pass back the previous response's 'lastId' for incremental polling." } } },
            { "categories",
              { { "type", "array" },
                { "items", { { "type", "string" }, { "enum", Json::array({ "scene_load", "play", "stop", "entity_spawn", "entity_destroy", "asset_reload", "script_error" }) } } },
                { "description", "Only return events whose category is in this list. Omit for all categories." } } } } },
        { "additionalProperties", false }
    };
    ExpectSameSchema(built, literal);
}

TEST(McpSchemaBuilderEquivalence, PhysicsRaycastInputAndOutputNested)
{
    const Json builtIn = Schema::Object()
                             .Prop("origin", Schema::Array().Desc("Ray start [x, y, z]."))
                             .Prop("direction", Schema::Array().Desc("Ray direction [x, y, z] (need not be normalised). Provide this or 'to'."))
                             .Prop("to", Schema::Array().Desc("Ray end point [x, y, z]; sets direction and distance. Provide this or 'direction'."))
                             .Prop("maxDistance", Schema::Number().Desc("Max ray length when using 'direction' (default 500)."))
                             .Prop("maxHits", Schema::Int().Min(1).Max(64).Desc("Return up to N ordered hits (default 1 = closest only)."))
                             .Required({ "origin" })
                             .NoAdditional();
    const Json literalIn = Json{
        { "type", "object" },
        { "properties",
          { { "origin", { { "type", "array" }, { "description", "Ray start [x, y, z]." } } },
            { "direction", { { "type", "array" }, { "description", "Ray direction [x, y, z] (need not be normalised). Provide this or 'to'." } } },
            { "to", { { "type", "array" }, { "description", "Ray end point [x, y, z]; sets direction and distance. Provide this or 'direction'." } } },
            { "maxDistance", { { "type", "number" }, { "description", "Max ray length when using 'direction' (default 500)." } } },
            { "maxHits", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 64 }, { "description", "Return up to N ordered hits (default 1 = closest only)." } } } } },
        { "required", Json::array({ "origin" }) },
        { "additionalProperties", false }
    };
    ExpectSameSchema(builtIn, literalIn);

    const Json builtOut = Schema::Object()
                              .Prop("origin", Schema::Array(Schema::Number()).Desc("Resolved ray origin [x, y, z]."))
                              .Prop("direction", Schema::Array(Schema::Number()).Desc("Resolved normalised ray direction [x, y, z]."))
                              .Prop("maxDistance", Schema::Number().Desc("Resolved ray length."))
                              .Prop("hitCount", Schema::Int().Min(0).Desc("Number of hits returned."))
                              .Prop("hits", Schema::Array(Schema::Object()
                                                              .Prop("entity", Schema::Object()
                                                                                  .Prop("id", Schema::String())
                                                                                  .Prop("name", Schema::String()))
                                                              .Prop("position", Schema::Array(Schema::Number()))
                                                              .Prop("normal", Schema::Array(Schema::Number()))
                                                              .Prop("distance", Schema::Number()))
                                                .Desc("Hits ordered nearest-first."))
                              .Required({ "origin", "direction", "maxDistance", "hitCount", "hits" });
    const Json literalOut = Json{
        { "type", "object" },
        { "properties",
          { { "origin", { { "type", "array" }, { "items", { { "type", "number" } } }, { "description", "Resolved ray origin [x, y, z]." } } },
            { "direction", { { "type", "array" }, { "items", { { "type", "number" } } }, { "description", "Resolved normalised ray direction [x, y, z]." } } },
            { "maxDistance", { { "type", "number" }, { "description", "Resolved ray length." } } },
            { "hitCount", { { "type", "integer" }, { "minimum", 0 }, { "description", "Number of hits returned." } } },
            { "hits",
              { { "type", "array" },
                { "description", "Hits ordered nearest-first." },
                { "items",
                  { { "type", "object" },
                    { "properties",
                      { { "entity",
                          { { "type", "object" },
                            { "properties",
                              { { "id", { { "type", "string" } } },
                                { "name", { { "type", "string" } } } } } } },
                        { "position", { { "type", "array" }, { "items", { { "type", "number" } } } } },
                        { "normal", { { "type", "array" }, { "items", { { "type", "number" } } } } },
                        { "distance", { { "type", "number" } } } } } } } } } } },
        { "required", Json::array({ "origin", "direction", "maxDistance", "hitCount", "hits" }) }
    };
    ExpectSameSchema(builtOut, literalOut);
}

TEST(McpSchemaBuilderEquivalence, RenderFrameBreakdownInputEnums)
{
    const Json built = Schema::Object()
                           .Prop("viewMode", Schema::String()
                                                 .Enum({ "presort", "postsort", "postbatch" })
                                                 .Desc("(json format only) Pipeline stage to list: 'presort' (submission order), 'postsort' "
                                                       "(after the radix sort), or 'postbatch' (what actually executed; default). Falls back "
                                                       "to an earlier, populated stage when the requested one is empty."))
                           .Prop("maxCommands", Schema::Int()
                                                    .Min(1)
                                                    .Max(5000)
                                                    .Desc("(json format only) Cap on commands returned (default 200). The full count and a "
                                                          "'truncated' flag are always reported."))
                           .Prop("format", Schema::String()
                                               .Enum({ "json", "markdown" })
                                               .Desc("'json' (default): structured per-command breakdown shaped by viewMode/maxCommands. "
                                                     "'markdown': the human/LLM analysis report (covers all stages and commands)."))
                           .NoAdditional();
    const Json literal = Json{
        { "type", "object" },
        { "properties",
          { { "viewMode",
              { { "type", "string" },
                { "enum", { "presort", "postsort", "postbatch" } },
                { "description",
                  "(json format only) Pipeline stage to list: 'presort' (submission order), 'postsort' "
                  "(after the radix sort), or 'postbatch' (what actually executed; default). Falls back "
                  "to an earlier, populated stage when the requested one is empty." } } },
            { "maxCommands",
              { { "type", "integer" },
                { "minimum", 1 },
                { "maximum", 5000 },
                { "description",
                  "(json format only) Cap on commands returned (default 200). The full count and a "
                  "'truncated' flag are always reported." } } },
            { "format",
              { { "type", "string" },
                { "enum", { "json", "markdown" } },
                { "description",
                  "'json' (default): structured per-command breakdown shaped by viewMode/maxCommands. "
                  "'markdown': the human/LLM analysis report (covers all stages and commands)." } } } } },
        { "additionalProperties", false }
    };
    ExpectSameSchema(built, literal);
}
