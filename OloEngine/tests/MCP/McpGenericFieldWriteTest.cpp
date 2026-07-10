#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpGenericFieldWriteTest — unit test (headless, no GL, no live editor).
//
// Pins the GENERIC consented, undoable MCP write tool olo_entity_set_field and
// its read companion olo_entity_list_fields (issue #306 item C, second slice —
// the catch-all successor to olo_set_collision_layer). Same two seams as
// McpConsentedWriteTest.cpp:
//
//   1. The dispatch seam (McpServer.cpp, compiled into the test binary): a tool
//      flagged ToolDef::ProjectWrite is REFUSED with a clean JSON-RPC error while
//      the session "Allow writes" gate is off, and ACCEPTED once on. The server's
//      inputSchema enforcement (#423) rejects a malformed call before the handler
//      runs. McpTools.cpp is deliberately NOT linked here, so the test registers a
//      fake tool wired to the SAME shared parse + apply code the real handler uses.
//
//   2. The shared core (MCP/McpGenericFieldWrite.h, header-only): the field
//      registry + JSON->field coercion + FieldToJson + the undoable
//      ComponentChangeCommand<T> apply, plus the read/list helper. These are the
//      halves the real handlers delegate to.
//
// The shared header is renderer/httplib-free, so only engine Scene/ECS types and
// the header-only UndoRedo stack are pulled in — no extra editor TU.
// =============================================================================

#include "MCP/McpGenericFieldWrite.h"
#include "MCP/McpServer.h"
#include "UndoRedo/EditorCommand.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

// OLO_TEST_LAYER: unit

namespace
{
    using OloEngine::CommandHistory;
    using OloEngine::DirectionalLightComponent;
    using OloEngine::Entity;
    using OloEngine::Ref;
    using OloEngine::Scene;
    using OloEngine::SpriteRendererComponent;
    using OloEngine::TagComponent;
    using OloEngine::TransformComponent;
    using OloEngine::UUID;
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;
    namespace GFW = OloEngine::MCP::GenericFieldWrite;

    constexpr int kInvalidParams = -32602;

    // Component-wise float comparison (gtest ULP-based) — never `==` on glm::vec*,
    // which SonarQube flags (cpp-coding-quality §2a). Values asserted here are
    // exactly representable, so EXPECT_FLOAT_EQ is exact in practice.
    void ExpectVec3(const glm::vec3& v, float x, float y, float z)
    {
        EXPECT_FLOAT_EQ(v.x, x);
        EXPECT_FLOAT_EQ(v.y, y);
        EXPECT_FLOAT_EQ(v.z, z);
    }
    void ExpectVec4(const glm::vec4& v, float x, float y, float z, float w)
    {
        EXPECT_FLOAT_EQ(v.x, x);
        EXPECT_FLOAT_EQ(v.y, y);
        EXPECT_FLOAT_EQ(v.z, z);
        EXPECT_FLOAT_EQ(v.w, w);
    }

    Json MakeCallRequest(const Json& id, const Json& arguments)
    {
        return Json{ { "jsonrpc", "2.0" },
                     { "id", id },
                     { "method", "tools/call" },
                     { "params", { { "name", "olo_entity_set_field" }, { "arguments", arguments } } } };
    }

