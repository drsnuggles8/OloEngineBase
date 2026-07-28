#pragma once
// ============================================================================
//  OloReflectYaml.h — full-type-parity generic YAML serializer built on
//  OloReflect.h's visit_fields. One recursive EmitValue covers every category
//  OloHeaderTool's classifier handles:
//    bool / int / uint / small-int(widened) / float / enum(->int) / std::string
//    / UUID|AssetHandle(->u64) / glm vec2-4 & quat / std::vector<any>
//    / nested all-trivial struct(->sub-map) / std::vector<struct>
//    / std::unordered_set<sortable>(->SORTED seq) / std::unordered_map<string,V>(->SORTED map)
//  Requires OloEngine::UUID / OloEngine::AssetHandle + glm + the YAML glm
//  converters to be visible at instantiation (as they are in the real engine).
// ============================================================================
#include "OloReflect.h"
#include <yaml-cpp/yaml.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/ext/vector_int2.hpp>
#include <glm/ext/vector_int3.hpp>
#include <glm/ext/vector_int4.hpp>
#include <vector>
#include <string>
#include <algorithm>
#include <ranges>
#include <type_traits>
#include <cmath>
#include <utility>
#include <array>
#include <cstddef>
#include <memory>

namespace OloEngine::Reflect
{
    namespace sm = std::meta;

    template <typename V> constexpr bool kIsUUIDLike =
        std::is_same_v<V, ::OloEngine::UUID> || std::is_same_v<V, ::OloEngine::AssetHandle>;

    template <typename V> consteval bool IsGlm() {
        constexpr auto t = sm::dealias(^^V);
        return sm::is_same_type(t, ^^glm::vec2)  || sm::is_same_type(t, ^^glm::vec3)
            || sm::is_same_type(t, ^^glm::vec4)  || sm::is_same_type(t, ^^glm::quat)
            || sm::is_same_type(t, ^^glm::mat3)  || sm::is_same_type(t, ^^glm::mat4)
            || sm::is_same_type(t, ^^glm::ivec2) || sm::is_same_type(t, ^^glm::ivec3) || sm::is_same_type(t, ^^glm::ivec4);
    }
    template <typename V> consteval bool IsTemplate(sm::info tmpl) {
        constexpr auto t = sm::dealias(^^V);
        return sm::is_class_type(t) && sm::has_template_arguments(t) && sm::template_of(t) == tmpl;
    }
    template <typename V> consteval bool IsStdVector()   { return IsTemplate<V>(^^std::vector); }
    template <typename V> consteval bool IsUnorderedSet(){ return IsTemplate<V>(^^std::unordered_set); }
    template <typename V> consteval bool IsUnorderedMap(){ return IsTemplate<V>(^^std::unordered_map); }
    // Ref<T> detected by template NAME ("Ref") — no dependency on Core/Ref.h. A
    // Ref<Asset-derived> serializes as its asset handle (u64), exactly like the .inl.
    template <typename V> consteval bool IsRef() {
        constexpr auto t = sm::dealias(^^V);
        return sm::is_class_type(t) && sm::has_template_arguments(t)
            && sm::has_identifier(sm::template_of(t)) && sm::identifier_of(sm::template_of(t)) == "Ref";
    }
    template <typename V> consteval bool IsStdArray(){ return IsTemplate<V>(^^std::array); }        // std::array<T,N>
    template <typename V> consteval bool IsUniquePtr(){ return IsTemplate<V>(^^std::unique_ptr); }  // unique_ptr<Struct>

    template <typename T> void EmitStructBody(YAML::Emitter& out, const T& obj);   // fwd (recursion)

