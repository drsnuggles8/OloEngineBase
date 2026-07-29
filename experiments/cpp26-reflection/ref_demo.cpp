// ============================================================================
//  Ref<T>-of-Asset support: a Ref<Asset-derived> serializes as its handle (u64),
//  exactly like the .inl's `comp.m_X->GetHandle()`. Validated with a stand-in Ref
//  that mirrors the engine's Ref interface (same template name "Ref" -> the
//  reflection detection is IDENTICAL for the real OloEngine::Ref).
//  GCC 16.1.0: -std=c++26 -freflection, glm + libyaml-cpp.a.
// ============================================================================
#include <yaml-cpp/yaml.h>
#include <glm/vec3.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
using u64 = std::uint64_t; using u32 = std::uint32_t; using i32 = std::int32_t; using f32 = float;

namespace OloEngine {
    struct UUID        { u64 v{}; UUID()=default; constexpr UUID(u64 x):v(x){} constexpr operator u64()const{return v;} bool operator==(const UUID&o)const{return v==o.v;} };
    struct AssetHandle { u64 v{}; AssetHandle()=default; constexpr AssetHandle(u64 x):v(x){} constexpr operator u64()const{return v;} bool operator==(const AssetHandle&o)const{return v==o.v;} };
    // stand-in mirroring the real Core/Ref.h interface
    template <typename T> struct Ref {
        T* m_Ptr = nullptr;
        Ref() = default; explicit Ref(T* p) : m_Ptr(p) {}
        explicit operator bool() const { return m_Ptr != nullptr; }
        T* operator->() const { return m_Ptr; }
    };
    struct MeshSource { u64 m_Handle{}; u64 GetHandle() const { return m_Handle; } };
    struct MeshComponent { Ref<MeshSource> m_Mesh; f32 m_Tint = 1.0f; };   // Ref<Asset> + a scalar
}
namespace YAML { inline Emitter& operator<<(Emitter& o, const glm::vec3& v){ o<<Flow<<BeginSeq<<v.x<<v.y<<v.z<<EndSeq; return o; } }

#include "OloReflectYaml.h"
namespace R = OloEngine::Reflect;
using namespace OloEngine;

int main() {
    int fails = 0;
    MeshSource ms{ 4242 };
    { MeshComponent c; c.m_Mesh = Ref<MeshSource>(&ms); c.m_Tint = 0.5f;
      std::string y = R::SerializeHolder(c);
      std::printf("non-null Ref:\n%s\n", y.c_str());
      YAML::Node root = YAML::Load(y);
      bool ok = root["Mesh"] && root["Mesh"].as<u64>() == 4242 && root["Tint"];
      std::printf("  [%s] Ref<Asset> serialized as its handle\n", ok ? "PASS":"FAIL"); if(!ok) ++fails; }
    { MeshComponent c;      // default: null Ref
      std::string y = R::SerializeHolder(c);
      std::printf("null Ref:\n%s\n", y.c_str());
      YAML::Node root = YAML::Load(y);
      bool ok = !root["Mesh"];   // null Ref -> key omitted (matches engine's `if (ref && ...)`)
      std::printf("  [%s] null Ref -> key omitted\n", ok ? "PASS":"FAIL"); if(!ok) ++fails; }
    std::printf("\nREF<T>: %s\n", fails==0 ? "ALL PASS" : "FAIL");
    return fails;
}
