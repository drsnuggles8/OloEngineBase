#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpSceneControlTest — unit test (headless, no GL, no live editor, no scene).
//
// Pins the consented MCP scene-control write tools olo_scene_open / olo_scene_play /
// olo_scene_stop (issue #316 Part 5): the scriptable scene switch + play-mode toggle.
// Two seams, the same shape as McpReloadScriptTest / McpConsentedWriteTest:
//
//   1. The dispatch seam (McpServer.cpp, compiled into the test binary): a tool
//      flagged ToolDef::ProjectWrite is REFUSED with a clean JSON-RPC error while
//      the session "Allow writes" gate is off, and ACCEPTED once on — and the scene
//      ACTION does NOT run while the gate is off. The server's inputSchema
//      enforcement (#423) rejects a malformed call before the handler runs.
//      McpTools.cpp is deliberately NOT linked here, so the test registers fake
//      tools wired to the SAME schema + result shaping the real handlers use.
//
//   2. The shared shaping + validation (MCP/McpSceneControl.h, header-only): the
//      inputSchemas, the pure ValidateScenePath / LowercaseExtension helpers, and
//      the McpSceneOpenResult / McpScenePlayResult -> JSON ToJson the real handlers
//      emit. The scene ACTIONS themselves are context hooks (the EnTT registry /
//      runtime in the editor), so the tests inject fakes and never touch a scene —
//      the same indirection the real handlers delegate to on the main thread.
//
// The shared header is renderer/httplib/engine-scene-free, so only McpServer.h + the
// schema DSL are pulled in — no extra editor TU.
// =============================================================================

#include "MCP/McpSceneControl.h"
#include "MCP/McpServer.h"

#include <optional>
#include <string>

// OLO_TEST_LAYER: unit

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpSceneOpenResult;
    using OloEngine::MCP::McpScenePlayResult;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;
    namespace SceneControl = OloEngine::MCP::SceneControl;

    constexpr int kInvalidParams = -32602;

    Json MakeCallRequest(const Json& id, const std::string& tool, const Json& arguments)
    {
        return Json{ { "jsonrpc", "2.0" },
                     { "id", id },
                     { "method", "tools/call" },
                     { "params", { { "name", tool }, { "arguments", arguments } } } };
    }

    // Fixture: an McpServer whose tools are fake olo_scene_open / olo_scene_play /
    // olo_scene_stop, each wired through the SAME schema + ToJson the real handlers
    // use, but with the scene ACTIONS replaced by test-owned closures that record
    // their invocations and return canned results — so the dispatch gate, schema
    // enforcement, and result shaping are exercised without a live scene / game
    // thread (the real handlers marshal onto the main thread; here they run
    // synchronously). The handlers mirror the real ones: olo_scene_open validates
    // the path via ValidateScenePath before invoking, olo_scene_play/stop pass a
    // fixed bool.
    class McpSceneControlTest : public ::testing::Test
    {
      protected:
        McpSceneControlTest()
            : m_Server(EditorMcpContext{})
        {
            {
                ToolDef tool;
                tool.Name = "olo_scene_open";
                tool.Description = "Open/switch scene (fake; test wiring).";
                tool.ProjectWrite = true;
                tool.InputSchema = SceneControl::OpenInputSchema();
                tool.Handler = [this](McpServer&, const Json& args) -> ToolResult
                {
                    const std::string path = args.value("path", std::string{});
                    if (const auto error = SceneControl::ValidateScenePath(path))
                        return ToolResult::Error(*error);
                    ++m_OpenCount;
                    m_LastOpenPath = path;
                    return ToolResult::Text(SceneControl::ToJson(m_FakeOpenResult).dump());
                };
                m_Server.RegisterTool(std::move(tool));
            }
            {
                ToolDef tool;
                tool.Name = "olo_scene_play";
                tool.Description = "Enter Play mode (fake; test wiring).";
                tool.ProjectWrite = true;
                tool.InputSchema = SceneControl::PlayStopInputSchema();
                tool.Handler = [this](McpServer&, const Json&) -> ToolResult
                {
                    ++m_PlayCount;
                    return ToolResult::Text(SceneControl::ToJson(m_FakePlayResult).dump());
                };
                m_Server.RegisterTool(std::move(tool));
            }
            {
                ToolDef tool;
                tool.Name = "olo_scene_stop";
                tool.Description = "Stop Play mode (fake; test wiring).";
                tool.ProjectWrite = true;
                tool.InputSchema = SceneControl::PlayStopInputSchema();
                tool.Handler = [this](McpServer&, const Json&) -> ToolResult
                {
                    ++m_StopCount;
                    return ToolResult::Text(SceneControl::ToJson(m_FakeStopResult).dump());
                };
                m_Server.RegisterTool(std::move(tool));
            }
        }

        McpSceneOpenResult m_FakeOpenResult;
        McpScenePlayResult m_FakePlayResult;
        McpScenePlayResult m_FakeStopResult;
        int m_OpenCount = 0;
        int m_PlayCount = 0;
        int m_StopCount = 0;
        std::string m_LastOpenPath;
        McpServer m_Server; // declared last → destroyed first (its handlers ref members)
    };
} // namespace

