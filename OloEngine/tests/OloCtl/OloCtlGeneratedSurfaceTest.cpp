// OLO_TEST_LAYER: unit
//
// oloctl is GENERATED from the registry, not written against it (issue #1125).
//
// This file holds the issue's three acceptance criteria, each as an assertion
// rather than an inspection:
//
//   1. Adding a command to the registry makes it appear in the CLI with no
//      CLI-side edit. A command is registered in a bare AutomationRegistry here
//      and the REAL RunCli finds it, helps on it and runs it — no table was
//      touched, and there is no table to touch.
//   2. oloctl returns the same structured payload the equivalent MCP call
//      returns. The same command is invoked twice — once through the CLI, once
//      through McpServer's real tools/call dispatch — and the two payloads are
//      compared byte for byte. (The live A/B against a running editor is the
//      other half of this criterion and is recorded in the PR; what a test can
//      pin is that the two code paths agree, which is the part that can rot.)
//   3. Nothing on the write path is reachable without the consent gate. The gate
//      is asserted at both layers it exists at, and the second one is the point:
//      even with the CLI's refusal removed, the registry default-denies.
//
// No McpServer is constructed for (1) and (3) — the registry, the host and the
// CLI, and nothing else. That is the property #1123 extracted the registry to
// create, and this is the frontend it was created for.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationCatalogue.h"
#include "Automation/AutomationCommand.h"
#include "Automation/AutomationHost.h"
#include "Automation/AutomationRegistry.h"
#include "Automation/AutomationResult.h"
#include "MCP/McpExposure.h"
#include "MCP/McpServer.h"
#include "MCP/McpTools.h"
#include "OloCtl/CliRunner.h"
#include "OloCtl/RegistryCommandSource.h"

#include <sstream>
#include <string>
#include <vector>

using OloCtl::ParseCommandLine;
using OloCtl::RegistryCommandSource;
using OloCtl::RunCli;
namespace ExitCode = OloCtl::ExitCode;

using OloEngine::Automation::AutomationCommand;
using OloEngine::Automation::AutomationInvocation;
using OloEngine::Automation::AutomationRegistry;
using OloEngine::Automation::AutomationResult;
using OloEngine::Automation::AutomationWriteConsent;
using OloEngine::Automation::IAutomationHost;
using OloEngine::MCP::EditorMcpContext;
using OloEngine::MCP::ExposurePolicy;
using OloEngine::MCP::ExposureProfile;
using OloEngine::MCP::McpServer;

using Json = nlohmann::json;

namespace
{
    // The smallest thing that can run a command: an editor context and nothing
    // else. Same shape as McpAutomationRegistryTest's, and for the same reason —
    // a CLI host is this plus a way to reach a live editor.
    class HeadlessHost final : public IAutomationHost
    {
      public:
        [[nodiscard]] const EditorMcpContext& Context() const override
        {
            return m_Context;
        }
        [[nodiscard]] bool IsCurrentCallCancelled() const override
        {
            return false;
        }
        [[nodiscard]] bool PublishArtifact(OloEngine::Automation::AutomationArtifact) override
        {
            return false;
        }

      protected:
        Json MarshalReadOnMainThread(const std::function<Json()>& readJob, std::chrono::milliseconds) override
        {
            // One thread, so the job runs here: "where the ECS is safe to read" is
            // right here in a single-threaded host.
            return readJob();
        }
        void EmitProgressUpdate(f64, f64, const std::string&) const override
        {
        }

      private:
        EditorMcpContext m_Context;
    };

    // A command nothing in oloctl has ever heard of.
    AutomationCommand MakeInventedCommand()
    {
        AutomationCommand command;
        command.Name = "olo_invented_measure_thing";
        command.Title = "Measure a thing";
        command.Toolset = "invented";
        command.Description = "Measures a thing that did not exist when oloctl was compiled.";
        command.InputSchema = Json{ { "type", "object" },
                                    { "properties",
                                      { { "thingId", { { "type", "integer" }, { "description", "Which thing to measure." } } } } },
                                    { "required", Json::array({ "thingId" }) } };
        command.Handler = [](IAutomationHost&, const Json& arguments)
        { return AutomationResult::Structured(Json{ { "measured", arguments.at("thingId") } }); };
        return command;
    }

    struct CliRun
    {
        int Code = 0;
        std::string Out;
        std::string Err;
    };

