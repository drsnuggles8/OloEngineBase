#pragma once

// One transport-independent automation command (issue #1123).
//
// This was `OloEngine::MCP::ToolDef`, and it is the same declaration it always
// was — name, schemas, annotations, authority, handler — with one thing removed:
// the handler no longer takes the MCP server. It takes `IAutomationHost`, so the
// command is a thing that exists on its own rather than a thing the transport
// owns. `MCP::ToolDef` is now an alias for this type, and the MCP server is one
// adapter over the registry that holds them.
//
// Registration stays HAND-DECLARED. #673 weighed a reflection-driven manifest
// bake against this project's size and rejected it; #1123 does not reverse that.
// What moved is the coupling, not the authoring model.

#include "Automation/AutomationHost.h"
#include "Automation/AutomationResult.h"

#include "OloEngine/Core/Base.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace OloEngine::Automation
{
    // A command handler runs on a caller-owned worker thread (under the MCP server,
    // a cpp-httplib worker). Lock-safe commands read the FMutex-guarded diagnostics
    // directly; main-marshaled ones wrap their registry/Scene reads in
    // host.MarshalRead(...).
    using AutomationHandler =
        std::function<AutomationResult(IAutomationHost& host, const nlohmann::json& arguments)>;

    // Whether running a command can be taken back, and how. Structural scene
    // authoring (#1126) declares its history contract here; older commands that
    // have not adopted the metadata remain Unspecified.
    enum class AutomationUndo : u8
    {
        Unspecified = 0, // not declared by this command.
        None,            // nothing to undo: the command does not mutate.
        EditorUndoStack, // routed through the editor's CommandHistory; Ctrl-Z takes it back.
        Irreversible,    // the effect cannot be taken back from inside this process.
    };

    // Everything the MCP adapter emits for one tools/list entry, plus the fields
    // dispatch reads. Field names are unchanged from ToolDef on purpose: this is an
    // extraction, and a rename would churn ~100 registration sites and the
    // documentation-coverage scanner for no architectural gain.
    struct AutomationCommand
    {
        std::string Name;
        // Optional human-friendly display name (MCP Tool.title, spec 2025-06-18).
        // Clients prefer it over `Name` for display (precedence: title >
        // annotations.title > name). Emitted as the top-level `title` only when
        // non-empty.
        std::string Title;
        std::string Description;
        nlohmann::json InputSchema;
        // Optional JSON Schema for the command's structured result (MCP `outputSchema`,
        // spec 2025-06-18). Emitted under `outputSchema` in tools/list only when it is
        // a non-empty object; pairs with a handler that returns
        // AutomationResult::Structured. Default-null commands stay text-only and omit
        // the field.
        nlohmann::json OutputSchema;
        // Optional MCP ToolAnnotations object — behavioural hints the client may
        // use to e.g. auto-approve a read-only command: `readOnlyHint`,
        // `destructiveHint`, `idempotentHint`, `openWorldHint`. Defaults to null;
        // emitted under `annotations` only when it is a non-empty object.
        nlohmann::json Annotations;
        // Lightweight grouping category (e.g. "render", "physics", "shader") so the
        // surface can be browsed/filtered instead of paged through flat — it was
        // 39 commands when this was introduced (#385) and is 96 now, which is what
        // made exposure profiles necessary (#1124). Used by `tools/search` (filter +
        // catalogue), surfaced under each entry's `_meta` in `tools/list`, and read
        // by ExposurePolicy under the `toolset` profile. Empty => uncategorized
        // (omitted from the metadata, and not listed under `toolset`). It does not
        // affect dispatch or resolution.
        std::string Toolset;
        AutomationHandler Handler;
        bool MainMarshaled = false;
        // THE AUTHORITY CLASS. True for a command that MUTATES the user's project
        // (scene / ECS components / assets) — as opposed to the read-only
        // diagnostics and the ephemeral editor-only camera/viewport/render-override
        // commands. A project-write command is gated behind the session
        // "Allow writes" toggle: the MCP adapter rejects it with a clean JSON-RPC
        // error when writes are disabled (the default). Issue #306; should be paired
        // with `readOnlyHint:false` annotations.
        //
        // The class MOVED here with the command; its meaning did not change, and
        // #1123 deliberately did not respell it as an enum — a second spelling of
        // the same two states is churn, and the consent model is out of scope.
        bool ProjectWrite = false;
        // True for a command registered from a project Lua script (McpScriptTools,
        // issue #357 / ADR 0005) rather than compiled-in. Script commands are
        // replaced wholesale by LoadScriptTools — safely even while the server is
        // RUNNING, via the copy-on-write snapshot (issue #607 live-reload item); see
        // AutomationRegistry::ReplaceScriptCommands.
        bool ScriptOwned = false;
        // Non-empty for a command BRIDGED from an outbound MCP client connection
        // (issue #673): the alias of the external server it proxies to.
        // Foreign provenance is deliberately a separate discriminator from
        // ScriptOwned — ReplaceScriptCommands wipes every ScriptOwned command on a
        // script rescan, and the two trust tiers are different (ADR 0005: a
        // script command's read-only hint is a sandbox-backed GUARANTEE; a foreign
        // command's behaviour lives in another process, so it is ALWAYS treated as
        // an open-world write — ReplaceClientCommands forces ProjectWrite=true).
        std::string ClientAlias;
        // Optional MCP `icons` array (SEP-973, spec 2025-11-25): display icons a
        // client may show next to the command. Each element is an object with a
        // required string `src` (an http(s) or data: URI) plus optional
        // `mimeType` and `sizes` (an array of "48x48"-style strings). Null /
        // empty => omitted from tools/list entirely (never emit an empty
        // `icons` key). Validated by AutomationRegistry::IsValidIcons at registration.
        nlohmann::json Icons;
        // Opt in to AUDIENCE-TAGGED content blocks for this command's typed results
        // (#673). When true, the adapter re-shapes a successful
        // AutomationResult::Structured() into the dual-audience pair via
        // AutomationResult::StructuredDualAudience — a compact JSON block tagged
        // audience ["assistant"] plus a Markdown report tagged ["user"], headed by
        // `Title` (falling back to `Name`). The handler stays a plain
        // `AutomationResult::Structured(...)`; adoption is declared HERE, next to
        // OutputSchema and Annotations, so the adopter set is inspectable from
        // registration alone — which is what McpAudienceBlocksTest ratchets
        // without ever issuing a tools/call.
        //
        // THE INCLUSION RULE — set this only when a human watching the session
        // would genuinely read the payload differently from how the model parses
        // it: the result is multi-field or tabular AND is something you'd stare at
        // while debugging (a per-pass timing table, per-channel target stats, an
        // explainer's check list). For a one-scalar result, a status echo, or a
        // payload that is already a sentence, the second rendering is pure noise —
        // leave this false. Blanket adoption across the surface would be bloat,
        // not spec parity.
        bool DualAudienceContent = false;
        // See AutomationUndo for the command's declared reversal mechanism.
        AutomationUndo Undo = AutomationUndo::Unspecified;
        // When set and it returns false, the command is not listed and cannot be
        // invoked — the host it would run against cannot serve it (no editor, no
        // GPU, a subsystem compiled out). Empty means always available, which is
        // what every command says today, so this changes nothing observable until
        // one declares otherwise.
        //
        // This is NOT the exposure profile (#1124), which hides a command from
        // `tools/list` while keeping it dispatchable by name. Unavailable means
        // genuinely absent: listing it would advertise something that cannot run,
        // and dispatching it would fail somewhere deeper and less legibly.
        //
        // WHERE IT IS HONOURED: dispatch (AutomationRegistry::Invoke and the MCP
        // adapter's tools/call) and listing (tools/list). The registry-introspection
        // surfaces — tools/search and the discovery gateway — deliberately still
        // report it, because they exist to describe what the REGISTRY holds and
        // already carry a per-entry `listed` flag for the reader. Unifying the two
        // filters is #1126's job, not a pure extraction's.
        std::function<bool(const IAutomationHost&)> IsAvailable;

        // True when this command can run against `host`. An availability predicate
        // that throws is treated as unavailable rather than letting the exception
        // escape a listing sweep.
        [[nodiscard]] bool AvailableOn(const IAutomationHost& host) const noexcept
        {
            if (!IsAvailable)
                return true;
            try
            {
                return IsAvailable(host);
            }
            catch (...)
            {
                return false;
            }
        }
    };
} // namespace OloEngine::Automation
