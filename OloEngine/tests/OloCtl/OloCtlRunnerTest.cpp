// OLO_TEST_LAYER: unit
//
// The oloctl command line end to end, over a fake command source (#1125).
//
// RunCli takes its source and its two streams as parameters precisely so this
// file can exist: every decision the CLI makes — resolution, help, the write
// refusal, the exit codes, and the output discipline that keeps stdout parseable
// — is exercised here without a process, a socket or a temp file.
//
// THE OUTPUT DISCIPLINE IS THE ONE WORTH SPELLING OUT. `oloctl ... | jq` in a CI
// step breaks the moment one warning reaches stdout, and it breaks silently,
// on the run that had something to warn about. So the assertions below check
// both streams: the payload is on `out` and NOTHING else ever is.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloCtl/CliRunner.h"
#include "OloCtl/CommandCatalogue.h"
#include "OloCtl/CommandSource.h"

#include <sstream>
#include <string>
#include <vector>

using OloCtl::AuthorityClass;
using OloCtl::Catalogue;
using OloCtl::CatalogueEntry;
using OloCtl::ICommandSource;
using OloCtl::ParseCommandLine;
using OloCtl::RunCli;
namespace ExitCode = OloCtl::ExitCode;

using Json = nlohmann::json;

namespace
{
    // Answers from a fixed catalogue and records what it was asked to run. The
    // recording is what proves the write gate refuses BEFORE dispatch rather than
    // after — a gate that let the request through and discarded the answer would
    // pass every assertion about the message and none about this vector.
    class FakeSource final : public ICommandSource
    {
      public:
        explicit FakeSource(Catalogue catalogue) : m_Catalogue(std::move(catalogue))
        {
        }

        const Catalogue* FetchCatalogue(std::string& outError) override
        {
            if (!m_ConnectionError.empty())
            {
                outError = m_ConnectionError;
                return nullptr;
            }
            return &m_Catalogue;
        }

        InvokeOutcome Invoke(const std::string& name, const Json& arguments) override
        {
            Invocations.push_back({ name, arguments });
            InvokeOutcome outcome;
            if (!m_TransportError.empty())
            {
                outcome.Error = m_TransportError;
                return outcome;
            }
            outcome.Ok = true;
            outcome.Result = m_Result;
            return outcome;
        }

        void FailToConnect(std::string error)
        {
            m_ConnectionError = std::move(error);
        }
        void FailTransport(std::string error)
        {
            m_TransportError = std::move(error);
        }
        void SetResult(Json result)
        {
            m_Result = std::move(result);
        }

        struct Invocation
        {
            std::string Name;
            Json Arguments;
        };
        std::vector<Invocation> Invocations;

      private:
        Catalogue m_Catalogue;
        std::string m_ConnectionError;
        std::string m_TransportError;
        Json m_Result = Json{ { "content", Json::array() }, { "isError", false } };
    };

    CatalogueEntry Entry(std::string name, std::string toolset, AuthorityClass authority)
    {
        CatalogueEntry entry;
        entry.Name = std::move(name);
        entry.Toolset = std::move(toolset);
        entry.Description = "A command.";
        entry.Authority = authority;
        entry.InputSchema = Json{ { "type", "object" },
                                  { "properties", { { "limit", { { "type", "integer" } } } } } };
        return entry;
    }

    Catalogue TestCatalogue()
    {
        Catalogue catalogue;
        catalogue.Source = "fake";
        catalogue.Entries.push_back(Entry("olo_scene_list_entities", "scene", AuthorityClass::ReadOnly));
        catalogue.Entries.push_back(Entry("olo_scene_open", "scene", AuthorityClass::ProjectWrite));
        catalogue.Entries.push_back(Entry("olo_mystery", "scene", AuthorityClass::Unknown));
        return catalogue;
    }

    // A catalogue whose single command declares one argument spelled like an
    // oloctl option.
    Catalogue CollidingCatalogue(const std::string& property, const std::string& type)
    {
        Catalogue catalogue;
        catalogue.Source = "fake";
        CatalogueEntry entry = Entry("olo_fake_arg", "scene", AuthorityClass::ReadOnly);
        entry.InputSchema = Json{ { "type", "object" }, { "properties", { { property, { { "type", type } } } } } };
        catalogue.Entries.push_back(std::move(entry));
        return catalogue;
    }

    struct CliRun
    {
        int Code = 0;
        std::string Out;
        std::string Err;
    };

    CliRun Invoke(FakeSource& source, const std::vector<std::string>& args)
    {
        std::ostringstream out;
        std::ostringstream err;
        const int code = RunCli(ParseCommandLine(args), source, out, err);
        return { code, out.str(), err.str() };
    }
} // namespace

