#include "OloEnginePCH.h"
#include "Automation/AutomationCatalogue.h"

#include <utility>

namespace OloEngine::Automation
{
    using Json = nlohmann::json;

    Json DescribeCommand(const AutomationCommand& command)
    {
        Json entry;
        entry["name"] = command.Name;
        // Top-level display title (spec 2025-06-18); omitted when unset so the
        // client falls back to the name.
        if (!command.Title.empty())
            entry["title"] = command.Title;
        entry["description"] = command.Description;
        entry["inputSchema"] = command.InputSchema.is_null() ? Json{ { "type", "object" } } : command.InputSchema;
        // JSON Schema for the structured result (spec 2025-06-18); omitted unless a
        // non-empty object so text-only commands stay clean.
        if (command.OutputSchema.is_object() && !command.OutputSchema.empty())
            entry["outputSchema"] = command.OutputSchema;
        // Behavioural hints (readOnlyHint, etc.); omitted unless a non-empty object.
        if (command.Annotations.is_object() && !command.Annotations.empty())
            entry["annotations"] = command.Annotations;
        // Display icons (SEP-973, spec 2025-11-25). Emitted ONLY when the array is
        // non-empty: the spec models `icons` as an optional field, and an empty array
        // would advertise "this command has icons" while carrying none, which a client
        // may render as a broken/blank slot. Register() already rejected a malformed
        // value, so a present array is well-formed here.
        if (command.Icons.is_array() && !command.Icons.empty())
            entry["icons"] = command.Icons;
        // Grouping category under the spec's `_meta` extension point; omitted for
        // uncategorized commands so their entry is unchanged from before toolsets.
        if (!command.Toolset.empty())
            entry["_meta"] = Json{ { kToolsetMetaKey, command.Toolset } };
        return entry;
    }

    Json DescribeForFrontend(const AutomationCommand& command)
    {
        Json entry = DescribeCommand(command);
        // ALWAYS written, both true and false — unlike every optional field above.
        // A frontend that gates its write path on this must be able to tell "this
        // command is read-only" from "this catalogue does not report the authority
        // class", and an omitted-when-false key collapses those two into one.
        entry[kProjectWriteKey] = command.ProjectWrite;
        return entry;
    }

    Json DescribeCatalogue(const std::vector<AutomationCommand>& commands)
    {
        Json entries = Json::array();
        entries.get_ref<Json::array_t&>().reserve(commands.size());
        for (const AutomationCommand& command : commands)
            entries.push_back(DescribeForFrontend(command));
        return entries;
    }

    void ApplyDualAudienceContent(const AutomationCommand& command, AutomationResult& result)
    {
        // Rebuilding from StructuredContent -- rather than annotating what the handler
        // returned -- is what makes the machine block compact. The single-block guard
        // means a handler that appended its own extra block (a resource_link) or
        // already emitted the pair itself is left alone.
        if (!command.DualAudienceContent || result.IsError || result.StructuredContent.is_null() ||
            !result.Content.is_array() || result.Content.size() != 1)
        {
            return;
        }
        result = AutomationResult::StructuredDualAudience(result.StructuredContent,
                                                          command.Title.empty() ? command.Name : command.Title);
    }

    Json DescribeResult(AutomationResult result)
    {
        Json object{ { "content", std::move(result.Content) }, { "isError", result.IsError } };
        // Omitted for text-only commands, so their result shape is unchanged from
        // before structured output existed (spec 2025-06-18).
        if (!result.StructuredContent.is_null())
            object["structuredContent"] = std::move(result.StructuredContent);
        return object;
    }
} // namespace OloEngine::Automation
