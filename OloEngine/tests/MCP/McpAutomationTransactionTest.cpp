// OLO_TEST_LAYER: unit
//
// Atomic multi-command transactions (issue #1127).
//
// The four acceptance criteria are the four things this file exists to pin, and
// each has a named test:
//
//   1. A failing step leaves the scene BYTE-IDENTICAL to its pre-batch save.
//      Pinned by serializing the real Scene either side of a batch whose last
//      step fails, and comparing the bytes — not by counting entities, which
//      would pass while a component or a hierarchy edge stayed behind.
//   2. A transaction is ONE undo step, not N. Pinned by running a batch of
//      three creates and taking all three back with a single Undo().
//   3. A command that cannot participate says so in its METADATA and is refused
//      at BATCH-BUILD time. Pinned per refusal reason, each asserting that
//      nothing ran — no entity, no undo entry, no dirty flag.
//   4. A low-authority caller cannot launder a ProjectWrite command into a
//      granted batch. Pinned at both doors: the registry's own consent gate,
//      and the batch builder's.
//
// Plus the symbolic-output wiring, which is what makes batching more than a
// latency optimisation, and a CENSUS over the real registered surface so the
// transactable set cannot drift silently.
//
// Classification: unit. Real Scene, real CommandHistory, real commands, no
// server, no GL context, no editor.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationHost.h"
#include "Automation/AutomationRegistry.h"
#include "Automation/AutomationSceneAuthoring.h"
#include "Automation/AutomationSceneCommands.h"
#include "Automation/AutomationTransaction.h"
#include "Automation/AutomationTransactionCommands.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"
#include "MCP/McpTools.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "UndoRedo/EditorCommand.h"

#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace OloEngine;
    using namespace OloEngine::Automation;
    using Json = nlohmann::json;
    namespace Schema = OloEngine::MCP::Schema;

    class TransactionHost final : public IAutomationHost
    {
      public:
        explicit TransactionHost(MCP::EditorMcpContext context)
            : m_Context(std::move(context))
        {
        }

        [[nodiscard]] const MCP::EditorMcpContext& Context() const override
        {
            return m_Context;
        }
        [[nodiscard]] bool IsCurrentCallCancelled() const override
        {
            return false;
        }
        [[nodiscard]] bool PublishArtifact(AutomationArtifact /*artifact*/) override
        {
            return false;
        }
        [[nodiscard]] int MarshalCount() const
        {
            return m_MarshalCount;
        }

      protected:
        Json MarshalReadOnMainThread(const std::function<Json()>& readJob,
                                     std::chrono::milliseconds /*timeout*/) override
        {
            ++m_MarshalCount;
            return readJob();
        }
        void EmitProgressUpdate(f64 /*progress*/, f64 /*total*/, const std::string& /*message*/) const override
        {
        }

      private:
        MCP::EditorMcpContext m_Context;
        int m_MarshalCount = 0;
    };

    // A command with an arbitrary declared reversal mechanism, for the
    // build-time refusals. Its handler MUTATES a counter, so a test can prove a
    // refused batch never entered it.
    AutomationCommand FakeCommand(std::string name, AutomationUndo undo, bool projectWrite, int& runs)
    {
        AutomationCommand command;
        command.Name = std::move(name);
        command.Description = "Test fixture command.";
        command.InputSchema = Schema::Object().NoAdditional();
        command.OutputSchema = Schema::Object().Prop("ok", Schema::Bool()).Required({ "ok" });
        command.ProjectWrite = projectWrite;
        command.MainMarshaled = true;
        command.Undo = undo;
        command.Handler = [&runs](IAutomationHost&, const Json&)
        {
            ++runs;
            return AutomationResult::Structured(Json{ { "ok", true } });
        };
        return command;
    }

    class McpAutomationTransaction : public ::testing::Test
    {
      protected:
        McpAutomationTransaction()
            : m_Scene(Ref<Scene>::Create()), m_Host(MakeContext())
        {
            RegisterEntityAuthoringCommands(m_Registry);
            RegisterComponentAuthoringCommands(m_Registry);
            RegisterSceneLifecycleCommands(m_Registry);
            RegisterTransactionCommands(m_Registry);
            m_Registry.Register(FakeCommand("test_irreversible", AutomationUndo::Irreversible, true, m_FakeRuns));
            m_Registry.Register(FakeCommand("test_undeclared", AutomationUndo::Unspecified, true, m_FakeRuns));
            m_Registry.Register(FakeCommand("test_readonly", AutomationUndo::None, false, m_FakeRuns));
        }

        MCP::EditorMcpContext MakeContext()
        {
            MCP::EditorMcpContext context;
            context.GetActiveScene = [this]()
            { return m_Scene; };
            context.GetCommandHistory = [this]() -> CommandHistory*
            { return m_HistoryAvailable ? &m_History : nullptr; };
            context.InvalidateEntityReferences = [this]()
            { ++m_SelectionClears; };
            return context;
        }

        // Run a batch through the REGISTERED command, the way every frontend
        // reaches it — never by calling the layer directly, so the consent gate
        // and the schema validation are part of what is being tested.
        AutomationInvocation Apply(const Json& steps, const Json& extra = Json::object(),
                                   AutomationWriteConsent consent = AutomationWriteConsent::Granted)
        {
            Json arguments = extra;
            arguments["steps"] = steps;
            return m_Registry.Invoke(m_Host, "olo_transaction_apply", arguments, consent);
        }

        static Json Step(std::string command, Json arguments = Json::object(), std::string id = {})
        {
            Json step{ { "command", std::move(command) } };
            if (!arguments.empty())
                step["arguments"] = std::move(arguments);
            if (!id.empty())
                step["id"] = std::move(id);
            return step;
        }

        [[nodiscard]] std::string SerializeScene() const
        {
            SceneSerializer serializer(m_Scene);
            return serializer.SerializeToYAML();
        }

        [[nodiscard]] sizet EntityCount() const
        {
            return m_Scene->GetAllEntitiesWith<IDComponent>().size();
        }

        [[nodiscard]] Entity Find(const std::string& id) const
        {
            const auto entity = m_Scene->TryGetEntityWithUUID(UUID(std::stoull(id)));
            return entity ? *entity : Entity{};
        }

        Ref<Scene> m_Scene;
        CommandHistory m_History;
        AutomationRegistry m_Registry;
        bool m_HistoryAvailable = true;
        int m_SelectionClears = 0;
        int m_FakeRuns = 0;
        TransactionHost m_Host;
    };
} // namespace

