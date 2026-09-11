#include "OloEnginePCH.h"
#include "Automation/AutomationTransactionCommands.h"

#include "Automation/AutomationRegistry.h"
#include "Automation/AutomationTransaction.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace OloEngine::Automation
{
    namespace
    {
        using Json = nlohmann::json;
    } // namespace

    void RegisterTransactionCommands(AutomationRegistry& registry)
    {
        AutomationCommand command;
        command.Name = "olo_transaction_apply";
        command.Title = "Apply a transaction";
        command.Description =
            "Apply several automation commands as ONE atomic step: all of them apply or none do. A failing step rolls "
            "the whole batch back, and a successful batch commits as a single editor undo entry. A step's argument may "
            "reference an earlier step's result as \"${id.field}\" (e.g. the entity id step 1 created), so no round "
            "trip is needed between steps. Only commands that declare how they are taken back may be steps; an "
            "irreversible one (asset import, bake, build) and undo/redo itself are refused before anything runs. Use "
            "dryRun to validate a batch and apply nothing.";
        command.Toolset = "scene";
        command.InputSchema = TransactionInputSchema();
        command.OutputSchema = TransactionOutputSchema();
        command.Annotations =
            Json{ { "readOnlyHint", false }, { "destructiveHint", true }, { "idempotentHint", false }, { "openWorldHint", false } };

        // THE AUTHORITY CLASS, and it is deliberately declared by what this
        // command CAN do rather than by what one call's arguments ask for. A
        // dryRun:true batch mutates nothing, so gating it looks over-strict --
        // but classing a command by its payload is authority-by-argument, which
        // is precisely the laundering shape ADR 0005 forbids ("a tool's write
        // authority is its OWN declared tier ... never inherited, never ambient,
        // and never laundered"). One gate, decided once, for the whole batch.
        command.ProjectWrite = true;
        command.MainMarshaled = true;

        // HistoryControl: a transaction GROUPS undo entries, so it is neither
        // "one entry on the stack" nor irreversible. It is also what stops a
        // transaction being a step of another transaction -- nesting would open a
        // second history scope inside the first.
        command.Undo = AutomationUndo::HistoryControl;

        command.Handler = [&registry](IAutomationHost& host, const Json& arguments) -> AutomationResult
        {
            // ONE snapshot for the whole transaction, exactly as a tools/call
            // pins one: a concurrent script reload must not swap the command
            // vector between admitting the batch and running it, or a step could
            // execute against a definition the batch was never checked against.
            const AutomationRegistry::CommandSnapshot snapshot = registry.Snapshot();

            // WHY Granted is asserted here rather than threaded in. The registry
            // refuses a ProjectWrite command outright unless the caller passed
            // Granted (AutomationRegistry::Invoke), and RunHandler is the only
            // door into a handler. Reaching this line therefore MEANS the caller
            // holds consent for this batch -- decided once, before any step was
            // even parsed. The steps then run under that one grant and never
            // under a stronger one, which is the property the acceptance
            // criterion asks for: a caller without consent is refused at the
            // registry door and never builds a batch at all.
            constexpr AutomationWriteConsent batchConsent = AutomationWriteConsent::Granted;

            const TransactionBuild build = BuildTransaction(snapshot, arguments, batchConsent);
            if (!build.Ok)
            {
                // Nothing ran. The refusal is reported in the same shape as every
                // other outcome so a caller parses one thing, with `admitted`
                // false saying the batch never started.
                AutomationResult refused = AutomationResult::Structured(DescribeInadmissibleBatch(build, arguments));
                refused.IsError = true;
                return refused;
            }

            if (build.Plan.DryRun)
                return AutomationResult::Structured(DescribeDryRun(snapshot, build.Plan));

            // ONE marshaled job for the WHOLE batch. Every step's own MarshalRead
            // is served inline inside it (InlineMarshalHost), so the transaction
            // is a single uninterrupted main-thread critical section: no frame
            // runs between steps, and nothing the editor does can land inside the
            // undo group.
            const Json report = host.MarshalRead(
                [&registry, &snapshot, &host, &build]()
                { return ExecuteTransactionOnMainThread(registry, snapshot, host, build.Plan, batchConsent); });

            AutomationResult result = AutomationResult::Structured(report);
            // A rolled-back batch is a command-level error: the caller asked for
            // N changes and has none. The structured report rides along so the
            // caller can see which step failed and why without scraping text.
            result.IsError = !report.value("applied", false);
            return result;
        };

        registry.Register(std::move(command));
    }
} // namespace OloEngine::Automation