    // Fixture: an McpServer whose only tool is a fake olo_entity_set_field wired to a
    // test-owned Scene + CommandHistory through the SAME schema + ParseArgs + Apply
    // code the real handler uses. The fake handler runs synchronously (no MarshalRead
    // — there is no game thread to service it here), exactly what the real handler
    // delegates to once on the main thread.
    class McpGenericFieldWriteTest : public ::testing::Test
    {
      protected:
        McpGenericFieldWriteTest()
            : m_Server(EditorMcpContext{})
        {
            m_Scene = Ref<Scene>::Create();
            Entity entity = m_Scene->CreateEntity("Thing"); // gets Transform + Tag
            entity.AddComponent<SpriteRendererComponent>();
            entity.AddComponent<DirectionalLightComponent>();
            m_EntityUuid = static_cast<u64>(entity.GetUUID());

            ToolDef tool;
            tool.Name = "olo_entity_set_field";
            tool.Description = "Set any registered component field (fake; test wiring).";
            tool.ProjectWrite = true;
            tool.InputSchema = GFW::InputSchema();
            tool.Handler = [this](McpServer&, const Json& args) -> ToolResult
            {
                u64 entityUuid = 0;
                std::string component;
                std::string field;
                Json value;
                if (const auto error = GFW::ParseArgs(args, entityUuid, component, field, value))
                    return ToolResult::Error(*error);
                const GFW::ApplyResult applied = GFW::Apply(m_Scene, m_History, entityUuid, component, field, value);
                if (!applied.Ok)
                    return ToolResult::Error(applied.Error);
                return ToolResult::Text(applied.Data.dump());
            };
            m_Server.RegisterTool(std::move(tool));
        }

        [[nodiscard]] Entity TheEntity() const
        {
            return *m_Scene->TryGetEntityWithUUID(UUID(m_EntityUuid));
        }
        [[nodiscard]] glm::vec3 CurrentTranslation() const
        {
            return TheEntity().GetComponent<TransformComponent>().Translation;
        }

        Ref<Scene> m_Scene;
        CommandHistory m_History;
        u64 m_EntityUuid = 0;
        McpServer m_Server; // declared last → destroyed first (its handler refs m_Scene/m_History)
    };
} // namespace

// ---- the session write gate (dispatch seam) --------------------------------

// Default state: the gate is OFF, so even a well-formed write call is refused with
// a JSON-RPC error and NOTHING is mutated / pushed onto the undo stack.
TEST_F(McpGenericFieldWriteTest, GateOffRejectsWriteAndMutatesNothing)
{
    ASSERT_FALSE(m_Server.AllowWrites()); // off by default

    const Json resp = m_Server.HandleMessage(MakeCallRequest(
        1, Json{ { "entity", std::to_string(m_EntityUuid) }, { "component", "TransformComponent" }, { "field", "Translation" }, { "value", Json::array({ 1.0, 2.0, 3.0 }) } }));

    ASSERT_TRUE(resp.contains("error"));
    EXPECT_FALSE(resp.contains("result"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);

    ExpectVec3(CurrentTranslation(), 0.0f, 0.0f, 0.0f);
    EXPECT_FALSE(m_History.CanUndo());
}

// With the gate ON the same call succeeds, the field changes, and the change is a
// single undoable command — an undo reverts it, a redo re-applies it.
TEST_F(McpGenericFieldWriteTest, GateOnAppliesUndoableWrite)
{
    m_Server.SetAllowWrites(true);

    const Json resp = m_Server.HandleMessage(MakeCallRequest(
        2, Json{ { "entity", std::to_string(m_EntityUuid) }, { "component", "TransformComponent" }, { "field", "Translation" }, { "value", Json::array({ 1.0, 2.0, 3.0 }) } }));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp.contains("error"));
    EXPECT_FALSE(resp["result"]["isError"]);

    ExpectVec3(CurrentTranslation(), 1.0f, 2.0f, 3.0f);
    ASSERT_TRUE(m_History.CanUndo());
    EXPECT_EQ(m_History.GetUndoDescription(), "Set TransformComponent.Translation");

    m_History.Undo();
    ExpectVec3(CurrentTranslation(), 0.0f, 0.0f, 0.0f);
    EXPECT_FALSE(m_History.CanUndo());

    m_History.Redo();
    ExpectVec3(CurrentTranslation(), 1.0f, 2.0f, 3.0f);
}

// ---- server-side inputSchema enforcement (#423), gate ON -------------------

