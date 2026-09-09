// OLO_TEST_LAYER: unit
// =============================================================================
// McpExposureProfileTest — tool exposure profiles + the discovery gateway (#1124).
//
// Two things are pinned here, and the second is the reason the file exists.
//
//   1. The profile ARITHMETIC (McpExposure.h): which tools each profile lists, and
//      the invariants that keep a narrowed default usable — the gateway and the
//      user's own script/bridged tools are listed under every profile.
//
//   2. The SIZE of `tools/list` under the default profile, asserted as a byte
//      ceiling against the real builtin surface. That assertion is the point of
//      issue #1124: the payload grew from ~15k tokens (#673) to 265 009 bytes /
//      ~66k tokens with nothing recording it, so "it got big again" was invisible
//      until someone re-measured by hand. A number in a test cannot regress
//      silently.
//
// Registration-only where it touches the real surface: RegisterBuiltinTools builds
// ToolDefs (names, schemas, annotations) and nothing else, so no handler runs, no
// MarshalRead needs a pumped game thread and no GL context is required. The
// end-to-end gateway round-trips use FAKE tools for the same reason — a real
// handler cannot run in this binary (see docs/agent-rules/notes-mcp-tool-authoring.md
// §2).
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpExposure.h"
#include "MCP/McpServer.h"
#include "MCP/McpTools.h"
// Editor-internal registration entry points (RegisterGatewayTools). The test
// binary already links the McpTools*.cpp family, so this is a declaration, not a
// new dependency.
#include "MCP/McpToolsCommon.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::IAutomationHost;
    using OloEngine::MCP::ExposurePolicy;
    using OloEngine::MCP::ExposureProfile;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolExposureFacts;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;

    Json MakeRequest(const Json& id, const std::string& method, const Json& params = Json::object())
    {
        Json req = { { "jsonrpc", "2.0" }, { "method", method } };
        if (!id.is_null())
            req["id"] = id;
        if (!params.is_null())
            req["params"] = params;
        return req;
    }

    // The real builtin surface under one profile, as the tools/list result. The
    // server is local to the lambda, so no handler can ever be invoked afterwards.
    Json BuiltinToolsList(ExposureProfile profile)
    {
        McpServer server{ EditorMcpContext{} };
        OloEngine::MCP::RegisterBuiltinTools(server);
        server.SetExposurePolicy(ExposurePolicy{ profile, {} });
        return server.HandleMessage(MakeRequest(1, "tools/list"));
    }

    std::set<std::string> ToolNamesIn(const Json& listResponse)
    {
        std::set<std::string> names;
        for (const Json& tool : listResponse["result"]["tools"])
            names.insert(tool.value("name", std::string{}));
        return names;
    }

    ToolDef MakeFakeTool(std::string name, std::string toolset)
    {
        ToolDef tool;
        tool.Name = std::move(name);
        tool.Toolset = std::move(toolset);
        tool.Description = "fake";
        tool.Handler = [](IAutomationHost&, const Json& args)
        { return ToolResult::Text("ran:" + args.value("echo", std::string{ "-" })); };
        return tool;
    }
} // namespace

// ---- the profile arithmetic (pure) -----------------------------------------

TEST(McpExposureProfile, ParseAcceptsTheThreeProfilesCaseInsensitivelyAndRejectsAnythingElse)
{
    EXPECT_EQ(OloEngine::MCP::ParseExposureProfile("core"), ExposureProfile::Core);
    EXPECT_EQ(OloEngine::MCP::ParseExposureProfile("TOOLSET"), ExposureProfile::Toolset);
    EXPECT_EQ(OloEngine::MCP::ParseExposureProfile("Full"), ExposureProfile::Full);
    // A typo must be reportable, not silently absorbed into the default.
    EXPECT_FALSE(OloEngine::MCP::ParseExposureProfile("fill").has_value());
    EXPECT_FALSE(OloEngine::MCP::ParseExposureProfile("").has_value());
}

TEST(McpExposureProfile, ToolsetListParsesSeparatorsAndCanonicalisesCase)
{
    const std::vector<std::string> parsed = OloEngine::MCP::ParseToolsetList("Render, physics;shader  render");
    EXPECT_EQ(parsed, (std::vector<std::string>{ "render", "physics", "shader" }))
        << "duplicates collapse and every entry is lowercased";
    EXPECT_TRUE(OloEngine::MCP::ParseToolsetList("  ,; ").empty());
}

