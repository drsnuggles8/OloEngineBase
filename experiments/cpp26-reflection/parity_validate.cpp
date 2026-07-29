// ============================================================================
//  Full-type-parity validation: OloReflectYaml.h's generic serializer vs the
//  EXACT hand-written codegen-test mirror functions (which reproduce
//  OloHeaderTool's emitted shape) — for every exotic type category.
//  GCC 16.1.0: -std=c++26 -freflection, linked against vendor libyaml-cpp.a.
// ============================================================================
#include <yaml-cpp/yaml.h>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <ranges>
#include <functional>

using u64 = std::uint64_t; using u32 = std::uint32_t; using i32 = std::int32_t; using f32 = float; using sizet = std::size_t;

// ---- minimal real-compatible id types (u64 wrappers, like Core/UUID.h) ----------
namespace OloEngine {
    struct UUID {
        u64 m_Value{};
        UUID() = default; constexpr UUID(u64 v) : m_Value(v) {}
        constexpr operator u64() const { return m_Value; }
        bool operator==(const UUID& o) const { return m_Value == o.m_Value; }
    };
    struct AssetHandle {
        u64 m_Value{};
        AssetHandle() = default; constexpr AssetHandle(u64 v) : m_Value(v) {}
        constexpr operator u64() const { return m_Value; }
        bool operator==(const AssetHandle& o) const { return m_Value == o.m_Value; }
    };
}
template <> struct std::hash<OloEngine::AssetHandle> {
    sizet operator()(OloEngine::AssetHandle h) const { return std::hash<u64>()(static_cast<u64>(h)); }
};

// ---- glm -> YAML converters (verbatim shape from Core/YAMLConverters.h) ----------
namespace YAML {
    inline Emitter& operator<<(Emitter& o, const glm::vec3& v){ o << Flow << BeginSeq << v.x << v.y << v.z << EndSeq; return o; }
    inline Emitter& operator<<(Emitter& o, const glm::vec4& v){ o << Flow << BeginSeq << v.x << v.y << v.z << v.w << EndSeq; return o; }
    inline Emitter& operator<<(Emitter& o, const glm::quat& q){ o << Flow << BeginSeq << q.x << q.y << q.z << q.w << EndSeq; return o; }
}

// ---- real structs the codegen tests use (verbatim field layout) -----------------
namespace OloEngine {
    struct OffMeshLink {
        glm::vec3 m_Start{}; glm::vec3 m_End{}; f32 m_Radius = 0.6f; bool m_Bidirectional = false;
        OffMeshLink() = default;
        OffMeshLink(glm::vec3 s, glm::vec3 e, f32 r, bool b) : m_Start(s), m_End(e), m_Radius(r), m_Bidirectional(b) {}
    };
    struct LODLevel {
        AssetHandle MeshHandle{}; f32 MaxDistance = 0.0f; u32 TriangleCount = 0;
        LODLevel() = default;
        LODLevel(AssetHandle h, f32 d, u32 t) : MeshHandle(h), MaxDistance(d), TriangleCount(t) {}
    };
    struct LODGroup { std::vector<LODLevel> Levels; f32 Bias = 0.0f; };
    enum class TestPhase : int { Idle = 0, Running = 1, Done = 2 };

    // A real component (verbatim from Components.h): UUID + std::vector<UUID>.
    struct RelationshipComponent {
        UUID m_ParentHandle{};
        std::vector<UUID> m_Children;
    };

    // field-holders mirroring how components carry these (non-component names, so
    // they are NOT swept into DiscoverComponents()).
    struct LinksHolder   { std::vector<OffMeshLink> Links; };
    struct LODHolder     { LODGroup m_LODGroup; };
    struct HandleSet     { std::unordered_set<AssetHandle> Handles; };
    struct PhaseSet      { std::unordered_set<TestPhase> Phases; };
    struct WeightsMap    { std::unordered_map<std::string, f32> Weights; };
    struct XformHolder   { glm::vec4 m_Color{1,2,3,4}; glm::quat m_Rotation{1,0,0,0}; };
}

#include "OloReflectYaml.h"      // generic serializer sees the types above
namespace R = OloEngine::Reflect;
using namespace OloEngine;

