#pragma once

// The input-schema validator a command's declared `InputSchema` is enforced with
// (issues #357 / #306; moved out of the MCP dispatch loop by #1123).
//
// A deliberately small JSON-Schema-subset validator covering exactly what the
// schema-builder DSL (MCP/McpSchemaBuilder.h) can emit. It lives beside the
// registry because the schema belongs to the COMMAND: a caller that reaches a
// command without a transport (the CLI in #1125, a headless harness) has to
// enforce the same contract, and a second validator would drift from this one.
//
// `McpServer::ValidateArguments` forwards here, so the MCP adapter and the
// registry's own Invoke() cannot disagree about what a well-formed call is.

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace OloEngine::Automation::AutomationSchema
{
    // Validate `args` against `schema`, returning the first error as a
    // field-naming English sentence, or nullopt when it satisfies the schema.
    // An absent / non-object / empty schema declares no constraints and is
    // permissive, so this is safe to call unconditionally.
    [[nodiscard]] std::optional<std::string> ValidateArguments(const nlohmann::json& schema,
                                                               const nlohmann::json& args);
} // namespace OloEngine::Automation::AutomationSchema