TEST(McpExposureProfile, FullListsEverythingIncludingUncategorizedTools)
{
    const ExposurePolicy policy{ ExposureProfile::Full, {} };
    EXPECT_TRUE(policy.ShouldList(ToolExposureFacts{ "olo_render_probe_pixel", "render", false }));
    EXPECT_TRUE(policy.ShouldList(ToolExposureFacts{ "olo_anything", "", false }));
}

TEST(McpExposureProfile, CoreListsTheCuratedSetAndTheGatewayAndNothingElse)
{
    const ExposurePolicy policy; // default-constructed == core
    ASSERT_EQ(policy.Profile, ExposureProfile::Core);

    for (const std::string_view name : OloEngine::MCP::CoreToolNames())
        EXPECT_TRUE(policy.ShouldList(ToolExposureFacts{ name, "scene", false })) << name;
    for (const std::string_view name : OloEngine::MCP::GatewayToolNames())
        EXPECT_TRUE(policy.ShouldList(ToolExposureFacts{ name, "gateway", false })) << name;

    EXPECT_FALSE(policy.ShouldList(ToolExposureFacts{ "olo_render_probe_pixel", "render", false }));
    EXPECT_FALSE(policy.ShouldList(ToolExposureFacts{ "olo_physics_raycast", "physics", false }));
}

TEST(McpExposureProfile, ToolsetAddsEnabledToolsetsOnTopOfCoreAndIgnoresTheRest)
{
    const ExposurePolicy policy{ ExposureProfile::Toolset, { "physics" } };
    EXPECT_TRUE(policy.ShouldList(ToolExposureFacts{ "olo_physics_raycast", "physics", false }));
    // Case-folded on both sides, so a mixed-case Toolset value still matches.
    EXPECT_TRUE(policy.ShouldList(ToolExposureFacts{ "olo_physics_other", "Physics", false }));
    EXPECT_FALSE(policy.ShouldList(ToolExposureFacts{ "olo_render_probe_pixel", "render", false }));
    // Core survives: enabling one toolset must not cost a session its orientation
    // tools, or every toolset user has to re-enable `scene` by hand.
    EXPECT_TRUE(policy.ShouldList(ToolExposureFacts{ "olo_scene_summary", "scene", false }));
}

TEST(McpExposureProfile, ToolsetWithNoEnabledToolsetsDegradesToCoreNotToFull)
{
    const ExposurePolicy policy{ ExposureProfile::Toolset, {} };
    EXPECT_TRUE(policy.ShouldList(ToolExposureFacts{ "olo_scene_summary", "scene", false }));
    EXPECT_FALSE(policy.ShouldList(ToolExposureFacts{ "olo_render_probe_pixel", "render", false }))
        << "an empty selection must not mean 'select everything'";
}

TEST(McpExposureProfile, UserProvidedToolsAreListedUnderEveryProfile)
{
    // A project Lua script tool and a bridged external tool exist only because this
    // user configured them. Hiding them would read as the configuration failing.
    for (const ExposureProfile profile : { ExposureProfile::Core, ExposureProfile::Toolset })
    {
        const ExposurePolicy policy{ profile, {} };
        EXPECT_TRUE(policy.ShouldList(ToolExposureFacts{ "script_my_tool", "script", true }));
        EXPECT_TRUE(policy.ShouldList(ToolExposureFacts{ "ext.files.read_file", "ext.files", true }));
    }
}

// ---- the core set is real ---------------------------------------------------

TEST(McpExposureProfile, EveryCoreToolNameResolvesAgainstTheRegisteredSurface)
{
    // The core set is a central list of NAMES, which is how it stays reviewable in
    // one place — and how it silently rots when a tool is renamed. This is the pin.
    const std::set<std::string> registered = ToolNamesIn(BuiltinToolsList(ExposureProfile::Full));
    ASSERT_FALSE(registered.empty());
    for (const std::string_view name : OloEngine::MCP::CoreToolNames())
    {
        EXPECT_TRUE(registered.contains(std::string(name)))
            << name << " is in CoreToolNames() but is not a registered tool — it was renamed or removed.";
    }
    for (const std::string_view name : OloEngine::MCP::GatewayToolNames())
        EXPECT_TRUE(registered.contains(std::string(name))) << name << " is not registered.";
}