// ---- the session write gate (dispatch seam) --------------------------------

// Default state: the gate is OFF, so even well-formed scene-control calls are
// refused with a JSON-RPC error and the scene action NEVER runs.
TEST_F(McpSceneControlTest, GateOffRejectsAllAndDoesNotInvoke)
{
    ASSERT_FALSE(m_Server.AllowWrites()); // off by default

    const Json openResp = m_Server.HandleMessage(MakeCallRequest(1, "olo_scene_open", Json{ { "path", "Scenes/Sandbox.olo" } }));
    ASSERT_TRUE(openResp.contains("error"));
    EXPECT_EQ(openResp["error"]["code"], kInvalidParams);
    EXPECT_EQ(m_OpenCount, 0);

    const Json playResp = m_Server.HandleMessage(MakeCallRequest(2, "olo_scene_play", Json::object()));
    ASSERT_TRUE(playResp.contains("error"));
    EXPECT_EQ(m_PlayCount, 0);

    const Json stopResp = m_Server.HandleMessage(MakeCallRequest(3, "olo_scene_stop", Json::object()));
    ASSERT_TRUE(stopResp.contains("error"));
    EXPECT_EQ(m_StopCount, 0);
}

// With the gate ON, olo_scene_open runs the action once and returns the hook's
// shaped result (available/ok/path/sceneName/entityCount).
TEST_F(McpSceneControlTest, GateOnOpenInvokesAndReturnsResult)
{
    m_Server.SetAllowWrites(true);
    m_FakeOpenResult.Available = true;
    m_FakeOpenResult.Ok = true;
    m_FakeOpenResult.Path = "C:/proj/Assets/Scenes/Sandbox.olo";
    m_FakeOpenResult.SceneName = "Sandbox";
    m_FakeOpenResult.EntityCount = 42;
    m_FakeOpenResult.Message = "Opened scene 'Sandbox' (42 entities).";

    const Json resp = m_Server.HandleMessage(MakeCallRequest(4, "olo_scene_open", Json{ { "path", "Scenes/Sandbox.olo" } }));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp["result"]["isError"]);
    EXPECT_EQ(m_OpenCount, 1);
    EXPECT_EQ(m_LastOpenPath, "Scenes/Sandbox.olo");

    const Json payload = Json::parse(resp["result"]["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(payload["available"].get<bool>());
    EXPECT_TRUE(payload["ok"].get<bool>());
    EXPECT_EQ(payload["sceneName"], "Sandbox");
    EXPECT_EQ(payload["entityCount"], 42);
}

// With the gate ON, olo_scene_play / olo_scene_stop each run once and carry the
// resulting play state through.
TEST_F(McpSceneControlTest, GateOnPlayStopInvokeAndReturnResult)
{
    m_Server.SetAllowWrites(true);

    m_FakePlayResult.Available = true;
    m_FakePlayResult.Ok = true;
    m_FakePlayResult.Playing = true;
    m_FakePlayResult.Changed = true;
    const Json playResp = m_Server.HandleMessage(MakeCallRequest(5, "olo_scene_play", Json::object()));
    ASSERT_TRUE(playResp.contains("result"));
    EXPECT_EQ(m_PlayCount, 1);
    const Json playPayload = Json::parse(playResp["result"]["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(playPayload["playing"].get<bool>());
    EXPECT_TRUE(playPayload["changed"].get<bool>());

    m_FakeStopResult.Available = true;
    m_FakeStopResult.Ok = true;
    m_FakeStopResult.Playing = false;
    m_FakeStopResult.Changed = true;
    const Json stopResp = m_Server.HandleMessage(MakeCallRequest(6, "olo_scene_stop", Json::object()));
    ASSERT_TRUE(stopResp.contains("result"));
    EXPECT_EQ(m_StopCount, 1);
    const Json stopPayload = Json::parse(stopResp["result"]["content"][0]["text"].get<std::string>());
    EXPECT_FALSE(stopPayload["playing"].get<bool>());
}

// Entering Play when the scene has no primary camera is a clean result — the call
// SUCCEEDS at the protocol level but ok:false / changed:false report the real
// outcome, so an agent isn't told a failed transition worked.
TEST_F(McpSceneControlTest, PlayFailureReportsOkFalse)
{
    m_Server.SetAllowWrites(true);
    m_FakePlayResult.Available = true;
    m_FakePlayResult.Ok = false;
    m_FakePlayResult.Playing = false;
    m_FakePlayResult.Changed = false;
    m_FakePlayResult.Message = "Could not enter Play mode: the scene has no primary CameraComponent.";

    const Json resp = m_Server.HandleMessage(MakeCallRequest(7, "olo_scene_play", Json::object()));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp["result"]["isError"]);
    const Json payload = Json::parse(resp["result"]["content"][0]["text"].get<std::string>());
    EXPECT_FALSE(payload["ok"].get<bool>());
    EXPECT_FALSE(payload["playing"].get<bool>());
}

// ---- server-side inputSchema enforcement (#423), gate ON -------------------

// olo_scene_open's schema requires `path`; omitting it is rejected at the schema
// layer before the handler runs.
TEST_F(McpSceneControlTest, OpenSchemaRejectsMissingPath)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(8, "olo_scene_open", Json::object()));

    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);
    EXPECT_EQ(m_OpenCount, 0);
}