TEST_F(McpGenericFieldWriteTest, SchemaRejectsMissingEntity)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(
        3, Json{ { "component", "TransformComponent" }, { "field", "Translation" }, { "value", Json::array({ 1.0, 2.0, 3.0 }) } }));
    ASSERT_TRUE(resp.contains("result")); // SEP-1303: schema failures are tool errors
    EXPECT_EQ(resp["result"]["isError"], true);
}

TEST_F(McpGenericFieldWriteTest, SchemaRejectsMissingComponentFieldOrValue)
{
    m_Server.SetAllowWrites(true);
    const Json base = Json{ { "entity", std::to_string(m_EntityUuid) }, { "component", "TransformComponent" }, { "field", "Translation" }, { "value", Json::array({ 1.0, 2.0, 3.0 }) } };
    for (const char* missing : { "component", "field", "value" })
    {
        Json args = base;
        args.erase(missing);
        const Json resp = m_Server.HandleMessage(MakeCallRequest(4, args));
        ASSERT_TRUE(resp.contains("result")) << "should reject missing '" << missing << "'"; // SEP-1303 tool error
        EXPECT_EQ(resp["result"]["isError"], true);
    }
}

// `value` is a union of boolean/number/string/array, so an object/null is rejected
// at the schema layer before the handler runs.
TEST_F(McpGenericFieldWriteTest, SchemaRejectsObjectValue)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(
        5, Json{ { "entity", std::to_string(m_EntityUuid) }, { "component", "TransformComponent" }, { "field", "Translation" }, { "value", Json::object({ { "x", 1 } }) } }));
    ASSERT_TRUE(resp.contains("result")); // SEP-1303: schema failures are tool errors
    EXPECT_EQ(resp["result"]["isError"], true);
}

TEST_F(McpGenericFieldWriteTest, SchemaRejectsUnknownProperty)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(
        6, Json{ { "entity", std::to_string(m_EntityUuid) }, { "component", "TransformComponent" }, { "field", "Translation" }, { "value", Json::array({ 1.0, 2.0, 3.0 }) }, { "extra", true } }));
    ASSERT_TRUE(resp.contains("result")); // SEP-1303: schema failures are tool errors
    EXPECT_EQ(resp["result"]["isError"], true);
}

// ---- handler-level (tool) errors, gate ON ----------------------------------

// A schema-valid call for a non-existent entity is a TOOL-level error (isError),
// not a JSON-RPC protocol error — the call reached the handler, the handler failed.
TEST_F(McpGenericFieldWriteTest, UnknownEntityIsToolError)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(
        7, Json{ { "entity", "99999999" }, { "component", "TransformComponent" }, { "field", "Translation" }, { "value", Json::array({ 1.0, 2.0, 3.0 }) } }));
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp.contains("error"));
    EXPECT_TRUE(resp["result"]["isError"]);
}

// A wrong value type for the field (string into a vec3) reaches the handler (schema
// allows a string) and is a tool-level coercion error.
TEST_F(McpGenericFieldWriteTest, WrongValueTypeIsToolError)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(
        8, Json{ { "entity", std::to_string(m_EntityUuid) }, { "component", "TransformComponent" }, { "field", "Translation" }, { "value", "not-a-vector" } }));
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_TRUE(resp["result"]["isError"]);
    ExpectVec3(CurrentTranslation(), 0.0f, 0.0f, 0.0f); // never applied
}

// ---- the shared apply core (MCP/McpGenericFieldWrite.h), no server ----------

TEST(McpGenericFieldWriteApply, ChangesVec3AndUndoReverts)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("E");
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    const auto result = GFW::Apply(scene, history, uuid, "TransformComponent", "Scale", Json::array({ 2.0, 3.0, 4.0 }));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_TRUE(result.Data["changed"].get<bool>());
    EXPECT_TRUE(result.Data["undoable"].get<bool>());
    EXPECT_EQ(result.Data["type"], "vec3");
    EXPECT_EQ(result.Data["previousValue"], Json::array({ 1.0, 1.0, 1.0 }));
    ExpectVec3(entity.GetComponent<TransformComponent>().Scale, 2.0f, 3.0f, 4.0f);

    ASSERT_TRUE(history.CanUndo());
    history.Undo();
    ExpectVec3(entity.GetComponent<TransformComponent>().Scale, 1.0f, 1.0f, 1.0f);
}

