#include "OloEnginePCH.h"
#include "Automation/AutomationTransaction.h"

#include "Automation/AutomationSchemaValidation.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"
#include "UndoRedo/EditorCommand.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <map>
#include <stdexcept>
#include <utility>

namespace OloEngine::Automation
{
    namespace
    {
        using Json = nlohmann::json;
        namespace Schema = MCP::Schema;

        constexpr const char* kAppliedKey = "applied";
        constexpr const char* kAdmittedKey = "admitted";

        // A step id and one path segment are the same alphabet: the identifier
        // characters, plus '-' and '_'. Deliberately NOT '.', which is the path
        // separator, and deliberately not whitespace, so a reference is one token.
        [[nodiscard]] bool IsIdentifierChar(char c)
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        }

        // The first line of a result's text content, for a failure message. A
        // handler that returned AutomationResult::Error put its reason there.
        [[nodiscard]] std::string ResultMessage(const AutomationResult& result)
        {
            if (result.Content.is_array())
            {
                for (const Json& block : result.Content)
                {
                    if (block.is_object() && block.value("type", std::string{}) == "text")
                    {
                        std::string text = block.value("text", std::string{});
                        if (const auto newline = text.find('\n'); newline != std::string::npos)
                            text.resize(newline);
                        if (!text.empty())
                            return text;
                    }
                }
            }
            return "the command reported an error with no message";
        }

        // A value of the JSON type `typeToken` names, for validating a step's
        // arguments at BUILD time: the real value does not exist yet, but its
        // declared type does, so the wiring can still be type-checked before
        // anything runs. Falls back to a string, which is what every symbolic
        // reference is syntactically.
        [[nodiscard]] Json PlaceholderForType(std::string_view typeToken)
        {
            if (typeToken == "integer")
                return Json(0);
            if (typeToken == "number")
                return Json(0.0);
            if (typeToken == "boolean")
                return Json(false);
            if (typeToken == "array")
                return Json::array();
            if (typeToken == "object")
                return Json::object();
            return Json("0");
        }

        // Walk `outputSchema`'s declared properties along `path`.
        //
        // `declared` comes back false only when the schema DOES describe its
        // properties and this path is not among them — which is a build-time
        // refusal, because the referenced command has told us the field will not
        // be there. A command with no output schema declares nothing, so nothing
        // can be refused on its behalf.
        [[nodiscard]] Json ResolveDeclaredOutput(const Json& outputSchema, const std::vector<std::string>& path,
                                                 bool& declared, bool& describable)
        {
            declared = true;
            describable = outputSchema.is_object() && !outputSchema.empty();
            if (!describable)
                return Json();
            // An empty path is "${id}" -- the whole structured result, whose type
            // is the output schema's own. Falling back to a string here would
            // type-check an object reference as a string and refuse a legitimate
            // batch at build time.
            if (path.empty())
                return outputSchema;

            const Json* node = &outputSchema;
            for (const std::string& segment : path)
            {
                const auto properties = node->find("properties");
                if (properties == node->end() || !properties->is_object())
                {
                    // An object the schema does not describe field-by-field. Its
                    // contents are unconstrained, so the path stays admissible and
                    // only its type is unknown.
                    describable = false;
                    return Json();
                }
                const auto property = properties->find(segment);
                if (property == properties->end())
                {
                    declared = false;
                    return Json();
                }
                node = &(*property);
            }
            return *node;
        }

        // Rewrite every whole-string symbolic reference in `value` in place.
        // `resolve` returns an empty string on success (having written the
        // substitution into its out-parameter), or the reason it could not.
        // Returns the first failure, or an empty string.
        using ReferenceResolver = std::function<std::string(const SymbolReference&, Json&)>;

        [[nodiscard]] std::string SubstituteReferences(Json& value, const ReferenceResolver& resolve)
        {
            if (const auto reference = ParseSymbolReference(value))
            {
                Json substituted;
                std::string error = resolve(*reference, substituted);
                if (!error.empty())
                    return error;
                value = std::move(substituted);
                return {};
            }
            if (value.is_object())
            {
                for (auto& entry : value.items())
                {
                    if (std::string error = SubstituteReferences(entry.value(), resolve); !error.empty())
                        return error;
                }
                return {};
            }
            if (value.is_array())
            {
                for (Json& element : value)
                {
                    if (std::string error = SubstituteReferences(element, resolve); !error.empty())
                        return error;
                }
            }
            return {};
        }

