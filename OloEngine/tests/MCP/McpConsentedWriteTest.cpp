#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpConsentedWriteTest — unit test (headless, no GL, no live editor).
//
// Pins the first consented, undoable MCP WRITE tool: olo_set_collision_layer
// (issue #306 item C, first slice). Two seams are exercised:
//
//   1. The dispatch seam (McpServer.cpp, compiled into the test binary): a tool
//      flagged ToolDef::ProjectWrite is REFUSED with a clean JSON-RPC error while
//      the session "Allow writes" gate is off, and ACCEPTED once it is on. The
//      server's inputSchema enforcement (#423) rejects a bad layer / missing
//      entity before the handler runs. McpTools.cpp (the real tool registrations)
//      is deliberately NOT linked here, so the test registers a fake tool wired to
//      the SAME shared apply code the real handler uses.
//
//   2. The shared apply core (MCP/McpSetCollisionLayer.h, header-only): it sets
//      the physics body's m_LayerID through a real CommandHistory so the change is
//      a single undoable command — assert the field changed AND that an undo
//      reverts it. This is the half the real handler delegates to.
//
// The shared header is renderer/httplib-free, so only engine Scene/ECS types and
// the header-only UndoRedo stack are pulled in — no extra editor TU.
// =============================================================================

#include "MCP/McpServer.h"
#include "MCP/McpSetCollisionLayer.h"
#include "UndoRedo/EditorCommand.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <optional>
#include <string>

// OLO_TEST_LAYER: unit

// u32 / u64 are global typedefs (Core/Base.h), so they need no using-declaration.
namespace
{
    using OloEngine::CharacterController3DComponent;
    using OloEngine::CommandHistory;
    using OloEngine::Entity;
    using OloEngine::Ref;
    using OloEngine::Rigidbody3DComponent;
    using OloEngine::Scene;
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;
    namespace SetCollisionLayer = OloEngine::MCP::SetCollisionLayer;

    constexpr int kInvalidParams = -32602;

    Json MakeCallRequest(const Json& id, const Json& arguments)
    {
        return Json{ { "jsonrpc", "2.0" },
                     { "id", id },
                     { "method", "tools/call" },
                     { "params", { { "name", "olo_set_collision_layer" }, { "arguments", arguments } } } };
    }

    // Fixture: an McpServer whose only tool is a fake olo_set_collision_layer wired
    // to a test-owned Scene + CommandHistory through the SAME schema + ParseArgs +
    // Apply code the real handler uses. The fake handler runs synchronously (no
    // MarshalRead — there is no game thread to service it here), which is exactly
    // what the real handler delegates to once on the main thread.
    class McpConsentedWriteTest : public ::testing::Test
    {
      protected:
        McpConsentedWriteTest()
            : m_Server(EditorMcpContext{})
        {
            m_Scene = Ref<Scene>::Create();
            Entity entity = m_Scene->CreateEntity("Body");
            Rigidbody3DComponent rb;
            rb.m_LayerID = kInitialLayer;
            entity.AddComponent<Rigidbody3DComponent>(rb);
            m_EntityUuid = static_cast<u64>(entity.GetUUID());

            ToolDef tool;
            tool.Name = "olo_set_collision_layer";
            tool.Description = "Set the collision layer of an entity's physics body (fake; test wiring).";
            tool.ProjectWrite = true;
            tool.InputSchema = SetCollisionLayer::InputSchema();
            tool.Handler = [this](McpServer&, const Json& args) -> ToolResult
            {
                u64 entityUuid = 0;
                u32 layer = 0;
                if (const auto error = SetCollisionLayer::ParseArgs(args, entityUuid, layer))
                    return ToolResult::Error(*error);
                const SetCollisionLayer::ApplyResult applied =
                    SetCollisionLayer::Apply(m_Scene, m_History, entityUuid, layer);
                if (!applied.Ok)
                    return ToolResult::Error(applied.Error);
                return ToolResult::Text(applied.Data.dump());
            };
            m_Server.RegisterTool(std::move(tool));
        }

        [[nodiscard]] u32 CurrentLayer() const
        {
            return m_Scene->TryGetEntityWithUUID(OloEngine::UUID(m_EntityUuid))
                ->GetComponent<Rigidbody3DComponent>()
                .m_LayerID;
        }

        static constexpr u32 kInitialLayer = 0;

        Ref<Scene> m_Scene;
        CommandHistory m_History;
        u64 m_EntityUuid = 0;
        McpServer m_Server; // declared last → destroyed first (its handler refs m_Scene/m_History)
    };
} // namespace

// ---- the session write gate (dispatch seam) --------------------------------

