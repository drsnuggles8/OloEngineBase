#pragma once

// Pure argument parsing and response shaping behind olo_memory_budget_set
// (issue #1343): the live pressure-injection seam of the renderer's memory
// admission.
//
// The tool replaces the budget the device reports with one the caller names, so
// a degradation, a recovery and the hysteresis between them can be driven on a
// running editor instead of waiting for a machine that is really out of memory.
// `clear: true` gives the device's own budget back.
//
// EXACTLY ONE OF `megabytes` AND `clear`. Both together have no single meaning,
// and neither would make a call with no arguments read as "set" while changing
// nothing; each is an error that says which.
//
// The override is session state in the renderer: it is never serialized and it
// does not edit the requested quality. The governor reads it at the next frame,
// so the `admission` block in the reply is the decision that was in force when
// the call landed, not the one the new budget produces. `note` says so.

#include "MCP/McpSchemaBuilder.h"

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace OloEngine::MCP::MemoryBudget
{
    using Json = nlohmann::json;

    // 1 TiB, the ceiling the streaming budgets use too: far above any device,
    // and small enough that megabytes * kBytesPerMegabyte is exact in a double.
    inline constexpr i64 kMaxMegabytes = 1024 * 1024;
    inline constexpr u64 kBytesPerMegabyte = 1024ull * 1024ull;

    struct Args
    {
        // The budget to inject, in bytes. Empty: clear the override.
        std::optional<u64> OverrideBytes;
    };

    [[nodiscard]] inline Json InputSchema()
    {
        return Schema::Object()
            .Prop("megabytes", Schema::Number().ExclusiveMin(0).Max(kMaxMegabytes).Desc("Device budget to inject, in MiB (finite, > 0, at most 1048576). Replaces whatever the device reports until cleared."))
            .Prop("clear", Schema::Bool().Desc("true gives the device's own budget back. Not together with 'megabytes'."))
            .NoAdditional();
    }

    [[nodiscard]] inline Json OutputSchema()
    {
        return Schema::Object()
            .Prop("overrideBytes", Schema::NullableNumber().Desc("The injected budget now in force, in bytes; null when the device's own budget is."))
            .Prop("admission", Schema::NullableObject().Desc("The admission record as olo_memory_report reports it, taken BEFORE the new budget was judged; null when no renderer is admitting."))
            .Prop("note", Schema::String())
            .Required({ "overrideBytes", "admission", "note" });
    }

    // Parse the tool arguments. The server validates against InputSchema() first,
    // but this stays defensive: it is also reached directly from tests, and a
    // programmatically built Json can hold a NaN the wire format cannot.
    // Returns a human-readable error, or nullopt with `out` filled.
    [[nodiscard("a parse error must reach the caller")]] inline std::optional<std::string> ParseArgs(const Json& args, Args& out)
    {
        if (!args.is_object())
            return "Expected an object with 'megabytes' or 'clear'.";

        const bool hasMegabytes = args.contains("megabytes") && !args["megabytes"].is_null();
        bool clear = false;
        if (args.contains("clear") && !args["clear"].is_null())
        {
            if (!args["clear"].is_boolean())
                return "'clear' must be a boolean.";
            clear = args["clear"].get<bool>();
        }

        if (hasMegabytes && clear)
            return "Pass either 'megabytes' or 'clear: true', not both.";
        if (!hasMegabytes && !clear)
            return "Pass 'megabytes' (a budget to inject) or 'clear: true' (give the device's budget back).";

        if (clear)
        {
            out.OverrideBytes.reset();
            return std::nullopt;
        }

        if (!args["megabytes"].is_number())
            return "'megabytes' must be a number.";
        const f64 megabytes = args["megabytes"].get<f64>();
        if (!std::isfinite(megabytes))
            return "'megabytes' must be finite.";
        if (megabytes <= 0.0)
            return "'megabytes' must be greater than 0; pass 'clear: true' to remove the injected budget.";
        if (megabytes > static_cast<f64>(kMaxMegabytes))
            return "'megabytes' must be at most " + std::to_string(kMaxMegabytes) + ".";

        // Rounded up, so a positive request below one byte is one byte, never the
        // zero-byte budget that would read as "no budget".
        out.OverrideBytes = static_cast<u64>(std::ceil(megabytes * static_cast<f64>(kBytesPerMegabyte)));
        return std::nullopt;
    }

    // `overrideBytes` is read back from the renderer after the write, not echoed
    // from the arguments. `admission` is the report's own JSON form of the record.
    [[nodiscard("this builds the response; it does not send it")]] inline Json BuildResponse(const std::optional<u64>& overrideBytes, Json admission)
    {
        return Json{ { "overrideBytes", overrideBytes ? Json(*overrideBytes) : Json(nullptr) },
                     { "admission", std::move(admission) },
                     { "note", "the decision is taken at the next frame" } };
    }
} // namespace OloEngine::MCP::MemoryBudget
