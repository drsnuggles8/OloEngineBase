#pragma once

// Atomic multi-command transactions over the automation registry (issue #1127).
//
// Every automation call is otherwise independent, so a multi-step edit — create
// an entity, add three components, set eight fields, parent it — is N calls that
// can each fail on their own and leave the scene in a state that is neither the
// before nor the after. That half-built scene is discovered much later, which is
// why the issue is scored as a stability problem rather than a capability one.
//
// This layer lives BESIDE the registry rather than inside any one command family
// on purpose: the semantics have to be identical for entities, components,
// scenes and assets, and defining them inside one domain guarantees the other
// three drift.
//
// ---- what a transaction is here ---------------------------------------------
//
// One batch of steps, resolved against ONE pinned CommandSnapshot, admitted or
// refused as a whole, then executed inside ONE main-thread critical section, and
// committed to the editor's CommandHistory as ONE undo entry. A failing step
// takes the whole batch back.
//
// ---- the transactability boundary (the spike the issue asked for) -----------
//
// Which commands may be steps is decided purely from AutomationCommand::Undo,
// and the rule DEFAULT-DENIES, exactly as AutomationRegistry::Invoke
// default-denies write consent:
//
//   None            -> yes. It does not mutate, so there is nothing to take back.
//   EditorUndoStack -> yes. Its undo entry is what rollback replays in reverse.
//   Irreversible    -> NO. An asset import, a bake, a build invocation. The
//                      honest answer is that these are not transactable, not
//                      that they pretend to roll back.
//   HistoryControl  -> NO. Undo/redo walk the very stack a transaction is
//                      rewriting.
//   Unspecified     -> NO. A command that never declared its reversal mechanism
//                      must not be ASSUMED reversible: a wrong guess here leaves
//                      the half-applied scene this layer exists to prevent, and
//                      it fails silently. This is also what keeps script-owned
//                      and bridged commands out until they declare.
//
// One further requirement, and it comes from HOW a transaction runs rather than
// from how a step reverses: a step must declare `MainMarshaled`. The whole batch
// executes inside ONE main-thread critical section, so a command that explicitly
// declared it must NOT run inside a marshaled job — the asset commands do, their
// whole-project scan would hold the game thread for its duration — cannot be a
// step without freezing the editor. That is a build-time refusal too.
//
// The refusal is a BUILD-TIME refusal — nothing executes until every step is
// admissible — which is the issue's third acceptance criterion.
//
// ---- what counts as a step FAILING ------------------------------------------
//
// The registry refusing to run it, or the handler returning a result with
// `IsError` set. Nothing else. A command that completes and reports inside its
// own payload that it declined to do something (`refused: true`, `changed:
// false`) has SUCCEEDED as far as this layer is concerned, and the batch
// continues. That is deliberate: reading a domain's payload fields to decide
// whether a batch should abort would put one domain's vocabulary inside the
// cross-cutting layer, which is the exact coupling this issue exists to avoid.
// A command that wants to abort a batch says so with an error result.
//
// ---- batch authority (ADR 0005) ----------------------------------------------
//
// A batch's authority is the MAXIMUM its members require, decided ONCE for the
// whole batch. A caller without Granted consent cannot put a ProjectWrite step
// into a batch at all: the batch is refused at build time, and no step ever runs
// with a consent stronger than the one the batch itself was granted. That is the
// "never inherited, never ambient, never laundered" property the outbound client
// already upholds, applied to batching.

