#pragma once

// The result of running one automation command (issue #1123).
//
// This was `OloEngine::MCP::ToolResult`, defined next to the JSON-RPC dispatch
// loop that happened to be its only caller. Nothing about it is MCP-specific:
// it is a content array, an error flag and an optional typed payload — the shape
// any caller of a command needs, whether it reached the command over Streamable
// HTTP, from a CLI, or from a test with no server at all. So it moved here with
// the registry, and `MCP::ToolResult` is now an alias for it.
//
// The `content` block vocabulary (`{"type":"text", ...}`, `resource_link`, the
// `annotations.audience` tags) is MCP's, and stays as-is deliberately: the
// extraction must not change one observable byte of what tools/call returns, and
// a second, parallel block vocabulary invented for the sake of purity would be a
// translation layer that can drift. A non-MCP caller reads `StructuredContent`.

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace OloEngine::Automation
{
    // Result of a command invocation. `Content` is the MCP content array
    // (e.g. [{ "type": "text", "text": "..." }]); `IsError` maps to the
    // tools/call `isError` flag (a command-level error, not a JSON-RPC protocol error).
    //
    // `StructuredContent` is the optional typed result (MCP `structuredContent`,
    // spec 2025-06-18): a JSON object an agent can parse directly instead of
    // scraping the `content` text. It is null (omitted) for inherently-textual
    // commands; a command that sets it should also declare a matching
    // `AutomationCommand::OutputSchema`. Per the spec, when `structuredContent` is
    // present `content` must still carry a backward-compatible serialized mirror —
    // `Structured()` builds both at once.
    struct AutomationResult
    {
        nlohmann::json Content = nlohmann::json::array();
        bool IsError = false;
        nlohmann::json StructuredContent; // null => omitted; must be a JSON object when set.

        [[nodiscard]] static AutomationResult Text(const std::string& text);
        [[nodiscard]] static AutomationResult Error(const std::string& message);
        // Typed success result: sets `StructuredContent` to `data` (must be a JSON
        // object) and mirrors it into `content` as pretty-printed text for clients
        // that don't read structured output. `IsError` stays false.
        [[nodiscard]] static AutomationResult Structured(const nlohmann::json& data);
        // Build one `resource_link` content block (MCP spec 2025-06-18): a URI
        // reference to a server resource the client fetches on demand via
        // resources/read, instead of an inline base64 `image`/`blob` payload.
        // Append it to `Content` by hand — capture commands offer this as an opt-in
        // delivery mode for large captures (issue #673). `sizeBytes` is
        // emitted as `size` only when non-zero.
        [[nodiscard]] static nlohmann::json ResourceLinkBlock(const std::string& uri, const std::string& name,
                                                              const std::string& description,
                                                              const std::string& mimeType, u64 sizeBytes = 0);

        // ---- audience-tagged content blocks (#673) ---------------------
        //
        // Which side of the session one CONTENT BLOCK is meant for (MCP spec
        // 2025-06-18 `Annotations.audience`). This is a different spec field on a
        // different object from AutomationCommand::Annotations, which carries the
        // command-level readOnlyHint / openWorldHint hints — don't conflate the two.
        //
        // Absent annotations already mean "no preference", so there is deliberately
        // no `Both`: a block for everyone is simply left unannotated.
        enum class Audience : u8
        {
            Assistant, // the model driving the session: compact and parseable.
            User,      // the human watching it: formatted and glanceable.
        };

        // Spec priority is an importance hint in [0,1] — 1 is "effectively
        // required", 0 "entirely optional". The machine block IS the result, so it
        // is required; the human rendering is presentation over the same data, so a
        // client under pressure may drop it first.
        static constexpr f64 kAssistantBlockPriority = 1.0;
        static constexpr f64 kUserBlockPriority = 0.3;

        // Stamp `annotations` onto one content block in place and return it, so it
        // composes with the block factories (e.g. AnnotateBlock(ResourceLinkBlock(...),
        // Audience::User)). A negative `priority` omits the field rather than
        // emitting a meaningless 0 (the same omit-when-unset rule as `size` on a
        // resource link). `audience` is always written, so `annotations` is never
        // emitted as an empty object.
        static nlohmann::json& AnnotateBlock(nlohmann::json& block, Audience audience, f64 priority = -1.0);

        // Typed success result that renders its payload TWICE, each block tagged
        // for one audience: a compact `data.dump()` for the assistant and a
        // Markdown report (AutomationAudienceReport.h) for the human.
        // `StructuredContent` is `data`, exactly as with Structured(), so the
        // outputSchema contract is unchanged and only the `content` array differs.
        // The assistant block stays at index 0 — content[0] remains the
        // machine-readable JSON mirror.
        //
        // WHEN TO ADOPT THIS OVER Structured(): only when a human watching the
        // session would genuinely read the payload differently from how the model
        // parses it — i.e. it is multi-field or tabular AND is something you'd stare
        // at while debugging (a per-pass timing table, a per-channel stats table, an
        // explainer's check list). For a one-scalar result, a status echo, or a
        // payload that is already a sentence, a second rendering is noise: keep
        // Structured(). Blanket adoption across the whole command surface would be
        // bloat, not spec parity.
        [[nodiscard]] static AutomationResult StructuredDualAudience(const nlohmann::json& data,
                                                                     std::string_view title);
    };
} // namespace OloEngine::Automation
