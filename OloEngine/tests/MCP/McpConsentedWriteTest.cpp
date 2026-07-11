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
#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <thread>

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
    using OloEngine::MCP::ConsentDecision;
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using OloEngine::MCP::WriteConsentMode;
    using Json = OloEngine::MCP::Json;
    namespace SetCollisionLayer = OloEngine::MCP::SetCollisionLayer;

    constexpr int kInvalidParams = -32602;
    // LSP-convention "request cancelled" code the plain-JSON path carries for a call
    // cancelled via notifications/cancelled (McpServer.h kRequestCancelledCode).
    constexpr int kRequestCancelledCode = -32800;

    Json MakeCallRequest(const Json& id, const Json& arguments)
    {
        return Json{ { "jsonrpc", "2.0" },
                     { "id", id },
                     { "method", "tools/call" },
                     { "params", { { "name", "olo_set_collision_layer" }, { "arguments", arguments } } } };
    }

    // A `notifications/cancelled` for the tools/call with this JSON-RPC id. Matching
    // is by exact id value (McpServer's RequestIdKey), so `requestId` must equal the
    // call's `id` verbatim (same JSON type).
    Json MakeCancelNotification(const Json& requestId)
    {
        return Json{ { "jsonrpc", "2.0" },
                     { "method", "notifications/cancelled" },
                     { "params", { { "requestId", requestId }, { "reason", "test cancel" } } } };
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

        // Spin (main test thread) until a worker thread has parked a consent prompt,
        // then return its id — or 0 if none appears within the budget. Mirrors what
        // the editor's main-thread panel poll does each frame. The budget is generous
        // (default ~30 s) so a heavily-loaded CI box that is slow to schedule the
        // worker onto RequestConsent doesn't spuriously fail ASSERT_NE(id, 0u); callers
        // resolve the prompt the instant it appears, so the ceiling is never reached in
        // the happy path.
        [[nodiscard]] u64 WaitForPendingConsent(std::chrono::milliseconds budget = std::chrono::seconds(30))
        {
            constexpr auto step = std::chrono::milliseconds(5);
            for (auto waited = std::chrono::milliseconds(0); waited < budget; waited += step)
            {
                const auto pending = m_Server.PendingConsents();
                if (!pending.empty())
                    return pending.front().Id;
                std::this_thread::sleep_for(step);
            }
            return 0;
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

// ---- the per-action consent dialog (Prompt mode) ---------------------------
// The write reaches HandleToolsCall on a "worker" thread that BLOCKS in
// RequestConsent until the main test thread resolves the prompt — the same
// handshake the editor's panel drives on its UI thread. AllowSession (== the old
// gate-on) still applies straight through with no prompt (asserted separately).

// Prompt mode surfaces a pending request; Approve applies the undoable write.
TEST_F(McpConsentedWriteTest, PromptModeApproveAppliesWrite)
{
    m_Server.SetWriteConsentMode(WriteConsentMode::Prompt);

    std::future<Json> call = std::async(std::launch::async, [this]
                                        { return m_Server.HandleMessage(
                                              MakeCallRequest(10, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 3 } })); });

    const u64 id = WaitForPendingConsent();
    ASSERT_NE(id, 0u) << "no consent prompt surfaced";
    // The write must NOT have applied while the prompt is unresolved.
    EXPECT_EQ(CurrentLayer(), kInitialLayer);
    m_Server.ResolveConsent(id, ConsentDecision::Approve);

    const Json resp = call.get();
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp["result"]["isError"]);
    EXPECT_EQ(CurrentLayer(), 3u);
    EXPECT_TRUE(m_History.CanUndo());
}

// Deny rejects the call with a JSON-RPC error and mutates nothing.
TEST_F(McpConsentedWriteTest, PromptModeDenyRejectsWrite)
{
    m_Server.SetWriteConsentMode(WriteConsentMode::Prompt);

    std::future<Json> call = std::async(std::launch::async, [this]
                                        { return m_Server.HandleMessage(
                                              MakeCallRequest(11, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 3 } })); });

    const u64 id = WaitForPendingConsent();
    ASSERT_NE(id, 0u);
    m_Server.ResolveConsent(id, ConsentDecision::Deny);

    const Json resp = call.get();
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);
    EXPECT_EQ(CurrentLayer(), kInitialLayer);
    EXPECT_FALSE(m_History.CanUndo());
}

// "Approve all this session" applies the current write AND flips the mode to
// AllowSession, so a subsequent write applies with no prompt.
TEST_F(McpConsentedWriteTest, ApproveAllFlipsToAllowSession)
{
    m_Server.SetWriteConsentMode(WriteConsentMode::Prompt);

    std::future<Json> first = std::async(std::launch::async, [this]
                                         { return m_Server.HandleMessage(
                                               MakeCallRequest(12, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 3 } })); });
    const u64 id = WaitForPendingConsent();
    ASSERT_NE(id, 0u);
    m_Server.ResolveConsent(id, ConsentDecision::ApproveAll);

    ASSERT_TRUE(first.get()["result"].is_object());
    EXPECT_EQ(CurrentLayer(), 3u);
    EXPECT_EQ(m_Server.GetWriteConsentMode(), WriteConsentMode::AllowSession);

    // The next write runs with no prompt outstanding (synchronous — no waiter needed).
    const Json second = m_Server.HandleMessage(
        MakeCallRequest(13, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 7 } }));
    ASSERT_TRUE(second.contains("result"));
    EXPECT_FALSE(second["result"]["isError"]);
    EXPECT_TRUE(m_Server.PendingConsents().empty());
    EXPECT_EQ(CurrentLayer(), 7u);
}