#include "Automation/AutomationCommand.h"
#include "Automation/AutomationRegistry.h"

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Automation
{
    // A batch is bounded well under CommandHistory::MaxHistorySize (128) so a
    // committed transaction can never be the thing that trims the user's history
    // to nothing, and so one runaway batch cannot hold the main thread for an
    // unbounded stretch.
    inline constexpr sizet kMaxTransactionSteps = 64;

    // Why a command may or may not be a transaction step. See the header comment
    // for the rule; this enum is the reportable form of it.
    enum class Transactability : u8
    {
        Yes = 0,
        Undeclared,       // Undo::Unspecified — default-denied.
        Irreversible,     // Undo::Irreversible.
        HistoryControl,   // Undo::HistoryControl.
        NotMainMarshaled, // declares it must NOT run inside a marshaled job.
        NoHandler,        // registered with a null handler.
    };

    [[nodiscard]] Transactability ClassifyTransactability(const AutomationCommand& command);
    // A sentence naming `command` and saying why it was refused. Empty for Yes.
    [[nodiscard]] std::string DescribeRefusal(const std::string& name, Transactability verdict);
    // The stable token reported under a dry-run step's `transactable` field.
    [[nodiscard]] std::string_view TransactabilityToken(Transactability verdict);
    // The stable token for a command's DECLARED reversal mechanism, reported
    // under a dry-run step's `undo` field. Exposed rather than kept internal so
    // the surface census reads the same spelling the report does.
    [[nodiscard]] std::string_view UndoToken(AutomationUndo undo);

    // ---- symbolic outputs ----------------------------------------------------
    //
    // A step's argument may reference an EARLIER step's structured result, so
    // step 4 can use the entity id step 1 created without a round trip.
    //
    // The reference is a WHOLE JSON STRING of the form "${<id>}" or
    // "${<id>.<dot.path>}" — never a fragment of a longer string. That one rule
    // removes the entire escaping problem: "cost: ${5}" is a literal, because it
    // is not the whole value, and no quoting convention has to be invented.
    struct SymbolReference
    {
        std::string StepId;
        std::vector<std::string> Path; // may be empty: the whole structured result.
        [[nodiscard]] std::string Text() const;
    };

    // A reference iff `value` is a string matching the whole-string form.
    [[nodiscard]] std::optional<SymbolReference> ParseSymbolReference(const nlohmann::json& value);

    struct TransactionStep
    {
        std::string Command;
        nlohmann::json Arguments = nlohmann::json::object();
        // Optional label other steps reference this one's output by. Unique
        // within a batch; referencing an id that is not declared by an EARLIER
        // step is a build-time refusal, never a runtime surprise.
        std::string Id;
    };

    // An ADMISSIBLE batch: every step resolved, transactable, argument-validated
    // and symbol-checked. Building one is the only way to get here, so holding a
    // plan means the batch was admitted.
    struct TransactionPlan
    {
        std::vector<TransactionStep> Steps;
        std::string Description;
        bool DryRun = false;
        // True when any step is ProjectWrite: the batch's authority, decided once.
        bool RequiresWriteConsent = false;
        // True when any step declares EditorUndoStack, i.e. the batch needs a
        // live CommandHistory to roll back into.
        bool RequiresCommandHistory = false;
    };

    struct TransactionBuild
    {
        bool Ok = false;
        TransactionPlan Plan;
        std::string Error;   // why the batch is inadmissible; empty when Ok.
        i64 FailedStep = -1; // the offending step index, or -1 for a batch-level refusal.
    };

    // Resolve and admit `arguments` (the command's declared input shape) against
    // `commands`. Pure: touches no editor state and runs nothing, so it is safe
    // on the handler thread and is what dry-run reports from.
    //
    // `consent` is the consent the CALLER holds for this batch. A batch needing
    // write authority the caller does not hold is refused HERE.
    [[nodiscard]] TransactionBuild BuildTransaction(const AutomationRegistry::CommandSnapshot& commands,
                                                    const nlohmann::json& arguments,
                                                    AutomationWriteConsent consent);

    // The report for a batch that was never admitted. Same shape as every other
    // outcome, so a caller parses one thing: `admitted` false, `applied` false,
    // `error` saying which step and why, and every step listed as skipped —
    // because that is literally what happened to all of them.
    [[nodiscard]] nlohmann::json DescribeInadmissibleBatch(const TransactionBuild& build,
                                                           const nlohmann::json& arguments);

    // What the batch WOULD do. Applies nothing.
    [[nodiscard]] nlohmann::json DescribeDryRun(const AutomationRegistry::CommandSnapshot& commands,
                                                const TransactionPlan& plan);

    // Run the whole plan. MUST be called on the main (game) thread, from inside a
    // single marshaled job — every step's own MarshalRead is then served inline,
    // so the batch is one uninterrupted editor critical section and no unrelated
    // edit can land inside the group.
    //
    // Returns the structured report. Never throws for a step failure: a failing
    // step rolls the batch back and is reported.
    [[nodiscard]] nlohmann::json ExecuteTransactionOnMainThread(const AutomationRegistry& registry,
                                                                const AutomationRegistry::CommandSnapshot& commands,
                                                                IAutomationHost& host, const TransactionPlan& plan,
                                                                AutomationWriteConsent consent);

    [[nodiscard]] nlohmann::json TransactionInputSchema();
    [[nodiscard]] nlohmann::json TransactionOutputSchema();
} // namespace OloEngine::Automation