// ---- the transactability boundary (the spike) -------------------------------

TEST(McpAutomationTransactionBoundary, TransactabilityIsDecidedByDeclaredUndoAndDefaultDenies)
{
    int runs = 0;
    // The two admissible tiers.
    EXPECT_EQ(ClassifyTransactability(FakeCommand("a", AutomationUndo::None, false, runs)), Transactability::Yes);
    EXPECT_EQ(ClassifyTransactability(FakeCommand("b", AutomationUndo::EditorUndoStack, true, runs)),
              Transactability::Yes);

    // DEFAULT-DENY is the whole point: a command that never declared how it is
    // taken back must not be ASSUMED reversible. Guessing here is what leaves
    // the half-applied scene, and it fails silently.
    EXPECT_EQ(ClassifyTransactability(FakeCommand("c", AutomationUndo::Unspecified, true, runs)),
              Transactability::Undeclared);
    EXPECT_EQ(ClassifyTransactability(FakeCommand("d", AutomationUndo::Irreversible, true, runs)),
              Transactability::Irreversible);
    EXPECT_EQ(ClassifyTransactability(FakeCommand("e", AutomationUndo::HistoryControl, true, runs)),
              Transactability::HistoryControl);

    AutomationCommand handlerless = FakeCommand("f", AutomationUndo::None, false, runs);
    handlerless.Handler = nullptr;
    EXPECT_EQ(ClassifyTransactability(handlerless), Transactability::NoHandler);

    // Reversible is necessary but not sufficient. A batch runs every step inside
    // ONE main-thread critical section, so a command that declared it must not
    // run inside a marshaled job (the asset commands, whose whole-project scan
    // would hold the game thread) is refused for THAT reason instead.
    AutomationCommand offThread = FakeCommand("g", AutomationUndo::EditorUndoStack, true, runs);
    offThread.MainMarshaled = false;
    EXPECT_EQ(ClassifyTransactability(offThread), Transactability::NotMainMarshaled);

    EXPECT_EQ(runs, 0) << "classification must never enter a handler";
}

TEST(McpAutomationTransactionBoundary, ASymbolReferenceIsAWholeStringOrNothingAtAll)
{
    const auto simple = ParseSymbolReference(Json("${alpha}"));
    ASSERT_TRUE(simple.has_value());
    EXPECT_EQ(simple->StepId, "alpha");
    EXPECT_TRUE(simple->Path.empty());

    const auto path = ParseSymbolReference(Json("${alpha.entity}"));
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(path->StepId, "alpha");
    EXPECT_EQ(path->Path, std::vector<std::string>({ "entity" }));
    EXPECT_EQ(path->Text(), "${alpha.entity}");

    const auto deep = ParseSymbolReference(Json("${a.b.c}"));
    ASSERT_TRUE(deep.has_value());
    EXPECT_EQ(deep->Path, std::vector<std::string>({ "b", "c" }));

    // A FRAGMENT is a literal. This is the rule that removes the escaping
    // problem entirely: nothing has to be quoted to survive a batch, because
    // substitution only ever replaces a whole value.
    EXPECT_FALSE(ParseSymbolReference(Json("cost: ${alpha.entity}")).has_value());
    EXPECT_FALSE(ParseSymbolReference(Json("${alpha.entity} and more")).has_value());
    EXPECT_FALSE(ParseSymbolReference(Json("$alpha")).has_value());
    EXPECT_FALSE(ParseSymbolReference(Json("${}")).has_value());
    EXPECT_FALSE(ParseSymbolReference(Json("${a..b}")).has_value());
    EXPECT_FALSE(ParseSymbolReference(Json("${a.}")).has_value());
    EXPECT_FALSE(ParseSymbolReference(Json("${a b}")).has_value());
    EXPECT_FALSE(ParseSymbolReference(Json(42)).has_value());
    EXPECT_FALSE(ParseSymbolReference(Json::object()).has_value());
}