TEST(McpExposureProfile, DefaultProfileListsExactlyTheCoreSetPlusTheGateway)
{
    const std::set<std::string> listed = ToolNamesIn(BuiltinToolsList(ExposureProfile::Core));

    std::set<std::string> expected;
    for (const std::string_view name : OloEngine::MCP::CoreToolNames())
        expected.insert(std::string(name));
    for (const std::string_view name : OloEngine::MCP::GatewayToolNames())
        expected.insert(std::string(name));

    EXPECT_EQ(listed, expected);
}

// ---- the acceptance criterion: the payload got small, and stays small --------

TEST(McpExposureProfile, DefaultToolsListIsASmallFractionOfTheFullCatalogue)
{
    McpServer server{ EditorMcpContext{} };
    OloEngine::MCP::RegisterBuiltinTools(server);

    server.SetExposurePolicy(ExposurePolicy{ ExposureProfile::Core, {} });
    const OloEngine::MCP::ToolRegistryMetrics metrics = server.ComputeRegistryMetrics();

    // PRINTED, not just asserted. #1124's root cause was that nobody knew the number
    // had tripled; a ceiling alone tells you when it breaks, this tells you where it
    // is on every run, including in a CI log someone reads six months from now.
    std::cout << "[  METRIC  ] tools/list core: " << metrics.ListedTools << " of " << metrics.TotalTools
              << " tools, " << metrics.ListedBytes << " bytes (~" << metrics.ApproxListedTokens()
              << " tokens); full: " << metrics.FullBytes << " bytes (~" << metrics.ApproxFullTokens()
              << " tokens)" << std::endl;

    // Sanity: the metric is measuring the real surface, not an empty registry. If
    // this trips, the assertions below are meaningless rather than reassuring.
    EXPECT_GE(metrics.TotalTools, 90u) << "the builtin surface shrank unexpectedly";
    EXPECT_GE(metrics.FullBytes, 200000u) << "the full catalogue is far smaller than #1124 measured "
                                             "(265 009 bytes) — check the serializer, not just this bound";

    // THE RATCHET. #1124's acceptance criterion is that the default profile costs a
    // small fraction of the full catalogue. The ceiling is deliberately absolute as
    // well as relative: a ratio alone would keep passing while both numbers grew.
    //
    // If this fails after adding a tool to CoreToolNames(), that is the trade-off
    // being made visible, which is the whole point — either the tool is worth the
    // bytes and this ceiling moves deliberately (say so in the commit), or it is
    // not and it stays discoverable through olo_tool_search.
    // Measured at 38 144 bytes (16 of 96 tools) when this landed. The ceiling sits
    // ~18% above that: enough that a core tool growing a description or a schema
    // property does not fail the build, tight enough that adding a whole tool to the
    // core set does.
    constexpr sizet kCoreProfileByteCeiling = 45000;
    EXPECT_LE(metrics.ListedBytes, kCoreProfileByteCeiling)
        << "the default tools/list is " << metrics.ListedBytes << " bytes (~" << metrics.ApproxListedTokens()
        << " tokens); ceiling is " << kCoreProfileByteCeiling;
    EXPECT_LT(metrics.ListedBytes * 4, metrics.FullBytes)
        << "the default profile must cost well under a quarter of the full catalogue";

    // The metrics describe the payload they claim to: ListedBytes is exactly the
    // `tools` array a client receives, not a model of it.
    const Json listed = server.HandleMessage(MakeRequest(2, "tools/list"));
    const Json reconstructed{ { "tools", listed["result"]["tools"] } };
    EXPECT_EQ(reconstructed.dump().size(), metrics.ListedBytes);
    EXPECT_EQ(metrics.ListedTools, listed["result"]["tools"].size());
}

TEST(McpExposureProfile, TokenEstimateIsBytesOverFour)
{
    // Documented as an estimate everywhere it surfaces; pinned so the two places
    // that report it cannot drift to different arithmetic.
    EXPECT_EQ(OloEngine::MCP::ApproxTokensForBytes(0u), 0u);
    EXPECT_EQ(OloEngine::MCP::ApproxTokensForBytes(4u), 1u);
    EXPECT_EQ(OloEngine::MCP::ApproxTokensForBytes(5u), 2u);
    EXPECT_EQ(OloEngine::MCP::ApproxTokensForBytes(265009u), 66253u);
}

