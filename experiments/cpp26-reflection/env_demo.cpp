// Prove the "custom component" mechanisms: OLO_KEY(name) rename + omit-if-null Ref,
// reproducing EnvironmentMapComponent's `EnvironmentMapHandle` custom key and its
// `if (ref && handle)` conditional emit — the reasons it's currently hand-written.
#include <yaml-cpp/yaml.h>
#include "OloReflectAnnotations.h"     // OLO_KEY + Reflect::Key
#include <glm/vec3.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
using u64 = std::uint64_t; using u32 = std::uint32_t; using i32 = std::int32_t; using f32 = float;

namespace OloEngine {
    struct UUID        { u64 v{}; UUID()=default; constexpr UUID(u64 x):v(x){} constexpr operator u64()const{return v;} bool operator==(const UUID&o)const{return v==o.v;} };
    struct AssetHandle { u64 v{}; AssetHandle()=default; constexpr AssetHandle(u64 x):v(x){} constexpr operator u64()const{return v;} bool operator==(const AssetHandle&o)const{return v==o.v;} };
    template <typename T> struct Ref { T* m_Ptr=nullptr; Ref()=default; explicit Ref(T* p):m_Ptr(p){} explicit operator bool()const{return m_Ptr!=nullptr;} T* operator->()const{return m_Ptr;} };
    struct EnvMap { u64 m_H{}; u64 GetHandle() const { return m_H; } };

    // mirrors EnvironmentMapComponent's custom bits
    struct EnvironmentMapComponent {
        AssetHandle m_EnvironmentMapAsset = 0;
        OLO_KEY("EnvironmentMapHandle") Ref<EnvMap> m_EnvironmentMap;   // custom key + omit-if-null
        f32 m_Exposure = 1.0f;
    };
}
namespace YAML { inline Emitter& operator<<(Emitter& o, const glm::vec3& v){ o<<Flow<<BeginSeq<<v.x<<v.y<<v.z<<EndSeq; return o; } }

#include "OloReflectYaml.h"
namespace R = OloEngine::Reflect;
using namespace OloEngine;

int main() {
    int fails = 0;
    EnvMap em{ 4242 };
    { EnvironmentMapComponent c; c.m_EnvironmentMap = Ref<EnvMap>(&em);
      std::string y = R::SerializeHolder(c);
      std::printf("non-null Ref:\n%s\n", y.c_str());
      bool ok = y.find("EnvironmentMapHandle: 4242") != std::string::npos
             && y.find("\n  EnvironmentMap:") == std::string::npos;   // NOT the m_-stripped name
      std::printf("  [%s] custom key 'EnvironmentMapHandle' used (not 'EnvironmentMap')\n", ok?"PASS":"FAIL"); if(!ok)++fails; }
    { EnvironmentMapComponent c;   // null Ref
      std::string y = R::SerializeHolder(c);
      std::printf("null Ref:\n%s\n", y.c_str());
      bool ok = y.find("EnvironmentMapHandle") == std::string::npos;  // omitted entirely
      std::printf("  [%s] null Ref -> key omitted (matches `if (ref && ...)`)\n", ok?"PASS":"FAIL"); if(!ok)++fails; }
    std::printf("\nCUSTOM-KEY + OMIT-NULL: %s\n", fails==0 ? "ALL PASS" : "FAIL");
    return fails;
}