    template <typename V>
    void EmitValue(YAML::Emitter& out, const V& value)
    {
        if      constexpr (std::is_same_v<V, bool>)        out << value;
        else if constexpr (std::is_enum_v<V>)              out << static_cast<int>(value);
        else if constexpr (kIsUUIDLike<V>)                 out << static_cast<u64>(value);
        else if constexpr (IsGlm<V>())                     out << value;                 // glm converter
        else if constexpr (std::is_same_v<V, std::string>) out << value;
        else if constexpr (std::is_integral_v<V>) {
            if constexpr (sizeof(V) < 4)                                                 // widen small ints
                out << static_cast<std::conditional_t<std::is_signed_v<V>, i32, u32>>(value);
            else out << value;
        }
        else if constexpr (std::is_floating_point_v<V>)    out << value;
        else if constexpr (IsStdVector<V>()) {
            out << YAML::BeginSeq;
            for (auto const& e : value) EmitValue(out, e);                               // vector<any>
            out << YAML::EndSeq;
        }
        else if constexpr (IsUnorderedSet<V>()) {
            using E = typename V::value_type;
            std::vector<E> sorted(value.begin(), value.end());                           // sort for determinism
            if constexpr (kIsUUIDLike<E>)
                std::ranges::sort(sorted, [](const E& a, const E& b){ return static_cast<u64>(a) < static_cast<u64>(b); });
            else std::ranges::sort(sorted);
            out << YAML::BeginSeq;
            for (auto const& e : sorted) EmitValue(out, e);
            out << YAML::EndSeq;
        }
        else if constexpr (IsUnorderedMap<V>()) {
            static_assert(std::is_same_v<typename V::key_type, std::string>, "only string-keyed maps");
            std::vector<std::string> keys; keys.reserve(value.size());
            for (auto const& kv : value) keys.push_back(kv.first);
            std::ranges::sort(keys);                                                      // sort keys
            out << YAML::BeginMap;
            for (auto const& k : keys) { out << YAML::Key << k << YAML::Value; EmitValue(out, value.at(k)); }
            out << YAML::EndMap;
        }
        else if constexpr (IsStdArray<V>()) {                                             // std::array<T,N> -> sequence
            out << YAML::BeginSeq;
            for (auto const& e : value) EmitValue(out, e);
            out << YAML::EndSeq;
        }
        else if constexpr (IsUniquePtr<V>()) {                                            // unique_ptr<Struct> -> sub-map (non-null here)
            out << YAML::BeginMap;
            EmitStructBody(out, *value);
            out << YAML::EndMap;
        }
        else if constexpr (IsRef<V>()) {                                                  // Ref<Asset> -> handle u64
            if (value) out << static_cast<u64>(value->GetHandle());
            else       out << static_cast<u64>(0);
        }
        else if constexpr (std::is_class_v<V>) {                                          // nested struct
            out << YAML::BeginMap;
            EmitStructBody(out, value);
            out << YAML::EndMap;
        }
        else static_assert(sizeof(V) == 0, "OloReflectYaml: unhandled member type");
    }

    template <typename T>
    void EmitStructBody(YAML::Emitter& out, const T& obj)
    {
        visit_fields(obj, [&out]<sm::info M>(std::string_view key, auto& ref) {
            using MT = std::remove_cvref_t<decltype(ref)>;
            if constexpr (HasFlatten(M)) { EmitStructBody(out, ref); return; }     // flatten nested struct's fields at this level
            if constexpr (IsRef<MT>() || IsUniquePtr<MT>()) { if (!ref) return; }   // omit null Ref / unique_ptr
            out << YAML::Key << std::string(key) << YAML::Value;
            EmitValue(out, ref);
        });
    }

    // serialize a component with its "TypeName:" wrapper map (matches the .inl)
    template <typename T>
    std::string SerializeComponent(const T& obj)
    {
        YAML::Emitter out;
        out << YAML::BeginMap << YAML::Key << std::string(ComponentName<T>()) << YAML::Value << YAML::BeginMap;
        EmitStructBody(out, obj);
        out << YAML::EndMap << YAML::EndMap;
        return std::string(out.c_str()) + "\n";
    }

