#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpInputInjectTest — unit test (headless, no GL, no window, no live editor).
//
// Pins the consented MCP synthetic-input WRITE tool olo_input_inject (issue #607):
// inject a click / drag / key / text into the running editor so an agent can verify
// that an interactive handler actually FIRES, not just that the editor renders.
//
// Two seams, the same shape as McpSceneControlTest / McpReloadScriptTest:
//
//   1. The dispatch seam (McpServer.cpp, compiled into the test binary): a tool
//      flagged ToolDef::ProjectWrite is REFUSED while the session "Allow writes" gate
//      is off — and the injection ACTION does not run. The server's inputSchema
//      enforcement rejects a malformed call before the handler runs.
//
//   2. The pure core (MCP/McpInputInject.h, header-only): key-name -> GLFW keycode
//      mapping, argument parsing, the coordinate-space math (the part that makes the
//      tool a liar if it is off by a pixel), and the frame-quantized PLAN — the
//      load-bearing timing that decides whether an injected click is seen at all.
//      The plan invariants tested here are the ones a wrong implementation silently
//      violates: a button is never pressed AND released in the same frame; the press
//      trails the cursor move by enough frames for the editor's two-frame-latent
//      async picking to have caught up; modifiers are armed before the press and
//      released after it.
//
// The ACTIONS themselves are EditorMcpContext hooks (ImGui / GLFW / the ECS in the
// editor), so the dispatch tests inject fakes and never touch a window.
// =============================================================================

#include "MCP/McpInputInject.h"
#include "MCP/McpServer.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

// OLO_TEST_LAYER: unit

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpInputEvent;
    using OloEngine::MCP::McpInputInjectResult;
    using OloEngine::MCP::McpInputPlan;
    using OloEngine::MCP::McpInputStateSnapshot;
    using OloEngine::MCP::McpInputViewportInfo;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;
    namespace Inject = OloEngine::MCP::InputInject;
    namespace Key = OloEngine::Key;
    namespace Mouse = OloEngine::Mouse;

    constexpr int kInvalidParams = -32602;

    Json MakeCallRequest(const Json& id, const std::string& tool, const Json& arguments)
    {
        return Json{ { "jsonrpc", "2.0" },
                     { "id", id },
                     { "method", "tools/call" },
                     { "params", { { "name", tool }, { "arguments", arguments } } } };
    }

    // A 1280x720 viewport whose panel sits at (300, 100) inside a 1920x1080 window
    // positioned at desktop (50, 40) — i.e. multi-viewport is on, so ImGui screen
    // coordinates are desktop coordinates. DpiScale 1 keeps the arithmetic readable;
    // a separate test covers the 2x case.
    McpInputViewportInfo MakeViewportInfo(f32 dpiScale = 1.0f)
    {
        McpInputViewportInfo info;
        info.Available = true;
        info.PanelX = 350.0f; // 50 (window) + 300 (panel offset within the window)
        info.PanelY = 140.0f; // 40 (window) + 100
        info.WindowX = 50.0f;
        info.WindowY = 40.0f;
        info.LogicalWidth = 1280.0f;
        info.LogicalHeight = 720.0f;
        info.DpiScale = dpiScale;
        info.WindowWidth = 1920;
        info.WindowHeight = 1080;
        return info;
    }

    // Count press/release events for a button across the whole plan, and record which
    // frame index each landed on.
    struct ButtonTimeline
    {
        std::vector<int> PressFrames;
        std::vector<int> ReleaseFrames;
        std::vector<int> MoveFrames;
    };

    ButtonTimeline Timeline(const McpInputPlan& plan)
    {
        ButtonTimeline timeline;
        for (int frameIndex = 0; frameIndex < static_cast<int>(plan.Frames.size()); ++frameIndex)
        {
            for (const McpInputEvent& event : plan.Frames[static_cast<std::size_t>(frameIndex)])
            {
                if (event.Type == McpInputEvent::Kind::MouseButton)
                {
                    if (event.Down)
                        timeline.PressFrames.push_back(frameIndex);
                    else
                        timeline.ReleaseFrames.push_back(frameIndex);
                }
                else if (event.Type == McpInputEvent::Kind::MousePos)
                {
                    timeline.MoveFrames.push_back(frameIndex);
                }
            }
        }
        return timeline;
    }

    // Fixture: an McpServer hosting a FAKE olo_input_inject wired through the SAME
    // schema + parse + plan-builder the real handler uses, but with the injection
    // ACTION replaced by a test-owned closure that records the plan it was handed.
    // Exercises the write gate, schema enforcement, and the pure core end to end
    // without a window or a game thread (the real handler marshals; here it is
    // synchronous).
    class McpInputInjectTest : public ::testing::Test
    {
      protected:
        McpInputInjectTest()
            : m_Server(EditorMcpContext{})
        {
            ToolDef tool;
            tool.Name = "olo_input_inject";
            tool.Description = "Inject synthetic input (fake; test wiring).";
            tool.ProjectWrite = true;
            tool.InputSchema = Inject::InputSchema();
            tool.Handler = [this](McpServer&, const Json& args) -> ToolResult
            {
                Inject::Request request;
                if (const auto error = Inject::ParseRequest(args, request))
                    return ToolResult::Error(*error);

                McpInputPlan plan;
                Inject::ResolvedPoint start;
                Inject::ResolvedPoint end;
                if (const auto error = Inject::BuildPlan(request, m_ViewportInfo, plan, start, end))
                    return ToolResult::Error(*error);

                ++m_InjectCount;
                m_LastPlan = plan;
                m_LastStart = start;

                McpInputInjectResult result;
                result.Available = true;
                result.Ok = true;
                result.FrameCount = static_cast<u32>(plan.Frames.size());

                // Model a LIVE editor reporting back where the injected cursor landed
                // (issue #854). By default it landed exactly where the plan asked, so
                // every pre-existing expectation of ok == true still exercises the
                // verification rather than skipping it. m_CursorLandingOffset makes
                // the editor report the position being overridden, which is the real
                // failure this test file has to keep pinned.
                McpInputStateSnapshot state;
                state.Available = true;
                if (Inject::ActionMovesTheCursor(request.Act))
                {
                    state.CursorLandingValid = m_ReportCursorLanding;
                    state.CursorAskedX = end.WindowX;
                    state.CursorAskedY = end.WindowY;
                    state.CursorLandedX = end.WindowX + m_CursorLandingOffsetX;
                    state.CursorLandedY = end.WindowY + m_CursorLandingOffsetY;
                }
                state.HoveredWindowName = m_HoveredWindowName;
                state.HoveredId = m_HoveredId;
                state.ActiveId = m_ActiveId;
                return ToolResult::Text(
                    Inject::ToJson(result, state, m_ViewportInfo, request, start, end, /*timedOut*/ false).dump());
            };
            m_Server.RegisterTool(std::move(tool));
        }

        McpInputViewportInfo m_ViewportInfo = MakeViewportInfo();
        // How far the editor reports the cursor ACTUALLY landed from where the plan
        // aimed it, and whether it measured at all (a host with no window does not).
        bool m_ReportCursorLanding = true;
        f32 m_CursorLandingOffsetX = 0.0f;
        f32 m_CursorLandingOffsetY = 0.0f;
        int m_InjectCount = 0;
        // What ImGui's hit-test resolved (issue #921) — defaults to "nothing
        // hovered", the honest answer for a fake with no ImGui context at all.
        std::string m_HoveredWindowName;
        u32 m_HoveredId = 0;
        u32 m_ActiveId = 0;
        McpInputPlan m_LastPlan;
        Inject::ResolvedPoint m_LastStart;
        McpServer m_Server; // declared last => destroyed first (its handler refs members)
    };
} // namespace

