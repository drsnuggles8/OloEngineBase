// The transport-independent automation registry (issue #1123).
//
// Two things are pinned here, and they are the two the extraction exists for:
//
//   1. A command runs with NO MCP SERVER CONSTRUCTED AT ALL. Not "it compiles
//      without one" — the real, production-registered commands are put into a
//      bare AutomationRegistry, invoked against a minimal IAutomationHost, and
//      their results read back. This is the property the CLI (#1125) and the
//      headless epics need, and an assertion that the types merely link would
//      not have caught the coupling this issue removed.
//
//   2. Every command in the registry is reachable through the MCP adapter with
//      the schema it declared. The adapter is now one caller among possible
//      several; a command it silently fails to surface would be invisible to
//      tools/list and to every ratchet that reads tools/list.
//
// The COW snapshot semantics themselves (natives-before-scripts ordering, an
// in-flight snapshot surviving a swap) stay pinned where they always were, in
// McpProtocolIconsTest and McpScriptToolsTest, which drive them through the
// server — moving those assertions here would have weakened them.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationCommand.h"
#include "Automation/AutomationHost.h"
#include "Automation/AutomationRegistry.h"
#include "Automation/AutomationResult.h"
#include "MCP/McpExposure.h"
#include "MCP/McpServer.h"
#include "MCP/McpTools.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using OloEngine::MCP::EditorMcpContext;
using OloEngine::MCP::ExposurePolicy;
using OloEngine::MCP::ExposureProfile;
using OloEngine::MCP::Json;
using OloEngine::MCP::McpServer;

using OloEngine::Automation::AutomationArtifact;
using OloEngine::Automation::AutomationCommand;
using OloEngine::Automation::AutomationInvocation;
using OloEngine::Automation::AutomationRegistry;
using OloEngine::Automation::AutomationResult;
using OloEngine::Automation::IAutomationHost;

namespace
{
    // The smallest thing that can run a command: an editor context and nothing
    // else. No socket, no session, no consent state, no JSON-RPC — which is the
    // point. A CLI host (#1125) is this plus a way to reach a live editor.
    class HeadlessHost final : public IAutomationHost
    {
      public:
        explicit HeadlessHost(EditorMcpContext context = EditorMcpContext{})
            : m_Context(std::move(context))
        {
        }

        [[nodiscard]] const EditorMcpContext& Context() const override
        {
            return m_Context;
        }

        [[nodiscard]] bool IsCurrentCallCancelled() const override
        {
            return m_Cancelled;
        }

        [[nodiscard]] bool PublishArtifact(AutomationArtifact /*artifact*/) override
        {
            // No artifact store: a command asking for resource-link delivery must be
            // told so, not handed a link to nothing.
            return false;
        }

        void Cancel()
        {
            m_Cancelled = true;
        }

        [[nodiscard]] const std::vector<std::string>& ProgressMessages() const
        {
            return m_Progress;
        }

        [[nodiscard]] int MarshalCount() const
        {
            return m_MarshalCount;
        }

      protected:
        Json MarshalReadOnMainThread(const std::function<Json()>& readJob,
                                     std::chrono::milliseconds /*timeout*/) override
        {
            // There is no game thread here, so the job runs inline. That is the
            // honest behaviour for a host with one thread — the contract a
            // main-marshaled command relies on is "this runs where the ECS is safe
            // to read", and in a single-threaded host that is right here.
            ++m_MarshalCount;
            return readJob();
        }

        void EmitProgressUpdate(f64 /*progress*/, f64 /*total*/, const std::string& message) const override
        {
            m_Progress.push_back(message);
        }

      private:
        EditorMcpContext m_Context;
        bool m_Cancelled = false;
        int m_MarshalCount = 0;
        mutable std::vector<std::string> m_Progress;
    };

