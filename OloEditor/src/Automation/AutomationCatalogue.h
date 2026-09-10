#pragma once

// Describing the registry to a frontend (issue #1125).
//
// One serializer turns an AutomationCommand into the JSON a frontend reads. It
// was `McpServer::BuildToolEntryImpl`, and it moved here for the reason the
// registry itself moved (#1123): the description of a command is a property of
// the command, not of the transport that happens to carry it. tools/list,
// tools/search, the discovery gateway and oloctl all go through DescribeCommand
// now, so none of them can drift on what a command LOOKS like — the same
// guarantee AutomationRegistry::RunHandler gives for what invoking one MEANS.
//
// Two shapes, because two audiences need different things:
//
//   DescribeCommand   — the MCP `Tool` entry, exactly as the spec models it. No
//                       field outside the spec's own `_meta` extension point, so
//                       a strict client validating the Tool schema accepts it.
//   DescribeForFrontend — that entry plus the facts a NON-MCP frontend has no
//                       other way to learn, currently the authority class. A CLI
//                       has to know whether a command mutates the user's project
//                       before it offers to run it, and `readOnlyHint` is an
//                       optional behavioural HINT, not the authority class.
//
// Kept free of MCP/, httplib and Scene.h so it compiles anywhere the registry
// does — which is the whole point of a frontend that constructs no server.

#include "Automation/AutomationCommand.h"

#include <nlohmann/json.hpp>

#include <vector>

namespace OloEngine::Automation
{
    // Reverse-DNS-namespaced `_meta` key carrying a command's toolset (grouping
    // category). `_meta` is the MCP-blessed extension point (spec 2025-06-18), so
    // surfacing the category there keeps a tools/list entry conformant.
    inline constexpr const char* kToolsetMetaKey = "io.oloengine/toolset";

    // The key DescribeForFrontend adds. Deliberately a plain top-level key rather
    // than a `_meta` entry: it is never emitted into a tools/list response, only
    // into the registry-introspection surfaces a frontend reads, and a frontend
    // that misses it must REFUSE rather than guess — see OloCtl::AuthorityClass.
    inline constexpr const char* kProjectWriteKey = "projectWrite";

    // Serialize one command into its MCP tools/list entry. Optional fields are
    // omitted rather than emitted empty: an absent `title` lets a client fall back
    // to the name, and an empty `icons` array would advertise icons that are not
    // there.
    [[nodiscard]] nlohmann::json DescribeCommand(const AutomationCommand& command);

    // DescribeCommand plus `projectWrite`. Note that this reports the authority
    // class; it does NOT grant anything. Consent stays with the caller facing the
    // human (see AutomationWriteConsent).
    [[nodiscard]] nlohmann::json DescribeForFrontend(const AutomationCommand& command);

    // Every command in `commands`, in registration order, through
    // DescribeForFrontend. The catalogue a frontend generates its surface from.
    [[nodiscard]] nlohmann::json DescribeCatalogue(const std::vector<AutomationCommand>& commands);

    // Re-shape a successful typed result into the DUAL-AUDIENCE content pair when
    // `command` declared it (#673): a compact JSON block tagged for the assistant
    // plus a Markdown report tagged for the human, headed by the command's Title.
    // A no-op for a command that did not declare it, for an error, for an untyped
    // result, and for a handler that already appended a block of its own — so it is
    // idempotent and never drops content.
    //
    // Called by every frontend rather than by one of them: the adoption is declared
    // on the COMMAND, so two frontends that disagreed about applying it would return
    // different `content` for the same command. Shaping runs before any
    // session-level policy the transport adds (redaction), which stays the
    // transport's own business.
    void ApplyDualAudienceContent(const AutomationCommand& command, AutomationResult& result);

    // The `result` object one invocation produces: `{ content, isError }`, plus
    // `structuredContent` when the command returned a typed payload. This is the
    // shape a tools/call response carries, and the MCP adapter builds its response
    // with it — so a frontend that invokes through the registry directly and one
    // that goes over JSON-RPC print the same bytes for the same command, which is
    // #1125's first acceptance criterion.
    //
    // Redaction and the dual-audience re-shape happen to `result` BEFORE this, in
    // the adapter: both are transport-session policy, not part of what a result is.
    //
    // Taken BY VALUE so a caller with a finished result can move it in:
    // `content` carries a base64 screenshot or a render-target readback for the
    // capture commands, and copying that on every call is not free.
    [[nodiscard]] nlohmann::json DescribeResult(AutomationResult result);
} // namespace OloEngine::Automation
