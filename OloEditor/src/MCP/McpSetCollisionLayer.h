#pragma once

// Shared, editor-side logic behind the first consented, undoable MCP write tool
// `olo_set_collision_layer` (issue #306 item C, first slice). Split out of
// McpTools.cpp into this header so it can be unit-tested without the httplib /
// real-tool translation unit — the same "extract the reusable core into a header"
// pattern McpPhysicsExplain.h / McpRenderOverrides.h / McpSchemaBuilder.h use, so
// the MCP test binary (which deliberately does NOT link McpTools.cpp) can exercise
// the real schema + parse + apply code rather than a re-implementation.
//
// The write is applied through the editor's `CommandHistory` undo stack, so an
// agent's fix is a single Ctrl-Z: it builds a `ComponentChangeCommand<T>` keyed by
// UUID (relocation-safe) and runs it through the history, exactly like the editor's
// own component edits (SceneHierarchyPanel::DrawComponent).
//
// Everything here is renderer/httplib/McpServer-free (engine Scene/ECS types + the
// header-only UndoRedo + nlohmann::json + the schema-builder DSL), so it pulls no
// extra editor TU into the test binary.

#include "MCP/McpSchemaBuilder.h"
#include "UndoRedo/ComponentCommands.h"
#include "UndoRedo/EditorCommand.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>

namespace OloEngine::MCP::SetCollisionLayer
{
    using Json = nlohmann::json;

    // Upper bound on a user-defined physics layer id. The engine maps a user layer id
    // to the Jolt object layer `ObjectLayers::NUM_LAYERS + id`, and the whole
    // object-layer budget is `JoltUtils::kMaxJoltLayers` (32). So the largest authored
    // `m_LayerID` (a u32) that still maps to a valid object-layer slot is
    // `kMaxJoltLayers - ObjectLayers::NUM_LAYERS - 1` = 32 - 5 - 1 = 26 — anything above
    // would overflow the object-layer arrays. This header stays Jolt-free (it compiles
    // into the test binary), so the value is written out here and McpTools.cpp
    // `static_assert`s it against the live engine constants: if the budget changes, that
    // fails the build instead of silently over-permitting. The actual set of *valid*
    // layers is project-defined — an agent discovers them via olo_physics_layer_matrix;
    // we don't cross-check the live registry here (layer 0 is the always-present
    // default), so this is the budget ceiling, not a per-project validity guarantee.
    inline constexpr u32 kMaxLayerId = 26;

    // The tool's inputSchema, authored with the schema-builder DSL. Shared by the real
    // registration (McpTools.cpp) and the dispatch-seam test so the test validates the
    // exact schema the server enforces (the server runs ValidateArguments before the
    // handler — issue #423).
    [[nodiscard]] inline Json InputSchema()
    {
        return Schema::Object()
            .Prop("entity", Schema::EntityId("UUID of the entity whose physics body collision layer to set (a decimal-digit string)."))
            .Prop("layer", Schema::Int().Min(0).Max(static_cast<std::int64_t>(kMaxLayerId)).Desc("Collision layer id to assign to the body (a user-defined physics layer; 0 is the default. Use olo_physics_layer_matrix to discover valid ids)."))
            .Required({ "entity", "layer" })
            .NoAdditional();
    }