// ---- compatibility: nothing was deleted -------------------------------------

TEST(McpExposureProfile, AToolHiddenByTheProfileIsStillCallableByName)
{
    // The backward-compatibility guarantee in one test: exposure filters the
    // LISTING, never dispatch. A client that learned a tool name from the docs, a
    // prompt or a previous session keeps working after the default narrowed.
    McpServer server{ EditorMcpContext{} };
    server.RegisterTool(MakeFakeTool("olo_hidden_probe", "render"));
    server.SetExposurePolicy(ExposurePolicy{ ExposureProfile::Core, {} });

    EXPECT_TRUE(ToolNamesIn(server.HandleMessage(MakeRequest(1, "tools/list"))).empty())
        << "the fake tool is neither core nor gateway, so it must not be listed";

    const Json call = server.HandleMessage(
        MakeRequest(2, "tools/call", Json{ { "name", "olo_hidden_probe" }, { "arguments", { { "echo", "x" } } } }));
    ASSERT_TRUE(call.contains("result")) << call.dump(2);
    EXPECT_FALSE(call["result"].value("isError", false));
    EXPECT_EQ(call["result"]["content"][0]["text"], "ran:x");
}

TEST(McpExposureProfile, ToolsSearchStillSeesTheWholeRegistryUnderTheNarrowDefault)
{
    // A narrowed tools/list that also narrowed its own escape hatch would be a dead
    // end. tools/search (#385) is deliberately profile-blind.
    McpServer server{ EditorMcpContext{} };
    server.RegisterTool(MakeFakeTool("olo_hidden_probe", "render"));
    server.SetExposurePolicy(ExposurePolicy{ ExposureProfile::Core, {} });

    const Json search = server.HandleMessage(MakeRequest(1, "tools/search", Json{ { "query", "hidden" } }));
    ASSERT_TRUE(search.contains("result")) << search.dump(2);
    ASSERT_EQ(search["result"]["tools"].size(), 1u);
    EXPECT_EQ(search["result"]["tools"][0]["name"], "olo_hidden_probe");
}

TEST(McpExposureProfile, WideningTheProfileNotifiesConnectedClients)
{
    // A client that cached the narrow listing must be told when the host widens it,
    // or it never learns the surface grew.
    McpServer server{ EditorMcpContext{} };
    std::vector<std::string> notifications;
    server.AddNotificationListener([&notifications](const Json& n)
                                   { notifications.push_back(n.value("method", std::string{})); });

    server.SetExposurePolicy(ExposurePolicy{ ExposureProfile::Full, {} });
    ASSERT_FALSE(notifications.empty());
    EXPECT_EQ(notifications.back(), "notifications/tools/list_changed");
}

// ---- the gateway round-trip -------------------------------------------------

TEST(McpExposureGateway, CapabilityReportsTheProfileTheHiddenCountAndHowToReachThem)
{
    McpServer server{ EditorMcpContext{} };
    OloEngine::MCP::RegisterBuiltinTools(server);
    server.SetExposurePolicy(ExposurePolicy{ ExposureProfile::Core, {} });

    const Json resp =
        server.HandleMessage(MakeRequest(1, "tools/call", Json{ { "name", "olo_capability" } }));
    ASSERT_TRUE(resp.contains("result")) << resp.dump(2);
    ASSERT_FALSE(resp["result"].value("isError", false)) << resp.dump(2);
    const Json& data = resp["result"]["structuredContent"];

    EXPECT_EQ(data["profile"], "core");
    EXPECT_GT(data["hiddenTools"].get<sizet>(), 60u);
    EXPECT_GT(data["registry"]["fullBytes"].get<sizet>(),
              data["registry"]["listedBytes"].get<sizet>());
    EXPECT_FALSE(data["howTo"].get<std::string>().empty());

    // The toolset catalogue describes the FULL registry, including what the profile
    // is hiding — otherwise it cannot answer "what else is there?".
    bool sawPartiallyHiddenToolset = false;
    for (const Json& toolset : data["toolsets"])
    {
        if (toolset["listed"].get<sizet>() < toolset["count"].get<sizet>())
            sawPartiallyHiddenToolset = true;
    }
    EXPECT_TRUE(sawPartiallyHiddenToolset);
}

