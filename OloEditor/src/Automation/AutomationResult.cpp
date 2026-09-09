#include "OloEnginePCH.h"
#include "Automation/AutomationResult.h"

#include "Automation/AutomationAudienceReport.h"

#include <algorithm>

namespace OloEngine::Automation
{
    using Json = nlohmann::json;

    AutomationResult AutomationResult::Text(const std::string& text)
    {
        AutomationResult r;
        r.Content = Json::array({ Json{ { "type", "text" }, { "text", text } } });
        r.IsError = false;
        return r;
    }

    AutomationResult AutomationResult::Error(const std::string& message)
    {
        AutomationResult r;
        r.Content = Json::array({ Json{ { "type", "text" }, { "text", message } } });
        r.IsError = true;
        return r;
    }

    AutomationResult AutomationResult::Structured(const Json& data)
    {
        AutomationResult r;
        // Mirror the structured object into a text block (spec: keep a back-compat
        // serialization in `content` for clients that don't parse structuredContent).
        r.Content = Json::array({ Json{ { "type", "text" }, { "text", data.dump(2) } } });
        r.StructuredContent = data;
        r.IsError = false;
        return r;
    }

    Json AutomationResult::ResourceLinkBlock(const std::string& uri, const std::string& name,
                                             const std::string& description, const std::string& mimeType,
                                             u64 sizeBytes)
    {
        Json block{ { "type", "resource_link" },
                    { "uri", uri },
                    { "name", name },
                    { "description", description },
                    { "mimeType", mimeType } };
        // `size` is optional per spec; 0 means "unknown", so omit it then.
        if (sizeBytes > 0)
            block["size"] = sizeBytes;
        return block;
    }

    Json& AutomationResult::AnnotateBlock(Json& block, Audience audience, f64 priority)
    {
        Json annotations{ { "audience",
                            Json::array({ audience == Audience::User ? "user" : "assistant" }) } };
        // Clamp rather than trust the caller: an out-of-range priority is not a
        // spec-legal hint, and a command author passing 100 "for emphasis" would
        // otherwise ship an invalid annotation to every client.
        if (priority >= 0.0)
            annotations["priority"] = std::clamp(priority, 0.0, 1.0);
        block["annotations"] = std::move(annotations);
        return block;
    }

    AutomationResult AutomationResult::StructuredDualAudience(const Json& data, std::string_view title)
    {
        AutomationResult r;
        // dump() compact, not dump(2): the model also receives the identical
        // `structuredContent`, and a client that ignores audience annotations
        // renders BOTH blocks — emitting the machine mirror compact keeps that
        // pair costing about what Structured()'s single pretty-printed block did.
        Json machine{ { "type", "text" }, { "text", data.dump() } };
        AnnotateBlock(machine, Audience::Assistant, kAssistantBlockPriority);

        Json human{ { "type", "text" }, { "text", AudienceReport::Render(data, title) } };
        AnnotateBlock(human, Audience::User, kUserBlockPriority);

        r.Content = Json::array({ std::move(machine), std::move(human) });
        r.StructuredContent = data;
        r.IsError = false;
        return r;
    }
} // namespace OloEngine::Automation