// No answer within the deadline is a clean timeout error, and nothing is mutated.
TEST_F(McpConsentedWriteTest, PromptModeTimesOutWhenUnanswered)
{
    m_Server.SetWriteConsentMode(WriteConsentMode::Prompt);
    m_Server.SetConsentTimeout(std::chrono::milliseconds(40));

    // Runs on this thread: RequestConsent blocks ~40ms, then returns Timeout.
    const Json resp = m_Server.HandleMessage(
        MakeCallRequest(14, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 3 } }));

    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);
    EXPECT_EQ(CurrentLayer(), kInitialLayer);
    EXPECT_FALSE(m_History.CanUndo());
}

// ---- cancellation while parked on the consent modal (issue #610) -----------
// A notifications/cancelled arriving while a write is blocked on the Prompt-mode
// modal must abort the wait promptly, return a cancelled response, run NO write,
// and drain the pending prompt — the whole point of registering the in-flight
// cancel flag BEFORE the consent gate.

TEST_F(McpConsentedWriteTest, PromptModeCancelledWhileParkedReturnsCancelled)
{
    m_Server.SetWriteConsentMode(WriteConsentMode::Prompt);
    // A generous consent timeout so a Timeout can't masquerade as the cancellation:
    // if cancellation did NOT reach the wait, the call would block far past the test.
    m_Server.SetConsentTimeout(std::chrono::seconds(30));

    std::future<Json> call = std::async(std::launch::async, [this]
                                        { return m_Server.HandleMessage(
                                              MakeCallRequest(20, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 3 } })); });

    const u64 id = WaitForPendingConsent();
    ASSERT_NE(id, 0u) << "no consent prompt surfaced";
    EXPECT_EQ(CurrentLayer(), kInitialLayer); // not applied while parked

    // Cancel by the call's JSON-RPC id (a number, matched verbatim). This wakes the
    // parked RequestConsent wait through m_ConsentCv.
    const Json ack = m_Server.HandleMessage(MakeCancelNotification(20));
    EXPECT_TRUE(ack.is_null()); // a notification produces no response

    const Json resp = call.get();
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_FALSE(resp.contains("result"));
    EXPECT_EQ(resp["error"]["code"], kRequestCancelledCode);

    // The write never ran, and the in-flight prompt was reaped (queue drained).
    EXPECT_EQ(CurrentLayer(), kInitialLayer);
    EXPECT_FALSE(m_History.CanUndo());
    EXPECT_TRUE(m_Server.PendingConsents().empty());
}

// Cancellation wins over a racing human decision: once the cancel flag is set, a
// later Approve on the (now-reaped) prompt is a no-op and the write stays unapplied.
TEST_F(McpConsentedWriteTest, PromptModeCancellationWinsOverLateApprove)
{
    m_Server.SetWriteConsentMode(WriteConsentMode::Prompt);
    m_Server.SetConsentTimeout(std::chrono::seconds(30));

    std::future<Json> call = std::async(std::launch::async, [this]
                                        { return m_Server.HandleMessage(
                                              MakeCallRequest(21, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 3 } })); });

    const u64 id = WaitForPendingConsent();
    ASSERT_NE(id, 0u);

    // Cancel first — the worker wakes, returns Cancel, and reaps its entry. The
    // subsequent Approve finds no live prompt for `id` and is a no-op.
    (void)m_Server.HandleMessage(MakeCancelNotification(21));
    const Json resp = call.get();
    m_Server.ResolveConsent(id, ConsentDecision::Approve); // late, stale id → no-op

    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], kRequestCancelledCode);
    EXPECT_EQ(CurrentLayer(), kInitialLayer);
    EXPECT_FALSE(m_History.CanUndo());
    EXPECT_TRUE(m_Server.PendingConsents().empty());
}

// A cancellation whose requestId does NOT match the parked call must not disturb
// it: the real call still resolves normally via its Approve. Guards against an
// over-broad wake that cancels the wrong (or every) in-flight write.
TEST_F(McpConsentedWriteTest, PromptModeUnrelatedCancelDoesNotAbortWrite)
{
    m_Server.SetWriteConsentMode(WriteConsentMode::Prompt);
    m_Server.SetConsentTimeout(std::chrono::seconds(30));

    std::future<Json> call = std::async(std::launch::async, [this]
                                        { return m_Server.HandleMessage(
                                              MakeCallRequest(22, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 3 } })); });

    const u64 id = WaitForPendingConsent();
    ASSERT_NE(id, 0u);

    // Cancel a DIFFERENT id — a spec-sanctioned no-op: the id isn't in the in-flight
    // registry, so no flag is set (and m_ConsentCv isn't even woken). Our prompt
    // stays pending after it.
    (void)m_Server.HandleMessage(MakeCancelNotification(9999));
    ASSERT_NE(WaitForPendingConsent(), 0u) << "unrelated cancel wrongly drained the prompt";
    EXPECT_EQ(CurrentLayer(), kInitialLayer);

    // The real decision still applies the write.
    m_Server.ResolveConsent(id, ConsentDecision::Approve);
    const Json resp = call.get();
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp["result"]["isError"]);
    EXPECT_EQ(CurrentLayer(), 3u);
    EXPECT_TRUE(m_History.CanUndo());
}

