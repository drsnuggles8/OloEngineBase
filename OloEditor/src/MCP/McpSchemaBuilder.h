#pragma once

// A tiny, header-only fluent DSL for authoring the JSON Schemas the MCP
// diagnostics tools declare (issue #357 P4). Every built-in tool used to
// hand-write a raw `nlohmann::json` literal for its `inputSchema` /
// `outputSchema` (≈48 literals across McpTools.cpp), which is verbose and easy to
// get subtly wrong — a dropped `additionalProperties`, an `"integer"` typo'd as
// `"string"`, a `minimum` bound forgotten. This builder captures the handful of
// shapes those schemas actually use behind a small chainable API so a schema
// reads like the constraint it expresses:
//
//     tool.InputSchema = Schema::Object()
//         .Prop("count", Schema::Int().Min(1).Max(200).Desc("How many lines (default 50)."))
//         .Prop("tag", Schema::String().Desc("Only lines whose [Tag] matches."))
//         .NoAdditional();
//
// Design / equivalence guarantee
//   * A `Node` is a thin wrapper around one `nlohmann::json` object. Every
//     factory/modifier just sets keys, so the emitted JSON is exactly what the
//     old literal produced — the migration is byte-identical on the wire
//     (locked by McpSchemaBuilderTest.cpp). Because the project uses the default
//     `nlohmann::json` (a sorted `std::map`), object-key insertion order never
//     affects `==` or `.dump()`; only array element order matters (enum /
//     required / items), and the builder preserves that.
//   * Numeric bounds are stored as JSON integers (every bound in the tool
//     surface is an integer literal), so `.dump()` stays "1", never "1.0".
//   * A `Node` converts implicitly to `Json`, so it drops straight into
//     `ToolDef::InputSchema` / nested `Prop()` / `Array()` calls.
//
// This header is renderer/editor-free (only nlohmann::json + the stdlib), so the
// MCP test binary — which deliberately does NOT link McpTools.cpp — can include
// it directly, the same split McpRenderOverrides.h / McpShaderReload.h use.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace OloEngine::MCP::Schema
{
    using Json = nlohmann::json;

    // One JSON-Schema node. Build it with a factory (Object/Int/String/...), refine
    // it with chainable modifiers, then let it convert to Json. Modifiers only ever
    // set the keyword they name; JSON Schema ignores keywords irrelevant to a node's
    // `type`, and the builder only calls the ones that apply, so the surface stays
    // small without a per-type class hierarchy.
    class Node
    {
      public:
        Node() = default;
        // Low-level: wrap an arbitrary schema object (see Raw()). Explicit so a bare
        // Json never silently becomes a Node (and vice-versa via operator Json).
        explicit Node(Json json) : m_Json(std::move(json)) {}

        // ---- modifiers (return *this for chaining) -----------------------------

        // `description` on any node.
        Node& Desc(std::string_view description)
        {
            m_Json["description"] = std::string(description);
            return *this;
        }

        // Numeric / size bounds. Stored as JSON integers (matches every bound in the
        // tool surface and keeps `.dump()` integral).
        Node& Min(std::int64_t minimum)
        {
            m_Json["minimum"] = minimum;
            return *this;
        }
        Node& Max(std::int64_t maximum)
        {
            m_Json["maximum"] = maximum;
            return *this;
        }
        Node& ExclusiveMin(std::int64_t exclusiveMinimum)
        {
            m_Json["exclusiveMinimum"] = exclusiveMinimum;
            return *this;
        }
        Node& MinItems(std::int64_t minItems)
        {
            m_Json["minItems"] = minItems;
            return *this;
        }
        Node& MaxItems(std::int64_t maxItems)
        {
            m_Json["maxItems"] = maxItems;
            return *this;
        }

        // Closed value set (string enums). Element order is preserved.
        Node& Enum(std::initializer_list<std::string_view> values)
        {
            Json arr = Json::array();
            for (const std::string_view value : values)
                arr.push_back(std::string(value));
            m_Json["enum"] = std::move(arr);
            return *this;
        }

        // ---- object modifiers --------------------------------------------------

        // Add one named property, lazily creating the `properties` map. An object
        // that never gets a Prop() stays property-less (e.g. an opaque sub-object
        // described only by Desc()), matching the hand-written literals.
        Node& Prop(std::string_view name, const Node& schema)
        {
            if (!m_Json.contains("properties"))
                m_Json["properties"] = Json::object();
            m_Json["properties"][std::string(name)] = schema.ToJson();
            return *this;
        }

        // The required-property list. Element order is preserved.
        Node& Required(std::initializer_list<std::string_view> names)
        {
            Json arr = Json::array();
            for (const std::string_view name : names)
                arr.push_back(std::string(name));
            m_Json["required"] = std::move(arr);
            return *this;
        }

        // `additionalProperties: false` — close an object to unknown keys. Every
        // tool *input* schema is closed; output schemas are left open (omit this).
        Node& NoAdditional()
        {
            m_Json["additionalProperties"] = false;
            return *this;
        }

        // Add the recurring `page` / `pageSize` pagination pair. The pageSize
        // description varies per tool (the noun being paged), so it is explicit; the
        // page description and the 0/1/200 bounds are identical everywhere.
        Node& Pagination(std::string_view pageSizeDesc)
        {
            return Prop("page", IntMin0Page()).Prop("pageSize", PageSize(pageSizeDesc));
        }

        [[nodiscard]] const Json& ToJson() const
        {
            return m_Json;
        }
        operator Json() const // NOLINT(google-explicit-constructor): drops into ToolDef.InputSchema
        {
            return m_Json;
        }

      private:
        // page / pageSize helpers (see Pagination). Declared after the public API so
        // the recurring strings live in one place.
        static Node IntMin0Page();
        static Node PageSize(std::string_view pageSizeDesc);

        Json m_Json = Json::object();
    };

    // ---- factories ---------------------------------------------------------------

    // `{ "type": "object" }`. Add properties with Prop(); close with NoAdditional().
    [[nodiscard]] inline Node Object()
    {
        return Node(Json{ { "type", "object" } });
    }
    // The closed, argument-less tool schema: `{ "type":"object", "properties":{},
    // "additionalProperties":false }` (≈8 tools take no arguments). Distinct from
    // Object().NoAdditional() because it carries an explicit empty `properties`.
    [[nodiscard]] inline Node EmptyObject()
    {
        return Node(Json{ { "type", "object" }, { "properties", Json::object() }, { "additionalProperties", false } });
    }
    [[nodiscard]] inline Node Int()
    {
        return Node(Json{ { "type", "integer" } });
    }
    [[nodiscard]] inline Node Number()
    {
        return Node(Json{ { "type", "number" } });
    }
    [[nodiscard]] inline Node Bool()
    {
        return Node(Json{ { "type", "boolean" } });
    }
    [[nodiscard]] inline Node String()
    {
        return Node(Json{ { "type", "string" } });
    }
    // `{ "type":"array" }` with no `items` — a loose array (the raycast/overlap
    // vector args, which validate element-by-element in the handler).
    [[nodiscard]] inline Node Array()
    {
        return Node(Json{ { "type", "array" } });
    }
    // `{ "type":"array", "items": <elem> }`.
    [[nodiscard]] inline Node Array(const Node& elem)
    {
        return Node(Json{ { "type", "array" }, { "items", elem.ToJson() } });
    }

    // ---- recurring-shape helpers -------------------------------------------------

    // A fixed-length 3-number array (a world-space position / direction / extents),
    // `{ "type":"array", "items":{"type":"number"}, "minItems":3, "maxItems":3, ... }`.
    [[nodiscard]] inline Node Vec3(std::string_view desc)
    {
        return Array(Number()).MinItems(3).MaxItems(3).Desc(desc);
    }

    // An entity-UUID string argument. The default description matches the common
    // call sites (olo_scene_get_entity, olo_camera_frame_entity); pass an explicit
    // one where the wording differs.
    [[nodiscard]] inline Node EntityId(std::string_view desc = "Entity UUID (as a string; also accepts a number).")
    {
        return String().Desc(desc);
    }

    // Wrap an arbitrary schema object so the chainable modifiers apply to it — for
    // the rare shape the factories don't cover (e.g. a multi-type `type`):
    //   Schema::Raw(Json{{ "type", Json::array({ "integer", "string" }) }}).Min(0).Desc(...)
    [[nodiscard]] inline Node Raw(Json json)
    {
        return Node(std::move(json));
    }

    // ---- out-of-line definitions for the pagination helpers ----------------------

    inline Node Node::IntMin0Page()
    {
        return Int().Min(0).Desc("Zero-based page index (default 0).");
    }
    inline Node Node::PageSize(std::string_view pageSizeDesc)
    {
        return Int().Min(1).Max(200).Desc(pageSizeDesc);
    }
} // namespace OloEngine::MCP::Schema