TEST(McpExposureGateway, CapabilityReportIsInternallyConsistentAcrossOneProfile)
{
    // The consistency INVARIANT a mixed-policy report would violate: every listed tool
    // sits in exactly one toolset bucket, so the buckets must sum to
    // registry.listedTools, and hiddenTools must be the complement.
    //
    // Honest scope: single-threaded, both policy reads would return the same value, so
    // this does NOT by itself prove the mixed-policy fix -- it would pass without it.
    // What rules the mixed report out is structural (ComputeRegistryMetrics no longer
    // reads the policy at all; it takes one), and that property is pinned by
    // MetricsHonourThePassedPolicyRatherThanRereadingServerState below. This test earns
    // its place as the ratchet for the arithmetic itself, including an uncategorized
    // tool escaping the buckets.
    for (const ExposureProfile profile : { ExposureProfile::Core, ExposureProfile::Full })
    {
        McpServer server{ EditorMcpContext{} };
        OloEngine::MCP::RegisterBuiltinTools(server);
        server.SetExposurePolicy(ExposurePolicy{ profile, {} });

        const Json resp =
            server.HandleMessage(MakeRequest(1, "tools/call", Json{ { "name", "olo_capability" } }));
        ASSERT_TRUE(resp.contains("result")) << resp.dump(2);
        const Json& data = resp["result"]["structuredContent"];

        EXPECT_EQ(data["profile"], std::string(OloEngine::MCP::ToStringView(profile)));

        sizet bucketTotal = 0;
        sizet bucketListed = 0;
        for (const Json& toolset : data["toolsets"])
        {
            bucketTotal += toolset["count"].get<sizet>();
            bucketListed += toolset["listed"].get<sizet>();
        }
        // Precondition, and a useful ratchet in its own right: every builtin tool
        // carries a Toolset, so nothing escapes the buckets. A new uncategorized tool
        // fails here and should either be categorized or this test taught about it.
        EXPECT_EQ(bucketTotal, data["registry"]["totalTools"].get<sizet>())
            << "an uncategorized builtin tool exists, so the toolset buckets no longer "
               "cover the surface";
        EXPECT_EQ(bucketListed, data["registry"]["listedTools"].get<sizet>())
            << "toolset `listed` counts and registry.listedTools disagree - the report "
               "mixed two exposure policies";
        EXPECT_EQ(data["hiddenTools"].get<sizet>(),
                  data["registry"]["totalTools"].get<sizet>() - data["registry"]["listedTools"].get<sizet>());
    }
}

TEST(McpExposureProfile, MetricsHonourThePassedPolicyRatherThanRereadingServerState)
{
    // THE structural pin for the mixed-policy bug (CodeRabbit, PR #1136).
    // ComputeRegistryMetrics used to load m_ExposurePolicy itself, so a caller that had
    // already read the policy could end up describing two different ones in a single
    // report. It now takes the policy as a parameter, and this asserts it actually uses
    // that one: the server is left on `core` while `full` is passed in, and vice versa.
    // If the function ever goes back to reading server state, these disagree.
    McpServer server{ EditorMcpContext{} };
    OloEngine::MCP::RegisterBuiltinTools(server);
    const McpServer::ToolSnapshot snapshot = server.ToolsSnapshot();

    server.SetExposurePolicy(ExposurePolicy{ ExposureProfile::Core, {} });
    const OloEngine::MCP::ToolRegistryMetrics asFull =
        server.ComputeRegistryMetrics(snapshot, ExposurePolicy{ ExposureProfile::Full, {} });
    EXPECT_EQ(asFull.Profile, ExposureProfile::Full);
    EXPECT_EQ(asFull.ListedTools, asFull.TotalTools) << "server is on core, but `full` was passed in";
    EXPECT_EQ(asFull.ListedBytes, asFull.FullBytes);

    server.SetExposurePolicy(ExposurePolicy{ ExposureProfile::Full, {} });
    const OloEngine::MCP::ToolRegistryMetrics asCore =
        server.ComputeRegistryMetrics(snapshot, ExposurePolicy{ ExposureProfile::Core, {} });
    EXPECT_EQ(asCore.Profile, ExposureProfile::Core);
    EXPECT_LT(asCore.ListedTools, asCore.TotalTools) << "server is on full, but `core` was passed in";
    EXPECT_LT(asCore.ListedBytes, asCore.FullBytes);

    // Same registry both times, so the full-surface totals must agree exactly.
    EXPECT_EQ(asFull.TotalTools, asCore.TotalTools);
    EXPECT_EQ(asFull.FullBytes, asCore.FullBytes);
}

