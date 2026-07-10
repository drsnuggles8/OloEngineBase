#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpRendererSettingsTest — unit test (headless, no GL, no live editor).
//
// Pins the consented MCP WRITE tool olo_renderer_settings_set (issue #306 item C):
// set a multi-valued, session-global renderer / post-process setting (FSR1 upscale
// mode, tone-map operator, rendering path) so an agent can verify a rendering
// feature LIVE at each value over MCP — the enum-valued sibling of the boolean
// olo_render_toggle_pass. Two seams are exercised:
//
//   1. The dispatch seam (McpServer.cpp, compiled into the test binary): a tool
//      flagged ToolDef::ProjectWrite is REFUSED with a clean JSON-RPC error while
//      the session "Allow writes" gate is off, and ACCEPTED once it is on. The
//      server's inputSchema enforcement (#423) rejects an unknown setting token /
//      unexpected property before the handler runs. McpTools.cpp (the real tool
//      registrations) is deliberately NOT linked here, so the test registers a fake
//      tool wired to the SAME shared apply code the real handler uses.
//
//   2. The shared core (MCP/McpRendererSettings.h, header-only): schema, ParseArgs,
//      ParseSetting/ParseValue, and Apply against the plain POD settings structs —
//      assert the field changed AND that the prior value is reported so the change
//      can be restored by setting it back (restore-prior-value, no undo stack).
//
// The shared header is renderer-light (the POD PostProcessSettings / RendererSettings
// structs) and httplib/editor-free, so no extra editor TU is pulled in.
// =============================================================================

#include "MCP/McpRendererSettings.h"
#include "MCP/McpServer.h"

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderingPath.h"

#include <algorithm>
#include <optional>
#include <string>

// OLO_TEST_LAYER: unit

namespace
{
    namespace RS = OloEngine::MCP::RendererSettings;
    using OloEngine::PostProcessSettings;
    using OloEngine::RendererSettings;
    using OloEngine::RenderingPath;
    using OloEngine::TonemapOperator;
    using OloEngine::UpscaleMode;
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;

    constexpr int kInvalidParams = -32602;

    Json MakeCallRequest(const Json& id, const Json& arguments)
    {
        return Json{ { "jsonrpc", "2.0" },
                     { "id", id },
                     { "method", "tools/call" },
                     { "params", { { "name", "olo_renderer_settings_set" }, { "arguments", arguments } } } };
    }

    // Fixture: an McpServer whose only tool is a fake olo_renderer_settings_set wired
    // to test-owned PostProcessSettings + RendererSettings through the SAME schema +
    // ParseArgs + Apply code the real handler uses. The fake handler runs
    // synchronously (no MarshalRead — there is no game thread here), which is exactly
    // what the real handler delegates to once on the main thread.
    class McpRendererSettingsWriteTest : public ::testing::Test
    {
      protected:
        McpRendererSettingsWriteTest()
            : m_Server(EditorMcpContext{})
        {
            ToolDef tool;
            tool.Name = "olo_renderer_settings_set";
            tool.Description = "Set a renderer / post-process setting (fake; test wiring).";
            tool.ProjectWrite = true;
            tool.InputSchema = RS::InputSchema();
            tool.Handler = [this](McpServer&, const Json& args) -> ToolResult
            {
                bool introspect = false;
                RS::Setting setting{};
                i32 value = 0;
                if (const auto error = RS::ParseArgs(args, introspect, setting, value))
                    return ToolResult::Error(*error);
                if (introspect)
                    return ToolResult::Text(RS::Describe(m_PP, m_RS, m_Lever).dump());
                const RS::ApplyResult applied = RS::Apply(setting, value, m_PP, m_RS, m_Lever);
                if (!applied.Ok)
                    return ToolResult::Error(applied.Error);
                return ToolResult::Text(applied.Data.dump());
            };
            m_Server.RegisterTool(std::move(tool));
        }

        PostProcessSettings m_PP;
        RendererSettings m_RS;
        RS::LeverState m_Lever;
        McpServer m_Server; // declared last → destroyed first (its handler refs m_PP/m_RS/m_Lever)
    };
} // namespace

// ---- the session write gate (dispatch seam) --------------------------------

