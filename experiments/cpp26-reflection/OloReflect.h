#pragma once
// ============================================================================
//  OloReflect.h  —  C++26-reflection replacement for OloHeaderTool's engine-side
//  codegen. GCC 16.1+ only (-std=c++26 -freflection). One header of generic code
//  subsumes:
//    - the whole-source-tree `struct *Component` scan + AllComponents.Generated.inl
//    - Scene{Serialize,Deserialize}Components.Generated.inl  (via visit_fields)
//    - SaveGameComponent{Capture,Restore}.Generated.inl      (via for_each_component)
//    - SceneBinary{Read,Write,Covered}Components.Generated.inl (same member-walk)
//    - OnComponent{Added,Removed}.Generated.inl  -> DELETED (generic no-op primary)
//  Out of scope on GCC: the C# .cs bindings (reflection can't emit C# source) and
//  the MCP field registry (editor is MSVC-only).
//
//  NOTE: components must be declared (in namespace OloEngine) BEFORE this header
//  is included, so the namespace scan sees them.
// ============================================================================
// NOTE: GCC 16.1 enables reflection via -freflection but does not (yet) define a
// __cpp_reflection feature macro, so we can't #error-guard on it. In the real engine
// this header would be included only from the GCC/experimental build.

#include <meta>
#include "OloReflectAnnotations.h"   // Clamp / Skip annotation types + OLO_CLAMP / OLO_SKIP
#include <vector>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace OloEngine::Reflect
{
    namespace sm = std::meta;

    // annotation types (Clamp/Skip) come from OloReflectAnnotations.h, included
    // above so components can be annotated before this header runs discovery.

    // -------- per-field helpers -------------------------------------------------
    consteval std::string_view KeyOf(std::string_view id)
    {   // strip a leading "m_", exactly like OloHeaderTool
        return (id.size() > 2 && id[0] == 'm' && id[1] == '_') ? id.substr(2) : id;
    }
    consteval bool  HasSkip (sm::info m) { return !sm::annotations_of_with_type(m, ^^Skip ).empty(); }
    // The YAML key for a member: a Reflect::Key(name) rename if present, else m_-stripped.
    consteval std::string_view KeyOfMember(sm::info m) {
        auto anns = sm::annotations_of_with_type(m, ^^Key);
        if (!anns.empty()) {
            Key k = sm::extract<Key>(anns[0]);
            return std::string_view(std::define_static_string(std::string_view(k.name)));  // promote to static storage
        }
        return KeyOf(sm::identifier_of(m));
    }
    consteval bool  HasClamp(sm::info m) { return !sm::annotations_of_with_type(m, ^^Clamp).empty(); }
    consteval Clamp GetClamp(sm::info m) { return sm::extract<Clamp>(sm::annotations_of_with_type(m, ^^Clamp)[0]); }
    consteval bool   HasReject(sm::info m) { return !sm::annotations_of_with_type(m, ^^Reject).empty(); }
    consteval Reject GetReject(sm::info m) { return sm::extract<Reject>(sm::annotations_of_with_type(m, ^^Reject)[0]); }
    consteval bool   HasFlatten(sm::info m) { return !sm::annotations_of_with_type(m, ^^Flatten).empty(); }
    consteval bool   HasKey    (sm::info m) { return !sm::annotations_of_with_type(m, ^^Key    ).empty(); }
    consteval bool   HasProperty(sm::info m){ return !sm::annotations_of_with_type(m, ^^Property).empty(); }
    // The verbatim OLO_PROPERTY args of a Property-annotated member ("" for a bare OLO_PROPERTY()).
    consteval std::string_view PropertyRaw(sm::info m) {
        auto anns = sm::annotations_of_with_type(m, ^^Property);
        if (anns.empty()) return {};
        Property p = sm::extract<Property>(anns[0]);
        return std::string_view(std::define_static_string(std::string_view(p.raw)));
    }
    // Script-facing name: the OLO_PROPERTY `Name = "..."` if present, else the m_-stripped field name.
    consteval std::string_view PropertyScriptName(sm::info m) {
        std::string_view raw = PropertyRaw(m);
        if (auto k = raw.find("Name"); k != std::string_view::npos) {
            if (auto q1 = raw.find('"', k); q1 != std::string_view::npos)
                if (auto q2 = raw.find('"', q1 + 1); q2 != std::string_view::npos)
                    return std::string_view(std::define_static_string(raw.substr(q1 + 1, q2 - q1 - 1)));
        }
        return KeyOf(sm::identifier_of(m));   // fallback: field name
    }
    // Access: a `ReadOnly` token in the args makes the schema property read-only (reserved; no field uses it yet).
    consteval bool PropertyReadOnly(sm::info m) { return PropertyRaw(m).find("ReadOnly") != std::string_view::npos; }

    consteval bool HasMethod(sm::info m) { return !sm::annotations_of_with_type(m, ^^Method).empty(); }

    // --- OnSet sync-hook (commitment #4) ---
    consteval bool HasOnSet(sm::info m) { return !sm::annotations_of_with_type(m, ^^OnSet).empty(); }
    consteval std::string_view OnSetMethod(sm::info m) {
        auto anns = sm::annotations_of_with_type(m, ^^OnSet);
        if (anns.empty()) return {};
        OnSet o = sm::extract<OnSet>(anns[0]);
        return std::string_view(std::define_static_string(std::string_view(o.method)));
    }
    // Reflect the zero-arg member function named `name` on type T (for splicing a call to it).
    template <typename T>
    consteval sm::info FindMethod(std::string_view name) {
        for (sm::info f : sm::members_of(^^T, sm::access_context::unchecked()))
            if (sm::is_function(f) && sm::has_identifier(f) && sm::identifier_of(f) == name) return f;
        return {};
    }

    // -------- component discovery: replaces the source-tree scan -----------------
    // Runtime-only set (mirrors kComponentsNotInTuple): identity + per-tick state.
    consteval bool IsRuntimeOnly(std::string_view n)
    {   // mirrors OloHeaderTool's kComponentsNotInTuple EXACTLY (not a broad *StateComponent
        // sweep — AnimationStateComponent / WeatherStateComponent are persisted, so they stay in)
        return n == "IDComponent" || n == "TagComponent"
            || n == "UIResolvedRectComponent" || n == "WorldTransformComponent"
            || n == "DialogueStateComponent" || n == "SpringBoneStateComponent"
            || n == "NoiseAnimationStateComponent" || n == "RetargetingStateComponent"
            || n == "FootIKStateComponent" || n == "LocomotionStateComponent";
    }

    // Raw scan — EVERY `struct *Component`, with NO exclusion. This is the primitive
    // each consumer filters its own way, so the tool's several *different* hand-kept
    // exclusion sets (kComponentsNotInTuple vs kComponentsNotInSaveGame vs …) become
    // derived predicates: the tuple excludes IsRuntimeOnly; save-games filter by the
    // Serialize overload; MCP by editability. One scan, many views.
    consteval std::vector<sm::info> DiscoverAllComponents()
    {
        std::vector<sm::info> out;
        for (sm::info m : sm::members_of(^^OloEngine, sm::access_context::unchecked()))
        {
            if (!sm::is_type(m) || !sm::is_class_type(m) || !sm::has_identifier(m)) continue;
            if (!sm::identifier_of(m).ends_with("Component")) continue;   // the *Component heuristic
            out.push_back(m);
        }
        return out;
    }

    // The ECS scene-copy / tuple view: all components minus the runtime-only identity/state set.
    consteval std::vector<sm::info> DiscoverComponents()
    {
        std::vector<sm::info> out;
        for (sm::info m : DiscoverAllComponents())
            if (!IsRuntimeOnly(sm::identifier_of(m))) out.push_back(m);
        return out;
    }

    // The ECS type list — replaces AllComponents.Generated.inl entirely.
    using AllComponents = [: sm::substitute(^^std::tuple, DiscoverComponents()) :];

    // -------- for_each_component: drive any per-component enumeration ------------
    // Replaces the SAVE_COMPONENT / TRY_LOAD_COMPONENT / binary-covered lists —
    // they are all "do X for every component in AllComponents".
    template <typename Fn>
    void for_each_component(Fn&& fn)
    {
        template for (constexpr auto c : std::define_static_array(DiscoverComponents()))
        {
            using C = [: c :];                 // splice the reflection to a type via alias
            fn(std::type_identity<C>{});
        }
    }

    // Same, over the RAW set (incl. identity IDComponent/TagComponent) — for consumers
    // like save-games whose base set differs from the tuple's.
    template <typename Fn>
    void for_each_all_component(Fn&& fn)
    {
        template for (constexpr auto c : std::define_static_array(DiscoverAllComponents()))
        {
            using C = [: c :];
            fn(std::type_identity<C>{});
        }
    }

    // -------- visit_fields: the generic member-walk every serializer shares ------
    // Calls visitor.template operator()<member_info>(key, field_ref) for each
    // non-Skip data member. The visitor does the compile-time type dispatch
    // (scalar / enum / glm / vector / nested) where the field type is known.
    template <typename T, typename Visitor>
    void visit_fields(T&& obj, Visitor&& visitor)
    {
        using U = std::remove_cvref_t<T>;
        // unchecked() so PRIVATE members are enumerated AND splice-accessible — a
        // capability beyond OloHeaderTool, which can't emit `comp.m_PrivateField`.
        template for (constexpr auto m :
                      std::define_static_array(sm::nonstatic_data_members_of(^^U, sm::access_context::unchecked())))
        {
            if constexpr (!HasSkip(m))
            {
                constexpr std::string_view key = KeyOfMember(m);   // Reflect::Key rename or m_-stripped
                auto&& ref = obj.[:m:];                 // bind splice before use
                visitor.template operator()<m>(key, ref);
            }
        }
    }

    // -------- OnComponentAdded/Removed: the artifact that DISAPPEARS -------------
    // OloHeaderTool generated a no-op specialization per component so the primary
    // template could stay declaration-only. With reflection there is nothing to
    // generate: the primary template IS the no-op; the engine hand-writes only the
    // components that need a real body (CameraComponent, the physics group, ...).
    template <typename T> void OnComponentAdded  (auto& /*scene*/, auto /*entity*/, T& /*c*/) { /* no-op */ }
    template <typename T> void OnComponentRemoved(auto& /*scene*/, auto /*entity*/, T& /*c*/) { /* no-op */ }

    // component type name (unqualified), for the outer YAML key
    template <typename T>
    consteval std::string_view ComponentName() { return sm::identifier_of(^^T); }

} // namespace OloEngine::Reflect