TEST(McpGenericFieldWriteApply, ChangesVec4Color)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("E");
    entity.AddComponent<SpriteRendererComponent>();
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    const auto result = GFW::Apply(scene, history, uuid, "SpriteRendererComponent", "Color", Json::array({ 0.1, 0.2, 0.3, 0.4 }));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_EQ(result.Data["type"], "vec4");
    ExpectVec4(entity.GetComponent<SpriteRendererComponent>().Color, 0.1f, 0.2f, 0.3f, 0.4f);
}

TEST(McpGenericFieldWriteApply, ChangesFloatField)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("E");
    entity.AddComponent<DirectionalLightComponent>();
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    const auto result = GFW::Apply(scene, history, uuid, "DirectionalLightComponent", "Intensity", Json(2.5));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_EQ(result.Data["type"], "float");
    EXPECT_FLOAT_EQ(entity.GetComponent<DirectionalLightComponent>().m_Intensity, 2.5f);
    history.Undo();
    EXPECT_FLOAT_EQ(entity.GetComponent<DirectionalLightComponent>().m_Intensity, 1.0f);
}

TEST(McpGenericFieldWriteApply, ChangesBoolField)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("E");
    entity.AddComponent<DirectionalLightComponent>();
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    const auto result = GFW::Apply(scene, history, uuid, "DirectionalLightComponent", "CastShadows", Json(false));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_EQ(result.Data["type"], "bool");
    EXPECT_FALSE(entity.GetComponent<DirectionalLightComponent>().m_CastShadows);
}

TEST(McpGenericFieldWriteApply, ChangesStringTag)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Old");
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    const auto result = GFW::Apply(scene, history, uuid, "TagComponent", "Tag", Json("New"));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_EQ(result.Data["type"], "string");
    EXPECT_EQ(result.Data["previousValue"], "Old");
    EXPECT_EQ(entity.GetComponent<TagComponent>().Tag, "New");
    history.Undo();
    EXPECT_EQ(entity.GetComponent<TagComponent>().Tag, "Old");
}

// Setting a field to the value it already has changes nothing and pushes no undo
// command (no spurious dirty / undo-stack entry).
TEST(McpGenericFieldWriteApply, NoOpWhenUnchanged)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("E");
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    // Scale defaults to (1,1,1).
    const auto result = GFW::Apply(scene, history, uuid, "TransformComponent", "Scale", Json::array({ 1.0, 1.0, 1.0 }));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_FALSE(result.Data["changed"].get<bool>());
    EXPECT_FALSE(result.Data["undoable"].get<bool>());
    EXPECT_FALSE(history.CanUndo());
}

TEST(McpGenericFieldWriteApply, UnknownComponentListsEditableComponents)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("E");
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    const auto result = GFW::Apply(scene, history, uuid, "NotAComponent", "Field", Json(1));
    EXPECT_FALSE(result.Ok);
    EXPECT_NE(result.Error.find("Editable components"), std::string::npos);
    EXPECT_FALSE(history.CanUndo());
}

TEST(McpGenericFieldWriteApply, UnknownFieldListsEditableFields)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("E");
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    const auto result = GFW::Apply(scene, history, uuid, "TransformComponent", "Nope", Json::array({ 1.0, 2.0, 3.0 }));
    EXPECT_FALSE(result.Ok);
    EXPECT_NE(result.Error.find("Editable fields"), std::string::npos);
    EXPECT_NE(result.Error.find("Translation"), std::string::npos);
}