// Default state: the gate is OFF, so even a well-formed write call is refused with a
// JSON-RPC error and NOTHING is mutated.
TEST_F(McpRendererSettingsWriteTest, GateOffRejectsWriteAndMutatesNothing)
{
    ASSERT_FALSE(m_Server.AllowWrites()); // off by default
    ASSERT_EQ(m_PP.Upscale, UpscaleMode::Off);

    const Json resp = m_Server.HandleMessage(MakeCallRequest(1, Json{ { "setting", "upscale" }, { "value", "performance" } }));

    ASSERT_TRUE(resp.contains("error"));
    EXPECT_FALSE(resp.contains("result"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);

    // The setting is untouched — the gate stopped the handler from ever running.
    EXPECT_EQ(m_PP.Upscale, UpscaleMode::Off);
}

// With the gate ON the same call succeeds and the field changes; the response reports
// the prior value so the change can be reverted by setting it back.
TEST_F(McpRendererSettingsWriteTest, GateOnAppliesWriteAndReportsPrior)
{
    m_Server.SetAllowWrites(true);

    const Json resp = m_Server.HandleMessage(MakeCallRequest(2, Json{ { "setting", "upscale" }, { "value", "performance" } }));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp.contains("error"));
    EXPECT_FALSE(resp["result"]["isError"]);
    EXPECT_EQ(m_PP.Upscale, UpscaleMode::Performance);

    // The result payload carries the restore hint (previousValue == "off").
    const Json payload = Json::parse(resp["result"]["content"][0]["text"].get<std::string>());
    EXPECT_EQ(payload["setting"], "upscale");
    EXPECT_EQ(payload["previousValue"], "off");
    EXPECT_EQ(payload["value"], "performance");
    EXPECT_TRUE(payload["changed"].get<bool>());
    EXPECT_EQ(payload["restoreWith"], "off");

    // Restore by setting it back to the reported prior value.
    const Json restore = m_Server.HandleMessage(MakeCallRequest(3, Json{ { "setting", "upscale" }, { "value", "off" } }));
    ASSERT_TRUE(restore.contains("result"));
    EXPECT_EQ(m_PP.Upscale, UpscaleMode::Off);
}

// Introspection (no arguments) still requires the gate (the whole tool is a write
// tool), and once on it lists every setting with its live current value.
TEST_F(McpRendererSettingsWriteTest, IntrospectionListsSettingsWhenGateOn)
{
    m_Server.SetAllowWrites(true);
    m_PP.Tonemap = TonemapOperator::ACES;

    const Json resp = m_Server.HandleMessage(MakeCallRequest(4, Json::object()));
    ASSERT_TRUE(resp.contains("result"));
    const Json payload = Json::parse(resp["result"]["content"][0]["text"].get<std::string>());
    ASSERT_TRUE(payload["settings"].is_array());
    EXPECT_EQ(payload["settings"].size(), RS::kSettings.size());

    bool sawTonemap = false;
    for (const auto& entry : payload["settings"])
    {
        if (entry["setting"] == "tonemap")
        {
            sawTonemap = true;
            EXPECT_EQ(entry["currentValue"], "aces");
        }
    }
    EXPECT_TRUE(sawTonemap);
}

// ---- server-side inputSchema enforcement (#423), gate ON -------------------

TEST_F(McpRendererSettingsWriteTest, SchemaRejectsUnknownSetting)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(5, Json{ { "setting", "bogus" }, { "value", "off" } }));
    ASSERT_TRUE(resp.contains("result")); // SEP-1303: schema failures are tool errors
    EXPECT_EQ(resp["result"]["isError"], true);
    EXPECT_EQ(m_PP.Upscale, UpscaleMode::Off); // never applied
}

TEST_F(McpRendererSettingsWriteTest, SchemaRejectsUnknownProperty)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(
        MakeCallRequest(6, Json{ { "setting", "upscale" }, { "value", "off" }, { "extra", true } }));
    ASSERT_TRUE(resp.contains("result")); // SEP-1303: schema failures are tool errors
    EXPECT_EQ(resp["result"]["isError"], true);
}

// ---- handler-level (tool) errors, gate ON ----------------------------------