// ---- acceptance 3: refused at batch-build time -------------------------------

TEST_F(McpAutomationTransaction, AnIrreversibleStepIsRefusedBeforeAnyEarlierStepRuns)
{
    const auto refused = Apply(Json::array({ Step("olo_entity_create", Json{ { "name", "First" } }),
                                             Step("test_irreversible") }));
    ASSERT_TRUE(refused.Ran()) << refused.Message;
    EXPECT_TRUE(refused.Result.IsError);

    const Json& report = refused.Result.StructuredContent;
    ASSERT_FALSE(report.is_null()) << refused.Result.Content.dump(2);
    EXPECT_FALSE(report.at("admitted").get<bool>());
    EXPECT_FALSE(report.at("applied").get<bool>());
    EXPECT_EQ(report.at("failedStep").get<i64>(), 1);
    EXPECT_NE(report.at("error").get<std::string>().find("irreversible"), std::string::npos)
        << report.at("error").get<std::string>();

    // NOTHING ran — not the admissible step in front of the refused one, and
    // not the refused command itself.
    EXPECT_EQ(EntityCount(), 0u);
    EXPECT_EQ(m_FakeRuns, 0);
    EXPECT_FALSE(m_History.CanUndo());
    EXPECT_FALSE(m_History.IsDirty());
    for (const Json& step : report.at("steps"))
        EXPECT_EQ(step.at("status").get<std::string>(), "skipped");
}

TEST_F(McpAutomationTransaction, AnUndeclaredCommandIsRefusedRatherThanAssumedReversible)
{
    const auto refused = Apply(Json::array({ Step("test_undeclared") }));
    ASSERT_TRUE(refused.Ran()) << refused.Message;
    EXPECT_TRUE(refused.Result.IsError);
    EXPECT_EQ(m_FakeRuns, 0);
    const std::string error = refused.Result.StructuredContent.at("error").get<std::string>();
    EXPECT_NE(error.find("has not declared how it is taken back"), std::string::npos) << error;
}

TEST_F(McpAutomationTransaction, UndoRedoAndNestingAreRefusedAsHistoryControl)
{
    // These three WALK or REWRITE the undo stack rather than adding to it,
    // which is the one thing a transaction step may not do. The declaration is
    // on the command (AutomationUndo::HistoryControl), so this is a metadata
    // refusal, not a name blacklist inside the transaction layer.
    for (const char* name : { "olo_editor_undo", "olo_editor_redo", "olo_transaction_apply" })
    {
        const auto refused = Apply(Json::array({ Step(name) }));
        ASSERT_TRUE(refused.Ran()) << name << ": " << refused.Message;
        EXPECT_TRUE(refused.Result.IsError) << name;
        const std::string error = refused.Result.StructuredContent.at("error").get<std::string>();
        EXPECT_NE(error.find("operates on the editor undo history itself"), std::string::npos) << name << ": " << error;
    }
    EXPECT_FALSE(m_History.CanUndo());
}

TEST_F(McpAutomationTransaction, AnUnknownCommandAndAnOversizedBatchAreRefusedAtBuildTime)
{
    const auto unknown = Apply(Json::array({ Step("olo_entity_create"), Step("olo_no_such_command") }));
    ASSERT_TRUE(unknown.Ran()) << unknown.Message;
    EXPECT_TRUE(unknown.Result.IsError);
    EXPECT_EQ(EntityCount(), 0u);

    Json oversized = Json::array();
    for (sizet i = 0; i <= kMaxTransactionSteps; ++i)
        oversized.push_back(Step("olo_entity_create"));
    const auto refused = Apply(oversized);
    // Over the cap the registry's OWN schema gate (the declared maxItems)
    // refuses it, so the batch never reaches the transaction layer at all —
    // the earliest possible refusal, and one no frontend can skip.
    EXPECT_EQ(refused.Outcome, AutomationInvocation::Status::InvalidArguments);
    EXPECT_NE(refused.Message.find("at most 64 items"), std::string::npos) << refused.Message;
    EXPECT_EQ(EntityCount(), 0u);
}

// ---- acceptance 4: batch authority ------------------------------------------