    CliRun RunOloCtl(const AutomationRegistry& registry, IAutomationHost& host,
                     const std::vector<std::string>& args)
    {
        RegistryCommandSource source(registry, host, "in-process registry");
        std::ostringstream out;
        std::ostringstream err;
        const int code = RunCli(ParseCommandLine(args), source, out, err);
        return { code, out.str(), err.str() };
    }
} // namespace

// ---- 1. a new command appears with no CLI-side edit -------------------------

TEST(OloCtlGeneratedSurface, ACommandRegisteredAtRuntimeAppearsInTheCli)
{
    AutomationRegistry registry;
    registry.Register(MakeInventedCommand());
    HeadlessHost host;

    // It has a group of its own, derived from the toolset it declared.
    const CliRun listing = RunOloCtl(registry, host, { "help" });
    ASSERT_EQ(listing.Code, ExitCode::Ok) << listing.Err;
    EXPECT_NE(listing.Out.find("invented"), std::string::npos) << listing.Out;

    // Its group lists it, under the spelling the derivation gave it.
    const CliRun group = RunOloCtl(registry, host, { "invented" });
    ASSERT_EQ(group.Code, ExitCode::Ok) << group.Err;
    EXPECT_NE(group.Out.find("measure-thing"), std::string::npos) << group.Out;

    // Its help is its own declared schema, rendered.
    const CliRun help = RunOloCtl(registry, host, { "invented", "measure-thing", "--help" });
    ASSERT_EQ(help.Code, ExitCode::Ok) << help.Err;
    EXPECT_NE(help.Out.find("--thing-id"), std::string::npos) << help.Out;
    EXPECT_NE(help.Out.find("Which thing to measure."), std::string::npos) << help.Out;
    EXPECT_NE(help.Out.find("(required)"), std::string::npos) << help.Out;

    // And it RUNS, with its argument typed by the schema it declared.
    const CliRun invocation = RunOloCtl(registry, host, { "invented", "measure-thing", "--thing-id", "7" });
    ASSERT_EQ(invocation.Code, ExitCode::Ok) << invocation.Err;
    const Json payload = Json::parse(invocation.Out, nullptr, false);
    ASSERT_FALSE(payload.is_discarded()) << invocation.Out;
    EXPECT_EQ(payload["structuredContent"]["measured"], 7);
}

// A registry that grows between two runs grows the CLI between two runs. The
// source deliberately re-reads rather than caching, because a registry can swap
// its command list while a frontend holds it (script-command live reload, #607).
TEST(OloCtlGeneratedSurface, TheCliSurfaceFollowsTheRegistryWithinOneProcess)
{
    AutomationRegistry registry;
    HeadlessHost host;

    const CliRun before = RunOloCtl(registry, host, { "invented", "measure-thing" });
    EXPECT_EQ(before.Code, ExitCode::Usage) << "the group must not exist before the command is registered";

    registry.Register(MakeInventedCommand());

    const CliRun after = RunOloCtl(registry, host, { "invented", "measure-thing", "--thing-id", "1" });
    EXPECT_EQ(after.Code, ExitCode::Ok) << after.Err;
}

// The whole real surface, through the CLI, with no server anywhere.
TEST(OloCtlGeneratedSurface, EveryProductionCommandIsAddressableThroughTheCli)
{
    AutomationRegistry registry;
    OloEngine::MCP::RegisterBuiltinCommands(registry);
    HeadlessHost host;

    const CliRun dump = RunOloCtl(registry, host, { "catalogue", "--compact" });
    ASSERT_EQ(dump.Code, ExitCode::Ok) << dump.Err;
    const Json catalogue = Json::parse(dump.Out, nullptr, false);
    ASSERT_FALSE(catalogue.is_discarded()) << dump.Out;

    ASSERT_GE(catalogue["commands"].size(), 80u) << "the production command surface failed to register";
    EXPECT_TRUE(catalogue["rejected"].empty()) << catalogue["rejected"].dump(2);
    EXPECT_EQ(catalogue["commands"].size(), registry.Count())
        << "every registered command must reach the CLI's catalogue";

    for (const Json& command : catalogue["commands"])
    {
        EXPECT_FALSE(command["group"].get<std::string>().empty()) << command["name"];
        EXPECT_FALSE(command["command"].get<std::string>().empty()) << command["name"];
        EXPECT_NE(command["authority"], "unknown")
            << command["name"] << " has no authority class, so oloctl would refuse it";
    }
}