// A schema-valid setting with a value token that doesn't belong to it is a TOOL-level
// error (isError) — the call reached the handler (setting is a valid enum), the
// handler's ParseArgs rejected the value.
TEST_F(McpRendererSettingsWriteTest, MismatchedValueIsToolError)
{
    m_Server.SetAllowWrites(true);
    // "reinhard" is a tonemap value, not an upscale value.
    const Json resp = m_Server.HandleMessage(MakeCallRequest(7, Json{ { "setting", "upscale" }, { "value", "reinhard" } }));
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp.contains("error"));
    EXPECT_TRUE(resp["result"]["isError"]);
    EXPECT_EQ(m_PP.Upscale, UpscaleMode::Off);
}

// A setting without a value is a tool error naming the valid values.
TEST_F(McpRendererSettingsWriteTest, MissingValueIsToolError)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(8, Json{ { "setting", "tonemap" } }));
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_TRUE(resp["result"]["isError"]);
}

// ---- the shared apply core (MCP/McpRendererSettings.h), no server ----------

TEST(McpRendererSettingsApply, SetsUpscaleAndReportsPrior)
{
    PostProcessSettings pp;
    RendererSettings rs;
    RS::LeverState lever;
    const auto result = RS::Apply(RS::Setting::Upscale, static_cast<i32>(UpscaleMode::Balanced), pp, rs, lever);
    ASSERT_TRUE(result.Ok);
    EXPECT_EQ(pp.Upscale, UpscaleMode::Balanced);
    EXPECT_EQ(result.Data["previousValue"], "off");
    EXPECT_EQ(result.Data["value"], "balanced");
    EXPECT_TRUE(result.Data["changed"].get<bool>());
    EXPECT_FALSE(result.PathChanged); // not a render-path write
}

TEST(McpRendererSettingsApply, SetsTonemap)
{
    PostProcessSettings pp;
    RendererSettings rs;
    RS::LeverState lever;
    const auto result = RS::Apply(RS::Setting::Tonemap, static_cast<i32>(TonemapOperator::ACES), pp, rs, lever);
    ASSERT_TRUE(result.Ok);
    EXPECT_EQ(pp.Tonemap, TonemapOperator::ACES);
    EXPECT_EQ(result.Data["previousValue"], "reinhard"); // struct default
    EXPECT_EQ(result.Data["value"], "aces");
}

// A render-path write flags PathChanged so the handler rebuilds the render graph.
TEST(McpRendererSettingsApply, RenderPathChangeFlagsRebuild)
{
    PostProcessSettings pp;
    RendererSettings rs; // default Forward
    RS::LeverState lever;
    const auto result = RS::Apply(RS::Setting::RenderPath, static_cast<i32>(RenderingPath::Deferred), pp, rs, lever);
    ASSERT_TRUE(result.Ok);
    EXPECT_EQ(rs.Path, RenderingPath::Deferred);
    EXPECT_TRUE(result.PathChanged);
    EXPECT_EQ(result.Data["previousValue"], "forward");
    EXPECT_EQ(result.Data["value"], "deferred");
}

// Setting a value it already has changes nothing and (for render path) does not
// request a rebuild.
TEST(McpRendererSettingsApply, NoOpWhenValueUnchanged)
{
    PostProcessSettings pp;
    RendererSettings rs; // default Forward
    RS::LeverState lever;
    const auto result = RS::Apply(RS::Setting::RenderPath, static_cast<i32>(RenderingPath::Forward), pp, rs, lever);
    ASSERT_TRUE(result.Ok);
    EXPECT_FALSE(result.Data["changed"].get<bool>());
    EXPECT_FALSE(result.PathChanged);
}

// Apply is a public seam callable independently of ParseArgs, so it must reject an
// out-of-range integer rather than casting it blindly into the engine enum: it
// returns a failure ApplyResult and leaves the settings untouched.
TEST(McpRendererSettingsApply, RejectsOutOfRangeValueAndMutatesNothing)
{
    PostProcessSettings pp;
    pp.Upscale = UpscaleMode::Quality;
    RendererSettings rs;
    RS::LeverState lever;

    const auto result = RS::Apply(RS::Setting::Upscale, 999, pp, rs, lever);
    EXPECT_FALSE(result.Ok);
    EXPECT_FALSE(result.Error.empty());
    EXPECT_TRUE(result.Data.is_null());
    EXPECT_EQ(pp.Upscale, UpscaleMode::Quality); // unchanged
}