TEST(OloCtlRunner, AResolvedCommandIsInvokedByItsRegistryName)
{
    FakeSource source(TestCatalogue());
    source.SetResult(Json{ { "content", Json::array() },
                           { "isError", false },
                           { "structuredContent", { { "entities", 3 } } } });

    const CliRun run = Invoke(source, { "scene", "list-entities", "--limit", "5", "--json" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    ASSERT_EQ(source.Invocations.size(), 1u);
    EXPECT_EQ(source.Invocations.front().Name, "olo_scene_list_entities");
    EXPECT_EQ(source.Invocations.front().Arguments, (Json{ { "limit", 5 } }));

    // stdout carries the host's result object and nothing else.
    const Json printed = Json::parse(run.Out, nullptr, false);
    ASSERT_FALSE(printed.is_discarded()) << run.Out;
    EXPECT_EQ(printed["structuredContent"]["entities"], 3);
    EXPECT_TRUE(run.Err.empty()) << run.Err;
}

TEST(OloCtlRunner, StructuredPrintsOnlyTheTypedPayload)
{
    FakeSource source(TestCatalogue());
    source.SetResult(Json{ { "content", Json::array({ Json{ { "type", "text" }, { "text", "mirror" } } }) },
                           { "isError", false },
                           { "structuredContent", { { "entities", 3 } } } });

    const CliRun run = Invoke(source, { "scene", "list-entities", "--structured", "--compact" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    EXPECT_EQ(Json::parse(run.Out), (Json{ { "entities", 3 } }));
}

TEST(OloCtlRunner, StructuredOnATextOnlyResultSaysSoRatherThanPrintingNothing)
{
    FakeSource source(TestCatalogue());
    source.SetResult(Json{ { "content", Json::array() }, { "isError", false } });

    const CliRun run = Invoke(source, { "scene", "list-entities", "--structured" });

    EXPECT_EQ(run.Code, ExitCode::NoStructured);
    EXPECT_TRUE(run.Out.empty()) << run.Out;
    EXPECT_NE(run.Err.find("structuredContent"), std::string::npos);
}

TEST(OloCtlRunner, ACommandThatReportsAnErrorStillPrintsItsResultAndExitsOne)
{
    FakeSource source(TestCatalogue());
    source.SetResult(Json{ { "content", Json::array() }, { "isError", true } });

    const CliRun run = Invoke(source, { "scene", "list-entities" });

    EXPECT_EQ(run.Code, ExitCode::CommandError);
    EXPECT_EQ(Json::parse(run.Out)["isError"], true) << "the payload is the answer even when it is an error";
}

// ---- the write path is closed ----------------------------------------------

TEST(OloCtlRunner, AProjectWriteCommandIsRefusedWithoutDispatching)
{
    FakeSource source(TestCatalogue());

    const CliRun run = Invoke(source, { "scene", "open", "--limit", "1" });

    EXPECT_EQ(run.Code, ExitCode::Refused);
    EXPECT_TRUE(source.Invocations.empty()) << "nothing about a write command may reach the host";
    EXPECT_TRUE(run.Out.empty()) << "a refusal is not a payload";
    EXPECT_NE(run.Err.find("read-only"), std::string::npos);
}

TEST(OloCtlRunner, AProjectWriteCommandIsRefusedThroughCallToo)
{
    FakeSource source(TestCatalogue());

    const CliRun run = Invoke(source, { "call", "olo_scene_open" });

    EXPECT_EQ(run.Code, ExitCode::Refused);
    EXPECT_TRUE(source.Invocations.empty()) << "`call` must not be a way past the gate";
}

// An editor whose catalogue does not report the authority class. Guessing
// read-only here would be a way around the consent model.
TEST(OloCtlRunner, AnUnknownAuthorityIsRefusedRatherThanAssumedSafe)
{
    FakeSource source(TestCatalogue());

    const CliRun run = Invoke(source, { "scene", "mystery" });

    EXPECT_EQ(run.Code, ExitCode::Refused);
    EXPECT_TRUE(source.Invocations.empty());
    EXPECT_NE(run.Err.find("does not report"), std::string::npos);
}

// ---- resolution and help ----------------------------------------------------

TEST(OloCtlRunner, AnUnknownGroupListsTheGroupsThatExist)
{
    FakeSource source(TestCatalogue());

    const CliRun run = Invoke(source, { "physics", "raycast" });

    EXPECT_EQ(run.Code, ExitCode::Usage);
    EXPECT_TRUE(run.Out.empty());
    EXPECT_NE(run.Err.find("scene"), std::string::npos);
}

TEST(OloCtlRunner, AnUnknownCommandPointsAtTheGroupListing)
{
    FakeSource source(TestCatalogue());

    const CliRun run = Invoke(source, { "scene", "nope" });

    EXPECT_EQ(run.Code, ExitCode::Usage);
    EXPECT_NE(run.Err.find("oloctl scene"), std::string::npos);
}

TEST(OloCtlRunner, CommandHelpIsRenderedFromTheSchemaAndNeedsNoInvocation)
{
    FakeSource source(TestCatalogue());

    const CliRun run = Invoke(source, { "scene", "list-entities", "--help" });

    EXPECT_EQ(run.Code, ExitCode::Ok);
    EXPECT_TRUE(source.Invocations.empty());
    EXPECT_NE(run.Out.find("--limit"), std::string::npos);
    EXPECT_NE(run.Out.find("olo_scene_list_entities"), std::string::npos);
}

TEST(OloCtlRunner, GroupHelpListsTheGroupsCommands)
{
    FakeSource source(TestCatalogue());

    const CliRun run = Invoke(source, { "scene" });

    EXPECT_EQ(run.Code, ExitCode::Ok);
    EXPECT_NE(run.Out.find("list-entities"), std::string::npos);
    EXPECT_NE(run.Out.find("open"), std::string::npos);
}

TEST(OloCtlRunner, TheCatalogueDumpCarriesTheDerivedSpellingForEveryCommand)
{
    FakeSource source(TestCatalogue());

    const CliRun run = Invoke(source, { "catalogue" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    const Json dump = Json::parse(run.Out, nullptr, false);
    ASSERT_FALSE(dump.is_discarded()) << run.Out;
    ASSERT_EQ(dump["commands"].size(), 3u);
    EXPECT_EQ(dump["commands"][0]["group"], "scene");
    EXPECT_EQ(dump["commands"][0]["command"], "list-entities");
    EXPECT_EQ(dump["commands"][0]["runnable"], true);
    EXPECT_EQ(dump["commands"][1]["runnable"], false) << "a write command is listed but not runnable";
}

TEST(OloCtlRunner, VersionAndHelpDoNotNeedAnEditor)
{
    FakeSource source(TestCatalogue());
    source.FailToConnect("no running editor found");

    const CliRun version = Invoke(source, { "version" });
    EXPECT_EQ(version.Code, ExitCode::Ok);
    EXPECT_NE(version.Out.find("oloctl"), std::string::npos);

    const CliRun help = Invoke(source, { "help" });
    EXPECT_EQ(help.Code, ExitCode::Ok);
    EXPECT_NE(help.Out.find("Usage:"), std::string::npos);
    EXPECT_NE(help.Out.find("no running editor found"), std::string::npos)
        << "an unreachable editor must be named, not silently omitted from help";
}

TEST(OloCtlRunner, AnUnreachableEditorIsAConnectionFailureNotAUsageError)
{
    FakeSource source(TestCatalogue());
    source.FailToConnect("no running editor found");

    const CliRun run = Invoke(source, { "scene", "list-entities" });

    EXPECT_EQ(run.Code, ExitCode::Connection);
    EXPECT_TRUE(run.Out.empty());
    EXPECT_NE(run.Err.find("no running editor found"), std::string::npos);
}

TEST(OloCtlRunner, ATransportFailureDuringTheCallIsAConnectionFailure)
{
    FakeSource source(TestCatalogue());
    source.FailTransport("the editor stopped answering");

    const CliRun run = Invoke(source, { "scene", "list-entities" });

    EXPECT_EQ(run.Code, ExitCode::Connection);
    EXPECT_TRUE(run.Out.empty()) << "a failed call prints no payload";
}

// ---- global options ---------------------------------------------------------

TEST(OloCtlRunner, ReservedOptionsAreRecognisedBeforeAndAfterTheCommand)
{
    const OloCtl::ParsedCommandLine leading =
        ParseCommandLine({ "--verbose", "--port", "9000", "scene", "list-entities" });
    ASSERT_TRUE(leading.Ok) << leading.Error;
    EXPECT_TRUE(leading.Options.Verbose);
    EXPECT_EQ(leading.Options.Port, 9000);
    EXPECT_EQ(leading.Rest, (std::vector<std::string>{ "scene", "list-entities" }));

    const OloCtl::ParsedCommandLine trailing =
        ParseCommandLine({ "scene", "list-entities", "--limit", "5", "--compact" });
    ASSERT_TRUE(trailing.Ok) << trailing.Error;
    EXPECT_TRUE(trailing.Options.Compact);
    EXPECT_EQ(trailing.Rest, (std::vector<std::string>{ "scene", "list-entities", "--limit", "5" }));
}

TEST(OloCtlRunner, ABadGlobalOptionValueIsAUsageError)
{
    EXPECT_FALSE(ParseCommandLine({ "--port", "0" }).Ok);
    EXPECT_FALSE(ParseCommandLine({ "--port", "70000" }).Ok);
    EXPECT_FALSE(ParseCommandLine({ "--timeout", "-1" }).Ok);
    EXPECT_FALSE(ParseCommandLine({ "--arguments-json", "[1]" }).Ok);
    EXPECT_FALSE(ParseCommandLine({ "--url" }).Ok);
    EXPECT_FALSE(ParseCommandLine({ "--compact=1" }).Ok);
}

TEST(OloCtlRunner, ArgumentsJsonSeedsTheArgumentObject)
{
    FakeSource source(TestCatalogue());

    const CliRun run = Invoke(source, { "scene", "list-entities", "--arguments-json", R"({"limit":9,"extra":true})" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    ASSERT_EQ(source.Invocations.size(), 1u);
    EXPECT_EQ(source.Invocations.front().Arguments, (Json{ { "limit", 9 }, { "extra", true } }));
}

// A command whose own argument is spelled like one of oloctl's options.
// ParseCommandLine has already eaten the option, so it can never reach the
// command: the user is told, and the run still proceeds when they did not
// actually try to use it.
TEST(OloCtlRunner, AnArgumentThatCollidesWithAnOloCtlOptionIsReported)
{
    FakeSource source(CollidingCatalogue("verbose", "boolean"));

    const CliRun run = Invoke(source, { "scene", "fake-arg" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    EXPECT_NE(run.Err.find("--arguments-json"), std::string::npos);
    EXPECT_EQ(source.Invocations.size(), 1u);
}

// ...but a run that ACTUALLY GAVE the colliding option is refused, not run
// without it. `--port 5` is consumed as oloctl's own port, so dispatching would
// call the command with `port` silently missing and exit 0 — a wrong answer
// wearing a right answer's exit code.
TEST(OloCtlRunner, AConsumedReservedOptionRefusesRatherThanDispatchingWithoutIt)
{
    FakeSource source(CollidingCatalogue("port", "integer"));

    const CliRun run = Invoke(source, { "scene", "fake-arg", "--port", "5" });

    EXPECT_EQ(run.Code, ExitCode::Usage);
    EXPECT_TRUE(source.Invocations.empty()) << "the command must not run with the argument dropped";
    EXPECT_NE(run.Err.find("you gave it"), std::string::npos) << run.Err;
    EXPECT_NE(run.Err.find("--arguments-json"), std::string::npos);
}

// The same argument, supplied the way the message tells you to.
TEST(OloCtlRunner, ACollidingArgumentIsStillSettableThroughArgumentsJson)
{
    FakeSource source(CollidingCatalogue("port", "integer"));

    const CliRun run = Invoke(source, { "scene", "fake-arg", "--arguments-json", R"({"port":5})" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    ASSERT_EQ(source.Invocations.size(), 1u);
    EXPECT_EQ(source.Invocations.front().Arguments, (Json{ { "port", 5 } }));
}

// --structured on a command that FAILED. The result carries the host's message in
// `content` and no `structuredContent`, and the two conditions must not be
// conflated: reporting "no structured content" would throw the explanation away
// and return an exit code that says nothing failed.
TEST(OloCtlRunner, StructuredOnAFailedCommandReportsTheFailureRatherThanTheMissingPayload)
{
    FakeSource source(TestCatalogue());
    source.SetResult(Json{ { "content", Json::array({ Json{ { "type", "text" },
                                                            { "text", "Boom: entity 7 not found" } } }) },
                           { "isError", true } });

    const CliRun run = Invoke(source, { "scene", "list-entities", "--structured" });

    EXPECT_EQ(run.Code, ExitCode::CommandError);
    EXPECT_TRUE(run.Out.empty()) << "a failed command has no structured payload to print";
    EXPECT_NE(run.Err.find("Boom: entity 7 not found"), std::string::npos)
        << "the host's explanation must not be discarded: " << run.Err;
}

// A catalogue entry this build could not read is reported on stderr on every
// run, not only when someone tries to use it.
TEST(OloCtlRunner, RejectedCatalogueEntriesAreReportedOnStderr)
{
    Catalogue catalogue = TestCatalogue();
    catalogue.Rejected.push_back({ 7, "olo_weird", "entry has no string `name`" });
    FakeSource source(std::move(catalogue));

    const CliRun run = Invoke(source, { "scene", "list-entities" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    EXPECT_NE(run.Err.find("olo_weird"), std::string::npos);
    EXPECT_FALSE(Json::parse(run.Out, nullptr, false).is_discarded())
        << "a warning on stderr must not disturb the payload on stdout";
}

TEST(OloCtlRunner, NoArgumentsIsAUsageErrorWithHelpOnStderr)
{
    FakeSource source(TestCatalogue());

    const CliRun run = Invoke(source, {});

    EXPECT_EQ(run.Code, ExitCode::Usage);
    EXPECT_TRUE(run.Out.empty()) << "an accidental bare `oloctl` must not put help into a pipe";
    EXPECT_NE(run.Err.find("Usage:"), std::string::npos);
}