// The exposure profile narrows what an MCP client is SHOWN (#1124/#1136); it has
// never narrowed what is callable. A CLI has no context window to protect, so it
// takes the whole registry — and this is the assertion that says so, because the
// alternative (a CLI that silently offers 16 of 93 commands) would look like a
// working CLI.
TEST(OloCtlGeneratedSurface, TheCliSurfaceIsTheWholeRegistryNotTheCoreProfile)
{
    AutomationRegistry registry;
    OloEngine::MCP::RegisterBuiltinCommands(registry);
    HeadlessHost host;

    const CliRun dump = RunOloCtl(registry, host, { "catalogue", "--compact" });
    ASSERT_EQ(dump.Code, ExitCode::Ok) << dump.Err;
    const Json catalogue = Json::parse(dump.Out);

    std::size_t coreCount = 0;
    for (const Json& command : catalogue["commands"])
    {
        if (OloEngine::MCP::IsCoreTool(command["name"].get<std::string>()))
            ++coreCount;
    }
    EXPECT_GT(catalogue["commands"].size(), coreCount * 2)
        << "the CLI is offering roughly the core profile, not the registry";
}

// ---- 2. the same payload as the equivalent MCP call -------------------------

TEST(OloCtlGeneratedSurface, TheCliPayloadIsByteIdenticalToTheMcpToolsCallPayload)
{
    // Two callers of the same command surface: a bare registry with no server,
    // and a real McpServer running its real JSON-RPC dispatch.
    AutomationRegistry registry;
    OloEngine::MCP::RegisterBuiltinCommands(registry);
    HeadlessHost host;

    McpServer server{ EditorMcpContext{} };
    OloEngine::MCP::RegisterBuiltinTools(server);
    server.SetExposurePolicy(ExposurePolicy{ ExposureProfile::Full, {} });

    // olo_physics_layer_matrix: a REAL registered command with a typed payload,
    // chosen because it is lock-safe (MainMarshaled false). A main-marshaled
    // command would compare a CLI result against an MCP error here — this server
    // is not Start()ed, so it has no game thread to marshal onto — and that
    // difference would be about the fixture, not about the two frontends.
    const CliRun cli = RunOloCtl(registry, host, { "physics", "layer-matrix", "--compact" });
    ASSERT_EQ(cli.Code, ExitCode::Ok) << cli.Err;
    const Json fromCli = Json::parse(cli.Out, nullptr, false);
    ASSERT_FALSE(fromCli.is_discarded()) << cli.Out;
    ASSERT_TRUE(fromCli.contains("structuredContent")) << fromCli.dump(2);

    const Json response = server.HandleMessage(Json{ { "jsonrpc", "2.0" },
                                                     { "id", 1 },
                                                     { "method", "tools/call" },
                                                     { "params",
                                                       { { "name", "olo_physics_layer_matrix" },
                                                         { "arguments", Json::object() } } } });
    ASSERT_TRUE(response.contains("result")) << response.dump(2);

    EXPECT_EQ(fromCli, response["result"])
        << "oloctl and tools/call disagree about the same command's result.\n  cli: " << fromCli.dump(2)
        << "\n  mcp: " << response["result"].dump(2);
}

// The same for a command's ERROR path. Invalid arguments are a command-level
// error under both frontends (SEP-1303) and the message is composed in one
// place, so a script parsing the failure sees the same bytes either way.
TEST(OloCtlGeneratedSurface, TheCliInvalidArgumentPayloadMatchesTheMcpOne)
{
    AutomationRegistry registry;
    registry.Register(MakeInventedCommand());
    HeadlessHost host;

    McpServer server{ EditorMcpContext{} };
    server.RegisterTool(MakeInventedCommand());
    server.SetExposurePolicy(ExposurePolicy{ ExposureProfile::Full, {} });

    // `thingId` is declared an integer and required.
    const CliRun cli = RunOloCtl(registry, host, { "invented", "measure-thing", "--arguments-json", R"({"thingId":"x"})", "--compact" });
    EXPECT_EQ(cli.Code, ExitCode::CommandError) << cli.Err;
    const Json fromCli = Json::parse(cli.Out, nullptr, false);
    ASSERT_FALSE(fromCli.is_discarded()) << cli.Out;
    EXPECT_EQ(fromCli["isError"], true);

    const Json response = server.HandleMessage(Json{ { "jsonrpc", "2.0" },
                                                     { "id", 1 },
                                                     { "method", "tools/call" },
                                                     { "params",
                                                       { { "name", "olo_invented_measure_thing" },
                                                         { "arguments", { { "thingId", "x" } } } } } });
    ASSERT_TRUE(response.contains("result")) << response.dump(2);
    EXPECT_EQ(fromCli, response["result"]);
}

