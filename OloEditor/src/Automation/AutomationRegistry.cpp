#include "OloEnginePCH.h"
#include "Automation/AutomationRegistry.h"

#include "Automation/AutomationSchemaValidation.h"

#include "OloEngine/Core/Assert.h"
#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace OloEngine::Automation
{
    using Json = nlohmann::json;

    bool AutomationRegistry::IsValidName(std::string_view name)
    {
        // MCP places no hard length cap on tool names, but the broader spec uses
        // 1..128 for identifier-like fields and clients/UIs assume a bounded,
        // shell-safe character set. Enforce 1..128 chars of [A-Za-z0-9_.-].
        if (name.empty() || name.size() > 128)
            return false;
        return std::all_of(name.begin(), name.end(),
                           [](char c)
                           {
                               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                      c == '_' || c == '.' || c == '-';
                           });
    }

    bool AutomationRegistry::IsValidClientAlias(std::string_view alias)
    {
        if (alias.empty() || alias.size() > 32)
            return false;
        for (std::size_t i = 0; i < alias.size(); ++i)
        {
            const char c = alias[i];
            const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            if (!(alnum || (c == '-' && i > 0)))
                return false;
        }
        return true;
    }

    std::string AutomationRegistry::ClientPrefix(const std::string& alias)
    {
        return "ext." + alias + ".";
    }

    void AutomationRegistry::Publish(std::shared_ptr<CommandList> next)
    {
        m_Commands.store(std::shared_ptr<const CommandList>(std::move(next)), std::memory_order_release);
    }

    void AutomationRegistry::Register(AutomationCommand command)
    {
        // Fail loudly at registration: every NATIVE command is registered in code at
        // startup, so a malformed name / icons value is a programmer error, not a
        // runtime condition. Catch it here instead of letting clients choke later.
        // (Script commands never reach this path with a malformed value — the Lua
        // registration validates and REJECTS with a logged message, because scripts
        // are runtime data; see McpScriptTools.cpp.)
        OLO_CORE_VERIFY(IsValidName(command.Name),
                        "[Automation] Invalid command name '{}': must be 1-128 chars of [A-Za-z0-9_.-].",
                        command.Name);
        OLO_CORE_VERIFY(
            IsValidIcons(command.Icons),
            "[Automation] Invalid icons for command '{}': expected a non-empty array of {{ src, mimeType?, sizes? }}.",
            command.Name);

        // Copy-on-write publish (see the CommandSnapshot contract): build the new
        // vector, then swap it in atomically. Registration is a startup-time,
        // ~100-element operation, so the copy is irrelevant; the payoff is that the
        // dispatch read path needs no lock at all.
        std::lock_guard writeLock(m_WriteMutex);
        auto next = std::make_shared<CommandList>(*Snapshot());
        next->push_back(std::move(command));
        Publish(std::move(next));
    }

    void AutomationRegistry::ReplaceScriptCommands(std::vector<AutomationCommand> scriptCommands)
    {
        {
            std::lock_guard writeLock(m_WriteMutex);
            const CommandSnapshot current = Snapshot();

            auto next = std::make_shared<CommandList>();
            next->reserve(current->size() + scriptCommands.size());
            for (const AutomationCommand& command : *current)
            {
                if (!command.ScriptOwned)
                    next->push_back(command);
            }
            for (AutomationCommand& command : scriptCommands)
                next->push_back(std::move(command));

            // The swap is the linearization point. A dispatch thread that already
            // took a snapshot keeps running against the OLD vector — including the
            // commands whose handlers own the previous Lua state, which therefore
            // stays alive exactly as long as some call still needs it.
            Publish(std::move(next));
        }

        NotifyChanged();
    }

    void AutomationRegistry::UnregisterScriptCommands()
    {
        ReplaceScriptCommands({});
    }

    sizet AutomationRegistry::ReplaceClientCommands(const std::string& alias,
                                                    std::vector<AutomationCommand> clientCommands)
    {
        // Foreign definitions are runtime network data — validate-and-reject,
        // never OLO_CORE_VERIFY (ADR 0005: asserts must not police data another
        // process controls). An invalid alias rejects the whole batch: without a
        // valid reserved prefix nothing below can be namespaced safely.
        if (!IsValidClientAlias(alias))
        {
            OLO_CORE_WARN("[Automation] ReplaceClientCommands: invalid client alias '{}' — batch rejected.", alias);
            return 0;
        }

        const std::string prefix = ClientPrefix(alias);
        std::vector<AutomationCommand> accepted;
        accepted.reserve(clientCommands.size());
        for (AutomationCommand& command : clientCommands)
        {
            if (!IsValidName(command.Name))
            {
                OLO_CORE_WARN("[Automation] ReplaceClientCommands('{}'): dropping command with invalid name '{}'.",
                              alias, command.Name);
                continue;
            }
            if (command.Name.size() <= prefix.size() || command.Name.compare(0, prefix.size(), prefix) != 0)
            {
                OLO_CORE_WARN("[Automation] ReplaceClientCommands('{}'): dropping command '{}' — bridged names must "
                              "live in the reserved '{}' namespace.",
                              alias, command.Name, prefix);
                continue;
            }
            if (std::any_of(accepted.begin(), accepted.end(),
                            [&command](const AutomationCommand& existing)
                            { return existing.Name == command.Name; }))
            {
                OLO_CORE_WARN("[Automation] ReplaceClientCommands('{}'): dropping duplicate command '{}'.", alias,
                              command.Name);
                continue;
            }
            if (!command.Handler)
            {
                OLO_CORE_WARN("[Automation] ReplaceClientCommands('{}'): dropping command '{}' with no bridge "
                              "handler.",
                              alias, command.Name);
                continue;
            }
            if (!IsValidIcons(command.Icons))
            {
                OLO_CORE_WARN("[Automation] ReplaceClientCommands('{}'): dropping malformed icons on '{}'.", alias,
                              command.Name);
                command.Icons = Json();
            }

            // FORCE the authority posture — never trust the child's claims
            // (ADR 0005: "a tool's write authority is its OWN declared tier...
            // never inherited, never ambient, and never laundered"). A foreign
            // command always faces the full write-consent gate, is annotated as an
            // open-world mutation (the first genuinely open-world entries in the
            // surface — everything native acts on the local editor session), and
            // keeps the spec's conservative destructiveHint default of true.
            command.ProjectWrite = true;
            command.ScriptOwned = false;
            command.ClientAlias = alias;
            command.Toolset = "ext." + alias;
            command.Annotations = Json{ { "readOnlyHint", false }, { "openWorldHint", true } };
            accepted.push_back(std::move(command));
        }

        {
            std::lock_guard writeLock(m_WriteMutex);
            const CommandSnapshot current = Snapshot();

            auto next = std::make_shared<CommandList>();
            next->reserve(current->size() + accepted.size());
            for (const AutomationCommand& command : *current)
            {
                if (command.ClientAlias != alias)
                    next->push_back(command);
            }
            for (AutomationCommand& command : accepted)
                next->push_back(std::move(command));

            // Same linearization contract as ReplaceScriptCommands: an in-flight
            // call keeps executing against the snapshot (and any per-connection
            // runtime its handler strongly captures) it started with.
            Publish(std::move(next));
        }

        NotifyChanged();
        return accepted.size();
    }

    void AutomationRegistry::NotifyChanged()
    {
        m_Generation.fetch_add(1, std::memory_order_acq_rel);
        if (m_OnChanged)
            m_OnChanged();
    }

    const AutomationCommand* AutomationRegistry::Find(const CommandList& commands, const std::string& name)
    {
        for (const auto& command : commands)
        {
            if (command.Name == name)
                return &command;
        }
        return nullptr;
    }

    AutomationResult AutomationRegistry::RunHandler(const AutomationCommand& command, IAutomationHost& host,
                                                    const Json& arguments)
    {
        try
        {
            return command.Handler(host, arguments);
        }
        catch (const std::exception& e)
        {
            return AutomationResult::Error(std::string("Tool failed: ") + e.what());
        }
    }

    AutomationInvocation AutomationRegistry::Invoke(IAutomationHost& host, const std::string& name,
                                                    const Json& arguments) const
    {
        // Pin the snapshot for the whole invocation, exactly as the MCP adapter does:
        // a concurrent script reload may swap the registry while the handler runs, and
        // the command (and any Lua state it closes over) must outlive the call.
        const CommandSnapshot snapshot = Snapshot();

        AutomationInvocation outcome;
        const AutomationCommand* command = Find(*snapshot, name);
        if (command == nullptr)
        {
            outcome.Outcome = AutomationInvocation::Status::UnknownCommand;
            outcome.Message = "Unknown tool: " + name;
            return outcome;
        }

        if (!command->AvailableOn(host))
        {
            outcome.Outcome = AutomationInvocation::Status::Unavailable;
            outcome.Message = "Tool '" + name + "' is not available on this host.";
            return outcome;
        }

        if (!command->Handler)
        {
            outcome.Outcome = AutomationInvocation::Status::NoHandler;
            outcome.Message = "Tool '" + name + "' has no handler.";
            return outcome;
        }

        // `arguments` is optional, but when present it MUST be an object. A
        // present-but-non-object payload is malformed: coercing it to {} would
        // validate against an empty object and hide the mismatch.
        const Json args = arguments.is_null() ? Json::object() : arguments;
        if (!args.is_object())
        {
            outcome.Outcome = AutomationInvocation::Status::InvalidArguments;
            outcome.Message = "Invalid arguments for tool '" + name + "': arguments must be an object";
            return outcome;
        }
        if (const auto error = AutomationSchema::ValidateArguments(command->InputSchema, args))
        {
            outcome.Outcome = AutomationInvocation::Status::InvalidArguments;
            outcome.Message = "Invalid arguments for tool '" + name + "': " + *error;
            return outcome;
        }

        outcome.Outcome = AutomationInvocation::Status::Ok;
        outcome.Result = RunHandler(*command, host, args);
        return outcome;
    }

    bool AutomationRegistry::IsValidIcons(const Json& icons)
    {
        if (icons.is_null())
            return true; // absent — the common case, and legal.
        if (!icons.is_array() || icons.empty())
            return false;
        for (const auto& icon : icons)
        {
            if (!icon.is_object())
                return false;
            const auto src = icon.find("src");
            if (src == icon.end() || !src->is_string() || src->get<std::string>().empty())
                return false;
            if (const auto mime = icon.find("mimeType"); mime != icon.end() && !mime->is_string())
                return false;
            if (const auto sizes = icon.find("sizes"); sizes != icon.end())
            {
                // SEP-973 models `sizes` as an array of "WxH" (or "any") strings.
                if (!sizes->is_array())
                    return false;
                for (const auto& size : *sizes)
                {
                    if (!size.is_string())
                        return false;
                }
            }
        }
        return true;
    }
} // namespace OloEngine::Automation
