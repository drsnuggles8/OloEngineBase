// ============================================================================
//  "Finagling" the genuinely-custom components: a POST-DESERIALIZE HOOK.
//  Reflection auto-reads the fields; the component keeps its cross-field
//  validation (Min<=Max swap, hysteresis, per-value clamp) as an OnDeserialized()
//  method + Skip for its runtime Ref. Mirrors the REAL NavMeshBounds /
//  StreamingVolume / MorphTarget hand-written serializers.
//  GCC 16.1.0: -std=c++26 -freflection, glm + libyaml-cpp.a.
// ============================================================================
#include <yaml-cpp/yaml.h>
#include "OloReflectAnnotations.h"
#include <glm/vec3.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cmath>
using u64 = std::uint64_t; using u32 = std::uint32_t; using i32 = std::int32_t; using f32 = float;

// self-contained glm::vec3 <-> YAML (encode + decode)
namespace YAML {
    template <> struct convert<glm::vec3> {
        static Node encode(const glm::vec3& v){ Node n; n.push_back(v.x); n.push_back(v.y); n.push_back(v.z); n.SetStyle(EmitterStyle::Flow); return n; }
        static bool decode(const Node& n, glm::vec3& v){ if(!n.IsSequence()||n.size()!=3) return false; f32 x=n[0].as<f32>(), y=n[1].as<f32>(), z=n[2].as<f32>(); if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)) return false; v.x=x; v.y=y; v.z=z; return true; }
    };
    inline Emitter& operator<<(Emitter& o, const glm::vec3& v){ o<<Flow<<BeginSeq<<v.x<<v.y<<v.z<<EndSeq; return o; }
}

namespace OloEngine {
    struct UUID        { u64 v{}; UUID()=default; constexpr UUID(u64 x):v(x){} constexpr operator u64()const{return v;} bool operator==(const UUID&o)const{return v==o.v;} };
    struct AssetHandle { u64 v{}; AssetHandle()=default; constexpr AssetHandle(u64 x):v(x){} constexpr operator u64()const{return v;} bool operator==(const AssetHandle&o)const{return v==o.v;} };
    template <typename T> struct Ref { T* m_Ptr=nullptr; Ref()=default; explicit operator bool()const{return m_Ptr!=nullptr;} T* operator->()const{return m_Ptr;} };

    // (1) NavMeshBounds-like — cross-field Min<=Max ordering
    struct NavBoundsComponent {
        glm::vec3 m_Min{-100,-10,-100};
        glm::vec3 m_Max{ 100, 50, 100};
        void OnDeserialized() { for (int i=0;i<3;++i) if (m_Min[i] > m_Max[i]) std::swap(m_Min[i], m_Max[i]); }
    };
    // (2) StreamingVolume-like — hysteresis (Unload must exceed Load)
    struct StreamVolComponent {
        f32 m_LoadRadius = 200.0f;
        f32 m_UnloadRadius = 250.0f;
        void OnDeserialized() { m_LoadRadius = std::max(m_LoadRadius, 1.0f);
                                m_UnloadRadius = std::max(m_UnloadRadius, m_LoadRadius + 10.0f); }
    };
    // (3) MorphTarget-like — per-value clamp + a runtime Ref<non-Asset> that Skip omits
    struct MorphSet {};   // NOT Asset-derived — no handle; the reason it was hand-written
    struct MorphComponent {
        std::unordered_map<std::string, f32> m_Weights;
        OLO_SKIP Ref<MorphSet> m_Set;   // runtime -> omitted; reflection never touches its type
        void OnDeserialized() { for (auto& [k,v] : m_Weights) v = std::clamp(v, 0.0f, 1.0f); }
    };
    // (4) SphereAreaLight-like — reject-not-clamp (out of range -> KEEP default, don't clamp)
    struct LightComponent { OLO_REJECT(0.0f, 100.0f) f32 m_Radius = 5.0f; };
}

#include "OloReflectYaml.h"
namespace R = OloEngine::Reflect;
using namespace OloEngine;

static int g_fails = 0;
#define CHECK(c, msg) do{ bool _c=(c); std::printf("  [%s] %s\n", _c?"PASS":"FAIL", msg); if(!_c)++g_fails; }while(0)

int main() {
    std::puts("post-deserialize hook drives the custom validation:");
    // (1) hostile Min>Max -> swapped by OnDeserialized
    { NavBoundsComponent c; R::DeserializeStructBody(YAML::Load("Min: [5, 50, 5]\nMax: [2, 10, 2]\n"), c);
      CHECK(c.m_Min.x==2 && c.m_Max.x==5 && c.m_Min.y==10 && c.m_Max.y==50, "NavBounds: Min>Max swapped per axis"); }
    // (2) Unload < Load -> hysteresis enforced
    { StreamVolComponent c; R::DeserializeStructBody(YAML::Load("LoadRadius: 300\nUnloadRadius: 100\n"), c);
      CHECK(c.m_LoadRadius==300.0f && c.m_UnloadRadius==310.0f, "StreamVol: hysteresis Unload>Load enforced"); }
    // (3) out-of-range weights -> clamped; the Ref<non-Asset> was omitted
    { MorphComponent c; R::DeserializeStructBody(YAML::Load("Weights:\n  a: 1.5\n  b: -0.5\n"), c);
      CHECK(c.m_Weights["a"]==1.0f && c.m_Weights["b"]==0.0f, "Morph: weights clamped to [0,1] (Ref Skipped)");
      std::string y = R::SerializeHolder(c);
      CHECK(y.find("Set") == std::string::npos, "Morph: Ref<non-Asset> omitted from output"); }
    // (4) reject-not-clamp via OLO_REJECT annotation
    { LightComponent c; R::DeserializeStructBody(YAML::Load("Radius: 999\n"), c);
      CHECK(c.m_Radius==5.0f, "Light: out-of-range Radius rejected -> default kept (NOT clamped to 100)"); }
    { LightComponent c; R::DeserializeStructBody(YAML::Load("Radius: 50\n"), c);
      CHECK(c.m_Radius==50.0f, "Light: in-range Radius accepted"); }

    std::printf("\nCUSTOM-VIA-HOOK: %s\n", g_fails==0 ? "ALL PASS — hand-written components become reflection-driven" : "FAIL");
    return g_fails;
}
