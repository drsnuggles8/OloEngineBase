#pragma once

// Shared, editor-side schema + arg-parsing + result shaping for the consented
// MCP write tool `olo_editor_select_entity` (issue #607): select (or clear) the
// Scene Hierarchy panel's selection so the editor's Properties inspector draws
// the chosen entity's components. This is what makes the inspector-drawer
// surface (every DrawComponent<T>) reachable for MCP screenshot verification —
// olo_input_inject can't reliably land a panel-space click (the OS cursor
// reasserts over the synthetic position between injected frames), so a direct
// selection write is the only way to drive it programmatically.
//
// Like McpSceneControl.h, the ACTION cannot live in this header — resolving a
// uuid against the live scene and mutating SceneHierarchyPanel's selection
// touch the EnTT registry / an editor-only ImGui panel, neither of which a unit
// test may invoke. So it is routed through EditorMcpContext::SelectEntityInEditor:
// the editor wires it to SceneHierarchyPanel::SetSelectedEntity / ClearSelection,
// a headless host leaves it null ("not available"), and a test injects a fake.
// What stays here, header-only and engine-free, is the bit the tests pin: the
// tool's inputSchema, the pure argument validation (entity XOR clear), and the
// result -> JSON shaping.
//
// Engine-free: only McpServer.h (McpSelectEntityResult + Json), McpToolsCommon.h
// (ParseUuid — it is entirely `inline`/header-only itself, so reusing it here
// pulls in no extra editor .cpp TU; only its RegisterXTools *declarations*,
// never called from this header, would need one), and the schema-builder DSL —
// the same split McpSceneControl.h / McpReloadScript.h use. Selection is
// deliberately NOT routed through CommandHistory/undo: it isn't project data, so
// there is nothing to undo (mirrors McpRendererSettings.h's restore-prior-value
// framing rather than McpGenericFieldWrite.h's ComponentChangeCommand path).

#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"
#include "MCP/McpToolsCommon.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace OloEngine::MCP::SelectEntity
{
    using Json = nlohmann::json;

    // Parsed + validated arguments for olo_editor_select_entity.
    struct Request
    {
        bool Clear = false;
        u64 EntityUuid = 0;
    };

    // Validate + parse olo_editor_select_entity's arguments. Exactly one of
    // 'entity' or 'clear':true must be given — giving both, or neither, is a
    // clean argument error rather than an ambiguous default. 'entity' accepts a
    // string (preferred — u64 exceeds JSON's safe integer range) or a number,
    // via the shared OloEngine::MCP::ParseUuid every other entity-uuid tool uses.
    [[nodiscard]] inline std::optional<std::string> ParseArgs(const Json& args, Request& out)
    {
        const auto clearIt = args.find("clear");
        const bool hasClearKey = clearIt != args.end();
        if (hasClearKey && !clearIt->is_boolean())
            return "Invalid 'clear': expected a boolean.";
        const bool clear = hasClearKey && clearIt->get<bool>();
        const bool hasEntity = args.contains("entity");

        if (clear && hasEntity)
            return "Give either 'entity' or 'clear':true, not both.";
        if (clear)
        {
            out.Clear = true;
            out.EntityUuid = 0;
            return std::nullopt;
        }
        if (!hasEntity)
            return "Missing required argument 'entity' (entity UUID), or pass 'clear':true to deselect.";

        u64 uuid = 0;
        if (!ParseUuid(args["entity"], uuid))
            return "Invalid 'entity': expected a UUID as a string or number.";
        out.Clear = false;
        out.EntityUuid = uuid;
        return std::nullopt;
    }

    // inputSchema: an optional `entity` UUID and an optional `clear` boolean —
    // the value-level "exactly one of the two" constraint can't be expressed in
    // JSON-Schema, so ParseArgs enforces it and the server's schema check only
    // guarantees the types. `entity` is built with Schema::Raw rather than
    // Schema::EntityId() so it can declare BOTH "string" and "number" as
    // acceptable JSON types — Schema::EntityId() emits "string" only (pinned
    // byte-for-byte by McpSchemaBuilderTest.cpp), which would make the server's
    // own inputSchema enforcement reject a numeric 'entity' before ParseUuid
    // above (which accepts one) ever sees it.
    [[nodiscard]] inline Json InputSchema()
    {
        return Schema::Object()
            .Prop("entity", Schema::Raw(Json{ { "type", Json::array({ "string", "number" }) } })
                                .Desc("UUID of the entity to select in the Scene Hierarchy panel (so the "
                                      "Properties inspector draws its components), as a string or a number. "
                                      "Mutually exclusive with 'clear'."))
            .Prop("clear", Schema::Bool().Desc("Deselect instead of selecting an entity (mutually exclusive with 'entity')."))
            .NoAdditional();
    }

    // Result shaping. `available` distinguishes "no editor in this host" from a
    // real result. `entity`/`name` are only present when something is actually
    // selected afterwards, so a cleared/failed selection doesn't carry a
    // misleading zero uuid.
    [[nodiscard]] inline Json ToJson(const McpSelectEntityResult& result)
    {
        Json j{
            { "available", result.Available },
            { "ok", result.Ok },
            { "changed", result.Changed },
            { "selected", result.Selected },
            { "message", result.Message },
        };
        if (result.Selected)
        {
            j["entity"] = std::to_string(result.EntityId);
            j["name"] = result.EntityName;
        }
        return j;
    }
} // namespace OloEngine::MCP::SelectEntity