    AutomationCommand MakeEchoCommand(std::string name = "olo_fake_echo")
    {
        AutomationCommand command;
        command.Name = std::move(name);
        command.Title = "Echo";
        command.Toolset = "diag";
        command.Description = "Echoes its argument back.";
        command.InputSchema = Json{ { "type", "object" },
                                    { "properties", { { "text", { { "type", "string" } } } } },
                                    { "required", Json::array({ "text" }) } };
        command.Handler = [](IAutomationHost&, const Json& args)
        { return AutomationResult::Structured(Json{ { "echo", args.at("text") } }); };
        return command;
    }
} // namespace

// ---- 1. a command runs with no server ---------------------------------------

TEST(McpAutomationRegistry, ACommandRunsAgainstABareRegistryWithNoServer)
{
    AutomationRegistry registry;
    registry.Register(MakeEchoCommand());

    HeadlessHost host;
    const AutomationInvocation outcome = registry.Invoke(host, "olo_fake_echo", Json{ { "text", "hello" } });

    ASSERT_TRUE(outcome.Ran()) << outcome.Message;
    EXPECT_FALSE(outcome.Result.IsError);
    ASSERT_TRUE(outcome.Result.StructuredContent.is_object());
    EXPECT_EQ(outcome.Result.StructuredContent["echo"], "hello");
}

// The one that would have failed before the extraction: the REAL registration
// functions, the REAL handlers, no McpServer anywhere in the test.
TEST(McpAutomationRegistry, TheProductionCommandSurfaceRegistersAndRunsWithNoServer)
{
    AutomationRegistry registry;
    OloEngine::MCP::RegisterBuiltinCommands(registry);

    // The full native surface minus the four discovery-gateway commands, which
    // describe an MCP catalogue and are therefore registered by the adapter.
    EXPECT_GE(registry.Count(), 85u) << "the builtin command surface should register into a bare registry";

    HeadlessHost host;
    const AutomationInvocation outcome = registry.Invoke(host, "olo_scene_summary", Json::object());

    // The handler RAN and produced its real typed payload: it marshalled a read
    // through the host, looked behind the (empty) editor-context hooks, and
    // answered honestly. A crash, an exception, or a "no such tool" would each
    // mean the command was still reachable only through the transport.
    ASSERT_TRUE(outcome.Ran()) << outcome.Message;
    EXPECT_GT(host.MarshalCount(), 0) << "olo_scene_summary is main-marshaled; the host should have been asked";
    EXPECT_FALSE(outcome.Result.IsError);
    ASSERT_TRUE(outcome.Result.StructuredContent.is_object()) << outcome.Result.Content.dump(2);
    EXPECT_EQ(outcome.Result.StructuredContent["hasActiveScene"], false);
    EXPECT_EQ(outcome.Result.StructuredContent["isPlaying"], false);
}

TEST(McpAutomationRegistry, AnUnknownCommandIsReportedRatherThanRun)
{
    AutomationRegistry registry;
    registry.Register(MakeEchoCommand());

    HeadlessHost host;
    const AutomationInvocation outcome = registry.Invoke(host, "olo_not_a_tool", Json::object());

    EXPECT_EQ(outcome.Outcome, AutomationInvocation::Status::UnknownCommand);
    EXPECT_NE(outcome.Message.find("olo_not_a_tool"), std::string::npos);
}

TEST(McpAutomationRegistry, ArgumentsAreValidatedAgainstTheDeclaredInputSchema)
{
    AutomationRegistry registry;
    registry.Register(MakeEchoCommand());

    HeadlessHost host;
    const AutomationInvocation missing = registry.Invoke(host, "olo_fake_echo", Json::object());
    EXPECT_EQ(missing.Outcome, AutomationInvocation::Status::InvalidArguments);
    EXPECT_NE(missing.Message.find("text"), std::string::npos);

    const AutomationInvocation wrongType = registry.Invoke(host, "olo_fake_echo", Json{ { "text", 7 } });
    EXPECT_EQ(wrongType.Outcome, AutomationInvocation::Status::InvalidArguments);
}