TEST(McpGenericFieldWriteApply, EntityLackingComponentIsError)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("E"); // no SpriteRendererComponent
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    const auto result = GFW::Apply(scene, history, uuid, "SpriteRendererComponent", "Color", Json::array({ 1.0, 1.0, 1.0, 1.0 }));
    EXPECT_FALSE(result.Ok);
    EXPECT_NE(result.Error.find("has no SpriteRendererComponent"), std::string::npos);
    EXPECT_FALSE(history.CanUndo());
}

TEST(McpGenericFieldWriteApply, MissingEntityIsError)
{
    auto scene = Ref<Scene>::Create();
    CommandHistory history;
    const auto result = GFW::Apply(scene, history, /*uuid*/ 424242, "TransformComponent", "Scale", Json::array({ 1.0, 1.0, 1.0 }));
    EXPECT_FALSE(result.Ok);
    EXPECT_FALSE(result.Error.empty());
}

// Non-finite vector components are rejected (the isfinite-from-JSON rule).
TEST(McpGenericFieldWriteApply, RejectsNonFiniteVector)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("E");
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    const double inf = std::numeric_limits<double>::infinity();
    const auto result = GFW::Apply(scene, history, uuid, "TransformComponent", "Translation", Json::array({ 0.0, inf, 0.0 }));
    EXPECT_FALSE(result.Ok);
    EXPECT_FALSE(history.CanUndo());
}

// ---- the shared coercion (CoerceJson<F>) -----------------------------------

TEST(McpGenericFieldWriteCoerce, Primitives)
{
    bool b = false;
    EXPECT_FALSE(GFW::CoerceJson<bool>(Json(true), b).has_value());
    EXPECT_TRUE(b);
    EXPECT_TRUE(GFW::CoerceJson<bool>(Json(1), b).has_value()); // 1 is not a JSON boolean

    f32 f = 0.0f;
    EXPECT_FALSE(GFW::CoerceJson<f32>(Json(3), f).has_value()); // integer accepted as number
    EXPECT_FLOAT_EQ(f, 3.0f);
    EXPECT_TRUE(GFW::CoerceJson<f32>(Json(std::numeric_limits<double>::quiet_NaN()), f).has_value());
    EXPECT_TRUE(GFW::CoerceJson<f32>(Json("x"), f).has_value());

    int i = 0;
    EXPECT_FALSE(GFW::CoerceJson<int>(Json(-5), i).has_value());
    EXPECT_EQ(i, -5);
    EXPECT_TRUE(GFW::CoerceJson<int>(Json(2.5), i).has_value()); // not an integer

    u32 u = 0;
    EXPECT_FALSE(GFW::CoerceJson<u32>(Json(7), u).has_value());
    EXPECT_EQ(u, 7u);
    EXPECT_TRUE(GFW::CoerceJson<u32>(Json(-1), u).has_value()); // out of range for u32

    std::string s;
    EXPECT_FALSE(GFW::CoerceJson<std::string>(Json("hi"), s).has_value());
    EXPECT_EQ(s, "hi");
    EXPECT_TRUE(GFW::CoerceJson<std::string>(Json(3), s).has_value());
}

TEST(McpGenericFieldWriteCoerce, Vectors)
{
    glm::vec3 v{};
    EXPECT_FALSE(GFW::CoerceJson<glm::vec3>(Json::array({ 1.0, 2.0, 3.0 }), v).has_value());
    ExpectVec3(v, 1.0f, 2.0f, 3.0f);

    EXPECT_TRUE(GFW::CoerceJson<glm::vec3>(Json::array({ 1.0, 2.0 }), v).has_value());           // too short
    EXPECT_TRUE(GFW::CoerceJson<glm::vec3>(Json::array({ 1.0, 2.0, 3.0, 4.0 }), v).has_value()); // too long
    EXPECT_TRUE(GFW::CoerceJson<glm::vec3>(Json::array({ 1.0, "x", 3.0 }), v).has_value());      // non-number
    EXPECT_TRUE(GFW::CoerceJson<glm::vec3>(Json(5), v).has_value());                             // not an array
}