// AllowSession (== the legacy gate-on) never prompts — the queue stays empty.
TEST_F(McpConsentedWriteTest, AllowSessionAppliesWithoutPrompt)
{
    m_Server.SetWriteConsentMode(WriteConsentMode::AllowSession);
    const Json resp = m_Server.HandleMessage(
        MakeCallRequest(15, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 3 } }));
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp["result"]["isError"]);
    EXPECT_TRUE(m_Server.PendingConsents().empty());
    EXPECT_EQ(CurrentLayer(), 3u);
}

// ---- server-side inputSchema enforcement (#423), gate ON -------------------

TEST_F(McpConsentedWriteTest, SchemaRejectsLayerBelowMinimum)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(3, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", -1 } }));
    ASSERT_TRUE(resp.contains("result")); // SEP-1303: schema failures are tool errors
    EXPECT_EQ(resp["result"]["isError"], true);
    EXPECT_EQ(CurrentLayer(), kInitialLayer); // never applied
}

TEST_F(McpConsentedWriteTest, SchemaRejectsLayerAboveMaximum)
{
    m_Server.SetAllowWrites(true);
    // Derive the over-limit value from the shared cap so this stays valid if it changes.
    const int overMax = static_cast<int>(SetCollisionLayer::kMaxLayerId) + 1;
    const Json resp = m_Server.HandleMessage(MakeCallRequest(4, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", overMax } }));
    ASSERT_TRUE(resp.contains("result")); // SEP-1303: schema failures are tool errors
    EXPECT_EQ(resp["result"]["isError"], true);
}

TEST_F(McpConsentedWriteTest, SchemaRejectsMissingEntity)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(5, Json{ { "layer", 2 } }));
    ASSERT_TRUE(resp.contains("result")); // SEP-1303: schema failures are tool errors
    EXPECT_EQ(resp["result"]["isError"], true);
}

TEST_F(McpConsentedWriteTest, SchemaRejectsNonIntegerLayer)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(6, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", "3" } }));
    ASSERT_TRUE(resp.contains("result")); // SEP-1303: schema failures are tool errors
    EXPECT_EQ(resp["result"]["isError"], true);
}

TEST_F(McpConsentedWriteTest, SchemaRejectsUnknownProperty)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(
        MakeCallRequest(7, Json{ { "entity", std::to_string(m_EntityUuid) }, { "layer", 2 }, { "extra", true } }));
    ASSERT_TRUE(resp.contains("result")); // SEP-1303: schema failures are tool errors
    EXPECT_EQ(resp["result"]["isError"], true);
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

// The entity contract is a decimal-digit STRING (matching the inputSchema, which the
// server enforces before the handler runs). A numeric entity is rejected — passing a
// u64 UUID as a JSON number would also lose precision above 2^53.
TEST(McpSetCollisionLayerParse, RejectsNumericUuid)
{
    u64 entity = 0;
    u32 layer = 0;
    EXPECT_TRUE(SetCollisionLayer::ParseArgs(Json{ { "entity", 42 }, { "layer", 0 } }, entity, layer).has_value());
}

// std::stoull would otherwise silently accept these; require the whole string to be digits.
TEST(McpSetCollisionLayerParse, RejectsPartialOrSignedUuidString)
{
    u64 entity = 0;
    u32 layer = 0;
    EXPECT_TRUE(SetCollisionLayer::ParseArgs(Json{ { "entity", "42abc" }, { "layer", 1 } }, entity, layer).has_value());
    EXPECT_TRUE(SetCollisionLayer::ParseArgs(Json{ { "entity", "-1" }, { "layer", 1 } }, entity, layer).has_value());
    EXPECT_TRUE(SetCollisionLayer::ParseArgs(Json{ { "entity", "" }, { "layer", 1 } }, entity, layer).has_value());
    EXPECT_TRUE(SetCollisionLayer::ParseArgs(Json{ { "entity", " 42" }, { "layer", 1 } }, entity, layer).has_value());
}

TEST(McpSetCollisionLayerParse, RejectsOutOfRangeAndMissing)
{
    u64 entity = 0;
    u32 layer = 0;
    const int overMax = static_cast<int>(SetCollisionLayer::kMaxLayerId) + 1;
    EXPECT_TRUE(SetCollisionLayer::ParseArgs(Json{ { "entity", "1" }, { "layer", -1 } }, entity, layer).has_value());
    EXPECT_TRUE(SetCollisionLayer::ParseArgs(Json{ { "entity", "1" }, { "layer", overMax } }, entity, layer).has_value());
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