// Default state: the gate is OFF, so even a well-formed write call is refused with
// a JSON-RPC error and NOTHING is mutated / pushed onto the undo stack.
TEST_F(McpConsentedWriteTest, GateOffRejectsWriteAndMutatesNothing)
{
    ASSERT_FALSE(m_Server.AllowWrites()); // off by default

    const Json resp = m_Server.HandleMessage(MakeCallRequest(1, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 3 } }));

    ASSERT_TRUE(resp.contains("error"));
    EXPECT_FALSE(resp.contains("result"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);

    // The body is untouched and the undo stack is empty — the gate stopped the
    // handler from ever running.
    EXPECT_EQ(CurrentLayer(), kInitialLayer);
    EXPECT_FALSE(m_History.CanUndo());
}

// With the gate ON the same call succeeds, the field changes, and the change is a
// single undoable command — an undo reverts it, a redo re-applies it.
TEST_F(McpConsentedWriteTest, GateOnAppliesUndoableWrite)
{
    m_Server.SetAllowWrites(true);

    const Json resp = m_Server.HandleMessage(MakeCallRequest(2, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 3 } }));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp.contains("error"));
    EXPECT_FALSE(resp["result"]["isError"]);

    EXPECT_EQ(CurrentLayer(), 3u);
    ASSERT_TRUE(m_History.CanUndo());
    EXPECT_EQ(m_History.GetUndoDescription(), "Set Collision Layer");

    // The defining property of a consented write: it is a single Ctrl-Z.
    m_History.Undo();
    EXPECT_EQ(CurrentLayer(), kInitialLayer);
    EXPECT_FALSE(m_History.CanUndo());

    m_History.Redo();
    EXPECT_EQ(CurrentLayer(), 3u);
}

// ---- server-side inputSchema enforcement (#423), gate ON -------------------

TEST_F(McpConsentedWriteTest, SchemaRejectsLayerBelowMinimum)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(3, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", -1 } }));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);
    EXPECT_EQ(CurrentLayer(), kInitialLayer); // never applied
}

TEST_F(McpConsentedWriteTest, SchemaRejectsLayerAboveMaximum)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(4, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 999 } }));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);
}

TEST_F(McpConsentedWriteTest, SchemaRejectsMissingEntity)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(5, Json{ { "layer", 2 } }));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);
}

TEST_F(McpConsentedWriteTest, SchemaRejectsNonIntegerLayer)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(6, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", "3" } }));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);
}

TEST_F(McpConsentedWriteTest, SchemaRejectsUnknownProperty)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(
        MakeCallRequest(7, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 2 }, { "extra", true } }));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);
}

// ---- handler-level (tool) errors, gate ON ----------------------------------

// A schema-valid call for a non-existent entity is a TOOL-level error (isError),
// not a JSON-RPC protocol error — the call reached the handler, the handler failed.
TEST_F(McpConsentedWriteTest, UnknownEntityIsToolError)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(8, Json{ { "entity", "99999999" }, { "layer", 2 } }));
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp.contains("error"));
    EXPECT_TRUE(resp["result"]["isError"]);
}

// ---- the shared apply core (MCP/McpSetCollisionLayer.h), no server ----------

TEST(McpSetCollisionLayerApply, ChangesRigidbodyLayerAndUndoReverts)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Body");
    entity.AddComponent<Rigidbody3DComponent>();
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    const auto result = SetCollisionLayer::Apply(scene, history, uuid, 5);
    ASSERT_TRUE(result.Ok);
    EXPECT_TRUE(result.Data["changed"].get<bool>());
    EXPECT_EQ(result.Data["component"], "Rigidbody3DComponent");
    EXPECT_EQ(result.Data["previousLayer"], 0u);
    EXPECT_EQ(result.Data["layer"], 5u);
    EXPECT_EQ(entity.GetComponent<Rigidbody3DComponent>().m_LayerID, 5u);

    ASSERT_TRUE(history.CanUndo());
    history.Undo();
    EXPECT_EQ(entity.GetComponent<Rigidbody3DComponent>().m_LayerID, 0u);
}

// Setting the layer to the value it already has changes nothing and pushes no undo
// command (no spurious dirty / undo-stack entry).
TEST(McpSetCollisionLayerApply, NoOpWhenLayerUnchanged)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Body");
    Rigidbody3DComponent rb;
    rb.m_LayerID = 4;
    entity.AddComponent<Rigidbody3DComponent>(rb);
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    const auto result = SetCollisionLayer::Apply(scene, history, uuid, 4);
    ASSERT_TRUE(result.Ok);
    EXPECT_FALSE(result.Data["changed"].get<bool>());
    EXPECT_FALSE(result.Data["undoable"].get<bool>());
    EXPECT_FALSE(history.CanUndo());
    EXPECT_EQ(entity.GetComponent<Rigidbody3DComponent>().m_LayerID, 4u);
}