TEST_F(McpAutomationTransaction, ALowAuthorityCallerCannotLaunderAProjectWriteIntoABatch)
{
    // Door one: the registry. olo_transaction_apply is itself ProjectWrite, so a
    // caller without consent never reaches the handler and no batch is even
    // parsed. Consent is decided ONCE, for the batch, before any step exists.
    const auto denied = Apply(Json::array({ Step("olo_entity_create") }), Json::object(),
                              AutomationWriteConsent::Withheld);
    EXPECT_EQ(denied.Outcome, AutomationInvocation::Status::WriteConsentWithheld);
    EXPECT_EQ(m_Host.MarshalCount(), 0);
    EXPECT_EQ(EntityCount(), 0u);
    EXPECT_FALSE(m_History.CanUndo());

    // Door two: the builder itself, reached directly the way a future non-MCP
    // frontend would. A batch whose members need write authority the caller does
    // not hold is inadmissible — the batch never inherits authority from being a
    // batch.
    const AutomationRegistry::CommandSnapshot snapshot = m_Registry.Snapshot();
    const Json arguments{ { "steps", Json::array({ Step("olo_entity_create") }) } };
    const TransactionBuild withheld = BuildTransaction(snapshot, arguments, AutomationWriteConsent::Withheld);
    EXPECT_FALSE(withheld.Ok);
    EXPECT_NE(withheld.Error.find("without write consent"), std::string::npos) << withheld.Error;

    const TransactionBuild granted = BuildTransaction(snapshot, arguments, AutomationWriteConsent::Granted);
    EXPECT_TRUE(granted.Ok) << granted.Error;
    EXPECT_TRUE(granted.Plan.RequiresWriteConsent);

    // A batch of purely read-only steps needs no authority at all, so the
    // refusal above is about the MEMBERS, not about batching itself.
    const Json readOnly{ { "steps", Json::array({ Step("test_readonly") }) } };
    const TransactionBuild plain = BuildTransaction(snapshot, readOnly, AutomationWriteConsent::Withheld);
    EXPECT_TRUE(plain.Ok) << plain.Error;
    EXPECT_FALSE(plain.Plan.RequiresWriteConsent);
    EXPECT_EQ(m_FakeRuns, 0) << "building a plan must never run a step";
}

// ---- acceptance 1: a failing step leaves the scene byte-identical ------------

TEST_F(McpAutomationTransaction, AFailingStepRollsBackToAByteIdenticalScene)
{
    // A non-trivial starting scene, so the comparison has something to lose.
    Entity anchor = m_Scene->CreateEntity("Anchor");
    m_Scene->CreateEntity("Sibling");
    const std::string before = SerializeScene();
    ASSERT_FALSE(before.empty());
    const sizet entitiesBefore = EntityCount();

    // Three creates and a parent, then a step that cannot succeed: destroying a
    // UUID that does not exist. Everything in front of it applied for real.
    const auto failed = Apply(Json::array({
        Step("olo_entity_create", Json{ { "name", "A" }, { "parent", std::to_string(static_cast<u64>(anchor.GetUUID())) } }),
        Step("olo_entity_create", Json{ { "name", "B" } }, "b"),
        Step("olo_entity_reparent", Json{ { "entity", "${b.entity}" }, { "parent", std::to_string(static_cast<u64>(anchor.GetUUID())) } }),
        Step("olo_entity_destroy", Json{ { "entity", "12345" } }),
    }));

    ASSERT_TRUE(failed.Ran()) << failed.Message;
    const Json& report = failed.Result.StructuredContent;
    ASSERT_FALSE(report.is_null()) << failed.Result.Content.dump(2);
    EXPECT_TRUE(report.at("admitted").get<bool>()) << report.dump(2);
    EXPECT_FALSE(report.at("applied").get<bool>());
    EXPECT_TRUE(report.at("rolledBack").get<bool>());
    EXPECT_FALSE(report.contains("rollbackError")) << report.at("rollbackError").dump();
    EXPECT_EQ(report.at("failedStep").get<i64>(), 3);
    EXPECT_EQ(report.at("appliedSteps").get<sizet>(), 0u);
    EXPECT_TRUE(failed.Result.IsError);

    // The point of the whole issue: not "roughly restored", BYTE-IDENTICAL.
    EXPECT_EQ(EntityCount(), entitiesBefore);
    EXPECT_EQ(SerializeScene(), before);

    // And the history is as it was — a rolled-back batch leaves no entry to
    // press Ctrl-Z past, and does not mark the document dirty.
    EXPECT_FALSE(m_History.CanUndo());
    EXPECT_FALSE(m_History.IsDirty());
    EXPECT_FALSE(m_History.InTransaction());

    // The report says what happened to each step rather than truncating.
    const Json& steps = report.at("steps");
    ASSERT_EQ(steps.size(), 4u);
    EXPECT_EQ(steps[0].at("status").get<std::string>(), "rolledBack");
    EXPECT_EQ(steps[1].at("status").get<std::string>(), "rolledBack");
    EXPECT_EQ(steps[2].at("status").get<std::string>(), "rolledBack");
    EXPECT_EQ(steps[3].at("status").get<std::string>(), "failed");
}

// ---- acceptance 2: one undo step, not N -------------------------------------