// ---- the perf-lever settings (#316: depthprepass / softshadows) ------------

// The depthprepass lever writes the LeverState snapshot (the handler pushes it
// to Renderer3D::EnableDepthPrepass afterwards) and never touches pp / rs.
TEST(McpRendererSettingsApply, DepthPrepassOffWritesLeverOnly)
{
    PostProcessSettings pp;
    RendererSettings rs;
    RS::LeverState lever;
    lever.DepthPrepassEnabled = true;

    const auto result = RS::Apply(RS::Setting::DepthPrepass, RS::kDepthPrepassOff, pp, rs, lever);
    ASSERT_TRUE(result.Ok);
    EXPECT_FALSE(lever.DepthPrepassEnabled);
    EXPECT_EQ(result.Data["previousValue"], "on");
    EXPECT_EQ(result.Data["value"], "off");
    EXPECT_TRUE(result.Data["changed"].get<bool>());
    EXPECT_FALSE(result.Data.contains("requested"));
    EXPECT_FALSE(result.PathChanged);
}

// 'auto' resolves to the settings-derived value BEFORE the write, so the
// response reports the actual resulting state (plus requested:"auto") and the
// restore hint still round-trips through off/on.
TEST(McpRendererSettingsApply, DepthPrepassAutoResolvesToDerivedValue)
{
    PostProcessSettings pp;
    RendererSettings rs;
    RS::LeverState lever;
    lever.DepthPrepassEnabled = false; // forced off by a prior 'off' write
    lever.DepthPrepassAuto = true;     // settings say it should be on

    const auto result = RS::Apply(RS::Setting::DepthPrepass, RS::kDepthPrepassAuto, pp, rs, lever);
    ASSERT_TRUE(result.Ok);
    EXPECT_TRUE(lever.DepthPrepassEnabled);
    EXPECT_EQ(result.Data["previousValue"], "off");
    EXPECT_EQ(result.Data["value"], "on"); // the resolved state, not "auto"
    EXPECT_EQ(result.Data["requested"], "auto");
    EXPECT_TRUE(result.Data["changed"].get<bool>());
}

// 'auto' that resolves to the current state is honestly reported unchanged.
TEST(McpRendererSettingsApply, DepthPrepassAutoNoOpWhenAlreadyDerived)
{
    PostProcessSettings pp;
    RendererSettings rs;
    RS::LeverState lever;
    lever.DepthPrepassEnabled = true;
    lever.DepthPrepassAuto = true;

    const auto result = RS::Apply(RS::Setting::DepthPrepass, RS::kDepthPrepassAuto, pp, rs, lever);
    ASSERT_TRUE(result.Ok);
    EXPECT_FALSE(result.Data["changed"].get<bool>());
    EXPECT_EQ(result.Data["requested"], "auto");
}

TEST(McpRendererSettingsApply, SoftShadowsPcfDisablesPcssAndReportsPrior)
{
    PostProcessSettings pp;
    RendererSettings rs;
    RS::LeverState lever;
    lever.SoftShadows = true; // engine default: PCSS on

    const auto result = RS::Apply(RS::Setting::SoftShadows, RS::kSoftShadowsPcf, pp, rs, lever);
    ASSERT_TRUE(result.Ok);
    EXPECT_FALSE(lever.SoftShadows);
    EXPECT_EQ(result.Data["previousValue"], "pcss");
    EXPECT_EQ(result.Data["value"], "pcf");
    EXPECT_EQ(result.Data["restoreWith"], "pcss");
    EXPECT_TRUE(result.Data["changed"].get<bool>());

    // Restore by setting the reported prior value back.
    const auto restored = RS::Apply(RS::Setting::SoftShadows, RS::kSoftShadowsPcss, pp, rs, lever);
    ASSERT_TRUE(restored.Ok);
    EXPECT_TRUE(lever.SoftShadows);
}