// A character controller carries its own m_LayerID and is the secondary write target.
TEST(McpSetCollisionLayerApply, ChangesCharacterControllerLayer)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("Player");
    entity.AddComponent<CharacterController3DComponent>();
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    const auto result = SetCollisionLayer::Apply(scene, history, uuid, 2);
    ASSERT_TRUE(result.Ok);
    EXPECT_EQ(result.Data["component"], "CharacterController3DComponent");
    EXPECT_EQ(entity.GetComponent<CharacterController3DComponent>().m_LayerID, 2u);
    ASSERT_TRUE(history.CanUndo());
    history.Undo();
    EXPECT_EQ(entity.GetComponent<CharacterController3DComponent>().m_LayerID, 0u);
}

TEST(McpSetCollisionLayerApply, RejectsEntityWithoutPhysicsBody)
{
    auto scene = Ref<Scene>::Create();
    Entity entity = scene->CreateEntity("NoBody");
    const u64 uuid = static_cast<u64>(entity.GetUUID());
    CommandHistory history;

    const auto result = SetCollisionLayer::Apply(scene, history, uuid, 1);
    EXPECT_FALSE(result.Ok);
    EXPECT_FALSE(result.Error.empty());
    EXPECT_FALSE(history.CanUndo());
}

TEST(McpSetCollisionLayerApply, RejectsMissingEntity)
{
    auto scene = Ref<Scene>::Create();
    CommandHistory history;
    const auto result = SetCollisionLayer::Apply(scene, history, /*entityUuid*/ 12345, 1);
    EXPECT_FALSE(result.Ok);
    EXPECT_FALSE(result.Error.empty());
}

// ---- the shared arg parser --------------------------------------------------

TEST(McpSetCollisionLayerParse, AcceptsStringUuidAndLayer)
{
    u64 entity = 0;
    u32 layer = 0;
    const auto error = SetCollisionLayer::ParseArgs(Json{ { "entity", "42" }, { "layer", 7 } }, entity, layer);
    EXPECT_FALSE(error.has_value());
    EXPECT_EQ(entity, 42u);
    EXPECT_EQ(layer, 7u);
}

TEST(McpSetCollisionLayerParse, AcceptsNumericUuid)
{
    u64 entity = 0;
    u32 layer = 0;
    const auto error = SetCollisionLayer::ParseArgs(Json{ { "entity", 42 }, { "layer", 0 } }, entity, layer);
    EXPECT_FALSE(error.has_value());
    EXPECT_EQ(entity, 42u);
}

TEST(McpSetCollisionLayerParse, RejectsOutOfRangeAndMissing)
{
    u64 entity = 0;
    u32 layer = 0;
    EXPECT_TRUE(SetCollisionLayer::ParseArgs(Json{ { "entity", "1" }, { "layer", -1 } }, entity, layer).has_value());
    EXPECT_TRUE(SetCollisionLayer::ParseArgs(Json{ { "entity", "1" }, { "layer", 9999 } }, entity, layer).has_value());
    EXPECT_TRUE(SetCollisionLayer::ParseArgs(Json{ { "layer", 1 } }, entity, layer).has_value());
    EXPECT_TRUE(SetCollisionLayer::ParseArgs(Json{ { "entity", "1" } }, entity, layer).has_value());
    EXPECT_TRUE(SetCollisionLayer::ParseArgs(Json{ { "entity", "not-a-number" }, { "layer", 1 } }, entity, layer).has_value());
}

// ---- the shared inputSchema -------------------------------------------------

TEST(McpSetCollisionLayerSchema, DeclaresRequiredFieldsAndLayerBounds)
{
    const Json schema = SetCollisionLayer::InputSchema();
    EXPECT_EQ(schema["type"], "object");
    EXPECT_EQ(schema["additionalProperties"], false);

    ASSERT_TRUE(schema["required"].is_array());
    const auto& required = schema["required"];
    EXPECT_NE(std::find(required.begin(), required.end(), "entity"), required.end());
    EXPECT_NE(std::find(required.begin(), required.end(), "layer"), required.end());

    EXPECT_EQ(schema["properties"]["entity"]["type"], "string");
    EXPECT_EQ(schema["properties"]["layer"]["type"], "integer");
    EXPECT_EQ(schema["properties"]["layer"]["minimum"], 0);
    EXPECT_EQ(schema["properties"]["layer"]["maximum"], static_cast<int>(SetCollisionLayer::kMaxLayerId));
}