TEST(McpExposureGateway, SearchThenDescribeReachesAToolTheProfileHid)
{
    McpServer server{ EditorMcpContext{} };
    OloEngine::MCP::RegisterBuiltinTools(server);
    server.SetExposurePolicy(ExposurePolicy{ ExposureProfile::Core, {} });

    const Json search = server.HandleMessage(MakeRequest(
        1, "tools/call", Json{ { "name", "olo_tool_search" }, { "arguments", { { "query", "raycast" } } } }));
    ASSERT_TRUE(search.contains("result")) << search.dump(2);
    const Json& hits = search["result"]["structuredContent"]["tools"];
    ASSERT_FALSE(hits.empty());

    const auto hit = std::find_if(hits.begin(), hits.end(),
                                  [](const Json& t)
                                  { return t["name"] == "olo_physics_raycast"; });
    ASSERT_NE(hit, hits.end()) << search["result"]["structuredContent"].dump(2);
    EXPECT_FALSE((*hit)["listed"].get<bool>()) << "the hit must say it is hidden, not merely exist";
    // A search hit is a summary, not a schema — that omission IS the saving.
    EXPECT_FALSE(hit->contains("inputSchema"));

    const Json describe = server.HandleMessage(
        MakeRequest(2, "tools/call",
                    Json{ { "name", "olo_tool_describe" }, { "arguments", { { "names", { "olo_physics_raycast" } } } } }));
    ASSERT_TRUE(describe.contains("result")) << describe.dump(2);
    const Json& described = describe["result"]["structuredContent"]["tools"];
    ASSERT_EQ(described.size(), 1u);
    EXPECT_EQ(described[0]["name"], "olo_physics_raycast");
    EXPECT_TRUE(described[0].contains("inputSchema"));
    EXPECT_FALSE(describe["result"]["structuredContent"].contains("notFound"));

    // The described entry is what tools/list would have emitted, so the tool can be
    // called straight from it.
    Json listEntry = described[0];
    listEntry.erase("listed");
    const Json fullList = BuiltinToolsList(ExposureProfile::Full);
    const Json& listedTools = fullList["result"]["tools"];
    const auto same = std::find_if(listedTools.begin(), listedTools.end(),
                                   [](const Json& t)
                                   { return t["name"] == "olo_physics_raycast"; });
    ASSERT_NE(same, listedTools.end());
    EXPECT_EQ(listEntry, *same);
}

TEST(McpExposureGateway, DescribeReportsUnknownNamesInsteadOfFailingTheWholeCall)
{
    McpServer server{ EditorMcpContext{} };
    OloEngine::MCP::RegisterBuiltinTools(server);

    const Json resp = server.HandleMessage(MakeRequest(
        1, "tools/call",
        Json{ { "name", "olo_tool_describe" },
              { "arguments", { { "names", { "olo_scene_summary", "olo_not_a_tool" } } } } }));
    ASSERT_TRUE(resp.contains("result")) << resp.dump(2);
    const Json& data = resp["result"]["structuredContent"];
    ASSERT_EQ(data["tools"].size(), 1u);
    EXPECT_EQ(data["tools"][0]["name"], "olo_scene_summary");
    EXPECT_EQ(data["notFound"], Json::array({ "olo_not_a_tool" }));
}

TEST(McpExposureGateway, ExecuteDispatchesTheTargetAndReturnsItsResultVerbatim)
{
    McpServer server{ EditorMcpContext{} };
    server.RegisterTool(MakeFakeTool("olo_hidden_probe", "render"));
    OloEngine::MCP::RegisterGatewayTools(server);
    server.SetExposurePolicy(ExposurePolicy{ ExposureProfile::Core, {} });

    const Json resp = server.HandleMessage(MakeRequest(
        1, "tools/call",
        Json{ { "name", "olo_tool_execute" },
              { "arguments", { { "tool", "olo_hidden_probe" }, { "arguments", { { "echo", "via-gateway" } } } } } }));
    ASSERT_TRUE(resp.contains("result")) << resp.dump(2);
    EXPECT_FALSE(resp["result"].value("isError", false));
    EXPECT_EQ(resp["result"]["content"][0]["text"], "ran:via-gateway");
}

