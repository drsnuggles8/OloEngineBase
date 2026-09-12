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

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <utility>
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
            if (m_Scripted.empty())
            {
                outcome.Result = m_Result;
                return outcome;
            }
            // Scripted answers are handed out in order; once they run out the last
            // one repeats, so a follower that keeps polling keeps getting an answer.
            outcome.Result = m_Scripted[std::min(m_NextScripted, m_Scripted.size() - 1)];
            ++m_NextScripted;
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
        void ScriptResults(std::vector<Json> results)
        {
            m_Scripted = std::move(results);
            m_NextScripted = 0;
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
        std::vector<Json> m_Scripted;
        std::size_t m_NextScripted = 0;
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

    // ---- olo_events_wait answers, in the shape the editor's handler returns ----

    Json Event(unsigned long long id, const char* category)
    {
        return Json{ { "id", id }, { "category", category }, { "message", std::string(category) + " happened" } };
    }

    Json WaitResponse(std::vector<Json> events, unsigned long long lastId, unsigned long long dropped = 0)
    {
        const bool timedOut = events.empty();
        return Json{ { "content", Json::array() },
                     { "isError", false },
                     { "structuredContent",
                       Json{ { "count", events.size() },
                             { "lastId", lastId },
                             { "dropped", dropped },
                             { "timedOut", timedOut },
                             { "cancelled", false },
                             { "events", std::move(events) } } } };
    }

    // stdout split into lines; every line must parse as one JSON object.
    std::vector<Json> NdjsonLines(const std::string& text)
    {
        std::vector<Json> lines;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            Json parsed = Json::parse(line, nullptr, false);
            EXPECT_FALSE(parsed.is_discarded()) << "not a JSON line: " << line;
            EXPECT_TRUE(parsed.is_object()) << "not an event object: " << line;
            lines.push_back(std::move(parsed));
        }
        return lines;
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

// ---- events follow (#1131) --------------------------------------------------
//
// The CLI consumer of the event bus is a loop over olo_events_wait through the
// same ICommandSource::Invoke as every other verb. What these pin: the NDJSON
// discipline (one object per line, nothing else on stdout), the cursor
// hand-back, the termination rules, and that a transport failure, an error
// result and a usage slip land on the exit codes a script is told to expect.

TEST(OloCtlRunner, EventsFollowPrintsOneJsonObjectPerEventAndNothingElse)
{
    FakeSource source(TestCatalogue());
    source.ScriptResults({ WaitResponse({ Event(1, "scene_load"), Event(2, "entity_spawn") }, 2),
                           WaitResponse({}, 2), // an idle poll: timed out, no events
                           WaitResponse({ Event(3, "play") }, 3) });

    const CliRun run = Invoke(source, { "events", "follow", "--count", "3" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    const std::vector<Json> lines = NdjsonLines(run.Out);
    ASSERT_EQ(lines.size(), 3u) << run.Out;
    EXPECT_EQ(lines[0]["id"], 1);
    EXPECT_EQ(lines[1]["id"], 2);
    EXPECT_EQ(lines[2]["category"], "play");
    EXPECT_EQ(lines[2]["message"], "play happened") << "the event record is printed verbatim";
    EXPECT_TRUE(run.Err.empty()) << run.Err;

    ASSERT_EQ(source.Invocations.size(), 3u) << "an idle poll must be followed by another poll";
    for (const FakeSource::Invocation& invocation : source.Invocations)
        EXPECT_EQ(invocation.Name, "olo_events_wait");
    EXPECT_EQ(source.Invocations.front().Arguments["waitMs"], 10000) << "the default long-poll is 10 s";
    EXPECT_FALSE(source.Invocations.front().Arguments.contains("sinceId"))
        << "without --since-id the first poll lets the editor start at now";
}

TEST(OloCtlRunner, EventsFollowUntilStopsAfterTheNamedCategory)
{
    FakeSource source(TestCatalogue());
    source.ScriptResults({ WaitResponse({ Event(1, "scene_load"), Event(2, "play"), Event(3, "entity_spawn") }, 3) });

    const CliRun run = Invoke(source, { "events", "follow", "--until", "play" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    const std::vector<Json> lines = NdjsonLines(run.Out);
    ASSERT_EQ(lines.size(), 2u) << "the play event is printed, and nothing after it: " << run.Out;
    EXPECT_EQ(lines.back()["category"], "play");
    EXPECT_EQ(source.Invocations.size(), 1u);
}

TEST(OloCtlRunner, EventsFollowCountStopsAfterThatManyEvents)
{
    FakeSource source(TestCatalogue());
    source.ScriptResults({ WaitResponse({ Event(1, "play"), Event(2, "stop"), Event(3, "play") }, 3) });

    const CliRun run = Invoke(source, { "events", "follow", "--count", "2" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    EXPECT_EQ(NdjsonLines(run.Out).size(), 2u) << run.Out;
    EXPECT_EQ(source.Invocations.front().Arguments["count"], 2) << "the server is asked for no more than needed";
}

TEST(OloCtlRunner, EventsFollowPassesTheLastIdBackAsTheNextSinceId)
{
    FakeSource source(TestCatalogue());
    // The first answer's lastId (5) is past its last event (1): ids 2-5 were
    // filtered out by the editor. The cursor is lastId, not the last printed id,
    // or the next poll would re-fetch what the editor already skipped.
    source.ScriptResults({ WaitResponse({ Event(1, "play") }, 5),
                           WaitResponse({ Event(6, "stop"), Event(7, "play") }, 7) });

    const CliRun run = Invoke(source, { "events", "follow", "--count", "3" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    ASSERT_EQ(source.Invocations.size(), 2u);
    EXPECT_FALSE(source.Invocations[0].Arguments.contains("sinceId"));
    EXPECT_EQ(source.Invocations[1].Arguments["sinceId"], 5);
    EXPECT_EQ(source.Invocations[1].Arguments["count"], 2) << "the remaining budget after one printed event";
}

TEST(OloCtlRunner, EventsFollowSinceIdIsSentVerbatimIncludingZero)
{
    FakeSource source(TestCatalogue());
    source.ScriptResults({ WaitResponse({ Event(1, "play") }, 1) });

    const CliRun zero = Invoke(source, { "events", "follow", "--since-id", "0", "--count", "1" });
    EXPECT_EQ(zero.Code, ExitCode::Ok) << zero.Err;
    ASSERT_EQ(source.Invocations.size(), 1u);
    EXPECT_EQ(source.Invocations[0].Arguments["sinceId"], 0) << "0 is the editor's no-lower-bound, so it is sent";

    const CliRun forty = Invoke(source, { "events", "follow", "--since-id=40", "--count", "1" });
    EXPECT_EQ(forty.Code, ExitCode::Ok) << forty.Err;
    ASSERT_EQ(source.Invocations.size(), 2u);
    EXPECT_EQ(source.Invocations[1].Arguments["sinceId"], 40);
}

TEST(OloCtlRunner, EventsFollowForwardsTheCategoryFilterAndUntilDoesNotWidenIt)
{
    FakeSource source(TestCatalogue());
    source.ScriptResults({ WaitResponse({ Event(1, "play") }, 1) });

    const CliRun run = Invoke(source, { "events", "follow", "--category", "play", "--category=stop", "--until",
                                        "scene_load", "--count", "1" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    ASSERT_EQ(source.Invocations.size(), 1u);
    EXPECT_EQ(source.Invocations.front().Arguments["categories"], (Json::array({ "play", "stop" })));
    EXPECT_FALSE(source.Invocations.front().Arguments.contains("until"))
        << "--until decides termination locally; it is not an argument of olo_events_wait";
}

TEST(OloCtlRunner, EventsFollowRejectsAnUnknownCategoryBeforePolling)
{
    FakeSource source(TestCatalogue());

    const CliRun category = Invoke(source, { "events", "follow", "--category", "bogus" });
    EXPECT_EQ(category.Code, ExitCode::Usage);
    EXPECT_TRUE(category.Out.empty()) << category.Out;
    EXPECT_NE(category.Err.find("bogus"), std::string::npos) << category.Err;
    EXPECT_NE(category.Err.find("scene_load"), std::string::npos) << "the valid tokens are named: " << category.Err;
    EXPECT_NE(category.Err.find("command_completed"), std::string::npos) << category.Err;

    const CliRun until = Invoke(source, { "events", "follow", "--until", "Play" });
    EXPECT_EQ(until.Code, ExitCode::Usage) << "--until is validated the same way";

    EXPECT_TRUE(source.Invocations.empty()) << "a usage error must not reach the editor";
}

TEST(OloCtlRunner, EventsFollowUsageSlipsAreUsageErrors)
{
    FakeSource source(TestCatalogue());

    EXPECT_EQ(Invoke(source, { "events", "follow", "--count", "0" }).Code, ExitCode::Usage);
    EXPECT_EQ(Invoke(source, { "events", "follow", "--count", "-1" }).Code, ExitCode::Usage);
    EXPECT_EQ(Invoke(source, { "events", "follow", "--since-id", "x" }).Code, ExitCode::Usage);
    EXPECT_EQ(Invoke(source, { "events", "follow", "--for", "0" }).Code, ExitCode::Usage);
    EXPECT_EQ(Invoke(source, { "events", "follow", "--until" }).Code, ExitCode::Usage) << "a missing value";
    EXPECT_EQ(Invoke(source, { "events", "follow", "--until", "--count", "1" }).Code, ExitCode::Usage)
        << "a value never begins with --";
    EXPECT_EQ(Invoke(source, { "events", "follow", "--limit", "1" }).Code, ExitCode::Usage) << "an unknown option";
    EXPECT_EQ(Invoke(source, { "events", "follow", "extra" }).Code, ExitCode::Usage) << "a stray positional";
    EXPECT_EQ(Invoke(source, { "events", "bogus" }).Code, ExitCode::Usage) << "an unknown sub-verb";
    EXPECT_TRUE(source.Invocations.empty());
}

TEST(OloCtlRunner, EventsFollowWarnsOnStderrWhenTheCursorFellBehind)
{
    FakeSource source(TestCatalogue());
    source.ScriptResults({ WaitResponse({ Event(600, "play") }, 600, /*dropped=*/3) });

    const CliRun run = Invoke(source, { "events", "follow", "--since-id", "80", "--count", "1" });

    EXPECT_EQ(run.Code, ExitCode::Ok);
    EXPECT_NE(run.Err.find("3 event(s) were dropped before id 600"), std::string::npos) << run.Err;
    EXPECT_NE(run.Err.find("512"), std::string::npos) << "the window size is named: " << run.Err;
    EXPECT_EQ(NdjsonLines(run.Out).size(), 1u) << "the warning must not disturb stdout: " << run.Out;
}

TEST(OloCtlRunner, EventsFollowReportsATransportFailureAsAConnectionError)
{
    FakeSource source(TestCatalogue());
    source.FailTransport("the editor stopped answering");

    const CliRun run = Invoke(source, { "events", "follow" });

    EXPECT_EQ(run.Code, ExitCode::Connection);
    EXPECT_TRUE(run.Out.empty()) << run.Out;
    EXPECT_NE(run.Err.find("the editor stopped answering"), std::string::npos) << run.Err;
}

// An editor older than the event bus has no olo_events_wait: tools/call answers
// with an error result, and the follower says what that means rather than
// retrying forever.
TEST(OloCtlRunner, EventsFollowReportsAnErrorResultAndNamesAnEditorThatPredatesTheBus)
{
    FakeSource source(TestCatalogue());
    source.SetResult(Json{ { "content", Json::array({ Json{ { "type", "text" },
                                                            { "text", "Unknown tool: olo_events_wait" } } }) },
                           { "isError", true } });

    const CliRun run = Invoke(source, { "events", "follow" });

    EXPECT_EQ(run.Code, ExitCode::CommandError);
    EXPECT_TRUE(run.Out.empty()) << run.Out;
    EXPECT_NE(run.Err.find("Unknown tool: olo_events_wait"), std::string::npos) << run.Err;
    EXPECT_NE(run.Err.find("predates"), std::string::npos) << run.Err;
    EXPECT_EQ(source.Invocations.size(), 1u) << "an error result is not retried";
}

// The verb is dispatched before the catalogue fetch: it hard-codes the one
// registry name it needs, so it must work when the fetch would fail.
TEST(OloCtlRunner, EventsFollowNeedsNoCatalogue)
{
    FakeSource source(TestCatalogue());
    source.FailToConnect("olo_tool_search is unreachable");
    source.ScriptResults({ WaitResponse({ Event(1, "play") }, 1) });

    const CliRun run = Invoke(source, { "events", "follow", "--count", "1" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    EXPECT_EQ(NdjsonLines(run.Out).size(), 1u);
    EXPECT_EQ(run.Err.find("olo_tool_search"), std::string::npos) << "the catalogue was never fetched: " << run.Err;
}

TEST(OloCtlRunner, EventsFollowRefusesATimeoutTooShortForTheLongPoll)
{
    FakeSource source(TestCatalogue());
    source.ScriptResults({ WaitResponse({ Event(1, "play") }, 1) });

    const CliRun tooShort = Invoke(source, { "--timeout", "1000", "events", "follow", "--count", "1" });
    EXPECT_EQ(tooShort.Code, ExitCode::Usage);
    EXPECT_TRUE(tooShort.Out.empty());
    EXPECT_NE(tooShort.Err.find("2000"), std::string::npos) << tooShort.Err;
    EXPECT_TRUE(source.Invocations.empty());

    // Just above the floor: the long-poll is half the read timeout, never more.
    const CliRun aboveFloor = Invoke(source, { "--timeout", "3000", "events", "follow", "--count", "1" });
    EXPECT_EQ(aboveFloor.Code, ExitCode::Ok) << aboveFloor.Err;
    ASSERT_EQ(source.Invocations.size(), 1u);
    EXPECT_EQ(source.Invocations.front().Arguments["waitMs"], 1500);
}

TEST(OloCtlRunner, EventsHelpGoesToStdoutAndNeedsNoEditor)
{
    FakeSource source(TestCatalogue());
    source.FailToConnect("no running editor found");

    const std::vector<std::vector<std::string>> spellings{ { "events" },
                                                           { "events", "--help" },
                                                           { "events", "follow", "--help" } };
    for (const std::vector<std::string>& args : spellings)
    {
        const CliRun run = Invoke(source, args);
        EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
        EXPECT_NE(run.Out.find("events follow"), std::string::npos) << run.Out;
        EXPECT_NE(run.Out.find("--since-id"), std::string::npos) << run.Out;
        EXPECT_NE(run.Out.find("until interrupted"), std::string::npos) << run.Out;
        EXPECT_TRUE(run.Err.empty()) << run.Err;
    }
    EXPECT_TRUE(source.Invocations.empty());
}

// A registry toolset named `events` can never be typed, because the verb is
// dispatched first. The catalogue report says so, as it does for `call`.
TEST(OloCtlRunner, AGroupNamedEventsIsReportedAsShadowed)
{
    Catalogue catalogue = TestCatalogue();
    catalogue.Entries.push_back(Entry("olo_events_tail", "events", AuthorityClass::ReadOnly));
    FakeSource source(std::move(catalogue));

    const CliRun run = Invoke(source, { "scene", "list-entities" });

    EXPECT_EQ(run.Code, ExitCode::Ok) << run.Err;
    EXPECT_NE(run.Err.find("`events` shares its name with an oloctl subcommand"), std::string::npos) << run.Err;
}