// The same for the CATALOGUE side: the entry oloctl generates a command from is
// the entry tools/list publishes, plus the authority class tools/list has no
// field for.
TEST(OloCtlGeneratedSurface, TheCatalogueEntryMatchesTheToolsListEntry)
{
    AutomationRegistry registry;
    OloEngine::MCP::RegisterBuiltinCommands(registry);

    for (const AutomationCommand& command : *registry.Snapshot())
    {
        Json frontend = OloEngine::Automation::DescribeForFrontend(command);
        ASSERT_TRUE(frontend.contains("projectWrite")) << command.Name;
        frontend.erase("projectWrite");
        EXPECT_EQ(frontend, McpServer::BuildToolEntry(command)) << command.Name;
    }
}

// ---- 3. the write path is closed --------------------------------------------

TEST(OloCtlGeneratedSurface, AProjectWriteCommandIsRefusedByTheCli)
{
    AutomationCommand writer = MakeInventedCommand();
    writer.Name = "olo_invented_break_thing";
    writer.ProjectWrite = true;
    bool ran = false;
    writer.Handler = [&ran](IAutomationHost&, const Json&)
    {
        ran = true;
        return AutomationResult::Text("mutated");
    };

    AutomationRegistry registry;
    registry.Register(std::move(writer));
    HeadlessHost host;

    const CliRun run = RunOloCtl(registry, host, { "invented", "break-thing", "--thing-id", "1" });

    EXPECT_EQ(run.Code, ExitCode::Refused);
    EXPECT_FALSE(ran) << "the handler must not run: oloctl has no way to ask the human";
    EXPECT_TRUE(run.Out.empty());
}

// The second layer, and the one that matters. If the CLI's refusal were deleted
// tomorrow, this is what would still stop a write: RegistryCommandSource calls
// AutomationRegistry::Invoke with the DEFAULT consent, and there is no parameter
// on it that could say otherwise.
TEST(OloCtlGeneratedSurface, TheRegistrySourceStillRefusesAWriteIfTheCliGateIsBypassed)
{
    AutomationCommand writer = MakeInventedCommand();
    writer.Name = "olo_invented_break_thing";
    writer.ProjectWrite = true;
    bool ran = false;
    writer.Handler = [&ran](IAutomationHost&, const Json&)
    {
        ran = true;
        return AutomationResult::Text("mutated");
    };

    AutomationRegistry registry;
    registry.Register(std::move(writer));
    HeadlessHost host;
    RegistryCommandSource source(registry, host, "in-process registry");

    // Straight past RunCli, to the seam the CLI would use.
    const OloCtl::ICommandSource::InvokeOutcome outcome =
        source.Invoke("olo_invented_break_thing", Json{ { "thingId", 1 } });

    EXPECT_FALSE(outcome.Ok);
    EXPECT_FALSE(ran);

    // And the registry says why, in its own words.
    const AutomationInvocation direct = registry.Invoke(host, "olo_invented_break_thing", Json{ { "thingId", 1 } });
    EXPECT_EQ(direct.Outcome, AutomationInvocation::Status::WriteConsentWithheld);
}

// The read-only surface is genuinely usable: a read command is not caught by the
// gate. Without this, a gate that refused everything would pass every assertion
// above.
TEST(OloCtlGeneratedSurface, AReadOnlyCommandIsNotCaughtByTheWriteGate)
{
    AutomationRegistry registry;
    registry.Register(MakeInventedCommand());
    HeadlessHost host;

    const CliRun run = RunOloCtl(registry, host, { "invented", "measure-thing", "--thing-id", "2" });
    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
}

// Argument validation stays the host's job, through the same validator an MCP
// call goes through — oloctl does not carry a second opinion that could disagree.
TEST(OloCtlGeneratedSurface, InvalidArgumentsAreRejectedByTheHostNotByTheCli)
{
    AutomationRegistry registry;
    registry.Register(MakeInventedCommand());
    HeadlessHost host;

    // `thingId` is required and absent. The CLI does not know that — `required` is
    // not something it reads — so the request is built and the host refuses it.
    const CliRun run = RunOloCtl(registry, host, { "invented", "measure-thing" });

    EXPECT_EQ(run.Code, ExitCode::CommandError) << run.Err;
    const Json payload = Json::parse(run.Out, nullptr, false);
    ASSERT_FALSE(payload.is_discarded()) << run.Out;
    EXPECT_EQ(payload["isError"], true);
    EXPECT_NE(payload.dump().find("thingId"), std::string::npos) << payload.dump(2);
}
