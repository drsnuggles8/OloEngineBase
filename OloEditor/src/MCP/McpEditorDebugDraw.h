#pragma once

#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"
#include "MCP/McpTokenNormalization.h"

#include <array>
#include <string>
#include <string_view>

namespace OloEngine::MCP::EditorDebugDraw
{
    inline constexpr std::array<std::string_view, 9> kCategories{
        "all",
        "grid",
        "physics_colliders",
        "light_gizmos",
        "world_axis",
        "camera_frustums",
        "bounding_boxes",
        "selection_outline",
        "component_gizmos",
    };

    [[nodiscard]] inline std::string_view CanonicalCategory(std::string_view value)
    {
        const std::string normalized = NormalizeToken(value);
        for (const auto category : kCategories)
        {
            if (NormalizeToken(category) == normalized)
                return category;
        }
        return {};
    }

    [[nodiscard]] inline Json SetInputSchema()
    {
        return Schema::Object()
            .Prop("category", Schema::String().EnumFrom(kCategories).Desc("Editor overlay category, or 'all' for the master switch."))
            .Prop("enabled", Schema::Bool().Desc("Whether the category is enabled."))
            .Required({ "category", "enabled" })
            .NoAdditional();
    }

    [[nodiscard]] inline Json StateToJson(const McpEditorDebugDrawState& state)
    {
        return Json{
            { "all", state.All },
            { "grid", state.Grid },
            { "physics_colliders", state.PhysicsColliders },
            { "light_gizmos", state.LightGizmos },
            { "world_axis", state.WorldAxis },
            { "camera_frustums", state.CameraFrustums },
            { "bounding_boxes", state.BoundingBoxes },
            { "selection_outline", state.SelectionOutline },
            { "component_gizmos", state.ComponentGizmos },
        };
    }

    [[nodiscard]] inline Json ToJson(const McpEditorDebugDrawSetResult& result)
    {
        return Json{
            { "available", result.Available },
            { "ok", result.Ok },
            { "changed", result.Changed },
            { "category", result.Category },
            { "enabled", result.Enabled },
            { "state", StateToJson(result.State) },
            { "message", result.Message },
        };
    }
} // namespace OloEngine::MCP::EditorDebugDraw