TEST(McpGenericFieldWriteCoerce, Handle)
{
    UUID h(0);
    EXPECT_FALSE(GFW::CoerceJson<UUID>(Json("123456789"), h).has_value());
    EXPECT_EQ(static_cast<u64>(h), 123456789ull);

    EXPECT_TRUE(GFW::CoerceJson<UUID>(Json(123), h).has_value());    // must be a string
    EXPECT_TRUE(GFW::CoerceJson<UUID>(Json("12ab"), h).has_value()); // non-digit
    EXPECT_TRUE(GFW::CoerceJson<UUID>(Json("-1"), h).has_value());   // signed
    EXPECT_TRUE(GFW::CoerceJson<UUID>(Json(""), h).has_value());     // empty
}

TEST(McpGenericFieldWriteSerialize, FieldToJsonRoundTrips)
{
    EXPECT_EQ(GFW::FieldToJson<bool>(true), Json(true));
    EXPECT_EQ(GFW::FieldToJson<glm::vec3>(glm::vec3(1.0f, 2.0f, 3.0f)), Json::array({ 1.0, 2.0, 3.0 }));
    EXPECT_EQ(GFW::FieldToJson<glm::vec4>(glm::vec4(1.0f, 2.0f, 3.0f, 4.0f)), Json::array({ 1.0, 2.0, 3.0, 4.0 }));
    EXPECT_EQ(GFW::FieldToJson<UUID>(UUID(42)), Json("42")); // handle as decimal string
    EXPECT_EQ(GFW::FieldToJson<std::string>(std::string("t")), Json("t"));
}

// ---- the read companion (ListFields) ---------------------------------------

TEST(McpGenericFieldWriteList, ListsFieldsTheEntityHasWithValues)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Listed"); // Transform + Tag
    entity.AddComponent<DirectionalLightComponent>();
    const u64 uuid = static_cast<u64>(entity.GetUUID());

    bool found = false;
    const Json out = GFW::ListFields(scene, uuid, /*filter*/ "", found);
    ASSERT_TRUE(found);
    EXPECT_TRUE(out["found"].get<bool>());
    ASSERT_TRUE(out["components"].is_array());

    // It should include the components the entity actually has, and NOT one it lacks.
    bool hasTransform = false;
    bool hasTag = false;
    bool hasLight = false;
    bool hasSprite = false;
    for (const auto& comp : out["components"])
    {
        const std::string name = comp["component"].get<std::string>();
        hasTransform = hasTransform || name == "TransformComponent";
        hasTag = hasTag || name == "TagComponent";
        hasLight = hasLight || name == "DirectionalLightComponent";
        hasSprite = hasSprite || name == "SpriteRendererComponent";
    }
    EXPECT_TRUE(hasTransform);
    EXPECT_TRUE(hasTag);
    EXPECT_TRUE(hasLight);
    EXPECT_FALSE(hasSprite); // entity has no SpriteRendererComponent
}

TEST(McpGenericFieldWriteList, ComponentFilterRestrictsListing)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Listed");
    const u64 uuid = static_cast<u64>(entity.GetUUID());

    bool found = false;
    const Json out = GFW::ListFields(scene, uuid, "TagComponent", found);
    ASSERT_TRUE(found);
    ASSERT_EQ(out["components"].size(), 1u);
    EXPECT_EQ(out["components"][0]["component"], "TagComponent");
    EXPECT_EQ(out["components"][0]["fields"][0]["field"], "Tag");
    EXPECT_EQ(out["components"][0]["fields"][0]["value"], "Listed");
}