        // Collect the references in `value` without rewriting it, for the
        // dry-run report.
        void CollectReferences(const Json& value, std::vector<SymbolReference>& out)
        {
            if (const auto reference = ParseSymbolReference(value))
            {
                out.push_back(*reference);
                return;
            }
            if (value.is_object())
            {
                for (const auto& entry : value.items())
                    CollectReferences(entry.value(), out);
            }
            else if (value.is_array())
            {
                for (const Json& element : value)
                    CollectReferences(element, out);
            }
        }

        // A host decorator that serves MarshalRead INLINE.
        //
        // A transaction executes inside ONE marshaled job, so every step's
        // handler is already on the game thread when it marshals. Re-marshalling
        // from there would deadlock outright — the game thread cannot drain a
        // queue it is blocked waiting on — and even if it could, letting a frame
        // run between steps is exactly the interleaving that would let an
        // unrelated editor edit land inside the transaction's undo group.
        //
        // Everything else forwards to the real host, so a step still reaches the
        // same editor context, cancellation flag and artifact store it would
        // outside a batch.
        class InlineMarshalHost final : public IAutomationHost
        {
          public:
            explicit InlineMarshalHost(IAutomationHost& inner)
                : m_Inner(inner)
            {
            }

            [[nodiscard]] const MCP::EditorMcpContext& Context() const override
            {
                return m_Inner.Context();
            }

            [[nodiscard]] bool IsCurrentCallCancelled() const override
            {
                return m_Inner.IsCurrentCallCancelled();
            }

            [[nodiscard]] bool PublishArtifact(AutomationArtifact artifact) override
            {
                return m_Inner.PublishArtifact(std::move(artifact));
            }

          protected:
            Json MarshalReadOnMainThread(const std::function<Json()>& readJob,
                                         std::chrono::milliseconds /*timeout*/) override
            {
                return readJob();
            }

            void EmitProgressUpdate(f64 progress, f64 total, const std::string& message) const override
            {
                m_Inner.EmitProgress(progress, total, message);
            }

          private:
            IAutomationHost& m_Inner;
        };

