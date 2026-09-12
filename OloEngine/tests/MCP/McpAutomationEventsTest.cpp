// OLO_TEST_LAYER: unit
//
// The automation event bus (issue #1131): the publish side, the subscription
// command, and the two contracts the issue named as design constraints.
//
// What is pinned here, and why each is a test rather than a comment:
//
//   1. THE PAYLOAD RULE. Every publisher builds its record from the closed key
//      set the engine header declares, and no key that could carry content
//      (arguments, a result, a document) exists in any set. This is what makes a
//      subscription offerable at read authority: nothing behind a read gate is
//      ever on the bus.
//   2. THE COMPLETION RULE. A mutating command publishes `command_completed`
//      from AutomationRegistry::RunHandler; a read-only one never does. The
//      second half is load-bearing: the subscription command is itself
//      read-only, and if its completion were an event, two waiting agents would
//      wake each other forever.
//   3. THE CURSOR MODEL. olo_events_wait blocks until a match, times out with
//      the cursor, is cancellable, and reports the gap when the cursor fell
//      behind the ring — the whole backpressure answer, with no per-subscriber
//      queue anywhere.
//   4. THE DIRTY EDGE. CommandHistory fires OnDirtyChanged exactly on a
//      transition, once per public mutation, including across a transaction.
//
// No editor, no server: a bare registry, the real diagnostics command
// registrations, and a host that runs marshaled jobs inline.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationCommand.h"
#include "Automation/AutomationEvents.h"
#include "Automation/AutomationHost.h"
#include "Automation/AutomationRegistry.h"
#include "Automation/AutomationResult.h"
#include "MCP/McpServer.h"
#include "MCP/McpToolsCommon.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"
#include "UndoRedo/EditorCommand.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace OloEngine::Automation::Tests
{
    namespace
    {
        using Json = nlohmann::json;

        class BusHost final : public IAutomationHost
        {
          public:
            [[nodiscard]] const MCP::EditorMcpContext& Context() const override
            {
                return m_Context;
            }
            [[nodiscard]] bool IsCurrentCallCancelled() const override
            {
                return Cancelled.load();
            }
            [[nodiscard]] bool PublishArtifact(AutomationArtifact) override
            {
                return false;
            }

            std::atomic<bool> Cancelled{ false };

          protected:
            Json MarshalReadOnMainThread(const std::function<Json()>& job, std::chrono::milliseconds) override
            {
                return job();
            }
            void EmitProgressUpdate(f64, f64, const std::string&) const override {}

          private:
            MCP::EditorMcpContext m_Context;
        };

        class NoopCommand final : public EditorCommand
        {
          public:
            void Execute() override {}
            void Undo() override {}
            [[nodiscard]] std::string GetDescription() const override
            {
                return "noop";
            }
        };

        AutomationCommand MakeCommand(std::string name, Json annotations, bool projectWrite,
                                      AutomationHandler handler)
        {
            AutomationCommand command;
            command.Name = std::move(name);
            command.Toolset = "fake";
            command.Description = "A fake command.";
            command.InputSchema = Json{ { "type", "object" } };
            command.Annotations = std::move(annotations);
            command.ProjectWrite = projectWrite;
            command.Undo = projectWrite ? AutomationUndo::Irreversible : AutomationUndo::None;
            command.Handler = std::move(handler);
            return command;
        }

        std::vector<DiagnosticEvent> Recorded(DiagnosticEventCategory category)
        {
            DiagnosticEventQuery query;
            query.MaxCount = 0;
            query.Categories = { category };
            return DiagnosticsEventLog::Get().Query(query);
        }

        Json DataOf(const DiagnosticEvent& event)
        {
            return Json::parse(event.Data);
        }
    } // namespace

    class McpAutomationEvents : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            DiagnosticsEventLog::Get().Clear();
        }
        void TearDown() override
        {
            DiagnosticsEventLog::Get().Clear();
        }
    };

    // ---- 1. the publishers and the payload rule ---------------------------------

    TEST_F(McpAutomationEvents, ProjectRelativePathNeverRecordsAnAbsolutePath)
    {
        const std::filesystem::path root = std::filesystem::path("C:/proj/Assets");
        EXPECT_EQ(Events::ProjectRelativePath(root / "Scenes" / "A.olo", root), "Scenes/A.olo");
        EXPECT_EQ(Events::ProjectRelativePath(std::filesystem::path("C:/elsewhere/B.olo"), root), "B.olo");
        EXPECT_EQ(Events::ProjectRelativePath(std::filesystem::path("C:/elsewhere/B.olo"), {}), "B.olo");
        EXPECT_EQ(Events::ProjectRelativePath({}, root), "");
    }

    TEST_F(McpAutomationEvents, SceneSavedCarriesIdentityOnly)
    {
        const std::filesystem::path root = std::filesystem::path("C:/proj/Assets");
        const u64 id = Events::PublishSceneSaved("Level1", root / "Scenes" / "Level1.olo", root, true);
        ASSERT_NE(0u, id);
        const auto events = Recorded(DiagnosticEventCategory::SceneSave);
        ASSERT_EQ(1u, events.size());
        EXPECT_EQ(events[0].Context, "Level1.olo");
        const Json data = DataOf(events[0]);
        EXPECT_EQ(data["scene"], "Level1");
        EXPECT_EQ(data["path"], "Scenes/Level1.olo");
        EXPECT_EQ(data["changed"], true);
        EXPECT_EQ(data.size(), 3u) << "exactly the declared keys";
    }

    TEST_F(McpAutomationEvents, SceneDirtyRecordsBothEdges)
    {
        Events::PublishSceneDirty("Level1", true);
        Events::PublishSceneDirty("Level1", false);
        const auto events = Recorded(DiagnosticEventCategory::SceneDirty);
        ASSERT_EQ(2u, events.size());
        EXPECT_EQ(DataOf(events[0])["dirty"], true);
        EXPECT_EQ(DataOf(events[1])["dirty"], false);
        EXPECT_NE(events[0].Message, events[1].Message);
    }

    TEST_F(McpAutomationEvents, AssetImportedCarriesHandleTypeAndSource)
    {
        const std::filesystem::path root = std::filesystem::path("C:/proj");
        Events::PublishAssetImported(77, "Texture2D", root / "Assets" / "t.png", root, "automation");
        const auto events = Recorded(DiagnosticEventCategory::AssetImport);
        ASSERT_EQ(1u, events.size());
        EXPECT_EQ(events[0].Entity, 77u);
        const Json data = DataOf(events[0]);
        EXPECT_EQ(data["asset"], "Assets/t.png");
        EXPECT_EQ(data["type"], "Texture2D");
        EXPECT_EQ(data["handle"], 77);
        EXPECT_EQ(data["source"], "automation");
    }

    TEST_F(McpAutomationEvents, CompileFinishedSaysFailedInTheMessage)
    {
        Events::PublishCompileFinished("build", "OloEngine-Tests", false, 3, 1, 12.5);
        const auto events = Recorded(DiagnosticEventCategory::CompileFinished);
        ASSERT_EQ(1u, events.size());
        EXPECT_NE(events[0].Message.find("FAILED"), std::string::npos);
        const Json data = DataOf(events[0]);
        EXPECT_EQ(data["kind"], "build");
        EXPECT_EQ(data["target"], "OloEngine-Tests");
        EXPECT_EQ(data["ok"], false);
        EXPECT_EQ(data["errors"], 3);
        EXPECT_EQ(data["warnings"], 1);
        EXPECT_NEAR(data["seconds"].get<f64>(), 12.5, 1e-6);
    }

    // ---- 2. the completion rule --------------------------------------------------

    TEST_F(McpAutomationEvents, ReadOnlyCommandsNeverEmitACompletionEvent)
    {
        EXPECT_FALSE(Events::EmitsCompletionEvent(MakeCommand("a", MCP::ReadOnlyAnnotations(), false, {})));
        EXPECT_TRUE(Events::EmitsCompletionEvent(MakeCommand("b", MCP::MutatingAnnotations(true), false, {})));
        EXPECT_TRUE(Events::EmitsCompletionEvent(MakeCommand("c", Json{}, true, {})))
            << "no annotations at all is treated as not read-only";
        EXPECT_TRUE(Events::EmitsCompletionEvent(MakeCommand("d", Json{ { "readOnlyHint", false } }, false, {})));
    }

    TEST_F(McpAutomationEvents, RunHandlerPublishesCompletionForMutatingCommandsOnly)
    {
        AutomationRegistry registry;
        registry.Register(MakeCommand("olo_fake_write", MCP::MutatingAnnotations(false), true,
                                      [](IAutomationHost&, const Json&)
                                      { return AutomationResult::Structured(Json{ { "ok", true } }); }));
        registry.Register(MakeCommand("olo_fake_read", MCP::ReadOnlyAnnotations(), false,
                                      [](IAutomationHost&, const Json&)
                                      { return AutomationResult::Structured(Json{ { "ok", true } }); }));
        registry.Register(MakeCommand("olo_fake_throw", MCP::MutatingAnnotations(false), true,
                                      [](IAutomationHost&, const Json&) -> AutomationResult
                                      { throw std::runtime_error("boom"); }));

        BusHost host;
        ASSERT_TRUE(registry.Invoke(host, "olo_fake_read", Json::object()).Ran());
        EXPECT_TRUE(Recorded(DiagnosticEventCategory::CommandCompleted).empty());

        ASSERT_TRUE(registry.Invoke(host, "olo_fake_write", Json::object(), AutomationWriteConsent::Granted).Ran());
        auto events = Recorded(DiagnosticEventCategory::CommandCompleted);
        ASSERT_EQ(1u, events.size());
        Json data = DataOf(events[0]);
        EXPECT_EQ(data["command"], "olo_fake_write");
        EXPECT_EQ(data["ok"], true);
        EXPECT_EQ(data["projectWrite"], true);
        EXPECT_EQ(data["toolset"], "fake");
        EXPECT_GE(data["durationMs"].get<f64>(), 0.0);
        EXPECT_FALSE(data.contains("arguments"));
        EXPECT_FALSE(data.contains("result"));

        const AutomationInvocation thrown =
            registry.Invoke(host, "olo_fake_throw", Json::object(), AutomationWriteConsent::Granted);
        ASSERT_TRUE(thrown.Ran());
        EXPECT_TRUE(thrown.Result.IsError);
        events = Recorded(DiagnosticEventCategory::CommandCompleted);
        ASSERT_EQ(2u, events.size());
        EXPECT_EQ(DataOf(events[1])["ok"], false) << "a handler that threw completed with ok:false";
    }

    TEST_F(McpAutomationEvents, TheSubscriptionCommandsAreReadOnlyAndSilent)
    {
        AutomationRegistry registry;
        MCP::RegisterDiagnosticsTools(registry);
        const auto snapshot = registry.Snapshot();
        for (const char* name : { "olo_events_tail", "olo_events_wait" })
        {
            const AutomationCommand* command = AutomationRegistry::Find(*snapshot, name);
            ASSERT_NE(command, nullptr) << name;
            EXPECT_FALSE(command->ProjectWrite) << name;
            EXPECT_FALSE(Events::EmitsCompletionEvent(*command))
                << name << " must not publish its own completion, or waiters would wake each other";
        }
    }

    // ---- 3. olo_events_wait: the cursor model ------------------------------------

    class McpEventsWait : public McpAutomationEvents
    {
      protected:
        void SetUp() override
        {
            McpAutomationEvents::SetUp();
            MCP::RegisterDiagnosticsTools(m_Registry);
        }

        Json Wait(const Json& args)
        {
            const AutomationInvocation outcome = m_Registry.Invoke(m_Host, "olo_events_wait", args);
            EXPECT_EQ(outcome.Outcome, AutomationInvocation::Status::Ok) << outcome.Message;
            EXPECT_FALSE(outcome.Result.IsError) << outcome.Result.Content.dump();
            return outcome.Result.StructuredContent;
        }

        AutomationRegistry m_Registry;
        BusHost m_Host;
    };

    TEST_F(McpEventsWait, ReturnsExistingMatchesImmediately)
    {
        DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::Play, "p", 0, "S");
        const Json out = Wait(Json{ { "sinceId", 0 }, { "waitMs", 0 } });
        EXPECT_EQ(out["count"], 1);
        EXPECT_EQ(out["lastId"], 1);
        EXPECT_EQ(out["dropped"], 0);
        EXPECT_EQ(out["timedOut"], false);
        EXPECT_EQ(out["cancelled"], false);
        EXPECT_EQ(out["events"][0]["category"], "play");
    }

    TEST_F(McpEventsWait, DefaultsToWaitingForEventsNewerThanTheCall)
    {
        DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::Play, "before the call");
        const Json out = Wait(Json{ { "waitMs", 50 } });
        EXPECT_EQ(out["count"], 0);
        EXPECT_EQ(out["timedOut"], true);
        EXPECT_EQ(out["lastId"], 1) << "the cursor to pass next, even on a timeout";
    }

    TEST_F(McpEventsWait, AcceptsTheCursorAsADecimalString)
    {
        DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::Play, "one");
        DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::Stop, "two");
        const Json out = Wait(Json{ { "sinceId", "1" }, { "waitMs", 0 } });
        EXPECT_EQ(out["count"], 1);
        EXPECT_EQ(out["events"][0]["id"], 2);
    }

    TEST_F(McpEventsWait, WakesOnAMatchingEventFromAnotherThread)
    {
        std::thread recorder(
            []
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::Play, "filtered out");
                DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::Stop, "wake");
            });
        const auto started = std::chrono::steady_clock::now();
        const Json out = Wait(Json{ { "categories", Json::array({ "stop" }) }, { "waitMs", 10000 } });
        recorder.join();
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
        EXPECT_EQ(out["count"], 1);
        EXPECT_EQ(out["events"][0]["category"], "stop");
        EXPECT_EQ(out["lastId"], 2);
    }

    TEST_F(McpEventsWait, StopsEarlyWhenTheCallIsCancelled)
    {
        std::thread canceller(
            [this]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                m_Host.Cancelled.store(true);
            });
        const auto started = std::chrono::steady_clock::now();
        const Json out = Wait(Json{ { "waitMs", 60000 } });
        canceller.join();
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
        EXPECT_EQ(out["count"], 0);
        EXPECT_EQ(out["cancelled"], true);
        EXPECT_EQ(out["timedOut"], false);
    }

    TEST_F(McpEventsWait, ReportsTheGapWhenTheCursorFellBehindTheRing)
    {
        for (std::size_t i = 0; i < DiagnosticsEventLog::kCapacity + 5; ++i)
            DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::EntitySpawn, "e");
        const Json out = Wait(Json{ { "sinceId", 2 }, { "waitMs", 0 }, { "count", 1 } });
        EXPECT_EQ(out["dropped"], 3) << "ids 3, 4 and 5 were evicted";
        EXPECT_EQ(out["count"], 1);
    }

    TEST_F(McpEventsWait, RejectsAnUnknownCategoryNamingTheValidOnes)
    {
        // Through the registry the schema enum refuses it before the handler runs.
        const AutomationInvocation outcome =
            m_Registry.Invoke(m_Host, "olo_events_wait", Json{ { "categories", Json::array({ "nope" }) } });
        EXPECT_EQ(outcome.Outcome, AutomationInvocation::Status::InvalidArguments) << outcome.Message;

        // A caller that reaches the handler directly gets the same refusal, naming
        // every valid token (the bus categories included).
        const AutomationCommand* command = AutomationRegistry::Find(*m_Registry.Snapshot(), "olo_events_wait");
        ASSERT_NE(command, nullptr);
        const AutomationResult result =
            AutomationRegistry::RunHandler(*command, m_Host, Json{ { "categories", Json::array({ "nope" }) } });
        EXPECT_TRUE(result.IsError);
        const std::string text = result.Content.dump();
        EXPECT_NE(text.find("command_completed"), std::string::npos);
        EXPECT_NE(text.find("scene_save"), std::string::npos);
    }

    TEST_F(McpEventsWait, TailReportsTheGapAndTheDataToo)
    {
        Events::PublishSceneDirty("S", true);
        const AutomationInvocation outcome = m_Registry.Invoke(m_Host, "olo_events_tail", Json::object());
        ASSERT_TRUE(outcome.Ran());
        const Json out = outcome.Result.StructuredContent;
        EXPECT_EQ(out["dropped"], 0);
        ASSERT_EQ(out["count"], 1);
        EXPECT_EQ(out["events"][0]["category"], "scene_dirty");
        EXPECT_EQ(out["events"][0]["data"]["dirty"], true);
    }

    // ---- 4. the dirty edge -------------------------------------------------------

    TEST_F(McpAutomationEvents, CommandHistoryFiresOnDirtyChangedOnTransitionsOnly)
    {
        CommandHistory history;
        std::vector<bool> edges;
        history.OnDirtyChanged = [&edges](bool dirty)
        { edges.push_back(dirty); };

        history.Execute(std::make_unique<NoopCommand>());
        ASSERT_EQ(edges, (std::vector<bool>{ true }));
        history.Execute(std::make_unique<NoopCommand>());
        EXPECT_EQ(edges.size(), 1u) << "still dirty: no second edge";
        history.MarkSaved();
        ASSERT_EQ(edges, (std::vector<bool>{ true, false }));
        history.MarkSaved();
        EXPECT_EQ(edges.size(), 2u) << "already clean: no edge";

        history.Undo();
        ASSERT_EQ(edges, (std::vector<bool>{ true, false, true }));
        history.Redo();
        ASSERT_EQ(edges, (std::vector<bool>{ true, false, true, false }));

        // A save that lands in the same mutation as the edit reports no edge: the
        // observable state never flipped.
        history.Execute(std::make_unique<NoopCommand>(), /*markSaved=*/true);
        EXPECT_EQ(edges.size(), 4u);
    }

    TEST_F(McpAutomationEvents, CommandHistoryTransactionCollapsesToOneEdgeEachWay)
    {
        CommandHistory history;
        std::vector<bool> edges;
        history.OnDirtyChanged = [&edges](bool dirty)
        { edges.push_back(dirty); };

        history.BeginTransaction("batch");
        history.Execute(std::make_unique<NoopCommand>());
        history.Execute(std::make_unique<NoopCommand>());
        ASSERT_EQ(edges, (std::vector<bool>{ true })) << "an open transaction with members is dirty";
        history.CommitTransaction();
        EXPECT_EQ(edges.size(), 1u) << "commit keeps it dirty: no edge";

        history.MarkSaved();
        ASSERT_EQ(edges, (std::vector<bool>{ true, false }));
        history.BeginTransaction("aborted");
        history.Execute(std::make_unique<NoopCommand>());
        ASSERT_EQ(edges, (std::vector<bool>{ true, false, true }));
        EXPECT_TRUE(history.RollbackTransaction().empty());
        ASSERT_EQ(edges, (std::vector<bool>{ true, false, true, false }))
            << "rollback restored the save point, so the document is clean again";
    }

    TEST_F(McpAutomationEvents, CommandHistoryWithoutAHookStillWorks)
    {
        CommandHistory history;
        history.Execute(std::make_unique<NoopCommand>());
        EXPECT_TRUE(history.IsDirty());
        history.MarkSaved();
        EXPECT_FALSE(history.IsDirty());
    }
} // namespace OloEngine::Automation::Tests
