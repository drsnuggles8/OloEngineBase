#pragma once

// Shared, editor-side core behind the GENERIC consented, undoable MCP write tool
// `olo_entity_set_field` (issue #306 item C, second slice) and its read companion
// `olo_entity_list_fields`. The first write slice (`olo_set_collision_layer`,
// McpSetCollisionLayer.h) shipped one tool per field; this is the catch-all that
// mutates ANY registered component field through one tool, so an agent can apply
// a fix to a transform, a light, a sprite, a camera, ... without a bespoke tool
// for each.
//
// Like McpSetCollisionLayer.h, the write is applied through the editor's
// `CommandHistory` undo stack (a UUID-keyed `ComponentChangeCommand<T>`,
// relocation-safe) so an agent's edit is a single Ctrl-Z, and the whole core is
// split into this header — renderer/httplib/McpServer-free (engine Scene/ECS
// types + the header-only UndoRedo + nlohmann::json + the schema-builder DSL) — so
// the MCP test binary (which deliberately does NOT link McpTools.cpp) exercises
// the real schema + parse + coerce + apply code, not a re-implementation.
//
// Why a hand-built field registry (and not OloHeaderTool codegen)
//   C++ has no runtime reflection, and `OLO_PROPERTY` is a pure compile-time
//   marker consumed by OloHeaderTool to emit *static* C#/Lua glue — there is no
//   runtime (component-name, field-name) -> typed-setter table to borrow. So the
//   registry below is type-safe codegen-free reflection: each entry pairs a script
//   facing name with a real pointer-to-member, and a templated closure does the
//   JSON->field coercion, the no-op detection, and the undoable command build. The
//   field NAMES match the m_-stripped keys an agent already sees in
//   `olo_scene_get_entity`'s YAML (and the OLO_PROPERTY convention), so the write
//   contract lines up with the read contract.
//
//   Adding a field is one `MakeField<Component>(...)` line in BuildRegistry(); a
//   future slice could repopulate the same registry from an OloHeaderTool-generated
//   list once codegen for it exists. Until then this is a curated set, not every
//   field — by design, not omission (an agent discovers exactly what's writable via
//   `olo_entity_list_fields`).