TEST(McpAutomationRegistry, AHandlerThatThrowsBecomesAnErrorResultNotAnEscapingException)
{
    AutomationCommand boom = MakeEchoCommand("olo_fake_boom");
    boom.InputSchema = Json{ { "type", "object" } };
    boom.Handler = [](IAutomationHost&, const Json&) -> AutomationResult
    { throw std::runtime_error("kaboom"); };

    AutomationRegistry registry;
    registry.Register(std::move(boom));

    HeadlessHost host;
    const AutomationInvocation outcome = registry.Invoke(host, "olo_fake_boom", Json::object());

    ASSERT_TRUE(outcome.Ran());
    EXPECT_TRUE(outcome.Result.IsError);
    ASSERT_TRUE(outcome.Result.Content.is_array());
    ASSERT_FALSE(outcome.Result.Content.empty());
    EXPECT_NE(outcome.Result.Content[0]["text"].get<std::string>().find("kaboom"), std::string::npos);
}

// ---- the availability predicate ---------------------------------------------

TEST(McpAutomationRegistry, AnUnavailableCommandCannotBeInvoked)
{
    AutomationCommand gated = MakeEchoCommand("olo_fake_gated");
    gated.IsAvailable = [](const IAutomationHost&)
    { return false; };

    AutomationRegistry registry;
    registry.Register(std::move(gated));

    HeadlessHost host;
    const AutomationInvocation outcome = registry.Invoke(host, "olo_fake_gated", Json{ { "text", "x" } });
    EXPECT_EQ(outcome.Outcome, AutomationInvocation::Status::Unavailable);
}

TEST(McpAutomationRegistry, AnAvailabilityPredicateThatThrowsMeansUnavailable)
{
    AutomationCommand gated = MakeEchoCommand("olo_fake_gated");
    gated.IsAvailable = [](const IAutomationHost&) -> bool
    { throw std::runtime_error("probe failed"); };

    HeadlessHost host;
    EXPECT_FALSE(gated.AvailableOn(host));
}

TEST(McpAutomationRegistry, NoBuiltinCommandDeclaresAnAvailabilityPredicateYet)
{
    // The predicate is new machinery with a deliberately empty adopter set: #1123
    // is a pure extraction, so tools/list must be byte-identical, and a command
    // that opted in would change it. This pins that, so the first adopter is a
    // deliberate act with a visible diff rather than a silent narrowing.
    AutomationRegistry registry;
    OloEngine::MCP::RegisterBuiltinCommands(registry);

    const AutomationRegistry::CommandSnapshot snapshot = registry.Snapshot();
    for (const AutomationCommand& command : *snapshot)
        EXPECT_FALSE(static_cast<bool>(command.IsAvailable)) << command.Name << " declares an availability predicate";
}

TEST(McpAutomationRegistry, NoBuiltinCommandDeclaresUndoSemanticsYet)
{
    // Same reasoning as the availability pin: AutomationUndo is declared for
    // #1128 and consumed by nothing, so every command must still say Unspecified.
    // When #1128 starts populating it, this test is the thing that has to change
    // on purpose.
    AutomationRegistry registry;
    OloEngine::MCP::RegisterBuiltinCommands(registry);

    const AutomationRegistry::CommandSnapshot snapshot = registry.Snapshot();
    for (const AutomationCommand& command : *snapshot)
        EXPECT_EQ(command.Undo, OloEngine::Automation::AutomationUndo::Unspecified) << command.Name;
}

// ---- copy-on-write publication ----------------------------------------------