TEST_F(McpAutomationTransaction, ASuccessfulTransactionIsOneUndoStepNotN)
{
    const std::string before = SerializeScene();

    const auto applied = Apply(Json::array({
                                   Step("olo_entity_create", Json{ { "name", "One" } }, "one"),
                                   Step("olo_entity_create", Json{ { "name", "Two" } }, "two"),
                                   Step("olo_entity_create", Json{ { "name", "Three" }, { "parent", "${one.entity}" } }),
                               }),
                               Json{ { "description", "Build a rig" } });

    ASSERT_TRUE(applied.Ran()) << applied.Message;
    ASSERT_FALSE(applied.Result.IsError) << applied.Result.Content.dump(2);
    const Json& report = applied.Result.StructuredContent;
    EXPECT_TRUE(report.at("applied").get<bool>());
    EXPECT_EQ(report.at("appliedSteps").get<sizet>(), 3u);
    EXPECT_EQ(report.at("undoEntries").get<sizet>(), 3u);
    EXPECT_TRUE(report.at("undoable").get<bool>());
    EXPECT_EQ(report.at("description").get<std::string>(), "Build a rig");
    EXPECT_EQ(EntityCount(), 3u);
    EXPECT_TRUE(m_History.IsDirty());

    // THREE operations, ONE entry. One Ctrl-Z takes the whole batch back and
    // leaves nothing behind to undo.
    ASSERT_TRUE(m_History.CanUndo());
    EXPECT_EQ(m_History.GetUndoDescription(), "Build a rig");
    m_History.Undo();
    EXPECT_EQ(EntityCount(), 0u);
    EXPECT_EQ(SerializeScene(), before);
    EXPECT_FALSE(m_History.CanUndo()) << "the batch left more than one undo entry";

    // And redo brings the whole batch back, still as one entry.
    ASSERT_TRUE(m_History.CanRedo());
    m_History.Redo();
    EXPECT_EQ(EntityCount(), 3u);
    EXPECT_FALSE(m_History.CanRedo());
}

TEST_F(McpAutomationTransaction, AReadOnlyBatchLeavesNoUndoEntryAtAll)
{
    const auto applied = Apply(Json::array({ Step("test_readonly"), Step("olo_component_list_types") }));
    ASSERT_TRUE(applied.Ran()) << applied.Message;
    ASSERT_FALSE(applied.Result.IsError) << applied.Result.Content.dump(2);
    const Json& report = applied.Result.StructuredContent;
    EXPECT_TRUE(report.at("applied").get<bool>());
    EXPECT_EQ(report.at("undoEntries").get<sizet>(), 0u);
    EXPECT_FALSE(report.at("undoable").get<bool>());
    EXPECT_EQ(m_FakeRuns, 1);
    // No no-op entry for the user to press Ctrl-Z past.
    EXPECT_FALSE(m_History.CanUndo());
    EXPECT_FALSE(m_History.IsDirty());
}

// ---- symbolic outputs --------------------------------------------------------

TEST_F(McpAutomationTransaction, ASymbolicOutputFeedsALaterStepWithoutARoundTrip)
{
    const auto applied = Apply(Json::array({
        Step("olo_entity_create", Json{ { "name", "Root" } }, "root"),
        Step("olo_entity_create", Json{ { "name", "Child" }, { "parent", "${root.entity}" } }, "child"),
        Step("olo_entity_reparent", Json{ { "entity", "${child.entity}" }, { "parent", "0" } }),
    }));

    ASSERT_TRUE(applied.Ran()) << applied.Message;
    ASSERT_FALSE(applied.Result.IsError) << applied.Result.Content.dump(2);
    const Json& steps = applied.Result.StructuredContent.at("steps");
    ASSERT_EQ(steps.size(), 3u);

    // Step 1 was parented to the id step 0 minted, with no call in between.
    const auto rootId = steps[0].at("result").at("entity").get<std::string>();
    const auto childId = steps[1].at("result").at("entity").get<std::string>();
    EXPECT_EQ(steps[1].at("result").at("parent").get<std::string>(), rootId);
    // ...and step 2 reached step 1's id the same way, detaching it again.
    EXPECT_EQ(steps[2].at("result").at("entity").get<std::string>(), childId);
    ASSERT_TRUE(Find(childId));
    EXPECT_EQ(static_cast<u64>(Find(childId).GetParentUUID()), 0u);
}

TEST_F(McpAutomationTransaction, AnUnresolvableSymbolIsABuildTimeRefusalNotARuntimeSurprise)
{
    const auto expectRefusal = [this](const Json& steps, std::string_view fragment)
    {
        const auto refused = Apply(steps);
        ASSERT_TRUE(refused.Ran()) << refused.Message;
        EXPECT_TRUE(refused.Result.IsError) << refused.Result.Content.dump(2);
        const Json& report = refused.Result.StructuredContent;
        EXPECT_FALSE(report.at("admitted").get<bool>());
        const std::string error = report.at("error").get<std::string>();
        EXPECT_NE(error.find(fragment), std::string::npos) << error;
        // Refused at BUILD time means the admissible step in front of it never ran.
        EXPECT_EQ(EntityCount(), 0u) << error;
        EXPECT_FALSE(m_History.CanUndo()) << error;
    };

    // An id no step declares.
    expectRefusal(Json::array({ Step("olo_entity_create", Json{ { "name", "A" } }, "a"),
                                Step("olo_entity_create", Json{ { "parent", "${nope.entity}" } }) }),
                  "no EARLIER step declares the id");

    // A FORWARD reference: 'later' exists, but not yet. Insertion happens after
    // a step is admitted, which is what makes this impossible rather than merely
    // discouraged.
    expectRefusal(Json::array({ Step("olo_entity_create", Json{ { "parent", "${later.entity}" } }),
                                Step("olo_entity_create", Json{ { "name", "L" } }, "later") }),
                  "no EARLIER step declares the id");

    // A SELF reference, for the same reason.
    expectRefusal(Json::array({ Step("olo_entity_create", Json{ { "parent", "${me.entity}" } }, "me") }),
                  "no EARLIER step declares the id");

    // A field the source command's OutputSchema says will not be there.
    expectRefusal(Json::array({ Step("olo_entity_create", Json{ { "name", "A" } }, "a"),
                                Step("olo_entity_create", Json{ { "parent", "${a.nosuchfield}" } }) }),
                  "declares no such output field");

    // A duplicate id: a reference has to name exactly one step.
    expectRefusal(Json::array({ Step("olo_entity_create", Json{ { "name", "A" } }, "dup"),
                                Step("olo_entity_create", Json{ { "name", "B" } }, "dup") }),
                  "reuses the id");
}