    // Parse + validate the tool arguments into native values. The server validates
    // `args` against InputSchema() before the handler runs, but this stays defensive
    // (it is also reached directly from tests). Returns a human-readable error, or
    // std::nullopt on success.
    [[nodiscard]] inline std::optional<std::string> ParseArgs(const Json& args, u64& entityUuid, u32& layer)
    {
        if (!args.contains("entity"))
            return "Missing required argument 'entity' (entity UUID).";
        const Json& entityValue = args["entity"];
        // UUIDs exceed JSON's safe-integer range, so they travel as a decimal string
        // (the inputSchema declares `entity` a string). Require the WHOLE string to be
        // decimal digits before converting: std::stoull would otherwise accept a leading
        // '-' (wrapping to a huge u64) or stop at the first non-digit ("42abc" -> 42).
        if (!entityValue.is_string())
            return "Invalid 'entity': expected a UUID as a decimal-digit string.";
        const std::string entityStr = entityValue.get<std::string>();
        if (entityStr.empty() ||
            !std::all_of(entityStr.begin(), entityStr.end(), [](unsigned char c)
                         { return c >= '0' && c <= '9'; }))
            return "Invalid 'entity': expected a UUID as a decimal-digit string.";
        try
        {
            entityUuid = std::stoull(entityStr);
        }
        catch (...) // std::out_of_range — value exceeds u64
        {
            return "Invalid 'entity': UUID value is out of range.";
        }

        if (!args.contains("layer"))
            return "Missing required argument 'layer'.";
        const Json& layerValue = args["layer"];
        if (!layerValue.is_number_integer())
            return "Invalid 'layer': expected an integer.";
        const long long signedLayer = layerValue.get<long long>();
        if (signedLayer < 0 || signedLayer > static_cast<long long>(kMaxLayerId))
            return "Invalid 'layer': expected an integer in [0, " + std::to_string(kMaxLayerId) + "].";
        layer = static_cast<u32>(signedLayer);
        return std::nullopt;
    }

    // Outcome of applying a collision-layer write. On success `Data` is the structured
    // result payload; on failure `Error` is a tool-level error message.
    struct ApplyResult
    {
        bool Ok = false;
        std::string Error;
        Json Data;
    };

    // Set the entity's physics-body collision layer (Rigidbody3DComponent::m_LayerID,
    // or CharacterController3DComponent::m_LayerID for a character controller) through
    // `history`, so the change is a single undoable command. A no-op change (the body
    // already on that layer) pushes nothing onto the undo stack and reports
    // changed=false. MUST run on the main (game) thread — it touches the EnTT registry
    // and the editor command stack.
    [[nodiscard]] inline ApplyResult Apply(const Ref<Scene>& scene, CommandHistory& history, u64 entityUuid, u32 layer)
    {
        ApplyResult result;
        if (!scene)
        {
            result.Error = "No active scene.";
            return result;
        }

        const auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityUuid));
        if (!entityOpt)
        {
            result.Error = "No entity with UUID " + std::to_string(entityUuid) + " in the active scene.";
            return result;
        }
        Entity entity = *entityOpt;

        std::string componentName;
        u32 previousLayer = 0;

        if (entity.HasComponent<Rigidbody3DComponent>())
        {
            componentName = "Rigidbody3DComponent";
            const auto& component = entity.GetComponent<Rigidbody3DComponent>();
            previousLayer = component.m_LayerID;
            if (previousLayer != layer)
            {
                Rigidbody3DComponent newData = component;
                newData.m_LayerID = layer;
                history.Execute(std::make_unique<ComponentChangeCommand<Rigidbody3DComponent>>(
                    scene, UUID(entityUuid), component, newData, "Set Collision Layer"));
            }
        }
        else if (entity.HasComponent<CharacterController3DComponent>())
        {
            componentName = "CharacterController3DComponent";
            const auto& component = entity.GetComponent<CharacterController3DComponent>();
            previousLayer = component.m_LayerID;
            if (previousLayer != layer)
            {
                CharacterController3DComponent newData = component;
                newData.m_LayerID = layer;
                history.Execute(std::make_unique<ComponentChangeCommand<CharacterController3DComponent>>(
                    scene, UUID(entityUuid), component, newData, "Set Collision Layer"));
            }
        }
        else
        {
            result.Error = "Entity has no physics body with a collision layer "
                           "(needs a Rigidbody3DComponent or CharacterController3DComponent).";
            return result;
        }

        const bool changed = previousLayer != layer;
        result.Ok = true;
        result.Data = Json{
            { "entity", std::to_string(entityUuid) },
            { "component", componentName },
            { "previousLayer", previousLayer },
            { "layer", layer },
            { "changed", changed },
            // `undoable` reflects whether a command was actually pushed (a no-op change
            // pushes nothing): an agent that wants to revert knows a single Ctrl-Z / undo
            // applies only when something changed.
            { "undoable", changed },
        };
        return result;
    }
} // namespace OloEngine::MCP::SetCollisionLayer