    // serialize a bare field-holder (no component wrapper) — for diffing against the
    // codegen-test mirror functions, which emit into a plain { ... } map.
    template <typename T>
    std::string SerializeHolder(const T& obj)
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        EmitStructBody(out, obj);
        out << YAML::EndMap;
        return out.c_str();
    }

    // ======================= DESERIALIZE (symmetric to EmitValue) =======================
    template <typename T> void DeserializeStructBody(const YAML::Node& node, T& obj);   // fwd (recursion)

    template <typename V>
    void ReadValue(const YAML::Node& n, V& ref)
    {
        if      constexpr (std::is_same_v<V, bool>)        ref = n.as<bool>(ref);
        else if constexpr (std::is_enum_v<V>)              { if (int ev; YAML::convert<int>::decode(n, ev)) ref = static_cast<V>(ev); }
        else if constexpr (kIsUUIDLike<V>)                 ref = static_cast<V>(n.as<u64>(static_cast<u64>(ref)));
        else if constexpr (IsGlm<V>())                     ref = n.as<V>(ref);            // Decode keeps default on non-finite
        else if constexpr (std::is_same_v<V, std::string>) ref = n.as<std::string>(ref);
        else if constexpr (std::is_integral_v<V>)          ref = n.as<V>(ref);
        else if constexpr (std::is_floating_point_v<V>)    { V v = n.as<V>(ref); if (std::isfinite(v)) ref = v; }  // non-finite -> keep default
        else if constexpr (IsStdVector<V>()) {
            if (n.IsSequence()) { ref.clear(); for (auto const& e : n) { typename V::value_type t{}; ReadValue(e, t); ref.push_back(std::move(t)); } }
        }
        else if constexpr (IsUnorderedSet<V>()) {
            if (n.IsSequence()) { ref.clear(); for (auto const& e : n) { typename V::value_type t{}; ReadValue(e, t); ref.insert(std::move(t)); } }
        }
        else if constexpr (IsUnorderedMap<V>()) {
            using MV = typename V::mapped_type;
            if (n.IsMap()) { ref.clear(); for (auto const& kv : n) {
                std::string k = kv.first.as<std::string>();
                if constexpr (std::is_floating_point_v<MV>) { MV v = kv.second.as<MV>(); if (std::isfinite(v)) ref[k] = v; }   // drop non-finite value
                else { MV t{}; ReadValue(kv.second, t); ref[k] = std::move(t); }
            } }
        }
        else if constexpr (IsStdArray<V>()) {
            if (n.IsSequence()) { std::size_t i = 0; for (auto const& e : n) { if (i >= ref.size()) break; ReadValue(e, ref[i]); ++i; } }
        }
        else if constexpr (IsUniquePtr<V>()) {                                            // unique_ptr<Struct>: construct + fill
            using T = typename V::element_type;
            if (n.IsMap()) { if (!ref) ref = std::make_unique<T>(); DeserializeStructBody(n, *ref); }
        }
        else if constexpr (IsRef<V>()) { (void)n; /* Ref<Asset>: handle resolves via AssetManager at load (engine service) */ }
        else if constexpr (std::is_class_v<V>) { DeserializeStructBody(n, ref); }         // nested struct sub-map
        else static_assert(sizeof(V) == 0, "OloReflectYaml: unhandled member type (deserialize)");
    }

    template <typename T>
    void DeserializeStructBody(const YAML::Node& node, T& obj)
    {
        visit_fields(obj, [&node]<sm::info M>(std::string_view key, auto& ref) {
            if constexpr (HasFlatten(M)) { DeserializeStructBody(node, ref); return; }  // flatten: read from the parent node
            auto n = node[std::string(key)];
            if (!n) return;                                       // missing key -> keep constructor default
            using MT = std::remove_cvref_t<decltype(ref)>;
            if constexpr (HasReject(M) && std::is_arithmetic_v<MT> && !std::is_same_v<MT, bool>) {
                MT before = ref;                                  // reject-not-clamp: keep default if out of range
                ReadValue(n, ref);
                constexpr Reject rj = GetReject(M);
                if (static_cast<double>(ref) < rj.min || static_cast<double>(ref) > rj.max) ref = before;
                return;
            }
            ReadValue(n, ref);
            if constexpr (HasClamp(M)) {                          // annotation-driven range clamp
                constexpr Clamp c = GetClamp(M);
                if      constexpr (std::is_floating_point_v<MT>)                       ref = std::clamp(ref, static_cast<MT>(c.min), static_cast<MT>(c.max));
                else if constexpr (std::is_integral_v<MT> && !std::is_same_v<MT, bool>) ref = std::clamp(ref, static_cast<MT>(c.min), static_cast<MT>(c.max));
                else if constexpr (std::is_enum_v<MT>)                                 ref = static_cast<MT>(std::clamp<int>(static_cast<int>(ref), static_cast<int>(c.min), static_cast<int>(c.max)));
            }
        });
        // POST-DESERIALIZE HOOK: reflection reads the fields, then a component keeps its
        // cross-field validation (Min<=Max swaps, hysteresis, per-value clamps, Sanitize)
        // as a method. This is the refactor that makes genuinely-"custom" components
        // reflectable — field I/O is generic, only the invariant stays hand-written.
        if constexpr (requires(T& t) { t.OnDeserialized(); }) obj.OnDeserialized();
    }

    template <typename T>
    T DeserializeComponentBody(const YAML::Node& node) { T obj{}; DeserializeStructBody(node, obj); return obj; }
}
