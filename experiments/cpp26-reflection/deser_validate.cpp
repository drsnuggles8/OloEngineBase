// ============================================================================
//  Full deserialize parity — round-trip identity + hostile-input behavior over
//  REAL engine types (OffMeshLink, LODGroup, AssetHandle) via their real headers,
//  matching the codegen tests' documented deserialize semantics.
//  GCC 16.1.0: -std=c++26 -freflection, real engine headers + UUID.cpp + libyaml-cpp.a.
// ============================================================================
#include <yaml-cpp/yaml.h>
#include "OloReflectAnnotations.h"           // OLO_CLAMP (before the annotated component)
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Asset/Asset.h"           // AssetHandle (= UUID), std::hash<UUID>
#include "OloEngine/Navigation/OffMeshLink.h"
#include "OloEngine/Renderer/LOD.h"
#include "OloEngine/Core/YAMLConverters.h"   // real glm encode + decode
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <cstdio>

namespace OloEngine {
    enum class Phase : int { Idle = 0, Running = 1, Done = 2 };
    struct LinksHolder { std::vector<OffMeshLink> Links; };
    struct LODHolder   { LODGroup m_LODGroup; };
    struct HandleSet   { std::unordered_set<AssetHandle> Handles; };
    struct PhaseSet    { std::unordered_set<Phase> Phases; };
    struct WeightsMap  { std::unordered_map<std::string, f32> Weights; };

    // an all-trivial component with clamp annotations (deserialize must clamp)
    struct WaterProbeComponent {
        OLO_CLAMP(1.0f, 100000.0f) f32 m_Density = 1000.0f;
        OLO_CLAMP(0.0f, 1.0f)      f32 m_Ratio   = 0.5f;
    };
}
#include "OloReflectYaml.h"
namespace R = OloEngine::Reflect;
using namespace OloEngine;

static int g_fails = 0;
#define CHECK(cond, msg) do { bool _c=(cond); std::printf("  [%s] %s\n", _c?"PASS":"FAIL", msg); if(!_c) ++g_fails; } while(0)

template <typename T>
static bool RoundTrips(const T& obj, const char* name) {
    std::string y1 = R::SerializeHolder(obj);
    YAML::Node node = YAML::Load(y1);
    T obj2{}; R::DeserializeStructBody(node, obj2);
    std::string y2 = R::SerializeHolder(obj2);
    bool ok = (y1 == y2);
    std::printf("  [%s] %-34s round-trip (serialize=deserialize=serialize)\n", ok ? "PASS" : "FAIL", name);
    if (!ok) { std::printf("      y1: %s\n      y2: %s\n", y1.c_str(), y2.c_str()); ++g_fails; }
    return ok;
}

int main() {
    std::puts("== round-trip identity (real types, all categories) ==");
    LinksHolder lh; lh.Links = { {{1,2,3},{4,5,6},0.75f,true}, {{-1.5f,0,9},{2,2,2},1.25f,false} };
    RoundTrips(lh, "vector<struct> OffMeshLink");
    LODHolder ld; ld.m_LODGroup.Bias = 1.5f;
    ld.m_LODGroup.Levels.emplace_back(AssetHandle(1234), 10.0f, 500u);
    ld.m_LODGroup.Levels.emplace_back(AssetHandle(5678), 50.0f, 120u);
    RoundTrips(ld, "nested struct LODGroup");
    HandleSet hs; hs.Handles = { AssetHandle(300), AssetHandle(100), AssetHandle(200) };
    RoundTrips(hs, "unordered_set<AssetHandle>");
    PhaseSet ps; ps.Phases = { Phase::Done, Phase::Idle, Phase::Running };
    RoundTrips(ps, "unordered_set<enum>");
    WeightsMap wm; wm.Weights = { {"Brow",0.8f}, {"Smile",0.5f}, {"Blink",1.0f} };
    RoundTrips(wm, "unordered_map<string,f32>");

    std::puts("\n== hostile-input behavior (matches codegen tests) ==");
    // NaN float element field -> element keeps constructor default
    { LinksHolder x; YAML::Node n = YAML::Load("Links:\n  - Start: [0,0,0]\n    End: [1,1,1]\n    Radius: .nan\n    Bidirectional: false\n");
      R::DeserializeStructBody(n, x);
      OffMeshLink def{};
      CHECK(x.Links.size()==1 && x.Links[0].m_Radius == def.m_Radius, "vector<struct>: NaN Radius -> element default kept"); }
    // NaN map value -> entry dropped, sibling kept
    { WeightsMap x; YAML::Node n = YAML::Load("Weights:\n  Brow: .nan\n  Smile: 0.5\n");
      R::DeserializeStructBody(n, x);
      CHECK(!x.Weights.contains("Brow") && x.Weights.count("Smile")==1 && x.Weights["Smile"]==0.5f, "map: NaN value dropped, sibling kept"); }
    // missing key -> container untouched
    { PhaseSet x; x.Phases = { Phase::Running }; YAML::Node n = YAML::Load("{ Unrelated: 1 }");
      R::DeserializeStructBody(n, x);
      CHECK(x.Phases.size()==1 && x.Phases.contains(Phase::Running), "missing key -> container untouched"); }
    // unordered_set input in any order -> serialized sorted
    { HandleSet x; x.Handles = { AssetHandle(9), AssetHandle(1), AssetHandle(5) };
      std::string y = R::SerializeHolder(x);
      CHECK(y.find("1") < y.find("5") && y.find("5") < y.find("9"), "unordered_set emitted sorted"); }

    std::puts("\n== annotation clamp on deserialize ==");
    { YAML::Node n = YAML::Load("Density: 9999999\nRatio: -3\n");   // over-max, under-min
      WaterProbeComponent c = R::DeserializeComponentBody<WaterProbeComponent>(n);
      CHECK(c.m_Density == 100000.0f && c.m_Ratio == 0.0f, "clamp: Density->max(100000), Ratio->min(0)"); }

    std::printf("\nDESERIALIZE PARITY: %s (%d failures)\n", g_fails==0 ? "ALL PASS" : "FAILURES", g_fails);
    return g_fails==0 ? 0 : 1;
}
