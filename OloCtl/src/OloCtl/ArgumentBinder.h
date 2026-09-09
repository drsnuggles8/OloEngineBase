#pragma once

// Turning `--since-id 12` into `{"sinceId": 12}` (issue #1125).
//
// The flags a command accepts are its InputSchema's properties — read at
// runtime, never declared here. So a command that grows an argument grows a flag
// with no edit to oloctl, which is the same property the command tree has and
// for the same reason.
//
// WHAT THIS DOES NOT DO: validate. `required`, `enum`, `minimum`, pattern,
// `additionalProperties` — all of it is enforced by the host, through the exact
// same AutomationSchemaValidation an MCP tools/call goes through, and a second
// implementation here would be a second opinion that can disagree with the one
// that matters. The only local errors are the ones the host cannot produce
// because the request would never be built: an unknown flag, a missing value, or
// a value that cannot be turned into the declared JSON type at all.
//
// The one house rule enforced locally is finiteness (CLAUDE.md → Conventions):
// `--strength nan` is rejected here rather than shipped to a renderer.

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace OloCtl
{
    struct BindResult
    {
        bool Ok = false;
        nlohmann::json Arguments = nlohmann::json::object();
        // Empty when Ok. A complete sentence, printed to stderr as-is.
        std::string Error;
    };

    // Bind `tokens` (everything after `oloctl <group> <command>`) against
    // `inputSchema`, starting from `base` — the object `--arguments-json` supplied,
    // or an empty object. A flag repeated for an `array`-typed property appends;
    // repeated for any other type it is an error, because the second value silently
    // replacing the first is how a mistyped command line becomes a wrong answer.
    //
    // Accepted spellings per property `sinceId`: `--since-id V`, `--since-id=V`,
    // `--sinceId V`, `--sinceId=V`. A `boolean` property additionally accepts the
    // bare `--flag` (true) and `--no-flag` (false).
    [[nodiscard]] BindResult BindArguments(const nlohmann::json& inputSchema,
                                           const std::vector<std::string>& tokens,
                                           nlohmann::json base = nlohmann::json::object());

    // The flag spelling for a property name: `sinceId` -> `since-id`. Exported for
    // the help renderer, so help and parsing cannot disagree about what to type.
    [[nodiscard]] std::string FlagSpelling(const std::string& property);
} // namespace OloCtl