#include "MCP/McpSchemaBuilder.h"
#include "UndoRedo/ComponentCommands.h"
#include "UndoRedo/EditorCommand.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace OloEngine::MCP::GenericFieldWrite
{
    using Json = nlohmann::json;

    // ---- type-name reporting -------------------------------------------------

    // The agent-facing type token for a field type, used in discovery output and
    // coercion error messages. Drives nothing in C++ — coercion dispatches on the
    // type itself via `if constexpr`, not on this string.
    template<typename F>
    [[nodiscard]] constexpr std::string_view FieldTypeName()
    {
        if constexpr (std::is_same_v<F, bool>)
            return "bool";
        else if constexpr (std::is_same_v<F, std::string>)
            return "string";
        else if constexpr (std::is_same_v<F, UUID>) // == AssetHandle
            return "handle";
        else if constexpr (std::is_same_v<F, glm::vec2>)
            return "vec2";
        else if constexpr (std::is_same_v<F, glm::vec3>)
            return "vec3";
        else if constexpr (std::is_same_v<F, glm::vec4>)
            return "vec4";
        else if constexpr (std::is_enum_v<F>)
            return "int"; // enums travel as their integer value
        else if constexpr (std::is_integral_v<F>)
            return "int";
        else if constexpr (std::is_floating_point_v<F>)
            return "float";
        else
        {
            static_assert(sizeof(F) == 0, "FieldTypeName: unsupported field type");
            return "";
        }
    }

    // ---- JSON -> field coercion ----------------------------------------------

    // Read exactly N finite JSON numbers into a glm vector. Validates length and
    // finiteness (the project rule: every float from JSON/network is isfinite-checked).
    template<int N, typename V>
    [[nodiscard]] inline std::optional<std::string> CoerceVec(const Json& v, V& out)
    {
        if (!v.is_array())
            return "expected an array of " + std::to_string(N) + " numbers";
        if (v.size() != static_cast<std::size_t>(N))
            return "expected " + std::to_string(N) + " elements, got " + std::to_string(v.size());
        for (int i = 0; i < N; ++i)
        {
            const Json& element = v[static_cast<std::size_t>(i)];
            if (!element.is_number())
                return "element " + std::to_string(i) + " is not a number";
            const double d = element.get<double>();
            if (!std::isfinite(d))
                return "element " + std::to_string(i) + " is not finite";
            out[static_cast<glm::length_t>(i)] = static_cast<float>(d);
        }
        return std::nullopt;
    }

    // Coerce a JSON value into the field's declared type. Returns a human-readable
    // reason on mismatch, or std::nullopt + the coerced value on success. Strict:
    // a boolean must be a JSON boolean, an integer a JSON integer (not 3.0), and a
    // UUID/AssetHandle a decimal-digit string (a u64 exceeds JSON's safe-integer
    // range, so it travels as a string — same contract as the entity UUID).
    template<typename F>
    [[nodiscard]] inline std::optional<std::string> CoerceJson(const Json& v, F& out)
    {
        if constexpr (std::is_same_v<F, bool>)
        {
            if (!v.is_boolean())
                return "expected a boolean";
            out = v.get<bool>();
            return std::nullopt;
        }
        else if constexpr (std::is_same_v<F, std::string>)
        {
            if (!v.is_string())
                return "expected a string";
            out = v.get<std::string>();
            return std::nullopt;
        }
        else if constexpr (std::is_same_v<F, UUID>) // == AssetHandle
        {
            if (!v.is_string())
                return "expected a decimal-digit string (u64 handle)";
            const std::string s = v.get<std::string>();
            if (s.empty() || !std::all_of(s.begin(), s.end(), [](unsigned char c)
                                          { return c >= '0' && c <= '9'; }))
                return "expected a decimal-digit string (u64 handle)";
            try
            {
                out = UUID(std::stoull(s));
            }
            catch (...) // std::out_of_range — exceeds u64
            {
                return "handle value is out of range";
            }
            return std::nullopt;
        }
        else if constexpr (std::is_same_v<F, glm::vec2>)
            return CoerceVec<2>(v, out);
        else if constexpr (std::is_same_v<F, glm::vec3>)
            return CoerceVec<3>(v, out);
        else if constexpr (std::is_same_v<F, glm::vec4>)
            return CoerceVec<4>(v, out);
        else if constexpr (std::is_enum_v<F>)
        {
            using U = std::underlying_type_t<F>;
            if (!v.is_number_integer())
                return "expected an integer (enum value)";
            const std::int64_t n = v.get<std::int64_t>();
            if (n < static_cast<std::int64_t>(std::numeric_limits<U>::min()) ||
                n > static_cast<std::int64_t>(std::numeric_limits<U>::max()))
                return "enum value is out of range";
            out = static_cast<F>(static_cast<U>(n));
            return std::nullopt;
        }
        else if constexpr (std::is_integral_v<F>)
        {
            // sizeof<=4 keeps numeric_limits<F> within int64 for the bound check;
            // 64-bit integral fields would be UUID/AssetHandle, handled above.
            static_assert(sizeof(F) <= 4, "CoerceJson: 64-bit integral fields go through the handle path");
            if (!v.is_number_integer())
                return "expected an integer";
            const std::int64_t n = v.get<std::int64_t>();
            if (n < static_cast<std::int64_t>(std::numeric_limits<F>::min()) ||
                n > static_cast<std::int64_t>(std::numeric_limits<F>::max()))
                return "value is out of range for the field";
            out = static_cast<F>(n);
            return std::nullopt;
        }
        else if constexpr (std::is_floating_point_v<F>)
        {
            if (!v.is_number())
                return "expected a number";
            const double d = v.get<double>();
            if (!std::isfinite(d))
                return "must be a finite number";
            out = static_cast<F>(d);
            return std::nullopt;
        }
        else
        {
            static_assert(sizeof(F) == 0, "CoerceJson: unsupported field type");
            return "unsupported field type";
        }
    }

    // The field's current value as JSON, the inverse of CoerceJson — used for the
    // write result's previousValue/value and the read tool's reported value. A
    // u64-wrapper (UUID/AssetHandle) is emitted as a decimal string to match its
    // input contract and stay in JSON's safe-integer range.
    template<typename F>
    [[nodiscard]] inline Json FieldToJson(const F& v)
    {
        if constexpr (std::is_same_v<F, bool>)
            return Json(v);
        else if constexpr (std::is_same_v<F, std::string>)
            return Json(v);
        else if constexpr (std::is_same_v<F, UUID>) // == AssetHandle
            return Json(std::to_string(static_cast<u64>(v)));
        else if constexpr (std::is_same_v<F, glm::vec2>)
            return Json::array({ v.x, v.y });
        else if constexpr (std::is_same_v<F, glm::vec3>)
            return Json::array({ v.x, v.y, v.z });
        else if constexpr (std::is_same_v<F, glm::vec4>)
            return Json::array({ v.x, v.y, v.z, v.w });
        else if constexpr (std::is_enum_v<F>)
            return Json(static_cast<std::int64_t>(v));
        else if constexpr (std::is_integral_v<F>)
            return Json(v);
        else if constexpr (std::is_floating_point_v<F>)
            return Json(v);
        else
        {
            static_assert(sizeof(F) == 0, "FieldToJson: unsupported field type");
            return Json();
        }
    }

    // Whether the coerced new value differs from the current one — gates whether a
    // command is pushed at all (a no-op write must not pollute the undo stack). For
    // trivially-copyable field types (primitives, glm::vec*, UUID, enums) this is a
    // byte compare — the exact change-detection the editor's own undo uses for
    // trivially-copyable components (SceneHierarchyPanel::DrawComponent tier 1) — so
    // it needs no float `==` (which the project lint/SonarQube flags). std::string
    // is the one non-trivial type, compared with !=.
    template<typename F>
    [[nodiscard]] inline bool FieldChanged(const F& a, const F& b)
    {
        if constexpr (std::is_same_v<F, std::string>)
            return a != b;
        else
        {
            static_assert(std::is_trivially_copyable_v<F>, "FieldChanged: type must be trivially copyable or std::string");
            return std::memcmp(&a, &b, sizeof(F)) != 0;
        }
    }

    // ---- the field registry --------------------------------------------------

    // Outcome of applying a field write. On success `Data` is the structured result
    // payload; on failure `Error` is a tool-level error message.
    struct ApplyResult
    {
        bool Ok = false;
        std::string Error;
        Json Data;
    };

    // One writable (component, field) pair, type-erased over the field type. The
    // closures bake in the component type C and a pointer-to-member, so dispatch on
    // a runtime string lands on real typed code with no per-type branching at the
    // call site.
    struct FieldEntry
    {
        std::string Component;
        std::string Field;
        std::string Type; // FieldTypeName<F>()

        // Coerce `value`, no-op-detect, and (when changed) push a single undoable
        // ComponentChangeCommand<C>. The entity is already resolved + known to exist.
        std::function<ApplyResult(const Ref<Scene>&, CommandHistory&, Entity, u64, const Json&)> Apply;
        // The field's current value as JSON, or nullopt when the entity lacks C.
        std::function<std::optional<Json>(Entity)> Read;
    };

    // Build one registry entry from a component type + a script-facing field name +
    // a pointer-to-member. The field type F is deduced, which selects the coercion /
    // serialization / change-detection specializations above.
    template<typename C, typename F>
    [[nodiscard]] inline FieldEntry MakeField(std::string component, std::string field, F C::* member)
    {
        FieldEntry entry;
        entry.Component = component;
        entry.Field = field;
        entry.Type = std::string(FieldTypeName<F>());

        entry.Apply = [member, component, field](const Ref<Scene>& scene, CommandHistory& history,
                                                 Entity entity, u64 uuid, const Json& value) -> ApplyResult
        {
            ApplyResult result;
            if (!entity.HasComponent<C>())
            {
                result.Error = "Entity has no " + component + ".";
                return result;
            }

            F coerced{};
            if (const auto error = CoerceJson<F>(value, coerced))
            {
                result.Error = "Invalid 'value' for " + component + "." + field +
                               " (expects " + std::string(FieldTypeName<F>()) + "): " + *error + ".";
                return result;
            }

            const C& current = entity.GetComponent<C>();
            const Json previousValue = FieldToJson<F>(current.*member);
            const bool changed = FieldChanged<F>(current.*member, coerced);
            if (changed)
            {
                C newData = current;
                newData.*member = coerced;
                history.Execute(std::make_unique<ComponentChangeCommand<C>>(
                    scene, UUID(uuid), current, newData, "Set " + component + "." + field));
            }

            result.Ok = true;
            result.Data = Json{
                { "entity", std::to_string(uuid) },
                { "component", component },
                { "field", field },
                { "type", std::string(FieldTypeName<F>()) },
                { "previousValue", previousValue },
                { "value", FieldToJson<F>(coerced) },
                { "changed", changed },
                // `undoable` reflects whether a command was actually pushed (a no-op
                // change pushes nothing): a single Ctrl-Z reverts only when changed.
                { "undoable", changed },
            };
            return result;
        };

        entry.Read = [member](Entity entity) -> std::optional<Json>
        {
            if (!entity.HasComponent<C>())
                return std::nullopt;
            return FieldToJson<F>(entity.GetComponent<C>().*member);
        };

        return entry;
    }

    // The curated set of writable fields. Field names are the m_-stripped keys an
    // agent already sees in olo_scene_get_entity's YAML; the component name is the
    // struct's name (also its serializer key), stringized from the type by the
    // helper macro so the two can't drift. Extend by adding an OLO_GFW_FIELD line
    // (and confirming the member exists in Components.h).
    //
    // OLO_GFW_FIELD(Comp, "FieldName", Member) registers Comp::Member under the
    // component name "Comp" and the script-facing field name "FieldName". Scoped to
    // this function and #undef'd at the end so it never leaks out of the header.
#define OLO_GFW_FIELD(Comp, FieldName, Member) MakeField<Comp>(#Comp, FieldName, &Comp::Member)
    [[nodiscard]] inline std::vector<FieldEntry> BuildRegistry()
    {
        std::vector<FieldEntry> registry;

        // Transform (rotation is a derived euler/quat pair behind setters, not a
        // plain member, so it is intentionally not here — translation + scale are).
        registry.push_back(OLO_GFW_FIELD(TransformComponent, "Translation", Translation));
        registry.push_back(OLO_GFW_FIELD(TransformComponent, "Scale", Scale));

        // Identity / labels.
        registry.push_back(OLO_GFW_FIELD(TagComponent, "Tag", Tag));

        // 2D renderers.
        registry.push_back(OLO_GFW_FIELD(SpriteRendererComponent, "Color", Color));
        registry.push_back(OLO_GFW_FIELD(SpriteRendererComponent, "TilingFactor", TilingFactor));
        registry.push_back(OLO_GFW_FIELD(CircleRendererComponent, "Color", Color));
        registry.push_back(OLO_GFW_FIELD(CircleRendererComponent, "Thickness", Thickness));
        registry.push_back(OLO_GFW_FIELD(CircleRendererComponent, "Fade", Fade));

        // Camera flags (projection/FOV live behind SceneCamera setters).
        registry.push_back(OLO_GFW_FIELD(CameraComponent, "Primary", Primary));
        registry.push_back(OLO_GFW_FIELD(CameraComponent, "FixedAspectRatio", FixedAspectRatio));

        // Lights — the highest-value iteration knobs for a rendering agent.
        registry.push_back(OLO_GFW_FIELD(DirectionalLightComponent, "Color", m_Color));
        registry.push_back(OLO_GFW_FIELD(DirectionalLightComponent, "Intensity", m_Intensity));
        registry.push_back(OLO_GFW_FIELD(DirectionalLightComponent, "CastShadows", m_CastShadows));

        registry.push_back(OLO_GFW_FIELD(PointLightComponent, "Color", m_Color));
        registry.push_back(OLO_GFW_FIELD(PointLightComponent, "Intensity", m_Intensity));
        registry.push_back(OLO_GFW_FIELD(PointLightComponent, "Range", m_Range));
        registry.push_back(OLO_GFW_FIELD(PointLightComponent, "CastShadows", m_CastShadows));

        registry.push_back(OLO_GFW_FIELD(SpotLightComponent, "Color", m_Color));
        registry.push_back(OLO_GFW_FIELD(SpotLightComponent, "Intensity", m_Intensity));
        registry.push_back(OLO_GFW_FIELD(SpotLightComponent, "Range", m_Range));
        registry.push_back(OLO_GFW_FIELD(SpotLightComponent, "InnerCutoff", m_InnerCutoff));
        registry.push_back(OLO_GFW_FIELD(SpotLightComponent, "OuterCutoff", m_OuterCutoff));
        registry.push_back(OLO_GFW_FIELD(SpotLightComponent, "CastShadows", m_CastShadows));

        registry.push_back(OLO_GFW_FIELD(SphereAreaLightComponent, "Color", m_Color));
        registry.push_back(OLO_GFW_FIELD(SphereAreaLightComponent, "Intensity", m_Intensity));
        registry.push_back(OLO_GFW_FIELD(SphereAreaLightComponent, "Radius", m_Radius));
        registry.push_back(OLO_GFW_FIELD(SphereAreaLightComponent, "Range", m_Range));
        registry.push_back(OLO_GFW_FIELD(SphereAreaLightComponent, "CastShadows", m_CastShadows));

        return registry;
    }
#undef OLO_GFW_FIELD

    // The process-wide registry, built once. `inline` => one shared instance across
    // every TU that includes this header (McpTools.cpp + the test binary).
    [[nodiscard]] inline const std::vector<FieldEntry>& Registry()
    {
        static const std::vector<FieldEntry> registry = BuildRegistry();
        return registry;
    }

    // Look up the entry for (component, field), or nullptr. Pointers into the static
    // registry are stable for the process lifetime.
    [[nodiscard]] inline const FieldEntry* Find(std::string_view component, std::string_view field)
    {
        for (const FieldEntry& entry : Registry())
        {
            if (entry.Component == component && entry.Field == field)
                return &entry;
        }
        return nullptr;
    }

    // The distinct editable component type names, in first-seen order.
    [[nodiscard]] inline std::vector<std::string> EditableComponents()
    {
        std::vector<std::string> names;
        for (const FieldEntry& entry : Registry())
        {
            if (std::find(names.begin(), names.end(), entry.Component) == names.end())
                names.push_back(entry.Component);
        }
        return names;
    }

    // The editable field names for one component (empty if the component is unknown).
    [[nodiscard]] inline std::vector<std::string> EditableFieldsFor(std::string_view component)
    {
        std::vector<std::string> fields;
        for (const FieldEntry& entry : Registry())
        {
            if (entry.Component == component)
                fields.push_back(entry.Field);
        }
        return fields;
    }

    // ---- schema + arg parsing (write tool) -----------------------------------

    namespace Detail
    {
        [[nodiscard]] inline std::string Join(const std::vector<std::string>& items)
        {
            std::string out;
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                if (i != 0)
                    out += ", ";
                out += items[i];
            }
            return out;
        }
    } // namespace Detail

    // A precise error for an unresolved (component, field): distinguishes an unknown
    // component from a known component lacking that field, and lists the valid
    // alternatives so an agent can self-correct without a round-trip.
    [[nodiscard]] inline std::string DescribeUnknownField(const std::string& component, const std::string& field)
    {
        const std::vector<std::string> fields = EditableFieldsFor(component);
        if (fields.empty())
            return "Unknown or non-editable component '" + component +
                   "'. Editable components: " + Detail::Join(EditableComponents()) + ".";
        return "Component '" + component + "' has no editable field '" + field +
               "'. Editable fields: " + Detail::Join(fields) + ".";
    }

    // The write tool's inputSchema. `value` is intentionally typed as a union of the
    // shapes a field can take (boolean / number / string / array) so the server's
    // pre-handler validation (#423) rejects an object/null up front, while the exact
    // per-field type is enforced by CoerceJson in the handler.
    [[nodiscard]] inline Json InputSchema()
    {
        return Schema::Object()
            .Prop("entity", Schema::EntityId("UUID of the entity to modify (a decimal-digit string)."))
            .Prop("component", Schema::String().Desc("Component type name, e.g. 'TransformComponent' or 'PointLightComponent'. Discover writable components/fields with olo_entity_list_fields."))
            .Prop("field", Schema::String().Desc("Field name to set, e.g. 'Translation' or 'Intensity' — the m_-stripped name shown in olo_scene_get_entity's YAML."))
            .Prop("value", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string", "array" }) } })
                               .Desc("New value, typed to match the field: a boolean, a number, a string, or an array of numbers for a vector (e.g. [x, y, z] for a vec3)."))
            .Required({ "entity", "component", "field", "value" })
            .NoAdditional();
    }

    // The read companion (olo_entity_list_fields) inputSchema.
    [[nodiscard]] inline Json ListInputSchema()
    {
        return Schema::Object()
            .Prop("entity", Schema::EntityId("UUID of the entity whose writable fields to list (a decimal-digit string)."))
            .Prop("component", Schema::String().Desc("Optional: restrict the listing to one component type name."))
            .Required({ "entity" })
            .NoAdditional();
    }

    // Parse the write tool's arguments. The server validates against InputSchema()
    // before the handler runs; this stays defensive (it is also reached from tests).
    // `value` is left as raw Json (its type depends on the target field).
    [[nodiscard]] inline std::optional<std::string> ParseArgs(const Json& args, u64& entityUuid,
                                                              std::string& component, std::string& field, Json& value)
    {
        if (!args.contains("entity"))
            return "Missing required argument 'entity' (entity UUID).";
        const Json& entityValue = args["entity"];
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

        if (!args.contains("component") || !args["component"].is_string())
            return "Missing or invalid required argument 'component' (a component type name).";
        component = args["component"].get<std::string>();
        if (component.empty())
            return "Invalid 'component': must be a non-empty component type name.";

        if (!args.contains("field") || !args["field"].is_string())
            return "Missing or invalid required argument 'field' (a field name).";
        field = args["field"].get<std::string>();
        if (field.empty())
            return "Invalid 'field': must be a non-empty field name.";

        if (!args.contains("value"))
            return "Missing required argument 'value'.";
        value = args["value"];
        return std::nullopt;
    }

    // ---- apply / list --------------------------------------------------------

    // Set one component field through `history` (a single undoable command). MUST run
    // on the main (game) thread — it touches the EnTT registry and the editor command
    // stack. Resolves the entry + entity centrally so the type-specific work in the
    // entry's closure only handles the component it was built for.
    [[nodiscard]] inline ApplyResult Apply(const Ref<Scene>& scene, CommandHistory& history, u64 entityUuid,
                                           const std::string& component, const std::string& field, const Json& value)
    {
        ApplyResult result;
        if (!scene)
        {
            result.Error = "No active scene.";
            return result;
        }

        const FieldEntry* entry = Find(component, field);
        if (entry == nullptr)
        {
            result.Error = DescribeUnknownField(component, field);
            return result;
        }

        const auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityUuid));
        if (!entityOpt)
        {
            result.Error = "No entity with UUID " + std::to_string(entityUuid) + " in the active scene.";
            return result;
        }

        return entry->Apply(scene, history, *entityOpt, entityUuid, value);
    }

    // List the writable fields of one entity, with their current values, restricted
    // to `componentFilter` when non-empty. Only components the entity actually HAS
    // (and that have registered fields) are reported, so the output is exactly what
    // an agent can write right now. `*entityFound` reports whether the UUID resolved.
    [[nodiscard]] inline Json ListFields(const Ref<Scene>& scene, u64 entityUuid,
                                         const std::string& componentFilter, bool& entityFound)
    {
        entityFound = false;
        Json out;
        out["entity"] = std::to_string(entityUuid);

        if (!scene)
        {
            out["found"] = false;
            out["components"] = Json::array();
            return out;
        }
        const auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityUuid));
        if (!entityOpt)
        {
            out["found"] = false;
            out["components"] = Json::array();
            return out;
        }
        entityFound = true;
        out["found"] = true;

        Entity entity = *entityOpt;
        Json components = Json::array();
        // Walk the editable component names in order; for each (optionally matching
        // the filter), collect the fields the entity currently exposes.
        for (const std::string& componentName : EditableComponents())
        {
            if (!componentFilter.empty() && componentFilter != componentName)
                continue;

            Json fields = Json::array();
            for (const FieldEntry& entry : Registry())
            {
                if (entry.Component != componentName)
                    continue;
                const std::optional<Json> current = entry.Read(entity);
                if (!current) // entity lacks this component
                    break;
                fields.push_back(Json{ { "field", entry.Field },
                                       { "type", entry.Type },
                                       { "value", *current } });
            }
            if (!fields.empty())
                components.push_back(Json{ { "component", componentName }, { "fields", std::move(fields) } });
        }
        out["components"] = std::move(components);
        return out;
    }
} // namespace OloEngine::MCP::GenericFieldWrite
