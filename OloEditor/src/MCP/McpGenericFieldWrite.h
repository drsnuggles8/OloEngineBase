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
// The field registry is GENERATED (issue #607)
//   C++ has no runtime reflection, so each entry still pairs a script-facing name
//   with a real pointer-to-member and a templated closure that does the JSON->field
//   coercion, the range clamp, the no-op detection, and the undoable command build.
//   What changed is where the list comes from: BuildRegistry() now #includes
//   MCP/Generated/McpFieldRegistry.Generated.inl, emitted by OloHeaderTool from the
//   SAME full data-member scan that drives the scene serializer. The original
//   hand-written list covered 9 components out of ~100 and quietly rotted as
//   components were added (VirtualMeshComponent, MeshComponent, the physics bodies,
//   … were all unreachable), which is what motivated the codegen.
//
//   The generator emits one entry per PUBLIC, JSON-coercible member of every
//   `struct *Component`, minus its runtime-only exclusion set (the *StateComponent
//   family, UIResolvedRectComponent, WorldTransformComponent, IDComponent) and minus
//   any field marked OLO_SERIALIZE(Skip) or typed with no scalar JSON shape (u64 /
//   ivec / quat / mat / nested struct / Ref<T> / any container). The field NAMES are
//   the m_-stripped keys an agent already sees in `olo_scene_get_entity`'s YAML, so
//   the write contract lines up with the read contract.
//
//   RANGES: a field whose scene-serializer load path clamps or rejects out-of-range
//   values carries the same bounds here (from its OLO_SERIALIZE(Clamp, Min=…, Max=…)
//   annotation, or from the generator's kMcpFieldClamps table for a component the
//   serializer keeps hand-written). A write outside the range is CLAMPED, not
//   silently accepted, so MCP can never put a component into a state a scene load
//   could not produce — and the response says `clamped: true` with the original
//   `requestedValue`.
//
//   HONESTY: the write result echoes the value READ BACK from the live component
//   after the command ran (not the value we intended to write) plus an explicit
//   `changed` flag, so a caller can verify a write actually took effect instead of
//   trusting that the call returned successfully.

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

    // ---- field ranges (mirroring the scene serializer's load-time clamps) -----

    // An inclusive [Min, Max] range on a numeric field — either bound may be absent
    // (a one-sided floor / ceiling). Bounds travel as `double`: every rangeable field
    // type (f32 / i32 / u32 / small int / enum underlying / a float vector component)
    // is exactly representable in a double, so the clamp is lossless.
    struct FieldRange
    {
        std::optional<double> Min;
        std::optional<double> Max;

        [[nodiscard]] bool IsEmpty() const
        {
            return !Min && !Max;
        }
    };

    [[nodiscard]] inline double ApplyBounds(double v, const FieldRange& range)
    {
        if (range.Min && v < *range.Min)
            v = *range.Min;
        if (range.Max && v > *range.Max)
            v = *range.Max;
        return v;
    }

    // Range the coerced value in place. A glm vector clamps component-wise (the same
    // semantics as the serializer's OLO_SERIALIZE(Clamp) on a glm::vec3, which emits
    // a glm::clamp). bool / string / handle have no meaningful range — the generator
    // never emits one for them, and this is a no-op if one ever appeared.
    template<typename F>
    inline void RangeField(F& value, const FieldRange& range)
    {
        if (range.IsEmpty())
            return;

        if constexpr (std::is_same_v<F, bool> || std::is_same_v<F, std::string> || std::is_same_v<F, UUID>)
        {
            // no ordering contract — intentionally unclamped
        }
        else if constexpr (std::is_enum_v<F>)
        {
            using U = std::underlying_type_t<F>;
            const double ranged = ApplyBounds(static_cast<double>(static_cast<U>(value)), range);
            value = static_cast<F>(static_cast<U>(ranged));
        }
        else if constexpr (std::is_same_v<F, glm::vec2> || std::is_same_v<F, glm::vec3> || std::is_same_v<F, glm::vec4>)
        {
            for (glm::length_t i = 0; i < static_cast<glm::length_t>(F::length()); ++i)
                value[i] = static_cast<float>(ApplyBounds(static_cast<double>(value[i]), range));
        }
        else if constexpr (std::is_arithmetic_v<F>)
        {
            value = static_cast<F>(ApplyBounds(static_cast<double>(value), range));
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

    namespace Detail
    {
        // Shared result-building tail for MakeFieldAccess::Apply and
        // MakeSetterField::Apply below — both coerce, range-clamp, and no-op-detect
        // the value THE SAME WAY and differ only in how the new value gets written
        // into the live component (a whole-object copy+swap vs. a direct setter
        // call). Keeping the JSON shape in one place means the two write paths can't
        // silently drift on what "changed"/"clamped"/"undoable" mean.
        template<typename F>
        [[nodiscard]] inline ApplyResult BuildFieldApplyResult(const std::string& component, const std::string& field,
                                                               u64 uuid, const F& requested, const F& previous,
                                                               const F& applied, bool clamped)
        {
            ApplyResult result;
            result.Ok = true;
            const bool changed = FieldChanged<F>(previous, applied);
            result.Data = Json{
                { "entity", std::to_string(uuid) },
                { "component", component },
                { "field", field },
                { "type", std::string(FieldTypeName<F>()) },
                { "previousValue", FieldToJson<F>(previous) },
                // Read back from the component AFTER the write, not the coerced input.
                { "value", FieldToJson<F>(applied) },
                { "changed", changed },
                // `undoable` reflects whether a command was actually pushed (a no-op
                // write pushes nothing): a single Ctrl-Z reverts only when changed.
                { "undoable", changed },
                // True when the requested value was outside the field's serializer-
                // enforced range and was clamped into it.
                { "clamped", clamped },
            };
            if (clamped)
                result.Data["requestedValue"] = FieldToJson<F>(requested);
            return result;
        }
    } // namespace Detail

    // One writable (component, field) pair, type-erased over the field type. The
    // closures bake in the component type C and a pointer-to-member, so dispatch on
    // a runtime string lands on real typed code with no per-type branching at the
    // call site.
    struct FieldEntry
    {
        std::string Component;
        std::string Field;
        std::string Type; // FieldTypeName<F>()
        FieldRange Range; // empty unless the serializer enforces a load-time range

        // Whether the entity currently carries the owning component C — checked
        // BEFORE Read/Apply (or ReadKeyed/ApplyKeyed) so ListFields can tell "entity
        // lacks this component" from every other outcome (an empty map, a false
        // bool, ...) uniformly across both plain and map-keyed fields.
        std::function<bool(Entity)> HasComponent;

        // Coerce `value`, range-clamp it, no-op-detect, and (when changed) push a
        // single undoable ComponentChangeCommand<C>. The entity is already resolved +
        // known to exist. The reported `value` is READ BACK from the live component
        // after the command ran, never the value we merely intended to write.
        std::function<ApplyResult(const Ref<Scene>&, CommandHistory&, Entity, u64, const Json&)> Apply;
        // The field's current value as JSON, or nullopt when the entity lacks C.
        std::function<std::optional<Json>(Entity)> Read;

        // ---- map-key addressing (issue #607's MorphTargetComponent::Weights slice) --
        //
        // A MAP-typed field has no compile-time-known key — MorphTargetComponent's
        // Weights (target/bone name -> weight) doesn't even have its keyset known
        // until a MorphTargetSet is bound at runtime — so it cannot be a plain
        // FieldEntry (Apply/Read above assume a single addressable F& per entity).
        // Instead ONE entry is registered per map FIELD (not per key), with
        // IsMapKeyed=true and Field holding the PREFIX ("Weights", no trailing dot).
        // Find() deliberately never matches this entry by itself (Field alone names
        // the container, not a value) — resolution instead happens against the
        // caller's dotted field string "Weights.<key>" via FindMapKeyed(), which
        // strips the entry's Field+"." prefix off to recover the runtime key. This
        // mirrors the existing dotted SUB-OBJECT ADDRESSING convention
        // ("System.Emitter.RateOverTime") rather than inventing a second argument.
        bool IsMapKeyed = false;
        // Same contract as Apply above, plus the resolved runtime key.
        std::function<ApplyResult(const Ref<Scene>&, CommandHistory&, Entity, u64, const std::string& key, const Json&)> ApplyKeyed;
        // Same contract as Read above, plus the resolved runtime key.
        std::function<std::optional<Json>(Entity, const std::string& key)> ReadKeyed;
        // The map's CURRENT keys (unsorted; callers sort for deterministic output) —
        // drives olo_entity_list_fields' "exactly what you can write right now"
        // contract for a map field, since no key is known ahead of time.
        std::function<std::vector<std::string>(Entity)> ListMapKeys;
    };

    // Build one registry entry from a component type + a script-facing field name +
    // an ACCESSOR (a callable `C& -> F&`) (+ an optional load-time range). The field
    // type F is deduced from the accessor's return type, which selects the coercion /
    // serialization / change-detection / clamping specializations above.
    //
    // The accessor — rather than a bare pointer-to-member — is what makes a NESTED
    // field addressable: the generator emits `[](C& c) -> auto& { return c.System.Emitter.RateOverTime; }`
    // for the dotted field name "System.Emitter.RateOverTime", and everything below
    // (coerce, clamp, no-op detect, undo command, read-back) is identical to a
    // top-level field. A plain member still goes through the pointer-to-member
    // overload right underneath, which just wraps it in the same accessor shape.
    template<typename C, typename Access>
    [[nodiscard]] inline FieldEntry MakeFieldAccess(std::string component, std::string field, Access access,
                                                    FieldRange range = {})
    {
        using F = std::remove_cvref_t<decltype(access(std::declval<C&>()))>;
        static_assert(std::is_lvalue_reference_v<decltype(access(std::declval<C&>()))>,
                      "MakeFieldAccess: the accessor must return an lvalue reference into the component");

        FieldEntry entry;
        entry.Component = component;
        entry.Field = field;
        entry.Type = std::string(FieldTypeName<F>());
        entry.Range = range;
        entry.HasComponent = [](Entity e)
        { return e.HasComponent<C>(); };

        entry.Apply = [access, component, field, range](const Ref<Scene>& scene, CommandHistory& history,
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

            // Range the request the same way a scene load would, and remember whether
            // that actually moved the value so the caller is told (never silently).
            const F requested = coerced;
            RangeField<F>(coerced, range);
            const bool clamped = FieldChanged<F>(requested, coerced);

            // Copy the previous value out BEFORE the command runs: the command
            // replaces the component, so a reference into it would be stale after.
            // Predicting `willChange` from previous-vs-coerced (rather than applying
            // first and checking previous-vs-applied, as MakeSetterField below does)
            // is deliberate here, not an inconsistency: `access(newData) = coerced` is
            // a bare assignment, so applied is ALWAYS exactly coerced — the prediction
            // can't diverge from the outcome. Applying unconditionally first would
            // instead mean invoking the component's operator= (inside
            // ComponentChangeCommand<C>::Execute) even for a genuine no-op, which is
            // exactly the kind of unwanted side effect this whole write path exists to
            // avoid for a component like AudioSourceComponent (see MakeSetterField).
            const F previous = access(entity.GetComponent<C>());
            const bool willChange = FieldChanged<F>(previous, coerced);
            if (willChange)
            {
                const C& current = entity.GetComponent<C>();
                C newData = current;
                access(newData) = coerced;
                history.Execute(std::make_unique<ComponentChangeCommand<C>>(
                    scene, UUID(uuid), current, newData, "Set " + component + "." + field));
            }

            // Read the value BACK OUT of the live component — the honest answer to
            // "did the write take effect?". If the command silently failed to apply,
            // `value` shows the old value and `changed` is false.
            const F applied = access(entity.GetComponent<C>());
            return Detail::BuildFieldApplyResult<F>(component, field, uuid, requested, previous, applied, clamped);
        };

        entry.Read = [access](Entity entity) -> std::optional<Json>
        {
            if (!entity.HasComponent<C>())
                return std::nullopt;
            return FieldToJson<F>(access(entity.GetComponent<C>()));
        };

        return entry;
    }

    // Pointer-to-member convenience overload — the shape the hand-written registry
    // used and the one the MCP tests exercise directly. Wraps the member pointer in
    // the accessor above so there is exactly one implementation of the write path.
    template<typename C, typename F>
    [[nodiscard]] inline FieldEntry MakeField(std::string component, std::string field, F C::* member,
                                              FieldRange range = {})
    {
        return MakeFieldAccess<C>(std::move(component), std::move(field), [member](C& c) -> F&
                                  { return c.*member; }, range);
    }

    // Build one registry entry from a component type + a script-facing field name +
    // a Get/Set EXPRESSION PAIR compiled from the SAME OLO_PROPERTY annotation that
    // drives Lua/C# scripting (issue #607's AudioSourceComponent slice). Unlike
    // MakeFieldAccess/MakeField above — which copy the whole component, mutate the
    // copy, and swap it in via ComponentChangeCommand<C> — this calls `setFn`
    // DIRECTLY on the live component on both Execute and Undo, through
    // PropertySetCommand<C, F, SetFn>. No whole-object copy or assignment ever
    // happens.
    //
    // This is required whenever a component's operator= cannot be trusted to
    // preserve runtime-only state or to push a value into a live subsystem handle:
    // AudioSourceComponent's operator= resets ActiveEventID to 0 and never
    // re-invokes Source->SetVolume()/SetPitch()/etc, so routing an MCP write through
    // the whole-object path would silently detach a playing sound AND leave its
    // actual playback parameter unchanged even though the reported "value" looked
    // right. `getFn`/`setFn` are generated from the exact `Get =` / `Set =`
    // expression strings the OLO_PROPERTY annotation already carries for Lua/C#
    // (OloHeaderTool's EmitMcpSetterFields reuses ParseHeaders' scan output — no
    // second parser).
    //
    // Reserved for fields the plain member/nested-member scan (MakeFieldAccess)
    // cannot reach AT ALL — a private field behind a getter/setter pair. A component
    // with a mix of plain public fields and OLO_PROPERTY setters keeps using
    // MakeFieldAccess for the former; OloHeaderTool's emitter skips a property here
    // that the field scan already exposes, so a (component, field) pair is never
    // registered twice.
    template<typename C, typename F, typename GetFn, typename SetFn>
    [[nodiscard]] inline FieldEntry MakeSetterField(std::string component, std::string field,
                                                    GetFn getFn, SetFn setFn, FieldRange range = {})
    {
        static_assert(std::is_same_v<std::invoke_result_t<GetFn, const C&>, F>,
                      "MakeSetterField: getFn must return exactly F");

        FieldEntry entry;
        entry.Component = component;
        entry.Field = field;
        entry.Type = std::string(FieldTypeName<F>());
        entry.Range = range;
        entry.HasComponent = [](Entity e)
        { return e.HasComponent<C>(); };

        entry.Apply = [getFn, setFn, component, field, range](const Ref<Scene>& scene, CommandHistory& history,
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

            // Range the request the same way a scene load would, and remember whether
            // that actually moved the value so the caller is told (never silently).
            const F requested = coerced;
            RangeField<F>(coerced, range);
            const bool clamped = FieldChanged<F>(requested, coerced);

            // Read the previous value via getFn BEFORE writing — there is no
            // whole-object copy here to read it out of afterward.
            const F previous = getFn(entity.GetComponent<C>());
            F applied = previous;
            if (FieldChanged<F>(previous, coerced))
            {
                // Unlike MakeFieldAccess's plain `access(newData) = coerced` (a bare
                // assignment, always exact), `setFn` is an arbitrary OLO_PROPERTY Set
                // expression that could in principle transform/clamp beyond RangeField
                // above. So apply it FIRST, then read the value BACK OUT of the live
                // component — the honest answer to "did the write take effect, and to
                // what?" — instead of assuming `applied == coerced`. The command is
                // pushed via PushAlreadyExecuted (setFn already ran) and stores the
                // OBSERVED `applied` value, not the merely-requested one, so a later
                // Redo reproduces exactly what was seen the first time.
                setFn(entity.GetComponent<C>(), coerced);
                applied = getFn(entity.GetComponent<C>());
                if (FieldChanged<F>(previous, applied))
                {
                    history.PushAlreadyExecuted(std::make_unique<PropertySetCommand<C, F, SetFn>>(
                        scene, UUID(uuid), setFn, previous, applied, "Set " + component + "." + field));
                }
            }

            return Detail::BuildFieldApplyResult<F>(component, field, uuid, requested, previous, applied, clamped);
        };

        entry.Read = [getFn](Entity entity) -> std::optional<Json>
        {
            if (!entity.HasComponent<C>())
                return std::nullopt;
            return FieldToJson<F>(getFn(entity.GetComponent<C>()));
        };

        return entry;
    }

    // Build one registry entry for a MAP-typed field, addressed at write time by a
    // RUNTIME key rather than a compile-time accessor chain (issue #607's
    // MorphTargetComponent::Weights slice — a target/bone name -> weight map whose
    // keyset doesn't exist until a MorphTargetSet is bound, so no static field name
    // can be generated ahead of time the way a nested struct's dotted chain is).
    //
    // ONE entry is registered per map FIELD (not per key): `field` is the PREFIX
    // ("Weights"), and a caller addresses one entry with the dotted name
    // "Weights.<key>" — resolved at call time by FindMapKeyed(), which strips the
    // registered prefix + "." off the requested field to recover `key`. This reuses
    // the existing dotted SUB-OBJECT ADDRESSING convention
    // ("System.Emitter.RateOverTime") instead of adding a second JSON argument.
    //
    // Like MakeSetterField (not MakeFieldAccess), the write calls `setFn` DIRECTLY on
    // the live component — never a whole-map copy+swap — reusing
    // PropertySetCommand<C, V, SetFn> for undo/redo by binding `key` into the setter
    // closure passed to it.
    //
    // getFn:  (const C&, const std::string& key) -> V   (a missing key returns the
    //         map's own semantic default, e.g. MorphTargetComponent::GetWeight()'s 0.0f)
    // setFn:  (C&, const std::string& key, const V& value) -> void
    // keysFn: (const C&) -> std::vector<std::string>   (the map's CURRENT keys, for
    //         olo_entity_list_fields discovery — order doesn't matter, ListFields sorts)
    template<typename C, typename V, typename GetFn, typename SetFn, typename KeysFn>
    [[nodiscard]] inline FieldEntry MakeMapKeyField(std::string component, std::string field,
                                                    GetFn getFn, SetFn setFn, KeysFn keysFn,
                                                    FieldRange range = {})
    {
        static_assert(std::is_same_v<std::invoke_result_t<GetFn, const C&, const std::string&>, V>,
                      "MakeMapKeyField: getFn must return exactly V");

        FieldEntry entry;
        entry.Component = component;
        entry.Field = field;
        entry.Type = "map<string," + std::string(FieldTypeName<V>()) + ">";
        entry.Range = range;
        entry.IsMapKeyed = true;
        entry.HasComponent = [](Entity e)
        { return e.HasComponent<C>(); };

        entry.ApplyKeyed = [getFn, setFn, component, field, range](const Ref<Scene>& scene, CommandHistory& history,
                                                                   Entity entity, u64 uuid, const std::string& key,
                                                                   const Json& value) -> ApplyResult
        {
            ApplyResult result;
            if (!entity.HasComponent<C>())
            {
                result.Error = "Entity has no " + component + ".";
                return result;
            }

            V coerced{};
            if (const auto error = CoerceJson<V>(value, coerced))
            {
                result.Error = "Invalid 'value' for " + component + "." + field + "." + key +
                               " (expects " + std::string(FieldTypeName<V>()) + "): " + *error + ".";
                return result;
            }

            const V requested = coerced;
            RangeField<V>(coerced, range);
            const bool clamped = FieldChanged<V>(requested, coerced);

            // Same "apply then read back" honesty contract as MakeSetterField: setFn
            // may itself clamp/transform (MorphTargetComponent::SetWeight does), so
            // the reported value comes from getFn AFTER the write, not from `coerced`.
            const V previous = getFn(entity.GetComponent<C>(), key);
            V applied = previous;
            if (FieldChanged<V>(previous, coerced))
            {
                auto keyedSetFn = [key, setFn](C& c, const V& v)
                { setFn(c, key, v); };
                keyedSetFn(entity.GetComponent<C>(), coerced);
                applied = getFn(entity.GetComponent<C>(), key);
                if (FieldChanged<V>(previous, applied))
                {
                    history.PushAlreadyExecuted(std::make_unique<PropertySetCommand<C, V, decltype(keyedSetFn)>>(
                        scene, UUID(uuid), keyedSetFn, previous, applied,
                        "Set " + component + "." + field + "." + key));
                }
            }

            ApplyResult out = Detail::BuildFieldApplyResult<V>(component, field + "." + key, uuid, requested, previous, applied, clamped);
            out.Data["key"] = key;
            return out;
        };

        entry.ReadKeyed = [getFn](Entity entity, const std::string& key) -> std::optional<Json>
        {
            if (!entity.HasComponent<C>())
                return std::nullopt;
            return FieldToJson<V>(getFn(entity.GetComponent<C>(), key));
        };

        entry.ListMapKeys = [keysFn](Entity entity) -> std::vector<std::string>
        {
            if (!entity.HasComponent<C>())
                return {};
            return keysFn(entity.GetComponent<C>());
        };

        return entry;
    }

    // The writable-field registry, GENERATED by OloHeaderTool (issue #607) from the
    // same component data-member scan that drives the scene serializer — see the
    // header comment at the top of this file. Field names are the m_-stripped keys an
    // agent already sees in olo_scene_get_entity's YAML; the component name is the
    // struct's name (also its serializer key), stringized from the type by the helper
    // macro so the two can't drift.
    //
    // The generated .inl is a flat list of:
    //   registry.push_back(OLO_GFW_FIELD(Comp, "FieldName", MemberExpr));
    //   registry.push_back(OLO_GFW_FIELD_RANGE(Comp, "FieldName", MemberExpr, min, max));
    // where a bound is OLO_GFW_BOUND(expr) or OLO_GFW_NO_BOUND. The macros (and the
    // #include of the generated .inl) live in McpFieldRegistry.cpp, file-scoped.
    //
    // SUB-OBJECT ADDRESSING: `MemberExpr` is a member-ACCESS CHAIN, not a single name
    // — `Translation` for a top-level field, `System.Emitter.RateOverTime` for a field
    // that lives inside a nested object — and `FieldName` is the matching dotted key
    // ("System.Emitter.RateOverTime"). The macro pastes the chain into an accessor
    // lambda, so both cases land on the same MakeFieldAccess write path. This is what
    // makes ParticleSystemComponent (whose entire authored surface lives inside a
    // `ParticleSystem System` member) writable; without it that component exposed
    // literally zero fields. The generator derives the chain from the SAME nested-
    // record classification that drives the scene serializer's nested-struct support,
    // so there is no second type classifier to drift.
    //
    // To make a new component/field writable: give it a public data member of a
    // supported type (bool / int / uint / small int / float / glm::vec2|3|4 / enum /
    // std::string / AssetHandle) — directly, or inside a public nested struct/class
    // member — and rebuild GenerateBindings. To keep a runtime-only field out, mark it
    // OLO_SERIALIZE(Skip); to keep a whole runtime-only component out, add it to
    // kComponentsNotMcpEditable in tools/OloHeaderTool/main.cpp.
    //
    // SETTER-BASED FIELDS (issue #607's AudioSourceComponent slice): a field behind a
    // PRIVATE member reached only through an OLO_PROPERTY Get/Set expression pair (no
    // public member/nested-member chain exists at all) is unreachable to the above —
    // AudioSourceComponent's 16 parameters live behind `private
    // std::unique_ptr<AudioSourceColdData> m_Cold`. For those, the generated .inl also
    // contains calls to MakeSetterField<Comp, F>(component, field, getFn, setFn[,
    // range]), whose getFn/setFn are lambdas compiled from the OLO_PROPERTY Get/Set
    // expression strings (OloHeaderTool's EmitMcpSetterFields, scoped to a small
    // allowlist — see that function's comment for why it is not "every OLO_PROPERTY
    // component"). Unlike MakeFieldAccess, MakeSetterField writes through the setter
    // DIRECTLY on the live component (PropertySetCommand, not ComponentChangeCommand<C>)
    // — required because AudioSourceComponent's operator= cannot be trusted to preserve
    // ActiveEventID or push a value into the live Ref<AudioSource> Source.
    //
    // MAP-KEY ADDRESSING (issue #607's MorphTargetComponent::Weights slice): a
    // map-typed field (a runtime keyset, e.g. target/bone name -> weight) has no
    // static field name to register per key. ONE MakeMapKeyField(...) entry is
    // hand-registered per map FIELD in McpFieldRegistry.cpp (not emitted by the
    // generator — there is exactly one such field in the engine today, so this
    // stays a manual addition rather than a speculative "any map field" codegen
    // classifier). A caller addresses one entry with a DOTTED key
    // ("Weights.Smile"), resolved by FindMapKeyed() below; Find() never matches a
    // map field's bare prefix ("Weights" alone names the container, not a value).
    //
    // The process-wide registry, built once. DEFINED in McpFieldRegistry.cpp —
    // the one TU that #includes the generated .inl. It used to be built by an
    // inline BuildRegistry() right here, but the ~100-component registry is
    // thousands of MakeFieldAccess/MakeSetterField instantiations (two lambdas
    // each), re-instantiated in EVERY includer of this header (McpToolsScene.cpp
    // + two MCP test TUs). Under clang-cl Release+ASan that pushed
    // McpToolsScene.cpp past the CI runner's memory ("LLVM ERROR: out of
    // memory") once the #635 + #633 component additions landed; a dedicated TU
    // compiles the instantiation storm exactly once, next to nothing else.
    // Both consumers link it: OloEditor (OloEditor/src/CMakeLists.txt) and
    // OloEngine-Tests (OloEngine/tests/CMakeLists.txt, MCP section).
    [[nodiscard]] const std::vector<FieldEntry>& Registry();

    // Look up the entry for (component, field), or nullptr. Pointers into the static
    // registry are stable for the process lifetime. Deliberately never matches a
    // map-keyed entry by its bare prefix — "Weights" alone names the container, not
    // an addressable value; use FindMapKeyed() for "Weights.<key>" instead.
    [[nodiscard]] inline const FieldEntry* Find(std::string_view component, std::string_view field)
    {
        for (const FieldEntry& entry : Registry())
        {
            if (!entry.IsMapKeyed && entry.Component == component && entry.Field == field)
                return &entry;
        }
        return nullptr;
    }

    // Resolve a dotted "<mapField>.<key>" against COMPONENT's registered map fields:
    // on a match, `outKey` receives the runtime key (the remainder after the longest
    // matching "<mapField>." prefix — longest wins only in case a component ever
    // registers more than one map field with an overlapping prefix, which does not
    // happen today) and the owning entry is returned. nullptr if `field` names no
    // known map field's prefix (either the component has none, or nothing follows
    // the dot).
    [[nodiscard]] inline const FieldEntry* FindMapKeyed(std::string_view component, std::string_view field, std::string& outKey)
    {
        const FieldEntry* best = nullptr;
        for (const FieldEntry& entry : Registry())
        {
            if (!entry.IsMapKeyed || entry.Component != component)
                continue;
            const std::string prefix = entry.Field + ".";
            if (field.size() <= prefix.size() || field.substr(0, prefix.size()) != prefix)
                continue;
            if (best == nullptr || entry.Field.size() > best->Field.size())
            {
                best = &entry;
                outKey = std::string(field.substr(prefix.size()));
            }
        }
        return best;
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

    // The editable field names for one component (empty if the component is
    // unknown). A map-keyed field is listed as "<field>.<key>" (e.g. "Weights.<key>")
    // so the placeholder form is self-explanatory in an error/discovery message
    // without implying the bare prefix is itself writable.
    [[nodiscard]] inline std::vector<std::string> EditableFieldsFor(std::string_view component)
    {
        std::vector<std::string> fields;
        for (const FieldEntry& entry : Registry())
        {
            if (entry.Component == component)
                fields.push_back(entry.IsMapKeyed ? (entry.Field + ".<key>") : entry.Field);
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
            .Prop("field", Schema::String().Desc("Field name to set, e.g. 'Translation' or 'Intensity' — the m_-stripped name shown in olo_scene_get_entity's YAML. For a map-typed field (e.g. MorphTargetComponent's 'Weights'), address one entry with a dotted key, e.g. 'Weights.Smile' — discover current keys with olo_entity_list_fields."))
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

        if (const FieldEntry* entry = Find(component, field))
        {
            const auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityUuid));
            if (!entityOpt)
            {
                result.Error = "No entity with UUID " + std::to_string(entityUuid) + " in the active scene.";
                return result;
            }
            return entry->Apply(scene, history, *entityOpt, entityUuid, value);
        }

        // Not a plain field — try a map-keyed one ("Weights.Smile" against the
        // registered "Weights" prefix) before giving up.
        std::string key;
        if (const FieldEntry* mapEntry = FindMapKeyed(component, field, key))
        {
            const auto entityOpt = scene->TryGetEntityWithUUID(UUID(entityUuid));
            if (!entityOpt)
            {
                result.Error = "No entity with UUID " + std::to_string(entityUuid) + " in the active scene.";
                return result;
            }
            return mapEntry->ApplyKeyed(scene, history, *entityOpt, entityUuid, key, value);
        }

        result.Error = DescribeUnknownField(component, field);
        return result;
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
                if (!entry.HasComponent(entity)) // entity lacks this component
                    break;

                if (entry.IsMapKeyed)
                {
                    // No key is known ahead of time — expand into one discoverable
                    // dotted field per CURRENT map entry ("Weights.Smile", ...),
                    // exactly what an agent can re-issue as a write right now.
                    std::vector<std::string> keys = entry.ListMapKeys(entity);
                    std::sort(keys.begin(), keys.end());
                    for (const std::string& mapKey : keys)
                    {
                        Json field = Json{ { "field", entry.Field + "." + mapKey },
                                           { "type", entry.Type },
                                           { "value", *entry.ReadKeyed(entity, mapKey) } };
                        if (entry.Range.Min)
                            field["min"] = *entry.Range.Min;
                        if (entry.Range.Max)
                            field["max"] = *entry.Range.Max;
                        fields.push_back(std::move(field));
                    }
                    continue;
                }

                const std::optional<Json> current = entry.Read(entity);
                if (!current) // shouldn't happen — HasComponent already checked above
                    break;
                Json field = Json{ { "field", entry.Field },
                                   { "type", entry.Type },
                                   { "value", *current } };
                // Surface the serializer-enforced range so an agent knows the valid
                // interval BEFORE writing (a write outside it is clamped, not refused).
                if (entry.Range.Min)
                    field["min"] = *entry.Range.Min;
                if (entry.Range.Max)
                    field["max"] = *entry.Range.Max;
                fields.push_back(std::move(field));
            }
            if (!fields.empty())
                components.push_back(Json{ { "component", componentName }, { "fields", std::move(fields) } });
        }
        out["components"] = std::move(components);
        return out;
    }
} // namespace OloEngine::MCP::GenericFieldWrite