// ============================================================================
//  Mirror functions — copied VERBATIM from the codegen tests (the known-correct
//  OloHeaderTool emit shape). These are the oracle.
// ============================================================================
static std::string EmitToString(const std::function<void(YAML::Emitter&)>& body) {
    YAML::Emitter out; out << YAML::BeginMap; body(out); out << YAML::EndMap; return out.c_str();
}
static void SerializeLinks(YAML::Emitter& out, const std::vector<OffMeshLink>& links) {
    out << YAML::Key << "Links" << YAML::Value << YAML::BeginSeq;
    for (auto const& e : links) {
        out << YAML::BeginMap;
        out << YAML::Key << "Start" << YAML::Value << e.m_Start;
        out << YAML::Key << "End" << YAML::Value << e.m_End;
        out << YAML::Key << "Radius" << YAML::Value << e.m_Radius;
        out << YAML::Key << "Bidirectional" << YAML::Value << e.m_Bidirectional;
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;
}
static void SerializeLODGroup(YAML::Emitter& out, const LODGroup& g) {
    out << YAML::Key << "LODGroup" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "Levels" << YAML::Value << YAML::BeginSeq;
    for (auto const& e : g.Levels) {
        out << YAML::BeginMap;
        out << YAML::Key << "MeshHandle" << YAML::Value << static_cast<u64>(e.MeshHandle);
        out << YAML::Key << "MaxDistance" << YAML::Value << e.MaxDistance;
        out << YAML::Key << "TriangleCount" << YAML::Value << e.TriangleCount;
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    out << YAML::Key << "Bias" << YAML::Value << g.Bias;
    out << YAML::EndMap;
}
static void SerializeHandleSet(YAML::Emitter& out, const HandleSet& comp) {
    std::vector<decltype(comp.Handles)::value_type> sorted0(comp.Handles.begin(), comp.Handles.end());
    std::ranges::sort(sorted0, [](auto const& lhs, auto const& rhs){ return static_cast<u64>(lhs) < static_cast<u64>(rhs); });
    out << YAML::Key << "Handles" << YAML::Value << YAML::BeginSeq;
    for (auto const& e : sorted0) out << static_cast<u64>(e);
    out << YAML::EndSeq;
}
static void SerializePhaseSet(YAML::Emitter& out, const PhaseSet& comp) {
    std::vector<decltype(comp.Phases)::value_type> sorted0(comp.Phases.begin(), comp.Phases.end());
    std::ranges::sort(sorted0);
    out << YAML::Key << "Phases" << YAML::Value << YAML::BeginSeq;
    for (auto const& e : sorted0) out << static_cast<int>(e);
    out << YAML::EndSeq;
}
static void SerializeWeights(YAML::Emitter& out, const WeightsMap& comp) {
    std::vector<std::string> keys0; keys0.reserve(comp.Weights.size());
    for (auto const& entry0 : comp.Weights) keys0.push_back(entry0.first);
    std::ranges::sort(keys0);
    out << YAML::Key << "Weights" << YAML::Value << YAML::BeginMap;
    for (auto const& k0 : keys0) out << YAML::Key << k0 << YAML::Value << comp.Weights.at(k0);
    out << YAML::EndMap;
}
// RelationshipComponent's real .inl block, reproduced as the oracle.
static std::string MirrorRelationship(const RelationshipComponent& c) {
    YAML::Emitter out; out << YAML::BeginMap;
    out << YAML::Key << "RelationshipComponent" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "ParentHandle" << YAML::Value << static_cast<u64>(c.m_ParentHandle);
    out << YAML::Key << "Children" << YAML::Value << YAML::BeginSeq;
    for (auto const& e : c.m_Children) out << static_cast<u64>(e);
    out << YAML::EndSeq;
    out << YAML::EndMap << YAML::EndMap;
    return std::string(out.c_str()) + "\n";
}
static void SerializeXform(YAML::Emitter& out, const XformHolder& h) {
    out << YAML::Key << "Color" << YAML::Value << h.m_Color;
    out << YAML::Key << "Rotation" << YAML::Value << h.m_Rotation;
}

int main() {
    int fails = 0;
    auto check = [&](const char* name, const std::string& expected, const std::string& actual) {
        bool ok = (expected == actual);
        std::printf("  %-46s %s\n", name, ok ? "MATCH" : "DIFFER");
        if (!ok) { std::printf("    expected: %s\n    actual  : %s\n", expected.c_str(), actual.c_str()); ++fails; }
    };

    // 1) UUID + vector<UUID>  (real component, real .inl oracle)
    RelationshipComponent rc; rc.m_ParentHandle = UUID(4200000000000000000ULL);
    rc.m_Children = { UUID(7), UUID(3), UUID(9) };
    check("UUID + vector<UUID> (RelationshipComponent)", MirrorRelationship(rc), R::SerializeComponent(rc));

    // 2) vector<struct>
    LinksHolder lh; lh.Links = { {{1,2,3},{4,5,6},0.75f,true}, {{-1.5f,0,9},{2,2,2},1.25f,false} };
    check("vector<struct> (OffMeshLink Links)",
          EmitToString([&](YAML::Emitter& o){ SerializeLinks(o, lh.Links); }), R::SerializeHolder(lh));

    // 3) scalar nested struct (containing vector<struct>) — two-level recursion
    LODHolder ldh; ldh.m_LODGroup.Bias = 1.5f;
    ldh.m_LODGroup.Levels = { {AssetHandle(1234),10.0f,500u}, {AssetHandle(5678),50.0f,120u} };
    check("nested struct (LODGroup)",
          EmitToString([&](YAML::Emitter& o){ SerializeLODGroup(o, ldh.m_LODGroup); }), R::SerializeHolder(ldh));

    // 4) unordered_set<AssetHandle> — SORTED by u64
    HandleSet hs; hs.Handles = { AssetHandle(300), AssetHandle(100), AssetHandle(200) };
    check("unordered_set<AssetHandle> (sorted)",
          EmitToString([&](YAML::Emitter& o){ SerializeHandleSet(o, hs); }), R::SerializeHolder(hs));

    // 5) unordered_set<enum> — SORTED, ints
    PhaseSet ps; ps.Phases = { TestPhase::Done, TestPhase::Idle, TestPhase::Running };
    check("unordered_set<enum> (sorted)",
          EmitToString([&](YAML::Emitter& o){ SerializePhaseSet(o, ps); }), R::SerializeHolder(ps));

    // 6) unordered_map<string, f32> — SORTED by key
    WeightsMap wm; wm.Weights = { {"Brow",0.8f}, {"Smile",0.5f}, {"Blink",1.0f} };
    check("unordered_map<string,f32> (sorted keys)",
          EmitToString([&](YAML::Emitter& o){ SerializeWeights(o, wm); }), R::SerializeHolder(wm));

    // 7) glm vec4 + quat (routes to the converter)
    XformHolder xf;
    check("glm::vec4 + glm::quat",
          EmitToString([&](YAML::Emitter& o){ SerializeXform(o, xf); }), R::SerializeHolder(xf));

    std::printf("\nPARITY: %s (%d differ)\n", fails == 0 ? "ALL TYPE CATEGORIES MATCH THE GENERATOR" : "MISMATCH", fails);
    return fails == 0 ? 0 : 1;
}