// ---- the session write gate (dispatch seam) ---------------------------------

// Injection can move a gizmo or delete an entity, so it MUST be gated: with "Allow
// writes" off a well-formed call is refused and no input is injected.
TEST_F(McpInputInjectTest, GateOffRejectsAndDoesNotInject)
{
    ASSERT_FALSE(m_Server.AllowWrites());

    const Json response = m_Server.HandleMessage(
        MakeCallRequest(1, "olo_input_inject", Json{ { "action", "click" }, { "x", 100 }, { "y", 100 } }));

    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"]["code"], kInvalidParams);
    EXPECT_EQ(m_InjectCount, 0);
}

TEST_F(McpInputInjectTest, GateOnInjectsAndReportsResolvedPoint)
{
    m_Server.SetAllowWrites(true);

    const Json response = m_Server.HandleMessage(
        MakeCallRequest(2, "olo_input_inject", Json{ { "action", "click" }, { "x", 640 }, { "y", 360 } }));

    ASSERT_TRUE(response.contains("result"));
    EXPECT_FALSE(response["result"]["isError"]);
    EXPECT_EQ(m_InjectCount, 1);

    const Json payload = Json::parse(response["result"]["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(payload["ok"].get<bool>());
    EXPECT_GT(payload["framesInjected"].get<int>(), 0);
    EXPECT_TRUE(payload["resolved"]["insideViewport"].get<bool>());
    // Panel (350, 140) in ImGui screen space, window at (50, 40) => window-client
    // origin of the viewport is (300, 100); +(640, 360) => (940, 460).
    EXPECT_FLOAT_EQ(payload["resolved"]["windowX"].get<f32>(), 940.0f);
    EXPECT_FLOAT_EQ(payload["resolved"]["windowY"].get<f32>(), 460.0f);
}

// ---- issue #854: a position that did not take must never report ok ----------
//
// The bug this pins is not a crash, it is a CONFIDENT WRONG ANSWER, which is the most
// expensive kind here. An injected cursor position is a REQUEST to the ImGui GLFW
// backend, and the backend can overrule it: ImGui_ImplGlfw_UpdateMouseData()
// re-injects the HARDWARE cursor position whenever the window is focused and the
// physical mouse is not over it — the normal state for an agent-driven session. That
// runs inside ImGui_ImplGlfw_NewFrame, i.e. AFTER the editor has drained the plan, so
// the hardware position was the last word: every injected click landed wherever the
// physical mouse happened to be, while the tool reported ok: true and echoed back a
// resolved point it never actually used.
//
// Measured live before the fix, the same call twice with only the physical mouse
// moving:
//   physical mouse INSIDE  the window -> ImGui cursor (60, 155)   == asked, click landed
//   physical mouse OUTSIDE the window -> ImGui cursor (927, 1313) != asked, click inert
// Both replies said ok: true. That also explains the "latch": the condition is a
// property of where the physical mouse is sitting, not of the drag that appeared to
// start it, so every LATER injection in the session was equally inert.
//
// EditorLayer now holds the backend's cursor-enter latch for the duration of a plan so
// the injected position wins, and MEASURES where it landed. These tests pin the second
// half, which is the part that must survive: "we believe the first half works" is
// exactly the reasoning that produced a lying tool, so a landing that disagrees has to
// fail loudly no matter WHY it disagreed.

TEST_F(McpInputInjectTest, CursorThatDidNotLandWhereAskedIsNotReportedOk)
{
    m_Server.SetAllowWrites(true);
    // The editor reports ImGui's cursor hundreds of pixels away — the measured
    // signature of the position being overridden by the hardware one.
    m_CursorLandingOffsetX = 867.0f;
    m_CursorLandingOffsetY = 853.0f;

    const Json response = m_Server.HandleMessage(
        MakeCallRequest(70, "olo_input_inject", Json{ { "action", "click" }, { "x", 640 }, { "y", 360 } }));

    ASSERT_TRUE(response.contains("result"));
    const Json payload = Json::parse(response["result"]["content"][0]["text"].get<std::string>());

    // The point: the events WERE injected and the frames WERE rendered, and it still
    // must not claim success.
    EXPECT_EQ(m_InjectCount, 1);
    EXPECT_FALSE(payload["ok"].get<bool>());
    EXPECT_NE(payload["message"].get<std::string>().find("did not go where the injection asked"),
              std::string::npos)
        << payload["message"].get<std::string>();

    // ...and it must be readable rather than merely asserted, so an agent can tell
    // this apart from "the click landed and the UI ignored it".
    ASSERT_TRUE(payload["after"].contains("cursorLanding"));
    const Json& landing = payload["after"]["cursorLanding"];
    EXPECT_FALSE(landing["landed"].get<bool>());
    EXPECT_FLOAT_EQ(landing["askedX"].get<f32>(), 940.0f);
    EXPECT_FLOAT_EQ(landing["landedX"].get<f32>(), 1807.0f);
}

TEST_F(McpInputInjectTest, DragIsVerifiedAgainstItsEndPointNotItsStart)
{
    m_Server.SetAllowWrites(true);

    // A drag that lands back on its start point has not dragged: the press registered
    // and the moves did not. Verifying against the START would call that a success —
    // which is precisely the reported symptom (ok: true, no wire created).
    const Json arguments{ { "action", "drag" }, { "fromX", 100 }, { "fromY", 100 }, { "toX", 500 }, { "toY", 400 } };
    m_CursorLandingOffsetX = -400.0f;
    m_CursorLandingOffsetY = -300.0f;

    const Json response = m_Server.HandleMessage(MakeCallRequest(71, "olo_input_inject", arguments));
    const Json payload = Json::parse(response["result"]["content"][0]["text"].get<std::string>());
    EXPECT_FALSE(payload["ok"].get<bool>());

    // The same drag, landing on its end point, is the success case.
    m_CursorLandingOffsetX = 0.0f;
    m_CursorLandingOffsetY = 0.0f;
    const Json good = m_Server.HandleMessage(MakeCallRequest(72, "olo_input_inject", arguments));
    const Json goodPayload = Json::parse(good["result"]["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(goodPayload["ok"].get<bool>());
    EXPECT_TRUE(goodPayload["after"]["cursorLanding"]["landed"].get<bool>());
}

TEST_F(McpInputInjectTest, SubPixelLandingErrorIsStillASuccess)
{
    m_Server.SetAllowWrites(true);
    // Round-tripping a position through ImGui's screen-space conversion costs a
    // fraction of a pixel (measured live: asked 880.5, landed 880.0). If that tripped
    // the check the tool would cry wolf on every successful call, and a check nobody
    // believes is worse than no check at all.
    m_CursorLandingOffsetX = 0.5f;
    m_CursorLandingOffsetY = -1.0f;

    const Json response = m_Server.HandleMessage(
        MakeCallRequest(73, "olo_input_inject", Json{ { "action", "click" }, { "x", 640 }, { "y", 360 } }));
    const Json payload = Json::parse(response["result"]["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(payload["ok"].get<bool>());
    EXPECT_TRUE(payload["after"]["cursorLanding"]["landed"].get<bool>());
}

TEST_F(McpInputInjectTest, ActionsThatInjectNoPositionAreNotLandingChecked)
{
    m_Server.SetAllowWrites(true);
    // key / text / mouseDelta deliberately never move ImGui's cursor (mouseDelta drives
    // the poll-based Input:: API only), so there is nothing to verify and a stale
    // measurement must not be turned into a failure.
    m_CursorLandingOffsetX = 999.0f;
    m_CursorLandingOffsetY = 999.0f;

    for (const Json& arguments : { Json{ { "action", "key" }, { "key", "s" } },
                                   Json{ { "action", "text" }, { "text", "hi" } },
                                   Json{ { "action", "mouseDelta" }, { "dx", 40 }, { "dy", 0 } } })
    {
        const Json response = m_Server.HandleMessage(MakeCallRequest(74, "olo_input_inject", arguments));
        const Json payload = Json::parse(response["result"]["content"][0]["text"].get<std::string>());
        EXPECT_TRUE(payload["ok"].get<bool>()) << arguments.dump();
        EXPECT_FALSE(payload["after"].contains("cursorLanding")) << arguments.dump();
    }
}

TEST_F(McpInputInjectTest, AHostThatCannotMeasureTheLandingStillReportsOk)
{
    m_Server.SetAllowWrites(true);
    // A host with no editor window measures nothing. Treating "no measurement" as "did
    // not land" would make the tool unusable there; treating it as landed is the honest
    // reading, because nothing claimed otherwise.
    m_ReportCursorLanding = false;
    m_CursorLandingOffsetX = 500.0f;

    const Json response = m_Server.HandleMessage(
        MakeCallRequest(75, "olo_input_inject", Json{ { "action", "click" }, { "x", 640 }, { "y", 360 } }));
    const Json payload = Json::parse(response["result"]["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(payload["ok"].get<bool>());
    EXPECT_FALSE(payload["after"].contains("cursorLanding"));
}

// ---- issue #921: telling "reached the widget, widget ignored it" apart from
// "never reached the widget" ---------------------------------------------------
//
// `cursorLanding` (issue #854) only proves the injected POSITION took. It says
// nothing about whether ImGui's hit-test then routed that position to a widget —
// the exact gap #921 reports: a docked-panel click lands (`cursorLanding.landed:
// true`), reports `ok: true`, and the widget never reacts. `after.hoveredWindow` /
// `hoveredId` / `activeId` are read straight off ImGui's own hit-test result
// (EditorLayer's g.HoveredWindow / g.HoveredId / g.ActiveId), so a session can now
// distinguish the two failure shapes instead of guessing from silence.

TEST_F(McpInputInjectTest, HoveredWindowSetButNoItemMeansReachedTheWidgetAreaNotTheWidget)
{
    m_Server.SetAllowWrites(true);
    // ImGui resolved a window at the click point, but no specific item — the
    // "landed in the right neighborhood, missed the actual control" shape.
    m_HoveredWindowName = "Terrain Editor";
    m_HoveredId = 0;
    m_ActiveId = 0;

    const Json response = m_Server.HandleMessage(
        MakeCallRequest(76, "olo_input_inject", Json{ { "action", "click" }, { "x", 640 }, { "y", 360 } }));
    const Json payload = Json::parse(response["result"]["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(payload["ok"].get<bool>());
    EXPECT_EQ(payload["after"]["hoveredWindow"].get<std::string>(), "Terrain Editor");
    EXPECT_EQ(payload["after"]["hoveredId"].get<u32>(), 0u);
    EXPECT_EQ(payload["after"]["activeId"].get<u32>(), 0u);
}

TEST_F(McpInputInjectTest, NoHoveredWindowMeansTheClickNeverReachedAnyWidgetAtAll)
{
    m_Server.SetAllowWrites(true);
    // ImGui resolved NO window at all — a stronger, different failure than the
    // above: the click never reached anything ImGui knows about.
    // m_HoveredWindowName left at its default (empty).

    const Json response = m_Server.HandleMessage(
        MakeCallRequest(77, "olo_input_inject", Json{ { "action", "click" }, { "x", 640 }, { "y", 360 } }));
    const Json payload = Json::parse(response["result"]["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(payload["ok"].get<bool>());
    EXPECT_TRUE(payload["after"]["hoveredWindow"].is_null());
    EXPECT_EQ(payload["after"]["hoveredId"].get<u32>(), 0u);
}

TEST_F(McpInputInjectTest, HoveredWindowAndItemBothSetMeansTheClickReachedARealWidget)
{
    m_Server.SetAllowWrites(true);
    // The success shape: a window AND an item id, and (once the button transition
    // is processed) an active id — the click reached an actual control.
    m_HoveredWindowName = "Renderer Settings";
    m_HoveredId = 1234;
    m_ActiveId = 1234;

    const Json response = m_Server.HandleMessage(
        MakeCallRequest(78, "olo_input_inject", Json{ { "action", "click" }, { "x", 640 }, { "y", 360 } }));
    const Json payload = Json::parse(response["result"]["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(payload["ok"].get<bool>());
    EXPECT_EQ(payload["after"]["hoveredWindow"].get<std::string>(), "Renderer Settings");
    EXPECT_EQ(payload["after"]["hoveredId"].get<u32>(), 1234u);
    EXPECT_EQ(payload["after"]["activeId"].get<u32>(), 1234u);
}

TEST(McpInputInjectLanding, ToleranceCatchesTheRealFailureAndForgivesFloatNoise)
{
    // Pin both edges, so a future widening that would let the real failure through has
    // to change this test on purpose.
    EXPECT_TRUE(Inject::CursorLanded(940.0f, 460.0f, 940.0f, 460.0f));
    EXPECT_TRUE(Inject::CursorLanded(940.0f, 460.0f, 938.0f, 462.0f));
    EXPECT_FALSE(Inject::CursorLanded(940.0f, 460.0f, 937.9f, 460.0f));
    // The measured live failure: off by hundreds of pixels in both axes.
    EXPECT_FALSE(Inject::CursorLanded(60.0f, 155.0f, 927.0f, 1313.0f));
}

// ---- server-side inputSchema enforcement ------------------------------------

TEST_F(McpInputInjectTest, SchemaRejectsUnknownAction)
{
    m_Server.SetAllowWrites(true);
    const Json response =
        m_Server.HandleMessage(MakeCallRequest(3, "olo_input_inject", Json{ { "action", "teleport" } }));

    ASSERT_TRUE(response.contains("result")); // SEP-1303: schema failures are tool errors
    EXPECT_TRUE(response["result"]["isError"]);
    EXPECT_EQ(m_InjectCount, 0);
}

TEST_F(McpInputInjectTest, SchemaRejectsMissingAction)
{
    m_Server.SetAllowWrites(true);
    const Json response = m_Server.HandleMessage(MakeCallRequest(4, "olo_input_inject", Json{ { "x", 10 } }));

    ASSERT_TRUE(response.contains("result"));
    EXPECT_TRUE(response["result"]["isError"]);
    EXPECT_EQ(m_InjectCount, 0);
}

TEST_F(McpInputInjectTest, SchemaRejectsUnknownProperty)
{
    m_Server.SetAllowWrites(true);
    const Json response = m_Server.HandleMessage(
        MakeCallRequest(5, "olo_input_inject", Json{ { "action", "move" }, { "x", 1 }, { "y", 1 }, { "wat", 3 } }));

    ASSERT_TRUE(response.contains("result"));
    EXPECT_TRUE(response["result"]["isError"]);
    EXPECT_EQ(m_InjectCount, 0);
}

// A click outside the viewport is a handler-level (value) error the schema cannot
// express — and it fails LOUDLY rather than injecting at a clamped position, because
// a silently-relocated click is exactly the "tool is a liar" failure mode.
TEST_F(McpInputInjectTest, ClickOutsideViewportIsRejected)
{
    m_Server.SetAllowWrites(true);
    const Json response = m_Server.HandleMessage(
        MakeCallRequest(6, "olo_input_inject", Json{ { "action", "click" }, { "x", 5000 }, { "y", 10 } }));

    ASSERT_TRUE(response.contains("result"));
    EXPECT_TRUE(response["result"]["isError"]);
    EXPECT_EQ(m_InjectCount, 0);
}

// ---- key-name mapping (MCP/McpInputInject.h) --------------------------------

TEST(McpInputInjectKeys, PrintableCharactersMapToGlfwCodes)
{
    // GLFW's printable-key codes are the UPPERCASE ASCII values, so both cases fold
    // to the same code — an agent writing "s" or "S" gets Key::S either way.
    EXPECT_EQ(Inject::MapKeyName("s").value(), Key::S);
    EXPECT_EQ(Inject::MapKeyName("S").value(), Key::S);
    EXPECT_EQ(Inject::MapKeyName("a").value(), Key::A);
    EXPECT_EQ(Inject::MapKeyName("5").value(), Key::D5);
    EXPECT_EQ(Inject::MapKeyName("-").value(), Key::Minus);
    EXPECT_EQ(Inject::MapKeyName("/").value(), Key::Slash);
}

TEST(McpInputInjectKeys, NamedKeysMapCaseInsensitively)
{
    EXPECT_EQ(Inject::MapKeyName("escape").value(), Key::Escape);
    EXPECT_EQ(Inject::MapKeyName("ESC").value(), Key::Escape);
    EXPECT_EQ(Inject::MapKeyName("Delete").value(), Key::Delete);
    EXPECT_EQ(Inject::MapKeyName("f5").value(), Key::F5);
    EXPECT_EQ(Inject::MapKeyName("F12").value(), Key::F12);
    EXPECT_EQ(Inject::MapKeyName("pagedown").value(), Key::PageDown);
    EXPECT_EQ(Inject::MapKeyName("ctrl").value(), Key::LeftControl);
    EXPECT_EQ(Inject::MapKeyName("space").value(), Key::Space);
}

TEST(McpInputInjectKeys, UnknownNamesAreRejectedNotGuessed)
{
    EXPECT_FALSE(Inject::MapKeyName("").has_value());
    EXPECT_FALSE(Inject::MapKeyName("banana").has_value());
    EXPECT_FALSE(Inject::MapKeyName("ctrl+s").has_value()); // chords go through 'modifiers'
    EXPECT_FALSE(Inject::MapKeyName("\x01").has_value());   // non-printable byte
}

TEST(McpInputInjectKeys, ButtonAndSpaceNames)
{
    EXPECT_EQ(Inject::MapButtonName("left").value(), Mouse::ButtonLeft);
    EXPECT_EQ(Inject::MapButtonName("RIGHT").value(), Mouse::ButtonRight);
    EXPECT_EQ(Inject::MapButtonName("middle").value(), Mouse::ButtonMiddle);
    EXPECT_FALSE(Inject::MapButtonName("thumb").has_value());

    EXPECT_EQ(Inject::MapSpaceName("viewport").value(), Inject::Space::Viewport);
    EXPECT_EQ(Inject::MapSpaceName("viewportNorm").value(), Inject::Space::ViewportNormalized);
    EXPECT_EQ(Inject::MapSpaceName("window").value(), Inject::Space::Window);
    EXPECT_FALSE(Inject::MapSpaceName("screen").has_value());
}

// ---- coordinate spaces -------------------------------------------------------
// The contract that makes the tool honest: "a click at the pixel the agent saw in
// olo_screenshot must land on the thing it saw".

TEST(McpInputInjectCoords, ViewportPixelsMapThroughPanelAndWindowOrigin)
{
    const McpInputViewportInfo info = MakeViewportInfo();
    Inject::ResolvedPoint point;

    ASSERT_FALSE(Inject::ResolvePoint(info, Inject::Space::Viewport, 0.0f, 0.0f, point).has_value());
    // Viewport (0,0) == the panel's top-left == window-client (300, 100).
    EXPECT_FLOAT_EQ(point.WindowX, 300.0f);
    EXPECT_FLOAT_EQ(point.WindowY, 100.0f);
    EXPECT_TRUE(point.InsideViewport);

    ASSERT_FALSE(Inject::ResolvePoint(info, Inject::Space::Viewport, 1279.0f, 719.0f, point).has_value());
    EXPECT_FLOAT_EQ(point.WindowX, 1579.0f);
    EXPECT_FLOAT_EQ(point.WindowY, 819.0f);
    EXPECT_TRUE(point.InsideViewport);
}

TEST(McpInputInjectCoords, ViewportNormalizedIsDownscaleProof)
{
    const McpInputViewportInfo info = MakeViewportInfo();
    Inject::ResolvedPoint point;

    // The centre of the viewport, expressed fractionally — the mode an agent should
    // use after a maxWidth-downscaled screenshot.
    ASSERT_FALSE(Inject::ResolvePoint(info, Inject::Space::ViewportNormalized, 0.5f, 0.5f, point).has_value());
    EXPECT_FLOAT_EQ(point.ViewportPixelX, 640.0f);
    EXPECT_FLOAT_EQ(point.ViewportPixelY, 360.0f);
    EXPECT_FLOAT_EQ(point.WindowX, 940.0f);
    EXPECT_FLOAT_EQ(point.WindowY, 460.0f);
}

TEST(McpInputInjectCoords, HighDpiViewportPixelsAreDividedByDpiScale)
{
    // At 2x DPI the framebuffer olo_screenshot reads back is 2560x1440 while the panel
    // still occupies 1280x720 logical pixels. Native pixel (2560/2, 1440/2) is the
    // viewport's centre => the panel's centre in logical/window coordinates.
    const McpInputViewportInfo info = MakeViewportInfo(2.0f);
    EXPECT_FLOAT_EQ(Inject::ViewportPixelWidth(info), 2560.0f);
    EXPECT_FLOAT_EQ(Inject::ViewportPixelHeight(info), 1440.0f);

    Inject::ResolvedPoint point;
    ASSERT_FALSE(Inject::ResolvePoint(info, Inject::Space::Viewport, 1280.0f, 720.0f, point).has_value());
    EXPECT_FLOAT_EQ(point.WindowX, 300.0f + 640.0f);
    EXPECT_FLOAT_EQ(point.WindowY, 100.0f + 360.0f);
    EXPECT_TRUE(point.InsideViewport);

    // 2560 is one past the right edge at 2x — out of bounds, not silently clamped.
    EXPECT_TRUE(Inject::ResolvePoint(info, Inject::Space::Viewport, 2560.0f, 0.0f, point).has_value());
}

TEST(McpInputInjectCoords, WindowSpaceIsUsedAsIsAndReportsViewportContainment)
{
    const McpInputViewportInfo info = MakeViewportInfo();
    Inject::ResolvedPoint point;

    // A menu-bar click at window-client (40, 10): passes straight through, and is
    // correctly reported as OUTSIDE the viewport (the panel starts at (300, 100)).
    ASSERT_FALSE(Inject::ResolvePoint(info, Inject::Space::Window, 40.0f, 10.0f, point).has_value());
    EXPECT_FLOAT_EQ(point.WindowX, 40.0f);
    EXPECT_FLOAT_EQ(point.WindowY, 10.0f);
    EXPECT_FALSE(point.InsideViewport);

    // A window point that DOES fall in the viewport reports so, and its viewport-pixel
    // echo round-trips the panel offset.
    ASSERT_FALSE(Inject::ResolvePoint(info, Inject::Space::Window, 940.0f, 460.0f, point).has_value());
    EXPECT_TRUE(point.InsideViewport);
    EXPECT_FLOAT_EQ(point.ViewportPixelX, 640.0f);
    EXPECT_FLOAT_EQ(point.ViewportPixelY, 360.0f);
}

TEST(McpInputInjectCoords, PointsOutsideTheWindowAreRejected)
{
    const McpInputViewportInfo info = MakeViewportInfo();
    Inject::ResolvedPoint point;
    EXPECT_TRUE(Inject::ResolvePoint(info, Inject::Space::Window, 5000.0f, 10.0f, point).has_value());
    EXPECT_TRUE(Inject::ResolvePoint(info, Inject::Space::Window, -1.0f, 10.0f, point).has_value());
}

TEST(McpInputInjectCoords, ZeroSizedViewportIsAnError)
{
    McpInputViewportInfo info = MakeViewportInfo();
    info.LogicalWidth = 0.0f;
    info.LogicalHeight = 0.0f;
    Inject::ResolvedPoint point;
    EXPECT_TRUE(Inject::ResolvePoint(info, Inject::Space::Viewport, 0.0f, 0.0f, point).has_value());
}

// ---- the frame-quantized plan (the load-bearing timing) ----------------------

namespace
{
    Inject::Request ClickRequest(f32 x, f32 y)
    {
        Inject::Request request;
        request.Act = Inject::Action::Click;
        request.CoordSpace = Inject::Space::Viewport;
        request.X = x;
        request.Y = y;
        return request;
    }
} // namespace

// The two invariants a naive implementation breaks, and which make the tool silently
// do nothing when broken: (a) press and release must NOT share a frame — ImGui's
// IsItemClicked never fires on a same-frame press+release; (b) the press must trail
// the cursor move by enough frames for the editor's async (PBO, two frames latent)
// entity picking to have resolved the hovered entity under the new cursor.
TEST(McpInputInjectPlan, ClickHoldsTheButtonAcrossFramesAndTrailsTheMove)
{
    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    ASSERT_FALSE(Inject::BuildPlan(ClickRequest(640.0f, 360.0f), MakeViewportInfo(), plan, start, end).has_value());

    const ButtonTimeline timeline = Timeline(plan);
    ASSERT_EQ(timeline.MoveFrames.size(), 1u);
    ASSERT_EQ(timeline.PressFrames.size(), 1u);
    ASSERT_EQ(timeline.ReleaseFrames.size(), 1u);

    EXPECT_EQ(timeline.MoveFrames[0], 0);
    // (a) held for at least one full frame.
    EXPECT_GE(timeline.ReleaseFrames[0] - timeline.PressFrames[0], 2);
    // (b) the press trails the move by at least the picking latency (issue+readback+
    // one throttle-spare frame).
    EXPECT_GE(timeline.PressFrames[0] - timeline.MoveFrames[0], Inject::s_MoveSettleFrames);
    // And the whole plan settles afterwards so the caller observes the consequence.
    EXPECT_GE(static_cast<int>(plan.Frames.size()) - timeline.ReleaseFrames[0] - 1, Inject::s_TrailingSettleFrames);
}

TEST(McpInputInjectPlan, DoubleClickSendsTwoSeparatedPressReleasePairs)
{
    Inject::Request request = ClickRequest(100.0f, 100.0f);
    request.DoubleClick = true;

    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    ASSERT_FALSE(Inject::BuildPlan(request, MakeViewportInfo(), plan, start, end).has_value());

    const ButtonTimeline timeline = Timeline(plan);
    ASSERT_EQ(timeline.PressFrames.size(), 2u);
    ASSERT_EQ(timeline.ReleaseFrames.size(), 2u);
    // press1 < release1 < press2 < release2 — strictly ordered, never overlapping.
    EXPECT_LT(timeline.PressFrames[0], timeline.ReleaseFrames[0]);
    EXPECT_LT(timeline.ReleaseFrames[0], timeline.PressFrames[1]);
    EXPECT_LT(timeline.PressFrames[1], timeline.ReleaseFrames[1]);
}

// A ctrl-click (multi-select) only works if Ctrl is already held when the press is
// DISPATCHED — the editor reads it via Input::IsKeyPressed(Key::LeftControl) inside
// its MouseButtonPressed handler, which runs synchronously as the press is applied.
TEST(McpInputInjectPlan, ModifiersAreArmedBeforeThePressAndReleasedAfter)
{
    Inject::Request request = ClickRequest(200.0f, 200.0f);
    request.Mods.Ctrl = true;

    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    ASSERT_FALSE(Inject::BuildPlan(request, MakeViewportInfo(), plan, start, end).has_value());

    int ctrlDownFrame = -1;
    int ctrlUpFrame = -1;
    for (int frameIndex = 0; frameIndex < static_cast<int>(plan.Frames.size()); ++frameIndex)
    {
        for (const McpInputEvent& event : plan.Frames[static_cast<std::size_t>(frameIndex)])
        {
            if (event.Type == McpInputEvent::Kind::Key && event.Code == Key::LeftControl)
            {
                if (event.Down)
                    ctrlDownFrame = frameIndex;
                else
                    ctrlUpFrame = frameIndex;
            }
        }
    }

    const ButtonTimeline timeline = Timeline(plan);
    ASSERT_NE(ctrlDownFrame, -1);
    ASSERT_NE(ctrlUpFrame, -1);
    ASSERT_EQ(timeline.PressFrames.size(), 1u);
    EXPECT_LT(ctrlDownFrame, timeline.PressFrames[0]); // armed strictly before the press
    EXPECT_GT(ctrlUpFrame, timeline.ReleaseFrames[0]); // released strictly after the release
}

TEST(McpInputInjectPlan, DragPressesMovesInStepsThenReleases)
{
    Inject::Request request;
    request.Act = Inject::Action::Drag;
    request.CoordSpace = Inject::Space::Viewport;
    request.X = 100.0f;
    request.Y = 100.0f;
    request.ToX = 500.0f;
    request.ToY = 300.0f;
    request.Steps = 4;

    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    ASSERT_FALSE(Inject::BuildPlan(request, MakeViewportInfo(), plan, start, end).has_value());

    const ButtonTimeline timeline = Timeline(plan);
    ASSERT_EQ(timeline.PressFrames.size(), 1u);
    ASSERT_EQ(timeline.ReleaseFrames.size(), 1u);
    // 1 initial positioning move + 4 interpolation steps.
    ASSERT_EQ(timeline.MoveFrames.size(), 5u);

    // The button is down across every interpolated move — a drag with the button up is
    // just a cursor wiggle.
    for (std::size_t i = 1; i < timeline.MoveFrames.size(); ++i)
    {
        EXPECT_GT(timeline.MoveFrames[i], timeline.PressFrames[0]);
        EXPECT_LT(timeline.MoveFrames[i], timeline.ReleaseFrames[0]);
    }

    // The final interpolated position lands EXACTLY on the requested end point (no
    // float drift): window-client (300+500, 100+300).
    const McpInputEvent& last = plan.Frames[static_cast<std::size_t>(timeline.MoveFrames.back())].front();
    EXPECT_FLOAT_EQ(last.X, 800.0f);
    EXPECT_FLOAT_EQ(last.Y, 400.0f);
    EXPECT_FLOAT_EQ(last.X, end.WindowX);
    EXPECT_FLOAT_EQ(last.Y, end.WindowY);
}

TEST(McpInputInjectPlan, KeyTapPressesAndReleases)
{
    Inject::Request request;
    request.Act = Inject::Action::Key;
    request.KeyCode = Key::S;
    request.KeyAct = Inject::KeyAction::Tap;
    request.Mods.Ctrl = true;

    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    ASSERT_FALSE(Inject::BuildPlan(request, McpInputViewportInfo{}, plan, start, end).has_value());

    int keyDown = -1;
    int keyUp = -1;
    int ctrlDown = -1;
    int ctrlUp = -1;
    for (int frameIndex = 0; frameIndex < static_cast<int>(plan.Frames.size()); ++frameIndex)
    {
        for (const McpInputEvent& event : plan.Frames[static_cast<std::size_t>(frameIndex)])
        {
            if (event.Type != McpInputEvent::Kind::Key)
                continue;
            if (event.Code == Key::S)
                (event.Down ? keyDown : keyUp) = frameIndex;
            else if (event.Code == Key::LeftControl)
                (event.Down ? ctrlDown : ctrlUp) = frameIndex;
        }
    }

    ASSERT_NE(keyDown, -1);
    ASSERT_NE(keyUp, -1);
    EXPECT_LT(ctrlDown, keyDown); // Ctrl+S: the modifier is held when S goes down
    EXPECT_GT(keyUp, keyDown);    // held for at least one frame
    EXPECT_GE(ctrlUp, keyUp);     // and Ctrl is not dropped before S comes back up
}

// "press" is the sticky half of a hand-built chord: it must NOT auto-release, or a
// follow-up call could never observe the key as held.
TEST(McpInputInjectPlan, KeyPressLeavesTheKeyHeld)
{
    Inject::Request request;
    request.Act = Inject::Action::Key;
    request.KeyCode = Key::LeftShift;
    request.KeyAct = Inject::KeyAction::Press;

    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    ASSERT_FALSE(Inject::BuildPlan(request, McpInputViewportInfo{}, plan, start, end).has_value());

    int releases = 0;
    for (const auto& frame : plan.Frames)
    {
        for (const McpInputEvent& event : frame)
        {
            if (event.Type == McpInputEvent::Kind::Key && !event.Down)
                ++releases;
        }
    }
    EXPECT_EQ(releases, 0);
}

TEST(McpInputInjectPlan, TextEmitsOneCharEventPerCharacter)
{
    Inject::Request request;
    request.Act = Inject::Action::Text;
    request.Text = "Hi 42";

    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    ASSERT_FALSE(Inject::BuildPlan(request, McpInputViewportInfo{}, plan, start, end).has_value());

    std::string typed;
    for (const auto& frame : plan.Frames)
    {
        for (const McpInputEvent& event : frame)
        {
            if (event.Type == McpInputEvent::Kind::Char)
                typed.push_back(static_cast<char>(event.Code));
        }
    }
    EXPECT_EQ(typed, "Hi 42");
}

// Non-ASCII is rejected rather than silently mangled into a garbage codepoint.
TEST(McpInputInjectPlan, NonAsciiTextIsRejected)
{
    Inject::Request request;
    request.Act = Inject::Action::Text;
    request.Text = "caf\xC3\xA9";

    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    EXPECT_TRUE(Inject::BuildPlan(request, McpInputViewportInfo{}, plan, start, end).has_value());
}

// ---- argument parsing --------------------------------------------------------

TEST(McpInputInjectParse, ClickDefaultsToLeftButtonInViewportSpace)
{
    Inject::Request request;
    ASSERT_FALSE(Inject::ParseRequest(Json{ { "action", "click" }, { "x", 10 }, { "y", 20 } }, request).has_value());
    EXPECT_EQ(request.Act, Inject::Action::Click);
    EXPECT_EQ(request.CoordSpace, Inject::Space::Viewport);
    EXPECT_EQ(request.Button, Mouse::ButtonLeft);
    EXPECT_FALSE(request.DoubleClick);
    EXPECT_FALSE(request.Mods.Any());
}

TEST(McpInputInjectParse, ModifiersAndButtonAreParsed)
{
    Inject::Request request;
    const Json args{ { "action", "click" },
                     { "x", 10 },
                     { "y", 20 },
                     { "button", "right" },
                     { "modifiers", Json::array({ "ctrl", "SHIFT" }) } };
    ASSERT_FALSE(Inject::ParseRequest(args, request).has_value());
    EXPECT_EQ(request.Button, Mouse::ButtonRight);
    EXPECT_TRUE(request.Mods.Ctrl);
    EXPECT_TRUE(request.Mods.Shift);
    EXPECT_FALSE(request.Mods.Alt);
}

TEST(McpInputInjectParse, MissingPerActionArgumentsAreRejected)
{
    Inject::Request request;
    // click without coordinates
    EXPECT_TRUE(Inject::ParseRequest(Json{ { "action", "click" } }, request).has_value());
    // drag missing an endpoint
    EXPECT_TRUE(
        Inject::ParseRequest(Json{ { "action", "drag" }, { "fromX", 1 }, { "fromY", 2 }, { "toX", 3 } }, request)
            .has_value());
    // key without a key name
    EXPECT_TRUE(Inject::ParseRequest(Json{ { "action", "key" } }, request).has_value());
    // text without text
    EXPECT_TRUE(Inject::ParseRequest(Json{ { "action", "text" } }, request).has_value());
    // an unknown key name is an error, not a guess
    EXPECT_TRUE(Inject::ParseRequest(Json{ { "action", "key" }, { "key", "banana" } }, request).has_value());
}

TEST(McpInputInjectParse, DragStepsAreClamped)
{
    Inject::Request request;
    const Json args{ { "action", "drag" }, { "fromX", 1 }, { "fromY", 2 }, { "toX", 3 }, { "toY", 4 }, { "steps", 999 } };
    ASSERT_FALSE(Inject::ParseRequest(args, request).has_value());
    EXPECT_LE(request.Steps, 64);
    EXPECT_GE(request.Steps, 1);
}

// ---- schema shape -------------------------------------------------------------

TEST(McpInputInjectSchema, RequiresActionAndIsClosed)
{
    const Json schema = Inject::InputSchema();
    EXPECT_EQ(schema["type"], "object");
    ASSERT_TRUE(schema.contains("required"));
    EXPECT_EQ(schema["required"][0], "action");
    ASSERT_TRUE(schema.contains("additionalProperties"));
    EXPECT_FALSE(schema["additionalProperties"].get<bool>());

    const Json actionEnum = schema["properties"]["action"]["enum"];
    ASSERT_TRUE(actionEnum.is_array());
    EXPECT_EQ(actionEnum.size(), 6u);
    // A count alone would pass if a rename dropped mouseDelta and added something
    // else; the schema enum is what a client validates against before the handler
    // ever sees the call, so the action has to be named here to be reachable.
    EXPECT_NE(std::ranges::find(actionEnum, Json("mouseDelta")), actionEnum.end());
}

// ---- relative injection: the "mouseDelta" action (issue #607) ------------------
//
// The gap: every other action injects an ABSOLUTE cursor position, held only while
// the plan is in flight. That is correct for ImGui widgets and gizmo drags, and
// provably useless for a consumer that integrates `mouse - lastMousePos` ACROSS
// frames — it sees the override as a one-frame spike and, when the plan drains,
// that spike's exact mirror, and the two cancel. Measured on the #645 player rig:
// four equal horizontal moves each produced dYaw = 0.000.
//
// Note what these tests deliberately do NOT do: assert that holding the absolute
// override for longer fixes it. It cannot — the mirror comes from REVERTING the
// override, not from its brevity, so the integral over any plan is still exactly
// zero however long it is latched. That is why this shipped as a relative action
// rather than the `holdFrames` parameter the gap report also floated.

namespace
{
    // Sum the per-frame MouseDelta displacements across a plan, and count how many
    // frames carried one. A delta consumer sees exactly this sequence.
    struct DeltaTimeline
    {
        f32 TotalX = 0.0f;
        f32 TotalY = 0.0f;
        int DeltaFrames = 0;
        int ResetFrame = -1;
        int FirstDeltaFrame = -1;
    };

    DeltaTimeline DeltaTimelineOf(const McpInputPlan& plan)
    {
        DeltaTimeline timeline;
        for (int frameIndex = 0; frameIndex < static_cast<int>(plan.Frames.size()); ++frameIndex)
        {
            for (const McpInputEvent& event : plan.Frames[frameIndex])
            {
                if (event.Type == McpInputEvent::Kind::MouseDelta)
                {
                    timeline.TotalX += event.X;
                    timeline.TotalY += event.Y;
                    ++timeline.DeltaFrames;
                    if (timeline.FirstDeltaFrame < 0)
                        timeline.FirstDeltaFrame = frameIndex;
                }
                else if (event.Type == McpInputEvent::Kind::MouseOffsetReset)
                {
                    timeline.ResetFrame = frameIndex;
                }
            }
        }
        return timeline;
    }

    Inject::Request DeltaRequest(f32 dx, f32 dy, int frames = 1, Inject::Space space = Inject::Space::Window)
    {
        Inject::Request request;
        request.Act = Inject::Action::MouseDelta;
        request.CoordSpace = space;
        request.Dx = dx;
        request.Dy = dy;
        request.DeltaFrames = frames;
        return request;
    }
} // namespace

TEST(McpInputInjectDelta, EmitsRelativeEventsAndNeverAnAbsolutePosition)
{
    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    Inject::ResolvedDelta delta;
    ASSERT_FALSE(Inject::BuildPlan(DeltaRequest(40.0f, -10.0f), MakeViewportInfo(), plan, start, end, &delta)
                     .has_value());

    // A single absolute MousePos anywhere in the plan would reintroduce the exact
    // bug this action exists to fix: the override is dropped when the plan drains,
    // and the delta consumer integrates the mirror back out again.
    for (const auto& frame : plan.Frames)
    {
        for (const McpInputEvent& event : frame)
            EXPECT_NE(event.Type, McpInputEvent::Kind::MousePos)
                << "mouseDelta must never inject an absolute position";
    }

    const DeltaTimeline timeline = DeltaTimelineOf(plan);
    EXPECT_EQ(timeline.DeltaFrames, 1);
    EXPECT_FLOAT_EQ(timeline.TotalX, 40.0f);
    EXPECT_FLOAT_EQ(timeline.TotalY, -10.0f);
    EXPECT_FLOAT_EQ(delta.WindowDx, 40.0f);
    EXPECT_FLOAT_EQ(delta.WindowDy, -10.0f);
    // Settled afterwards, like every other action, so the caller observes the result.
    EXPECT_GE(static_cast<int>(plan.Frames.size()) - timeline.FirstDeltaFrame - 1, Inject::s_TrailingSettleFrames);
}

TEST(McpInputInjectDelta, FramesSpreadTheDisplacementAndSumExactly)
{
    // Spreading matters because a consumer is entitled to reject an implausible
    // single-frame jump as a discontinuity, and several do — PlayerRigSystem drops
    // any look sample worth over kMaxLookDegreesPerSample. A real mouse delivers a
    // large movement as many small per-frame deltas.
    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    Inject::ResolvedDelta delta;
    ASSERT_FALSE(Inject::BuildPlan(DeltaRequest(30.0f, 9.0f, /*frames*/ 4), MakeViewportInfo(), plan, start, end,
                                   &delta)
                     .has_value());

    const DeltaTimeline timeline = DeltaTimelineOf(plan);
    EXPECT_EQ(timeline.DeltaFrames, 4);
    EXPECT_EQ(delta.Frames, 4);
    // Exactness is the point: the plan walks the running TOTAL rather than adding a
    // rounded per-frame step, so N frames cannot drift away from the requested
    // displacement — which would leave the virtual cursor permanently mis-placed,
    // since the offset is never reverted.
    EXPECT_FLOAT_EQ(timeline.TotalX, 30.0f);
    EXPECT_FLOAT_EQ(timeline.TotalY, 9.0f);
}

TEST(McpInputInjectDelta, AwkwardSplitStillSumsExactly)
{
    // 10 / 3 has no exact binary representation; a naive per-frame step would leave
    // a residue.
    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    ASSERT_FALSE(Inject::BuildPlan(DeltaRequest(10.0f, -7.0f, /*frames*/ 3), MakeViewportInfo(), plan, start, end)
                     .has_value());

    const DeltaTimeline timeline = DeltaTimelineOf(plan);
    EXPECT_EQ(timeline.DeltaFrames, 3);
    EXPECT_FLOAT_EQ(timeline.TotalX, 10.0f);
    EXPECT_FLOAT_EQ(timeline.TotalY, -7.0f);
}

TEST(McpInputInjectDelta, ResetPrecedesTheDisplacementInItsOwnFrame)
{
    Inject::Request request = DeltaRequest(25.0f, 0.0f);
    request.ResetOffset = true;

    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    Inject::ResolvedDelta delta;
    ASSERT_FALSE(Inject::BuildPlan(request, MakeViewportInfo(), plan, start, end, &delta).has_value());

    const DeltaTimeline timeline = DeltaTimelineOf(plan);
    ASSERT_GE(timeline.ResetFrame, 0);
    EXPECT_TRUE(delta.Reset);
    // Its own frame, ahead of the displacement: the reset IS a jump (the virtual
    // cursor lands back on the hardware one), and folding it into the same frame
    // would hand a delta consumer the algebraic sum of the two, hiding both.
    EXPECT_LT(timeline.ResetFrame, timeline.FirstDeltaFrame);
    EXPECT_FLOAT_EQ(timeline.TotalX, 25.0f);
}

TEST(McpInputInjectDelta, ResetAloneNeedsNoDisplacement)
{
    // "Put the virtual cursor back" is a meaningful call on its own; demanding
    // dx:0, dy:0 to express it would be noise.
    Inject::Request request;
    const Json args{ { "action", "mouseDelta" }, { "resetOffset", true } };
    ASSERT_FALSE(Inject::ParseRequest(args, request).has_value());
    EXPECT_TRUE(request.ResetOffset);
    EXPECT_FLOAT_EQ(request.Dx, 0.0f);

    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    ASSERT_FALSE(Inject::BuildPlan(request, MakeViewportInfo(), plan, start, end).has_value());
    const DeltaTimeline timeline = DeltaTimelineOf(plan);
    EXPECT_GE(timeline.ResetFrame, 0);
    EXPECT_FLOAT_EQ(timeline.TotalX, 0.0f);
}

TEST(McpInputInjectDelta, SpacesScaleTheDisplacementLikeTheyScaleAPosition)
{
    const McpInputViewportInfo hiDpi = MakeViewportInfo(2.0f);

    f32 dx = 0.0f;
    f32 dy = 0.0f;
    // window: already the logical pixels Input::GetMousePosition reports.
    ASSERT_FALSE(Inject::ResolveDelta(hiDpi, Inject::Space::Window, 40.0f, 10.0f, dx, dy).has_value());
    EXPECT_FLOAT_EQ(dx, 40.0f);
    EXPECT_FLOAT_EQ(dy, 10.0f);

    // viewport: native screenshot pixels -> logical, i.e. divided by the DPI scale,
    // exactly as ResolvePoint treats a position. The two must agree, or `space`
    // would silently mean two different things depending on the action.
    ASSERT_FALSE(Inject::ResolveDelta(hiDpi, Inject::Space::Viewport, 40.0f, 10.0f, dx, dy).has_value());
    EXPECT_FLOAT_EQ(dx, 20.0f);
    EXPECT_FLOAT_EQ(dy, 5.0f);

    // viewportNorm: a fraction of the viewport's logical extent.
    ASSERT_FALSE(Inject::ResolveDelta(hiDpi, Inject::Space::ViewportNormalized, 0.5f, 0.25f, dx, dy).has_value());
    EXPECT_FLOAT_EQ(dx, 640.0f);
    EXPECT_FLOAT_EQ(dy, 180.0f);
}

TEST(McpInputInjectDelta, ADisplacementHasNoPositionAndSoCannotBeOutsideTheViewport)
{
    // ResolvePoint rejects an out-of-viewport POSITION, and rightly so. A
    // displacement names no position at all, so the same guard applied here would
    // refuse every movement larger than the viewport — including the perfectly
    // ordinary "spin the camera all the way round".
    f32 dx = 0.0f;
    f32 dy = 0.0f;
    EXPECT_FALSE(Inject::ResolveDelta(MakeViewportInfo(), Inject::Space::Viewport, 99999.0f, -99999.0f, dx, dy)
                     .has_value());
    EXPECT_FLOAT_EQ(dx, 99999.0f);
}

TEST(McpInputInjectDelta, ParseRejectsNonFiniteAndClampsFrames)
{
    // Both components are required unless this is a pure reset — a half-given
    // displacement is a typo, and silently treating the missing axis as 0 would
    // move the cursor somewhere the caller never asked for. The offset PERSISTS,
    // so a wrong displacement is not self-correcting the way a one-frame absolute
    // override is.
    Inject::Request missingDx;
    EXPECT_TRUE(Inject::ParseRequest(Json{ { "action", "mouseDelta" }, { "dy", 1 } }, missingDx).has_value());
    Inject::Request missingDy;
    EXPECT_TRUE(Inject::ParseRequest(Json{ { "action", "mouseDelta" }, { "dx", 1 } }, missingDy).has_value());

    // Non-finite input must never reach the accumulator: a NaN/Inf added to the
    // persistent offset poisons EVERY later mouse read for the process, not just
    // this call (Math::IsFinite then zeroes the rig's look input forever).
    for (const char* axis : { "dx", "dy" })
    {
        for (const f64 bad : { std::numeric_limits<f64>::quiet_NaN(), std::numeric_limits<f64>::infinity(),
                               -std::numeric_limits<f64>::infinity() })
        {
            Json args{ { "action", "mouseDelta" }, { "dx", 1.0 }, { "dy", 1.0 } };
            args[axis] = bad;
            Inject::Request request;
            EXPECT_TRUE(Inject::ParseRequest(args, request).has_value())
                << "non-finite '" << axis << "' was accepted";
        }
    }

    Inject::Request clamped;
    const Json args{ { "action", "mouseDelta" }, { "dx", 1 }, { "dy", 2 }, { "frames", 9999 } };
    ASSERT_FALSE(Inject::ParseRequest(args, clamped).has_value());
    EXPECT_LE(clamped.DeltaFrames, 64);
    EXPECT_GE(clamped.DeltaFrames, 1);
}

TEST(McpInputInjectDelta, ResetOnlyNeedsNoViewportGeometry)
{
    // A pure reset carries no coordinates, so it must not depend on viewport
    // geometry — it is exactly the cleanup call a session makes when the Viewport
    // panel is closed or has no size yet, i.e. when ResolveDelta would fail.
    Inject::Request request;
    ASSERT_FALSE(
        Inject::ParseRequest(Json{ { "action", "mouseDelta" }, { "resetOffset", true } }, request).has_value());
    ASSERT_TRUE(request.ResetOnly);

    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    const McpInputViewportInfo noViewport; // Available == false, zero size, zero DPI
    EXPECT_FALSE(Inject::BuildPlan(request, noViewport, plan, start, end).has_value())
        << "a standalone resetOffset must not require a resolvable viewport";
    EXPECT_GE(DeltaTimelineOf(plan).ResetFrame, 0);

    // The bypass is scoped to the reset, not a blanket relaxation: a displacement
    // that genuinely needs the viewport's extent still fails against the same info.
    //
    // viewportNorm, not viewport, is the honest negative control here — a fraction
    // of the viewport is meaningless without its size, whereas a delta in native
    // viewport PIXELS needs only the DPI ratio (which defaults to 1.0) and so
    // legitimately resolves even against an unsized viewport.
    Inject::Request normalized = DeltaRequest(0.1f, 0.0f, 1, Inject::Space::ViewportNormalized);
    McpInputPlan rejected;
    EXPECT_TRUE(Inject::BuildPlan(normalized, noViewport, rejected, start, end).has_value());
}

TEST(McpInputInjectDelta, AbsoluteActionsAreUnchangedByTheAdditionOfTheDeltaAction)
{
    // The trap this whole slice had to avoid: the one-frame override behaviour is
    // CORRECT for ImGui widgets and gizmo drags, so relative injection had to be
    // strictly additive. A click plan must still be exactly what it was — an
    // absolute move, then press/hold/release — with no delta events smuggled in.
    McpInputPlan plan;
    Inject::ResolvedPoint start;
    Inject::ResolvedPoint end;
    Inject::ResolvedDelta delta;
    ASSERT_FALSE(Inject::BuildPlan(ClickRequest(640.0f, 360.0f), MakeViewportInfo(), plan, start, end, &delta)
                     .has_value());

    const DeltaTimeline timeline = DeltaTimelineOf(plan);
    EXPECT_EQ(timeline.DeltaFrames, 0);
    EXPECT_EQ(timeline.ResetFrame, -1);

    const ButtonTimeline buttons = Timeline(plan);
    ASSERT_EQ(buttons.MoveFrames.size(), 1u);
    ASSERT_EQ(buttons.PressFrames.size(), 1u);
    EXPECT_GE(buttons.PressFrames[0] - buttons.MoveFrames[0], Inject::s_MoveSettleFrames);
}