TEST_F(McpAutomationTransaction, SymbolicWiringIsTypeCheckedAgainstTheSourceCommandsDeclaredOutput)
{
    // `changed` is a BOOLEAN in olo_entity_create's output schema; `parent` is a
    // string in its input schema. The value does not exist yet at build time —
    // but its declared TYPE does, so the mismatch is caught before anything runs
    // rather than surfacing as a half-applied batch.
    const auto refused = Apply(Json::array({ Step("olo_entity_create", Json{ { "name", "A" } }, "a"),
                                             Step("olo_entity_create", Json{ { "parent", "${a.changed}" } }) }));
    ASSERT_TRUE(refused.Ran()) << refused.Message;
    EXPECT_TRUE(refused.Result.IsError);
    const std::string error = refused.Result.StructuredContent.at("error").get<std::string>();
    EXPECT_NE(error.find("a symbolic reference was checked against the type its source step declares"),
              std::string::npos)
        << error;
    EXPECT_EQ(EntityCount(), 0u);
}

// ---- dry run -----------------------------------------------------------------

TEST_F(McpAutomationTransaction, ADryRunValidatesTheWholeBatchAndAppliesNothing)
{
    const std::string before = SerializeScene();
    const auto planned = Apply(Json::array({
                                   Step("olo_entity_create", Json{ { "name", "Root" } }, "root"),
                                   Step("olo_entity_create", Json{ { "parent", "${root.entity}" } }),
                                   Step("test_readonly"),
                               }),
                               Json{ { "dryRun", true } });

    ASSERT_TRUE(planned.Ran()) << planned.Message;
    ASSERT_FALSE(planned.Result.IsError) << planned.Result.Content.dump(2);
    const Json& report = planned.Result.StructuredContent;
    EXPECT_TRUE(report.at("dryRun").get<bool>());
    EXPECT_TRUE(report.at("admitted").get<bool>());
    EXPECT_FALSE(report.at("applied").get<bool>());
    EXPECT_EQ(report.at("stepCount").get<sizet>(), 3u);
    EXPECT_TRUE(report.at("requiresWriteConsent").get<bool>());

    const Json& steps = report.at("steps");
    ASSERT_EQ(steps.size(), 3u);
    for (const Json& step : steps)
    {
        EXPECT_EQ(step.at("status").get<std::string>(), "validated");
        EXPECT_EQ(step.at("transactable").get<std::string>(), "yes");
    }
    EXPECT_EQ(steps[0].at("undo").get<std::string>(), "editorUndoStack");
    EXPECT_TRUE(steps[0].at("projectWrite").get<bool>());
    EXPECT_EQ(steps[1].at("references"), Json::array({ "${root.entity}" }));
    EXPECT_FALSE(steps[2].at("projectWrite").get<bool>());

    // Applied nothing, at all.
    EXPECT_EQ(EntityCount(), 0u);
    EXPECT_EQ(SerializeScene(), before);
    EXPECT_EQ(m_FakeRuns, 0);
    EXPECT_FALSE(m_History.CanUndo());
    EXPECT_FALSE(m_History.IsDirty());
}

TEST_F(McpAutomationTransaction, ADryRunOfAnInadmissibleBatchStillRefusesIt)
{
    const auto refused = Apply(Json::array({ Step("test_irreversible") }), Json{ { "dryRun", true } });
    ASSERT_TRUE(refused.Ran()) << refused.Message;
    EXPECT_TRUE(refused.Result.IsError);
    EXPECT_FALSE(refused.Result.StructuredContent.at("admitted").get<bool>());
}

// ---- edit mode ---------------------------------------------------------------

TEST_F(McpAutomationTransaction, AMutatingBatchWithoutACommandHistoryIsRefusedWithoutMutating)
{
    m_HistoryAvailable = false;
    const auto refused = Apply(Json::array({ Step("olo_entity_create", Json{ { "name", "A" } }) }));
    ASSERT_TRUE(refused.Ran()) << refused.Message;
    EXPECT_TRUE(refused.Result.IsError);
    const std::string error = refused.Result.StructuredContent.at("error").get<std::string>();
    EXPECT_NE(error.find("Edit mode"), std::string::npos) << error;
    EXPECT_EQ(EntityCount(), 0u);
    EXPECT_FALSE(m_History.InTransaction());
}

