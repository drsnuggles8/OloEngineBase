#pragma once

// The registry that owns the automation commands (issue #1123).
//
// This is `McpServer::m_Tools` lifted out of the server, semantics unchanged:
// a copy-on-write vector, atomically published, read lock-free by dispatch
// threads and replaceable wholesale while calls are in flight. Script-tool live
// reload (#607) depends on exactly that, so the contract moved verbatim.
//
// What changed is ownership. The server used to BE the registry; now it holds
// one and adapts it to a transport. A caller with no server — the CLI in #1125,
// a headless harness, a test — constructs a registry, registers the same
// commands, and invokes them through Invoke().

#include "Automation/AutomationCommand.h"
#include "Automation/AutomationHost.h"
#include "Automation/AutomationResult.h"

#include "OloEngine/Core/Base.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Automation
{
    // Whether the CALLER has obtained the user's consent for a project-mutating
    // command on this invocation.
    //
    // A ProjectWrite command mutates the user's project, and the decision to allow
    // that belongs to whatever is facing the human — under MCP, the session
    // write-consent mode and its modal. The registry has no consent model of its
    // own and must not invent one (#1123 leaves the consent model exactly as it
    // was), but it must not be a way AROUND the one that exists either. So Invoke
    // default-denies: a caller that has not thought about consent gets a clean
    // refusal instead of a silent mutation, and one that has says so at the call
    // site, where a reader can see it.
    enum class AutomationWriteConsent : u8
    {
        Withheld = 0, // a ProjectWrite command is refused. The default.
        Granted,      // the caller has already obtained consent for THIS call.
    };

    // Outcome of running one command through the registry. The MCP adapter does
    // NOT go through this — it needs to distinguish a protocol error from a
    // command error, and it interleaves the consent gate and the cancellation
    // scope — but it runs the handler through the same RunHandler(), so the two
    // paths cannot disagree about what invoking a command means.
    struct AutomationInvocation
    {
        enum class Status : u8
        {
            Ok = 0,               // the handler ran; Result is what it returned (which may itself be an error).
            UnknownCommand,       // no command of that name is registered.
            Unavailable,          // registered, but its availability predicate said no on this host.
            NoHandler,            // registered with a null handler — a registration bug, never a runtime condition.
            InvalidArguments,     // `arguments` did not satisfy the declared InputSchema.
            WriteConsentWithheld, // a ProjectWrite command, and the caller did not assert consent.
        };

        Status Outcome = Status::UnknownCommand;
        // Only meaningful when Outcome == Ok. For every other status the caller
        // shapes its own error from `Message`, because how a refusal is reported is
        // the transport's business.
        AutomationResult Result;
        // Why, for every non-Ok status. Empty when Outcome == Ok.
        std::string Message;

        [[nodiscard]] bool Ran() const
        {
            return Outcome == Status::Ok;
        }
    };

    class AutomationRegistry
    {
      public:
        // ---- the copy-on-write, atomically swapped snapshot --------------------
        //
        // Dispatch worker threads read the command vector LOCK-FREE on the hot path,
        // and script-command live reload (issue #607) must be able to REPLACE that
        // vector while the server is serving. Both hold because the vector is
        // immutable once published: every writer (Register / ReplaceScriptCommands,
        // serialized by m_WriteMutex) builds a fresh vector and atomically stores it;
        // every reader takes a `shared_ptr` snapshot and holds it for as long as it
        // needs the commands (a whole tools/call, in the MCP adapter) — which also
        // keeps a *replaced* script command's Lua runtime alive until the call using
        // it finishes. There is NO lock on the read path.
        //
        // Never hand out a reference into the snapshot's vector: a concurrent swap
        // would leave it dangling the moment the temporary shared_ptr dies. Take a
        // CommandSnapshot and keep it in scope instead.
        using CommandList = std::vector<AutomationCommand>;
        using CommandSnapshot = std::shared_ptr<const CommandList>;

        AutomationRegistry() = default;
        ~AutomationRegistry() = default;

        AutomationRegistry(const AutomationRegistry&) = delete;
        AutomationRegistry& operator=(const AutomationRegistry&) = delete;
        AutomationRegistry(AutomationRegistry&&) = delete;
        AutomationRegistry& operator=(AutomationRegistry&&) = delete;

        [[nodiscard]] CommandSnapshot Snapshot() const
        {
            return m_Commands.load(std::memory_order_acquire);
        }
        [[nodiscard]] sizet Count() const
        {
            return Snapshot()->size();
        }
        // Monotonic counter bumped on every command-list swap (the two Replace
        // paths, never plain Register). A transport polls it to announce that its
        // catalogue changed.
        [[nodiscard]] u64 Generation() const
        {
            return m_Generation.load(std::memory_order_acquire);
        }

        // Append one command. Startup-time and hand-declared, so a malformed name or
        // icons value is a programmer error and fails loudly here rather than
        // confusing a client later.
        void Register(AutomationCommand command);

        // Swap every ScriptOwned command for `scriptCommands`, keeping the natives
        // (and any bridged commands) in their existing order ahead of them. Fires the
        // change listener.
        void ReplaceScriptCommands(std::vector<AutomationCommand> scriptCommands);

        // Drop every ScriptOwned command. Thin wrapper over ReplaceScriptCommands({}).
        void UnregisterScriptCommands();

        // Swap every command bridged from `alias` for `clientCommands`. Foreign
        // definitions are runtime network data, so this VALIDATES AND REJECTS
        // (logging what it dropped) instead of asserting, and it FORCES the authority
        // posture — a bridged command is always a consent-gated open-world write, no
        // matter what the child claimed. Returns how many were accepted. Fires the
        // change listener.
        sizet ReplaceClientCommands(const std::string& alias, std::vector<AutomationCommand> clientCommands);

        // Called after a Replace swaps the published vector, so a transport can
        // announce the change (MCP: notifications/tools/list_changed). Set once at
        // wiring time, before any worker thread exists; not synchronized.
        void SetChangeListener(std::function<void()> onChanged)
        {
            m_OnChanged = std::move(onChanged);
        }

        // ---- invocation --------------------------------------------------------

        // Resolve `name` in `commands` and run it against `host` with no transport
        // anywhere in the picture. Validates arguments against the command's declared
        // InputSchema, honours its availability predicate, and turns a handler that
        // throws into a command-level error result — the same three things the MCP
        // adapter does, because it calls RunHandler() below for the last of them.
        //
        // A ProjectWrite command is REFUSED unless `consent` is Granted; see
        // AutomationWriteConsent for why the default is deny.
        [[nodiscard]] AutomationInvocation Invoke(
            IAutomationHost& host, const std::string& name, const nlohmann::json& arguments,
            AutomationWriteConsent consent = AutomationWriteConsent::Withheld) const;

        // Run `command`'s handler, converting an escaping exception into an error
        // result. The single place a handler is entered, so the registry path and the
        // MCP adapter cannot drift on what "running a command" means.
        [[nodiscard]] static AutomationResult RunHandler(const AutomationCommand& command, IAutomationHost& host,
                                                         const nlohmann::json& arguments);

        // Linear scan by exact name. Order is registration order and the scan returns
        // the FIRST match, which is what keeps a script or bridged command from
        // shadowing a native one of the same name.
        [[nodiscard]] static const AutomationCommand* Find(const CommandList& commands, const std::string& name);

        // ---- validation (shared with the MCP adapter) --------------------------

        // A command name is 1-128 characters of [A-Za-z0-9_.-].
        [[nodiscard]] static bool IsValidName(std::string_view name);

        // Validate an MCP `icons` value (SEP-973 shape): a non-empty array whose
        // every element is an object with a non-empty string `src`, an optional
        // string `mimeType`, and an optional `sizes` array of strings. A null /
        // absent value is valid (it means "no icons") — check `icons.is_null()`
        // separately when you need "present".
        [[nodiscard]] static bool IsValidIcons(const nlohmann::json& icons);

        // A bridge alias is 1-32 characters of [a-z0-9-], never leading with '-'.
        [[nodiscard]] static bool IsValidClientAlias(std::string_view alias);

        // The reserved name prefix bridged commands from `alias` must live under.
        [[nodiscard]] static std::string ClientPrefix(const std::string& alias);

      private:
        void Publish(std::shared_ptr<CommandList> next);
        // Bump the generation, then tell the transport its catalogue moved. Bump
        // FIRST: a listener that polls the generation must never observe a stale
        // count alongside a fresh command list, or it would skip the notification.
        void NotifyChanged();

        // Copy-on-write, atomically published (see the CommandSnapshot contract).
        // Writers serialize on m_WriteMutex; readers are lock-free.
        std::atomic<CommandSnapshot> m_Commands{ std::make_shared<const CommandList>() };
        std::mutex m_WriteMutex;
        std::atomic<u64> m_Generation{ 0 };
        std::function<void()> m_OnChanged;
    };
} // namespace OloEngine::Automation
