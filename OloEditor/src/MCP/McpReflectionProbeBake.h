#pragma once

#include "MCP/McpSchemaBuilder.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>

namespace OloEngine::MCP::ReflectionProbeBake
{
    using Json = nlohmann::json;

    [[nodiscard]] inline Json InputSchema()
    {
        return Schema::Object()
            .Prop("entity", Schema::Raw(Json{ { "type", "string" }, { "minLength", 1 } })
                                .Desc("Exact TagComponent name of the reflection-probe entity."))
            .Required({ "entity" })
            .NoAdditional();
    }

    [[nodiscard]] inline std::optional<std::string> ParseEntityName(const Json& args, std::string& name)
    {
        if (!args.contains("entity") || !args["entity"].is_string())
            return "Missing or invalid 'entity': expected a non-empty probe entity name.";
        name = args["entity"].get<std::string>();
        if (name.empty())
            return "Invalid 'entity': expected a non-empty probe entity name.";
        return std::nullopt;
    }

    [[nodiscard]] inline Json Result(std::string entity, bool baked, std::string message)
    {
        return Json{ { "entity", std::move(entity) },
                     { "baked", baked },
                     { "message", std::move(message) } };
    }
} // namespace OloEngine::MCP::ReflectionProbeBake