// olo_scene_play/stop take no arguments — an unexpected property is rejected before
// the handler runs.
TEST_F(McpSceneControlTest, PlaySchemaRejectsUnknownProperty)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(9, "olo_scene_play", Json{ { "speed", 2 } }));

    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);
    EXPECT_EQ(m_PlayCount, 0);
}

// A valid `path` string passes the schema but a bad EXTENSION is caught by the
// handler's ValidateScenePath (a value-level constraint JSON-Schema can't express),
// so the scene action never runs and the call is a tool error.
TEST_F(McpSceneControlTest, OpenHandlerRejectsBadExtension)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(10, "olo_scene_open", Json{ { "path", "notes.txt" } }));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_TRUE(resp["result"]["isError"]);
    EXPECT_EQ(m_OpenCount, 0);
}

// ---- the shared shaping + validation core (MCP/McpSceneControl.h) ----------

TEST(McpSceneControlShaping, OpenToJsonCarriesEveryField)
{
    McpSceneOpenResult r;
    r.Available = true;
    r.Ok = true;
    r.Path = "C:/proj/Scenes/A.olo";
    r.SceneName = "A";
    r.EntityCount = 3;
    r.Message = "ok";

    const Json j = SceneControl::ToJson(r);
    EXPECT_TRUE(j["available"].get<bool>());
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_EQ(j["path"], "C:/proj/Scenes/A.olo");
    EXPECT_EQ(j["sceneName"], "A");
    EXPECT_EQ(j["entityCount"], 3);
    EXPECT_EQ(j["message"], "ok");
}

