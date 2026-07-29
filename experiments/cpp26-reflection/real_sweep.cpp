// ============================================================================
//  Full-110 sweep: for EVERY real engine component, if the generic serializer can
//  fully handle it (CanSerialize), construct + serialize it; else skip. Output is
//  diffed against the generated .inl to produce a hard coverage number.
//  GCC 16.1.0: -std=c++26 -freflection -fpermissive, real engine + PCH + stubs.
// ============================================================================
// --- MACRO BRIDGE: wire the engine's OLO_SERIALIZE(Skip) to a real reflectable
//     annotation so the serializer omits runtime fields exactly like the generator.
//     ComponentReflection.h is #pragma once, so pre-including it + overriding wins.
#include "OloReflectAnnotations.h"                  // defines OloEngine::Reflect::Skip
#include "OloEngine/Scene/ComponentReflection.h"    // engine's OLO_SERIALIZE (empty marker)
#undef OLO_SERIALIZE
#define OLO_SERIALIZE(mode, ...) OLO_SER_##mode(__VA_ARGS__)
#define OLO_SER_Skip(...)  [[=::OloEngine::Reflect::Skip{}]]
#define OLO_SER_Clamp(...)                          // deserialize-only -> no serialize effect
#define OLO_SER_Key(k)     [[=::OloEngine::Reflect::Key{ k }]]   // custom YAML key rename

#include "OloEngine/Scene/Components.h"             // runtime fields now carry [[=Reflect::Skip]]
#include "OloEngine/Core/YAMLConverters.h"
#include <type_traits>
#include <cstdio>

#include "OloReflectYaml.h"
namespace R  = OloEngine::Reflect;
namespace sm = std::meta;

namespace OloEngine::RefUtils { bool Release(RefCounted*) { return false; } }   // leaf stub
namespace OloEngine { Ref<Font> Font::GetDefault() { return {}; } }             // stub: null default font (-> handle 0)
namespace OloEngine { SceneCamera::SceneCamera() {} }                           // stub ctor (in-class member inits still run)

// CanSerialize<T> — compile-time gate mirroring EmitValue's handled type set, and
// requiring every member accessible (no private/protected — those the engine
// hand-writes). Defined here, not in the header, because it references OloEngine::Asset.
consteval bool CanSerType(sm::info t, int depth);
consteval bool CanSerStruct(sm::info t, int depth) {
    if (depth > 24) return false;                    // cycle / too-deep guard -> not serializable
    // IMPROVEMENT over OloHeaderTool: no private-member skip. unchecked() enumerates
    // ALL members (incl. private), and reflection can splice-access them — so a
    // private-member component is serializable iff every member (public or not) is a
    // handled type. The text tool must hand-write these; we don't.
    for (sm::info m : sm::nonstatic_data_members_of(t, sm::access_context::unchecked())) {
        if (R::HasSkip(m)) continue;                 // OLO_SERIALIZE(Skip) field -> omitted, don't gate on its type
        if (!CanSerType(sm::type_of(m), depth + 1)) return false;
    }
    return true;
}
consteval bool CanSortableSetElem(sm::info e) {
    e = sm::dealias(e);
    return sm::is_enum_type(e) || sm::is_same_type(e, ^^std::string)
        || sm::is_same_type(e, ^^OloEngine::UUID) || sm::is_same_type(e, ^^OloEngine::AssetHandle)
        || sm::is_integral_type(e);
}
consteval bool CanSerType(sm::info t, int depth) {
    if (depth > 24) return false;
    t = sm::dealias(t);
    if (sm::is_arithmetic_type(t) || sm::is_enum_type(t)) return true;
    if (sm::is_same_type(t, ^^std::string)) return true;
    if (sm::is_same_type(t, ^^OloEngine::UUID) || sm::is_same_type(t, ^^OloEngine::AssetHandle)) return true;
    if (sm::is_same_type(t, ^^glm::vec2)  || sm::is_same_type(t, ^^glm::vec3)  || sm::is_same_type(t, ^^glm::vec4)
     || sm::is_same_type(t, ^^glm::quat)  || sm::is_same_type(t, ^^glm::mat3)  || sm::is_same_type(t, ^^glm::mat4)
     || sm::is_same_type(t, ^^glm::ivec2) || sm::is_same_type(t, ^^glm::ivec3) || sm::is_same_type(t, ^^glm::ivec4)) return true;
    if (!sm::is_class_type(t)) return false;
    if (sm::has_template_arguments(t)) {
        sm::info tmpl = sm::template_of(t);
        if (sm::has_identifier(tmpl) && sm::identifier_of(tmpl) == "Ref")
            return sm::is_base_of_type(^^OloEngine::Asset, sm::dealias(sm::template_arguments_of(t)[0]));
        if (tmpl == ^^std::vector)        return CanSerType(sm::template_arguments_of(t)[0], depth + 1);
        if (tmpl == ^^std::array)         return CanSerType(sm::template_arguments_of(t)[0], depth + 1);   // beyond OloHeaderTool
        if (tmpl == ^^std::unordered_set) return CanSortableSetElem(sm::template_arguments_of(t)[0]);
        if (tmpl == ^^std::unordered_map) { auto a = sm::template_arguments_of(t);
                                            return sm::is_same_type(sm::dealias(a[0]), ^^std::string) && CanSerType(a[1], depth + 1); }
        return false;                     // std::function, Ref<non-asset>, other templates
    }
    return CanSerStruct(t, depth + 1);    // nested plain struct
}
template <typename T> consteval bool CanSerialize() { return CanSerStruct(^^T, 0); }

// COMPILE-TIME count (no construction): how many real components are serializable,
// now that private members are reachable. CanSerStruct takes the type info directly.
consteval int CountSerializable() {
    int n = 0;
    for (sm::info c : R::DiscoverComponents()) if (CanSerStruct(c, 0)) ++n;
    return n;
}

static int g_ser = 0, g_skip = 0;

int main(int /*argc*/, char** /*argv*/) {
    std::fprintf(stderr, "[compile-time] components CanSerialize (incl. private members) = %d / 110\n", CountSerializable());
    R::for_each_component([]<class C>(std::type_identity<C>) {
        if constexpr (CanSerialize<C>() && std::is_default_constructible_v<C>) {
            std::fputs(R::SerializeComponent(C{}).c_str(), stdout);       // real component -> YAML on stdout
            ++g_ser;
        } else {
            constexpr std::string_view n = R::ComponentName<C>();
            std::fprintf(stderr, "SKIP %.*s\n", (int)n.size(), n.data());  // skips -> stderr
            ++g_skip;
        }
    });
    std::fprintf(stderr, "\n[sweep] serialized=%d skipped=%d\n", g_ser, g_skip);
    return 0;
}