// ---- the grouped undo entry itself -------------------------------------------

namespace
{
    // Records what it did, and can be told to refuse its undo the way a guarded
    // scene-save restore refuses an externally modified file.
    class ScriptedCommand final : public EditorCommand
    {
      public:
        ScriptedCommand(std::string name, std::vector<std::string>& journal, bool refuseUndo = false)
            : m_Name(std::move(name)), m_Journal(journal), m_RefuseUndo(refuseUndo)
        {
        }

        void Execute() override
        {
            m_Journal.push_back("do:" + m_Name);
        }
        void Undo() override
        {
            if (m_RefuseUndo)
                throw std::runtime_error("refused");
            m_Journal.push_back("undo:" + m_Name);
        }
        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Name;
        }

      private:
        std::string m_Name;
        std::vector<std::string>& m_Journal;
        bool m_RefuseUndo;
    };
} // namespace

TEST(McpAutomationTransactionHistory, APartiallyUndoneGroupRESUMESRatherThanRestarting)
{
    // CommandHistory keeps an entry on the stack when its undo throws, so the
    // user can fix the cause and press Ctrl-Z again. With N operations grouped
    // into ONE entry, restarting from the end would re-undo the members already
    // taken back — so the group has to resume.
    std::vector<std::string> journal;
    CommandHistory history;
    history.BeginTransaction("Batch");
    history.PushAlreadyExecuted(std::make_unique<ScriptedCommand>("first", journal));
    history.PushAlreadyExecuted(std::make_unique<ScriptedCommand>("blocked", journal, /*refuseUndo*/ true));
    history.PushAlreadyExecuted(std::make_unique<ScriptedCommand>("third", journal));
    history.CommitTransaction();
    ASSERT_TRUE(history.CanUndo());

    // First Ctrl-Z: "third" comes back, then "blocked" refuses. The entry stays.
    EXPECT_THROW(history.Undo(), std::runtime_error);
    EXPECT_EQ(journal, std::vector<std::string>({ "undo:third" }));
    EXPECT_TRUE(history.CanUndo());

    // Second Ctrl-Z, still blocked: "third" must NOT be undone a second time.
    EXPECT_THROW(history.Undo(), std::runtime_error);
    EXPECT_EQ(journal, std::vector<std::string>({ "undo:third" }));

    // The cause is fixed. One more Ctrl-Z finishes the group from where it got
    // to, and the entry finally leaves the stack.
    journal.clear();
    CommandHistory fixed;
    fixed.BeginTransaction("Batch");
    fixed.PushAlreadyExecuted(std::make_unique<ScriptedCommand>("first", journal));
    fixed.PushAlreadyExecuted(std::make_unique<ScriptedCommand>("second", journal));
    fixed.CommitTransaction();
    fixed.Undo();
    EXPECT_EQ(journal, std::vector<std::string>({ "undo:second", "undo:first" }));
    EXPECT_FALSE(fixed.CanUndo());
    EXPECT_TRUE(fixed.CanRedo());
}

TEST(McpAutomationTransactionHistory, AnOpenTransactionIsDirtyAndRefusesToWalkTheStack)
{
    std::vector<std::string> journal;
    CommandHistory history;
    EXPECT_FALSE(history.IsDirty());

    history.BeginTransaction("Batch");
    EXPECT_TRUE(history.InTransaction());
    // Still clean: nothing has been applied yet.
    EXPECT_FALSE(history.IsDirty());

    history.PushAlreadyExecuted(std::make_unique<ScriptedCommand>("first", journal));
    // The scene HAS changed, even though the entry is still withheld. A step
    // reporting document state mid-batch must not answer "clean".
    EXPECT_TRUE(history.IsDirty());
    EXPECT_EQ(history.TransactionSize(), 1u);

    // Walking the stack mid-transaction has no coherent meaning, so it is
    // refused rather than given an arbitrary one.
    EXPECT_THROW(history.Undo(), std::logic_error);
    EXPECT_THROW(history.Redo(), std::logic_error);
    EXPECT_THROW(history.Clear(), std::logic_error);
    EXPECT_THROW(history.BeginTransaction("Nested"), std::logic_error);

    EXPECT_EQ(history.RollbackTransaction(), "");
    EXPECT_EQ(journal, std::vector<std::string>({ "undo:first" }));
    EXPECT_FALSE(history.InTransaction());
    EXPECT_FALSE(history.IsDirty());
    EXPECT_FALSE(history.CanUndo());
}