TEST(McpAutomationRegistry, ReplacingScriptCommandsKeepsNativesAheadAndBumpsTheGeneration)
{
    AutomationRegistry registry;
    registry.Register(MakeEchoCommand("olo_native"));

    const u64 before = registry.Generation();

    AutomationCommand scripted = MakeEchoCommand("script_one");
    scripted.ScriptOwned = true;
    registry.ReplaceScriptCommands({ std::move(scripted) });

    const AutomationRegistry::CommandSnapshot first = registry.Snapshot();
    ASSERT_EQ(first->size(), 2u);
    EXPECT_EQ((*first)[0].Name, "olo_native");
    EXPECT_EQ((*first)[1].Name, "script_one");
    EXPECT_GT(registry.Generation(), before);

    AutomationCommand replacement = MakeEchoCommand("script_two");
    replacement.ScriptOwned = true;
    registry.ReplaceScriptCommands({ std::move(replacement) });

    // The snapshot taken before the swap is untouched — the property an in-flight
    // call (and the Lua state its handler owns) depends on.
    ASSERT_EQ(first->size(), 2u);
    EXPECT_EQ((*first)[1].Name, "script_one");

    const AutomationRegistry::CommandSnapshot second = registry.Snapshot();
    ASSERT_EQ(second->size(), 2u);
    EXPECT_EQ((*second)[0].Name, "olo_native");
    EXPECT_EQ((*second)[1].Name, "script_two");
}

TEST(McpAutomationRegistry, AChangeListenerFiresOnAReplaceButNotOnAPlainRegister)
{
    AutomationRegistry registry;
    int fired = 0;
    registry.SetChangeListener([&fired]
                               { ++fired; });

    registry.Register(MakeEchoCommand("olo_native"));
    EXPECT_EQ(fired, 0) << "startup registration is not a catalogue change anyone is listening for";

    registry.ReplaceScriptCommands({});
    EXPECT_EQ(fired, 1);

    EXPECT_EQ(registry.ReplaceClientCommands("files", {}), 0u);
    EXPECT_EQ(fired, 2);
}

// ---- 2. every registry command is reachable through the MCP adapter ---------

TEST(McpAutomationRegistry, EveryRegistryCommandIsReachableThroughTheMcpAdapterWithItsDeclaredSchema)
{
    McpServer server{ EditorMcpContext{} };
    OloEngine::MCP::RegisterBuiltinTools(server);
    // `full` so the exposure profile (a LISTING filter, #1124) is not what this
    // measures — the question here is whether the adapter can see the registry,
    // not which subset it chooses to advertise.
    server.SetExposurePolicy(ExposurePolicy{ ExposureProfile::Full, {} });

    const Json response = server.HandleMessage(
        Json{ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "tools/list" } });
    ASSERT_TRUE(response.contains("result")) << response.dump(2);
    const Json& listed = response["result"]["tools"];
    ASSERT_TRUE(listed.is_array());

    const AutomationRegistry::CommandSnapshot snapshot = server.Registry().Snapshot();
    ASSERT_FALSE(snapshot->empty());
    EXPECT_EQ(listed.size(), snapshot->size())
        << "the full profile must list exactly the registry, no more and no fewer";

    for (const AutomationCommand& command : *snapshot)
    {
        const Json* entry = nullptr;
        for (const Json& candidate : listed)
        {
            if (candidate.value("name", std::string{}) == command.Name)
            {
                entry = &candidate;
                break;
            }
        }
        ASSERT_NE(entry, nullptr) << command.Name << " is registered but the adapter does not list it";
        // Byte-for-byte the entry the adapter builds from the command: the schema a
        // client receives IS the schema the command declared, not a copy that can drift.
        EXPECT_EQ(*entry, McpServer::BuildToolEntry(command)) << command.Name;
    }
}

TEST(McpAutomationRegistry, TheServerRegistersIntoTheRegistryItAdapts)
{
    McpServer server{ EditorMcpContext{} };
    EXPECT_EQ(server.Registry().Count(), 0u);

    server.RegisterTool(MakeEchoCommand());

    EXPECT_EQ(server.Registry().Count(), 1u);
    EXPECT_EQ(server.ToolCount(), 1u);
    ASSERT_EQ(server.ToolsSnapshot()->size(), 1u);
    EXPECT_EQ((*server.ToolsSnapshot())[0].Name, "olo_fake_echo");
}