        [[nodiscard]] Json StepEntry(sizet index, const TransactionStep& step, std::string_view status)
        {
            Json entry{ { "index", index }, { "command", step.Command }, { "status", std::string(status) } };
            if (!step.Id.empty())
                entry["id"] = step.Id;
            return entry;
        }
    } // namespace

    // ---- transactability --------------------------------------------------------

    Transactability ClassifyTransactability(const AutomationCommand& command)
    {
        if (!command.Handler)
            return Transactability::NoHandler;
        switch (command.Undo)
        {
            case AutomationUndo::None:
            case AutomationUndo::EditorUndoStack:
                // Reversible, but a step ALSO has to be safe to run inside the
                // batch's single main-thread critical section. A command that
                // declared MainMarshaled=false said the opposite about itself.
                return command.MainMarshaled ? Transactability::Yes : Transactability::NotMainMarshaled;
            case AutomationUndo::Irreversible:
                return Transactability::Irreversible;
            case AutomationUndo::HistoryControl:
                return Transactability::HistoryControl;
            case AutomationUndo::Unspecified:
                break;
        }
        return Transactability::Undeclared;
    }

    std::string_view UndoToken(AutomationUndo undo)
    {
        switch (undo)
        {
            case AutomationUndo::None:
                return "none";
            case AutomationUndo::EditorUndoStack:
                return "editorUndoStack";
            case AutomationUndo::Irreversible:
                return "irreversible";
            case AutomationUndo::HistoryControl:
                return "historyControl";
            case AutomationUndo::Unspecified:
                break;
        }
        return "unspecified";
    }

    std::string_view TransactabilityToken(Transactability verdict)
    {
        switch (verdict)
        {
            case Transactability::Yes:
                return "yes";
            case Transactability::Undeclared:
                return "undeclared";
            case Transactability::Irreversible:
                return "irreversible";
            case Transactability::HistoryControl:
                return "historyControl";
            case Transactability::NotMainMarshaled:
                return "notMainMarshaled";
            case Transactability::NoHandler:
                return "noHandler";
        }
        return "undeclared";
    }

    std::string DescribeRefusal(const std::string& name, Transactability verdict)
    {
        switch (verdict)
        {
            case Transactability::Yes:
                return {};
            case Transactability::Undeclared:
                return "'" + name +
                       "' has not declared how it is taken back, so it cannot be a transaction step. A command whose "
                       "reversal is unknown must not be ASSUMED reversible: the batch would apply and then be unable "
                       "to roll back. Declare AutomationCommand::Undo on it.";
            case Transactability::Irreversible:
                return "'" + name +
                       "' declares its effect irreversible (an import, a bake, a build), so it cannot be a "
                       "transaction step. Run it on its own, before or after the batch.";
            case Transactability::HistoryControl:
                return "'" + name +
                       "' operates on the editor undo history itself, so it cannot be a transaction step -- a "
                       "transaction is already a rewrite of the stack it would be walking.";
            case Transactability::NotMainMarshaled:
                return "'" + name +
                       "' declares that it must not run inside a marshaled main-thread job (it does its own "
                       "marshalling around long off-thread work), so it cannot be a transaction step -- a batch runs "
                       "every step in one main-thread critical section and this one would freeze the editor for the "
                       "duration. Run it on its own, before or after the batch.";
            case Transactability::NoHandler:
                return "'" + name + "' is registered with no handler.";
        }
        return "'" + name + "' cannot be a transaction step.";
    }

    // ---- symbolic references ----------------------------------------------------

    std::string SymbolReference::Text() const
    {
        std::string text = "${" + StepId;
        for (const std::string& segment : Path)
        {
            text += '.';
            text += segment;
        }
        text += '}';
        return text;
    }

    std::optional<SymbolReference> ParseSymbolReference(const Json& value)
    {
        if (!value.is_string())
            return std::nullopt;
        const auto text = value.get<std::string>();
        // WHOLE string only. A fragment like "cost: ${5}" stays a literal, which
        // is what removes the escaping problem entirely: there is no convention
        // to learn and no value that has to be quoted to survive.
        if (text.size() < 4 || text.front() != '$' || text[1] != '{' || text.back() != '}')
            return std::nullopt;

        const std::string body = text.substr(2, text.size() - 3);
        SymbolReference reference;
        std::string segment;
        for (const char c : body)
        {
            if (c == '.')
            {
                if (segment.empty())
                    return std::nullopt;
                if (reference.StepId.empty())
                    reference.StepId = std::move(segment);
                else
                    reference.Path.push_back(std::move(segment));
                segment.clear();
                continue;
            }
            if (!IsIdentifierChar(c))
                return std::nullopt;
            segment.push_back(c);
        }
        if (segment.empty())
            return std::nullopt;
        if (reference.StepId.empty())
            reference.StepId = std::move(segment);
        else
            reference.Path.push_back(std::move(segment));
        return reference;
    }

    // ---- building ---------------------------------------------------------------

    TransactionBuild BuildTransaction(const AutomationRegistry::CommandSnapshot& commands, const Json& arguments,
                                      AutomationWriteConsent consent)
    {
        TransactionBuild build;
        const auto refuse = [&build](std::string error, i64 step) -> TransactionBuild&
        {
            build.Ok = false;
            build.Error = std::move(error);
            build.FailedStep = step;
            return build;
        };

        if (!arguments.is_object())
            return refuse("Transaction arguments must be an object.", -1);

        const auto stepsIt = arguments.find("steps");
        if (stepsIt == arguments.end() || !stepsIt->is_array())
            return refuse("A transaction requires a 'steps' array.", -1);
        if (stepsIt->empty())
            return refuse("A transaction requires at least one step.", -1);
        if (stepsIt->size() > kMaxTransactionSteps)
        {
            return refuse("A transaction is limited to " + std::to_string(kMaxTransactionSteps) + " steps; this one has " +
                              std::to_string(stepsIt->size()) + ".",
                          -1);
        }

        // Read defensively rather than with value(): through the registry the
        // declared InputSchema has already refused a wrong type, but the header
        // documents this function as directly callable, and a type_error thrown
        // out of the builder would not be the refusal REPORT it promises.
        const auto dryRunIt = arguments.find("dryRun");
        if (dryRunIt != arguments.end() && !dryRunIt->is_null())
        {
            if (!dryRunIt->is_boolean())
                return refuse("'dryRun' must be a boolean.", -1);
            build.Plan.DryRun = dryRunIt->get<bool>();
        }
        if (const auto descriptionIt = arguments.find("description");
            descriptionIt != arguments.end() && !descriptionIt->is_null())
        {
            if (!descriptionIt->is_string())
                return refuse("'description' must be a string.", -1);
            build.Plan.Description = descriptionIt->get<std::string>();
        }
        if (build.Plan.Description.empty())
            build.Plan.Description = "Automation transaction (" + std::to_string(stepsIt->size()) + " steps)";

        // Which step declares each id, so a reference can be resolved to an
        // EARLIER step and nothing else. Insertion happens after a step is
        // admitted, which is what makes a self- or forward-reference impossible
        // rather than merely discouraged.
        std::map<std::string, sizet> declaredIds;

        for (sizet index = 0; index < stepsIt->size(); ++index)
        {
            const Json& raw = (*stepsIt)[index];
            const auto stepIndex = static_cast<i64>(index);
            if (!raw.is_object())
                return refuse("Step " + std::to_string(index) + " is not an object.", stepIndex);

            TransactionStep step;
            const auto commandIt = raw.find("command");
            if (commandIt == raw.end() || !commandIt->is_string() || commandIt->get<std::string>().empty())
                return refuse("Step " + std::to_string(index) + " requires a nonempty 'command' name.", stepIndex);
            step.Command = commandIt->get<std::string>();

            if (const auto argsIt = raw.find("arguments"); argsIt != raw.end() && !argsIt->is_null())
            {
                if (!argsIt->is_object())
                    return refuse("Step " + std::to_string(index) + " ('" + step.Command + "'): 'arguments' must be an object.", stepIndex);
                step.Arguments = *argsIt;
            }

            if (const auto idIt = raw.find("id"); idIt != raw.end() && !idIt->is_null())
            {
                if (!idIt->is_string())
                    return refuse("Step " + std::to_string(index) + " ('" + step.Command + "'): 'id' must be a string.", stepIndex);
                step.Id = idIt->get<std::string>();
                if (step.Id.empty() || !std::all_of(step.Id.begin(), step.Id.end(), IsIdentifierChar))
                {
                    return refuse("Step " + std::to_string(index) + " ('" + step.Command +
                                      "'): 'id' must be 1 or more characters of [A-Za-z0-9_-].",
                                  stepIndex);
                }
                if (declaredIds.contains(step.Id))
                {
                    return refuse("Step " + std::to_string(index) + " reuses the id '" + step.Id + "', already declared by step " +
                                      std::to_string(declaredIds.at(step.Id)) + ". A reference must name exactly one step.",
                                  stepIndex);
                }
            }

            const AutomationCommand* command = AutomationRegistry::Find(*commands, step.Command);
            if (command == nullptr)
                return refuse("Step " + std::to_string(index) + ": unknown command '" + step.Command + "'.", stepIndex);

            if (const Transactability verdict = ClassifyTransactability(*command); verdict != Transactability::Yes)
                return refuse("Step " + std::to_string(index) + ": " + DescribeRefusal(step.Command, verdict), stepIndex);

            // BATCH AUTHORITY (ADR 0005). The batch's authority is the maximum
            // its members require and is decided once, here, against the consent
            // the CALLER holds for the batch. A caller without it cannot put a
            // write into the batch at all -- which is the laundering this refuses.
            if (command->ProjectWrite)
            {
                if (consent != AutomationWriteConsent::Granted)
                {
                    return refuse("Step " + std::to_string(index) + " ('" + step.Command +
                                      "') mutates the project, and this transaction was built without write consent. "
                                      "A batch's authority is decided once, for the whole batch, at the highest "
                                      "authority any member requires -- it is never inherited from the batch itself.",
                                  stepIndex);
                }
                build.Plan.RequiresWriteConsent = true;
            }
            if (command->Undo == AutomationUndo::EditorUndoStack)
                build.Plan.RequiresCommandHistory = true;

            // Resolve every symbolic reference to its DECLARING step and check
            // the wiring types, then validate the step's arguments with a
            // placeholder of each reference's declared output type. The values
            // do not exist yet; the types do.
            Json probe = step.Arguments;
            std::string wiringError = SubstituteReferences(
                probe,
                [&](const SymbolReference& reference, Json& substituted) -> std::string
                {
                    const auto declaring = declaredIds.find(reference.StepId);
                    if (declaring == declaredIds.end())
                    {
                        return "Step " + std::to_string(index) + " ('" + step.Command + "') references " +
                               reference.Text() + ", but no EARLIER step declares the id '" + reference.StepId +
                               "'. A reference may only read a step that has already run.";
                    }
                    const TransactionStep& source = build.Plan.Steps[declaring->second];
                    const AutomationCommand* sourceCommand = AutomationRegistry::Find(*commands, source.Command);
                    bool declared = true;
                    bool describable = false;
                    const Json field =
                        sourceCommand != nullptr
                            ? ResolveDeclaredOutput(sourceCommand->OutputSchema, reference.Path, declared, describable)
                            : Json();
                    if (!declared)
                    {
                        return "Step " + std::to_string(index) + " ('" + step.Command + "') references " +
                               reference.Text() + ", but step " + std::to_string(declaring->second) + " ('" +
                               source.Command + "') declares no such output field.";
                    }
                    substituted = describable && field.is_object()
                                      ? PlaceholderForType(field.value("type", std::string("string")))
                                      : Json("0");
                    return {};
                });
            if (!wiringError.empty())
                return refuse(std::move(wiringError), stepIndex);

            if (const auto error = AutomationSchema::ValidateArguments(command->InputSchema, probe))
            {
                std::string message = "Step " + std::to_string(index) + " ('" + step.Command + "'): " + *error;
                std::vector<SymbolReference> references;
                CollectReferences(step.Arguments, references);
                if (!references.empty())
                {
                    message += " (a symbolic reference was checked against the type its source step declares, not its "
                               "eventual value)";
                }
                return refuse(std::move(message), stepIndex);
            }

            if (!step.Id.empty())
                declaredIds.emplace(step.Id, index);
            build.Plan.Steps.push_back(std::move(step));
        }

        build.Ok = true;
        return build;
    }

    Json DescribeInadmissibleBatch(const TransactionBuild& build, const Json& arguments)
    {
        Json steps = Json::array();
        const auto stepsIt = arguments.is_object() ? arguments.find("steps") : arguments.end();
        if (stepsIt != arguments.end() && stepsIt->is_array())
        {
            for (sizet index = 0; index < stepsIt->size(); ++index)
            {
                const Json& raw = (*stepsIt)[index];
                const auto commandIt = raw.is_object() ? raw.find("command") : raw.end();
                const std::string name =
                    commandIt != raw.end() && commandIt->is_string() ? commandIt->get<std::string>() : std::string{};
                Json entry{ { "index", index }, { "command", name }, { "status", "skipped" } };
                if (raw.is_object() && raw.contains("id") && raw["id"].is_string())
                    entry["id"] = raw["id"];
                if (build.FailedStep >= 0 && static_cast<sizet>(build.FailedStep) == index)
                    entry["error"] = build.Error;
                steps.push_back(std::move(entry));
            }
        }

        const auto dryRunIt = arguments.is_object() ? arguments.find("dryRun") : arguments.end();
        Json report{ { kAppliedKey, false },
                     { kAdmittedKey, false },
                     { "dryRun", dryRunIt != arguments.end() && dryRunIt->is_boolean() && dryRunIt->get<bool>() },
                     { "stepCount", steps.size() },
                     { "appliedSteps", 0 },
                     { "undoEntries", 0 },
                     { "undoable", false },
                     { "rolledBack", false },
                     { "error", build.Error },
                     { "steps", std::move(steps) } };
        if (build.FailedStep >= 0)
            report["failedStep"] = build.FailedStep;
        return report;
    }

    // ---- dry run ----------------------------------------------------------------

    Json DescribeDryRun(const AutomationRegistry::CommandSnapshot& commands, const TransactionPlan& plan)
    {
        Json steps = Json::array();
        for (sizet index = 0; index < plan.Steps.size(); ++index)
        {
            const TransactionStep& step = plan.Steps[index];
            Json entry = StepEntry(index, step, "validated");
            if (const AutomationCommand* command = AutomationRegistry::Find(*commands, step.Command))
            {
                entry["projectWrite"] = command->ProjectWrite;
                entry["undo"] = std::string(UndoToken(command->Undo));
                entry["transactable"] = std::string(TransactabilityToken(ClassifyTransactability(*command)));
            }
            std::vector<SymbolReference> references;
            CollectReferences(step.Arguments, references);
            if (!references.empty())
            {
                Json resolved = Json::array();
                for (const SymbolReference& reference : references)
                    resolved.push_back(reference.Text());
                entry["references"] = std::move(resolved);
            }
            steps.push_back(std::move(entry));
        }

        return Json{ { kAppliedKey, false },
                     { kAdmittedKey, true },
                     { "dryRun", true },
                     { "stepCount", plan.Steps.size() },
                     { "appliedSteps", 0 },
                     { "description", plan.Description },
                     { "requiresWriteConsent", plan.RequiresWriteConsent },
                     { "undoEntries", 0 },
                     { "undoable", false },
                     { "rolledBack", false },
                     { "steps", std::move(steps) } };
    }

    // ---- execution --------------------------------------------------------------

    Json ExecuteTransactionOnMainThread(const AutomationRegistry& registry,
                                        const AutomationRegistry::CommandSnapshot& commands, IAutomationHost& host,
                                        const TransactionPlan& plan, AutomationWriteConsent consent)
    {
        Json report{ { kAppliedKey, false },
                     { kAdmittedKey, true },
                     { "dryRun", false },
                     { "stepCount", plan.Steps.size() },
                     { "appliedSteps", 0 },
                     { "description", plan.Description },
                     { "requiresWriteConsent", plan.RequiresWriteConsent },
                     { "undoEntries", 0 },
                     { "undoable", false },
                     { "rolledBack", false } };

        const auto& context = host.Context();
        CommandHistory* history = context.GetCommandHistory ? context.GetCommandHistory() : nullptr;
        if (plan.RequiresCommandHistory && history == nullptr)
        {
            report["error"] = "This transaction mutates the scene, which requires Edit mode and an editor command "
                              "history to roll back into. Stop Play or Simulate first.";
            report["steps"] = Json::array();
            return report;
        }
        // A batch of read-only steps needs no group and must not open one.
        if (!plan.RequiresCommandHistory)
            history = nullptr;

        InlineMarshalHost inlineHost(host);
        std::map<std::string, Json> outputs;
        std::vector<Json> entries;
        entries.reserve(plan.Steps.size());

        sizet applied = 0;
        i64 failedStep = -1;
        std::string failure;

        if (history != nullptr)
            history->BeginTransaction(plan.Description);

        // A transaction scope left open WEDGES the editor's history: every later
        // Undo, Redo and Clear refuses while one is open. The loop below cannot
        // throw past its own catch, but the report building after it can (a bad
        // allocation on a large batch), so the close is tied to the scope's
        // lifetime rather than to reaching the right line.
        struct HistoryScopeGuard
        {
            CommandHistory* History = nullptr;
            ~HistoryScopeGuard()
            {
                // Swallowing here is not a silent fallback: this runs only on the
                // path where the report is already being abandoned to an escaping
                // exception, and a destructor that throws during that unwind
                // terminates the editor outright. Closing the scope is the more
                // important of the two jobs -- an open one wedges every later
                // Undo -- so it is the one that gets to fail quietly.
                try
                {
                    if (History != nullptr && History->InTransaction())
                        (void)History->RollbackTransaction();
                }
                catch (...)
                {
                }
            }
        } scopeGuard{ history };

        try
        {
            for (sizet index = 0; index < plan.Steps.size() && failedStep < 0; ++index)
            {
                const TransactionStep& step = plan.Steps[index];
                Json arguments = step.Arguments;
                std::string resolveError = SubstituteReferences(
                    arguments,
                    [&](const SymbolReference& reference, Json& substituted) -> std::string
                    {
                        const auto source = outputs.find(reference.StepId);
                        if (source == outputs.end())
                        {
                            return "step '" + reference.StepId + "' produced no structured result, so " +
                                   reference.Text() + " cannot be resolved";
                        }
                        const Json* node = &source->second;
                        for (const std::string& segment : reference.Path)
                        {
                            if (!node->is_object() || !node->contains(segment))
                                return reference.Text() + " is not present in step '" + reference.StepId + "'s result";
                            node = &(*node)[segment];
                        }
                        substituted = *node;
                        return {};
                    });

                if (!resolveError.empty())
                {
                    failedStep = static_cast<i64>(index);
                    failure = "Step " + std::to_string(index) + " ('" + step.Command + "'): " + resolveError + ".";
                    entries.push_back(StepEntry(index, step, "failed"));
                    entries.back()["error"] = failure;
                    break;
                }

                // Through Invoke, never around it: the REAL arguments are
                // validated against the step's InputSchema here, its availability
                // predicate is honoured, and its own consent gate fires. The
                // build-time check ran against a typed placeholder, so it is an
                // early refusal, not a substitute for this one.
                const AutomationInvocation invocation =
                    registry.Invoke(commands, inlineHost, step.Command, arguments, consent);
                if (!invocation.Ran())
                {
                    failedStep = static_cast<i64>(index);
                    failure = "Step " + std::to_string(index) + " ('" + step.Command + "'): " + invocation.Message;
                    entries.push_back(StepEntry(index, step, "failed"));
                    entries.back()["error"] = failure;
                    break;
                }
                if (invocation.Result.IsError)
                {
                    failedStep = static_cast<i64>(index);
                    failure = "Step " + std::to_string(index) + " ('" + step.Command +
                              "'): " + ResultMessage(invocation.Result);
                    entries.push_back(StepEntry(index, step, "failed"));
                    entries.back()["error"] = failure;
                    break;
                }

                Json entry = StepEntry(index, step, "applied");
                if (!invocation.Result.StructuredContent.is_null())
                    entry["result"] = invocation.Result.StructuredContent;
                entries.push_back(std::move(entry));
                if (!step.Id.empty())
                    outputs[step.Id] = invocation.Result.StructuredContent;
                ++applied;
            }
        }
        catch (const std::exception& e)
        {
            failedStep = static_cast<i64>(entries.size());
            failure = std::string("The transaction aborted: ") + e.what();
        }
        catch (...)
        {
            failedStep = static_cast<i64>(entries.size());
            failure = "The transaction aborted on a non-standard exception.";
        }

        // Every step past the failure never ran. Say so explicitly rather than
        // leaving them out: a caller counting entries must be able to see that
        // the batch stopped, not guess it from a short array.
        for (sizet index = entries.size(); index < plan.Steps.size(); ++index)
            entries.push_back(StepEntry(index, plan.Steps[index], "skipped"));

        if (failedStep >= 0)
        {
            std::string rollbackFailure;
            if (history != nullptr)
                rollbackFailure = history->RollbackTransaction();

            const bool rolledBack = rollbackFailure.empty();

            // The earlier steps' status must describe the state they are in NOW.
            // After a clean rollback that is "taken back". After an INCOMPLETE
            // one it is genuinely unknown -- an undo that refused leaves its
            // member applied, and one step can push zero or several members, so
            // nothing here can say which steps survived. Claiming "rolledBack"
            // there would be the silent half-applied scene this layer exists to
            // prevent, dressed up as a clean failure.
            for (sizet index = 0; index < static_cast<sizet>(failedStep) && index < entries.size(); ++index)
                entries[index]["status"] = rolledBack ? "rolledBack" : "rollbackFailed";

            // Likewise appliedSteps: zero only when the rollback is CONFIRMED.
            // Otherwise report how many had been applied, as the upper bound on
            // what may still be.
            report["appliedSteps"] = rolledBack ? 0 : applied;
            report["failedStep"] = failedStep;
            report["error"] = failure;
            report["rolledBack"] = rolledBack;
            if (!rolledBack)
            {
                // Loud, and countable.
                report["rollbackError"] = rollbackFailure;
                report["error"] = failure + " THE ROLLBACK WAS INCOMPLETE: " + rollbackFailure +
                                  ". The scene is partially modified; inspect it before continuing.";
            }
            report["steps"] = std::move(entries);
            return report;
        }

        const sizet undoEntries = history != nullptr ? history->TransactionSize() : 0;
        if (history != nullptr)
            history->CommitTransaction();

        report[kAppliedKey] = true;
        report["appliedSteps"] = applied;
        report["undoEntries"] = undoEntries;
        // ONE undo step, not N: the whole batch committed as a single
        // CompoundCommand, so Ctrl-Z takes the transaction back as a unit.
        report["undoable"] = undoEntries > 0;
        report["steps"] = std::move(entries);
        return report;
    }

    // ---- schemas ----------------------------------------------------------------

    Json TransactionInputSchema()
    {
        return Schema::Object()
            .Prop("steps",
                  Schema::Array(Schema::Object()
                                    .Prop("command", Schema::String().Desc("Registered command name to run as this step."))
                                    .Prop("arguments", Schema::Object().Desc("The command's own arguments. Any string of the exact form ${id} or ${id.field} is replaced by that earlier step's structured result."))
                                    .Prop("id", Schema::String().Desc("Label later steps reference this step's output by, 1+ chars of [A-Za-z0-9_-]. Unique within the batch."))
                                    .Required({ "command" })
                                    .NoAdditional())
                      .MinItems(1)
                      .MaxItems(static_cast<std::int64_t>(kMaxTransactionSteps))
                      .Desc("The steps, applied in order. All of them apply or none do."))
            .Prop("dryRun", Schema::Bool().Desc("Validate the whole batch and apply nothing, reporting what would happen. Default false."))
            .Prop("description", Schema::String().Desc("Label for the single undo entry the batch commits as."))
            .Required({ "steps" })
            .NoAdditional();
    }

    Json TransactionOutputSchema()
    {
        return Schema::Object()
            .Prop("applied", Schema::Bool().Desc("Whether the batch was applied. False for a dry run, a refusal and a rollback alike -- this is the success flag."))
            .Prop("admitted", Schema::Bool().Desc("Whether the batch was admissible at build time. False means nothing ran at all."))
            .Prop("dryRun", Schema::Bool())
            .Prop("stepCount", Schema::Int().Min(0))
            .Prop("appliedSteps", Schema::Int().Min(0).Desc("How many steps are applied NOW; zero after a rollback."))
            .Prop("description", Schema::String())
            .Prop("requiresWriteConsent", Schema::Bool().Desc("Whether any step mutates the project, i.e. the batch's authority."))
            .Prop("undoEntries", Schema::Int().Min(0).Desc("Undo operations grouped into the batch's single history entry."))
            .Prop("undoable", Schema::Bool().Desc("Whether one Ctrl-Z takes the whole batch back."))
            .Prop("rolledBack", Schema::Bool().Desc("Whether a failed batch was fully taken back. False with rollbackError set means it was not."))
            .Prop("rollbackError", Schema::String().Desc("Present only when the rollback itself was incomplete."))
            .Prop("failedStep", Schema::Int().Min(0))
            .Prop("error", Schema::String())
            .Prop("steps", Schema::Array(Schema::Object()
                                             .Prop("index", Schema::Int().Min(0))
                                             .Prop("command", Schema::String())
                                             .Prop("id", Schema::String())
                                             .Prop("status", Schema::String().Enum({ "validated", "applied", "failed", "skipped", "rolledBack", "rollbackFailed" }))
                                             .Prop("result", Schema::Object())
                                             .Prop("error", Schema::String())
                                             .Prop("projectWrite", Schema::Bool())
                                             .Prop("undo", Schema::String())
                                             .Prop("transactable", Schema::String())
                                             .Prop("references", Schema::Array(Schema::String()))
                                             .Required({ "index", "command", "status" })))
            .Required({ "applied", "admitted", "dryRun", "stepCount", "steps" });
    }
} // namespace OloEngine::Automation