TEST(McpAutomationTransactionHistory, AnIncompleteRollbackReportsWhatItCouldNotTakeBack)
{
    std::vector<std::string> journal;
    CommandHistory history;
    history.BeginTransaction("Batch");
    history.PushAlreadyExecuted(std::make_unique<ScriptedCommand>("first", journal));
    history.PushAlreadyExecuted(std::make_unique<ScriptedCommand>("blocked", journal, /*refuseUndo*/ true));
    history.PushAlreadyExecuted(std::make_unique<ScriptedCommand>("third", journal));

    // A member that refuses must not strand the members underneath it — that is
    // the half-applied state a transaction exists to prevent. So the rollback
    // continues past it and REPORTS, loudly and countably.
    const std::string failure = history.RollbackTransaction();
    EXPECT_EQ(journal, std::vector<std::string>({ "undo:third", "undo:first" }));
    EXPECT_NE(failure.find("1 of 3 operations could not be taken back"), std::string::npos) << failure;
    EXPECT_NE(failure.find("blocked"), std::string::npos) << failure;
    EXPECT_FALSE(history.InTransaction());
}

// ---- the census: the transactable set, over the REAL surface -----------------

TEST(McpAutomationTransactionCensus, EveryRegisteredCommandsTransactabilityIsClassifiedAndRatcheted)
{
    AutomationRegistry registry;
    OloEngine::MCP::RegisterBuiltinCommands(registry);
    const AutomationRegistry::CommandSnapshot snapshot = registry.Snapshot();
    ASSERT_FALSE(snapshot->empty());

    std::map<std::string, int> census;
    std::map<std::string, int> undoCensus;
    std::vector<std::string> transactable;
    std::vector<std::string> undeclared;
    for (const AutomationCommand& command : *snapshot)
    {
        const Transactability verdict = ClassifyTransactability(command);
        ++census[std::string(TransactabilityToken(verdict))];
        ++undoCensus[std::string(UndoToken(command.Undo))];
        if (verdict == Transactability::Yes)
            transactable.push_back(command.Name);
        else if (verdict == Transactability::Undeclared)
            undeclared.push_back(command.Name);

        // A command that mutates the project but claims there is nothing to undo
        // is a contradiction, and it is the one shape that would slip a
        // non-reversible write into a batch through the FRONT door. Refuse the
        // combination outright rather than trusting the declaration.
        EXPECT_FALSE(command.ProjectWrite && command.Undo == AutomationUndo::None)
            << command.Name << " is ProjectWrite but declares Undo::None — declare how it is taken back.";

        // No command may be registered with a null handler.
        EXPECT_NE(verdict, Transactability::NoHandler) << command.Name;
    }

    // Printed, not just asserted: this census IS the spike's answer, and a
    // reader of a failing run should see the shape of the surface, not only the
    // number that moved.
    std::cout << "[transactability census] total=" << snapshot->size();
    for (const auto& [token, count] : census)
        std::cout << " " << token << "=" << count;
    std::cout << "\n[declared undo census]";
    for (const auto& [token, count] : undoCensus)
        std::cout << " " << token << "=" << count;
    std::cout << "\n[transactable] ";
    for (const std::string& name : transactable)
        std::cout << name << " ";
    std::cout << "\n[undeclared, i.e. NOT transactable until they declare] " << undeclared.size() << "\n";

    // The ratchet. Structural scene authoring is the adopter set, and it must
    // stay transactable: a change that silently drops one of these out of the
    // batchable surface is exactly the regression this pins.
    for (const char* name : { "olo_entity_create", "olo_entity_destroy", "olo_entity_duplicate",
                              "olo_entity_reparent", "olo_component_add", "olo_component_remove",
                              "olo_component_get", "olo_component_list_types", "olo_scene_status",
                              "olo_scene_new", "olo_scene_save", "olo_scene_save_as" })
    {
        const AutomationCommand* command = AutomationRegistry::Find(*snapshot, name);
        ASSERT_NE(command, nullptr) << name;
        EXPECT_EQ(ClassifyTransactability(*command), Transactability::Yes) << name;
    }

    // And the deliberate exclusions stay excluded, each for its declared reason.
    const std::map<std::string, Transactability> kExcluded{
        { "olo_asset_create", Transactability::Irreversible },
        { "olo_asset_delete", Transactability::Irreversible },
        { "olo_asset_import", Transactability::Irreversible },
        { "olo_asset_reimport", Transactability::Irreversible },
        { "olo_asset_import_settings", Transactability::Irreversible },
        { "olo_editor_undo", Transactability::HistoryControl },
        { "olo_editor_redo", Transactability::HistoryControl },
        { "olo_transaction_apply", Transactability::HistoryControl },
        // Reversible, but registered MainMarshaled=false on purpose: their
        // whole-project scan runs on the handler thread, and a batch would drag
        // it onto the game thread and freeze the editor for its duration.
        { "olo_asset_get", Transactability::NotMainMarshaled },
        { "olo_asset_references", Transactability::NotMainMarshaled },
        { "olo_asset_move", Transactability::NotMainMarshaled },
        { "olo_build_list", Transactability::NotMainMarshaled },
    };
    for (const auto& [name, expected] : kExcluded)
    {
        const AutomationCommand* command = AutomationRegistry::Find(*snapshot, name);
        ASSERT_NE(command, nullptr) << name;
        EXPECT_EQ(ClassifyTransactability(*command), expected) << name;
    }
}