TEST(McpExposureGateway, ExecuteEnforcesTheTargetsInputSchemaNotItsOwn)
{
    // The reason execute is a REWRITE and not a handler: the target's validation,
    // consent gate and cancellation scope must apply unchanged. If this passed a
    // malformed payload through, the gateway would be a hole in the validator.
    McpServer server{ EditorMcpContext{} };
    ToolDef strict = MakeFakeTool("olo_strict", "render");
    strict.InputSchema = Json{ { "type", "object" },
                               { "properties", { { "count", { { "type", "integer" } } } } },
                               { "required", Json::array({ "count" }) } };
    server.RegisterTool(std::move(strict));
    OloEngine::MCP::RegisterGatewayTools(server);

    const Json resp = server.HandleMessage(MakeRequest(
        1, "tools/call",
        Json{ { "name", "olo_tool_execute" },
              { "arguments", { { "tool", "olo_strict" }, { "arguments", { { "count", "not-an-int" } } } } } }));
    ASSERT_TRUE(resp.contains("result")) << resp.dump(2);
    EXPECT_TRUE(resp["result"].value("isError", false)) << resp.dump(2);
    EXPECT_NE(resp["result"]["content"][0]["text"].get<std::string>().find("count"), std::string::npos);
}

TEST(McpExposureGateway, ExecuteHonoursTheWriteConsentGateOfItsTarget)
{
    McpServer server{ EditorMcpContext{} };
    ToolDef writer = MakeFakeTool("olo_fake_write", "scene");
    writer.ProjectWrite = true;
    server.RegisterTool(std::move(writer));
    OloEngine::MCP::RegisterGatewayTools(server);
    ASSERT_FALSE(server.AllowWrites()) << "writes are off by default";

    const Json refused = server.HandleMessage(MakeRequest(
        1, "tools/call",
        Json{ { "name", "olo_tool_execute" }, { "arguments", { { "tool", "olo_fake_write" } } } }));
    ASSERT_TRUE(refused.contains("error")) << refused.dump(2);

    server.SetAllowWrites(true);
    const Json allowed = server.HandleMessage(MakeRequest(
        2, "tools/call",
        Json{ { "name", "olo_tool_execute" }, { "arguments", { { "tool", "olo_fake_write" } } } }));
    ASSERT_TRUE(allowed.contains("result")) << allowed.dump(2);
    EXPECT_FALSE(allowed["result"].value("isError", false));
}

TEST(McpExposureGateway, ExecuteRejectsMalformedPayloadsAndRefusesToNest)
{
    McpServer server{ EditorMcpContext{} };
    OloEngine::MCP::RegisterGatewayTools(server);

    const auto execute = [&server](const Json& args, int id)
    {
        return server.HandleMessage(
            MakeRequest(id, "tools/call", Json{ { "name", "olo_tool_execute" }, { "arguments", args } }));
    };

    int id = 100;
    for (const Json& bad : { Json::object(),                                  // no 'tool'
                             Json{ { "tool", 7 } },                           // wrong type
                             Json{ { "tool", "" } },                          // empty
                             Json{ { "tool", "olo_x" }, { "arguments", 3 } }, // non-object args
                             Json{ { "tool", "olo_tool_execute" } } })        // nesting
    {
        const Json resp = execute(bad, ++id);
        ASSERT_TRUE(resp.contains("result")) << bad.dump() << " -> " << resp.dump(2);
        EXPECT_TRUE(resp["result"].value("isError", false)) << bad.dump() << " -> " << resp.dump(2);
    }

    // An unknown target is a protocol error, exactly as a direct tools/call for an
    // unknown name is — the rewrite hands the same envelope to the same check.
    const Json unknown = execute(Json{ { "tool", "olo_not_a_tool" } }, 200);
    ASSERT_TRUE(unknown.contains("error")) << unknown.dump(2);
}