TEST(McpSceneControlShaping, PlayToJsonCarriesEveryField)
{
    McpScenePlayResult r;
    r.Available = true;
    r.Ok = true;
    r.Playing = true;
    r.Changed = true;
    r.SceneName = "A";
    r.Message = "Entered Play mode.";

    const Json j = SceneControl::ToJson(r);
    EXPECT_TRUE(j["available"].get<bool>());
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_TRUE(j["playing"].get<bool>());
    EXPECT_TRUE(j["changed"].get<bool>());
    EXPECT_EQ(j["sceneName"], "A");
}

TEST(McpSceneControlShaping, OpenInputSchemaRequiresPath)
{
    const Json schema = SceneControl::OpenInputSchema();
    EXPECT_EQ(schema["type"], "object");
    ASSERT_TRUE(schema.contains("required"));
    ASSERT_TRUE(schema["required"].is_array());
    EXPECT_EQ(schema["required"][0], "path");
    ASSERT_TRUE(schema.contains("additionalProperties"));
    EXPECT_FALSE(schema["additionalProperties"].get<bool>());
}

TEST(McpSceneControlShaping, PlayStopSchemaIsClosedEmptyObject)
{
    const Json schema = SceneControl::PlayStopInputSchema();
    EXPECT_EQ(schema["type"], "object");
    ASSERT_TRUE(schema.contains("additionalProperties"));
    EXPECT_FALSE(schema["additionalProperties"].get<bool>());
}

// ValidateScenePath: accepts .olo / .scene (case-insensitive), rejects empty, a bad
// extension, and parent-directory traversal.
TEST(McpSceneControlValidation, ValidateScenePathAcceptsSceneFiles)
{
    EXPECT_FALSE(SceneControl::ValidateScenePath("Scenes/Sandbox.olo").has_value());
    EXPECT_FALSE(SceneControl::ValidateScenePath("A.scene").has_value());
    EXPECT_FALSE(SceneControl::ValidateScenePath("Deep/Nested/Path/level.OLO").has_value()); // case-insensitive
    EXPECT_FALSE(SceneControl::ValidateScenePath("C:/abs/path/level.olo").has_value());      // absolute allowed
}

TEST(McpSceneControlValidation, ValidateScenePathRejectsBadInputs)
{
    EXPECT_TRUE(SceneControl::ValidateScenePath("").has_value());                   // empty
    EXPECT_TRUE(SceneControl::ValidateScenePath("notes.txt").has_value());          // bad extension
    EXPECT_TRUE(SceneControl::ValidateScenePath("Scenes").has_value());             // no extension
    EXPECT_TRUE(SceneControl::ValidateScenePath("../secrets/x.olo").has_value());   // traversal
    EXPECT_TRUE(SceneControl::ValidateScenePath("Scenes/../../x.olo").has_value()); // traversal mid-path
    EXPECT_TRUE(SceneControl::ValidateScenePath("Scenes\\..\\x.olo").has_value());  // traversal, backslash
    // A filename that merely CONTAINS ".." between dots is fine (not a "." component).
    EXPECT_FALSE(SceneControl::ValidateScenePath("weird..name.olo").has_value());
}

TEST(McpSceneControlValidation, LowercaseExtensionHandlesEdgeCases)
{
    EXPECT_EQ(SceneControl::LowercaseExtension("a/b/c.OLO"), ".olo");
    EXPECT_EQ(SceneControl::LowercaseExtension("Scene.Scene"), ".scene");
    EXPECT_EQ(SceneControl::LowercaseExtension("noext"), "");
    EXPECT_EQ(SceneControl::LowercaseExtension("dir.with.dot/file"), ""); // dot in dir, not leaf
    EXPECT_EQ(SceneControl::LowercaseExtension(".gitignore"), "");        // dotfile → no extension
}