TEST(McpGenericFieldWriteList, MissingEntityReportsNotFound)
{
    auto scene = Ref<Scene>::Create();
    bool found = false;
    const Json out = GFW::ListFields(scene, /*uuid*/ 7777, "", found);
    EXPECT_FALSE(found);
    EXPECT_FALSE(out["found"].get<bool>());
    EXPECT_TRUE(out["components"].is_array());
    EXPECT_TRUE(out["components"].empty());
}

// ---- the shared arg parser --------------------------------------------------

TEST(McpGenericFieldWriteParse, AcceptsValidArgs)
{
    u64 entity = 0;
    std::string component;
    std::string field;
    Json value;
    const auto error = GFW::ParseArgs(
        Json{ { "entity", "42" }, { "component", "TransformComponent" }, { "field", "Scale" }, { "value", Json::array({ 1.0, 1.0, 1.0 }) } },
        entity, component, field, value);
    EXPECT_FALSE(error.has_value());
    EXPECT_EQ(entity, 42u);
    EXPECT_EQ(component, "TransformComponent");
    EXPECT_EQ(field, "Scale");
    EXPECT_TRUE(value.is_array());
}

TEST(McpGenericFieldWriteParse, RejectsNumericOrPartialUuid)
{
    u64 entity = 0;
    std::string component;
    std::string field;
    Json value;
    const Json good = Json{ { "component", "C" }, { "field", "F" }, { "value", 1 } };
    auto with = [&](const Json& e)
    {
        Json a = good;
        a["entity"] = e;
        return GFW::ParseArgs(a, entity, component, field, value);
    };
    EXPECT_TRUE(with(Json(42)).has_value());      // numeric UUID rejected
    EXPECT_TRUE(with(Json("42abc")).has_value()); // partial
    EXPECT_TRUE(with(Json("-1")).has_value());    // signed
    EXPECT_TRUE(with(Json("")).has_value());      // empty
}

TEST(McpGenericFieldWriteParse, RejectsMissingFields)
{
    u64 entity = 0;
    std::string component;
    std::string field;
    Json value;
    EXPECT_TRUE(GFW::ParseArgs(Json{ { "component", "C" }, { "field", "F" }, { "value", 1 } }, entity, component, field, value).has_value());
    EXPECT_TRUE(GFW::ParseArgs(Json{ { "entity", "1" }, { "field", "F" }, { "value", 1 } }, entity, component, field, value).has_value());
    EXPECT_TRUE(GFW::ParseArgs(Json{ { "entity", "1" }, { "component", "C" }, { "value", 1 } }, entity, component, field, value).has_value());
    EXPECT_TRUE(GFW::ParseArgs(Json{ { "entity", "1" }, { "component", "C" }, { "field", "F" } }, entity, component, field, value).has_value());
}

// ---- the shared inputSchema -------------------------------------------------

TEST(McpGenericFieldWriteSchema, DeclaresRequiredFieldsAndValueUnion)
{
    const Json schema = GFW::InputSchema();
    EXPECT_EQ(schema["type"], "object");
    EXPECT_EQ(schema["additionalProperties"], false);

    ASSERT_TRUE(schema["required"].is_array());
    const auto& required = schema["required"];
    for (const char* name : { "entity", "component", "field", "value" })
        EXPECT_NE(std::find(required.begin(), required.end(), name), required.end()) << "required should contain " << name;

    EXPECT_EQ(schema["properties"]["entity"]["type"], "string");
    EXPECT_EQ(schema["properties"]["component"]["type"], "string");
    EXPECT_EQ(schema["properties"]["field"]["type"], "string");

    // `value` is a type union accepting boolean/number/string/array (so the server
    // rejects an object/null up front; the exact per-field type is coerced later).
    const auto& valueType = schema["properties"]["value"]["type"];
    ASSERT_TRUE(valueType.is_array());
    for (const char* t : { "boolean", "number", "string", "array" })
        EXPECT_NE(std::find(valueType.begin(), valueType.end(), t), valueType.end()) << "value type union should contain " << t;
}