// Describe reads the lever state for the two new settings — 'auto' is
// write-only, so the current value is always off/on.
TEST(McpRendererSettingsApply, DescribeReportsLeverCurrentValues)
{
    PostProcessSettings pp;
    RendererSettings rs;
    RS::LeverState lever;
    lever.DepthPrepassEnabled = true;
    lever.SoftShadows = false;

    const Json described = RS::Describe(pp, rs, lever);
    bool sawPrepass = false;
    bool sawShadows = false;
    for (const auto& entry : described["settings"])
    {
        if (entry["setting"] == "depthprepass")
        {
            sawPrepass = true;
            EXPECT_EQ(entry["currentValue"], "on");
        }
        if (entry["setting"] == "softshadows")
        {
            sawShadows = true;
            EXPECT_EQ(entry["currentValue"], "pcf");
        }
    }
    EXPECT_TRUE(sawPrepass);
    EXPECT_TRUE(sawShadows);
}

// ---- the shared arg parser + token resolution ------------------------------

TEST(McpRendererSettingsParse, ResolvesSettingAndValueCaseAndSeparatorInsensitive)
{
    RS::Setting setting{};
    EXPECT_TRUE(RS::ParseSetting("RenderPath", setting));
    EXPECT_EQ(setting, RS::Setting::RenderPath);
    EXPECT_TRUE(RS::ParseSetting("render_path", setting));
    EXPECT_EQ(setting, RS::Setting::RenderPath);

    i32 value = -1;
    EXPECT_TRUE(RS::ParseValue(RS::Setting::Upscale, "Ultra Performance", value));
    EXPECT_EQ(value, static_cast<i32>(UpscaleMode::UltraPerformance));
    EXPECT_TRUE(RS::ParseValue(RS::Setting::Upscale, "ultra-performance", value));
    EXPECT_EQ(value, static_cast<i32>(UpscaleMode::UltraPerformance));
}

TEST(McpRendererSettingsParse, NoArgsIsIntrospection)
{
    bool introspect = false;
    RS::Setting setting{};
    i32 value = 0;
    EXPECT_FALSE(RS::ParseArgs(Json::object(), introspect, setting, value).has_value());
    EXPECT_TRUE(introspect);
}

TEST(McpRendererSettingsParse, ValueWithoutSettingIsRejected)
{
    bool introspect = false;
    RS::Setting setting{};
    i32 value = 0;
    EXPECT_TRUE(RS::ParseArgs(Json{ { "value", "off" } }, introspect, setting, value).has_value());
}

TEST(McpRendererSettingsParse, RejectsUnknownSettingAndMismatchedValue)
{
    bool introspect = false;
    RS::Setting setting{};
    i32 value = 0;
    EXPECT_TRUE(RS::ParseArgs(Json{ { "setting", "nope" }, { "value", "off" } }, introspect, setting, value).has_value());
    EXPECT_TRUE(RS::ParseArgs(Json{ { "setting", "upscale" }, { "value", "aces" } }, introspect, setting, value).has_value());
    EXPECT_TRUE(RS::ParseArgs(Json{ { "setting", "upscale" } }, introspect, setting, value).has_value());
}

TEST(McpRendererSettingsParse, RoundTripsEveryValueTokenForEverySetting)
{
    for (const auto& info : RS::kSettings)
    {
        for (const auto& v : RS::SettingValues(info.Id))
        {
            i32 parsed = -12345;
            EXPECT_TRUE(RS::ParseValue(info.Id, v.Token, parsed)) << "token: " << v.Token;
            EXPECT_EQ(parsed, v.Value);
            EXPECT_EQ(RS::ValueToken(info.Id, v.Value), std::string(v.Token));
        }
    }
}

// ---- the shared inputSchema -------------------------------------------------

TEST(McpRendererSettingsSchema, DeclaresSettingEnumAndIsClosed)
{
    const Json schema = RS::InputSchema();
    EXPECT_EQ(schema["type"], "object");
    EXPECT_EQ(schema["additionalProperties"], false);
    EXPECT_EQ(schema["properties"]["setting"]["type"], "string");
    EXPECT_EQ(schema["properties"]["value"]["type"], "string");

    ASSERT_TRUE(schema["properties"]["setting"]["enum"].is_array());
    const auto& settingEnum = schema["properties"]["setting"]["enum"];
    // The schema enum lists exactly the known setting tokens.
    EXPECT_EQ(settingEnum.size(), RS::kSettings.size());
    for (const auto& info : RS::kSettings)
        EXPECT_NE(std::find(settingEnum.begin(), settingEnum.end(), std::string(info.Token)), settingEnum.end())
            << "setting token: " << info.Token;
}
