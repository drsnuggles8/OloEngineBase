#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
// =============================================================================
// NestedStructSerializerCodegenTest
//
// Pins the OloHeaderTool scene-serializer codegen for NESTED STRUCT and
// std::vector<struct> members (issue #451 nested-struct slice). The generator
// (tools/OloHeaderTool/main.cpp) recursively classifies a component member whose
// type is an all-trivial-public struct (or a std::vector of one) as
// PropType::Struct and emits, respectively, a nested YAML sub-map or a sequence
// of sub-maps — see EmitSerializeFields / EmitDeserializeFields.
//
// Every component that would exercise this in the engine build is currently in
// kComponentsCustomSerialize (LODGroupComponent flattens its sub-struct on disk,
// NavMeshBoundsComponent sanitizes on load, the *StateComponents are runtime),
// so the generated Struct-emit code path is NOT compiled into OloEngine itself.
// This test therefore reproduces the generator's EXACT emitted code shape for
// the real engine structs those components wrap (LODGroup / LODLevel, OffMeshLink)
// and round-trips them through yaml-cpp — giving the pattern both compile
// coverage and runtime correctness until a clean all-trivial nested-struct
// component lands and compiles it directly.
//
// If you change EmitSerializeFields / EmitDeserializeFields, update the mirrored
// Serialize*/Deserialize* helpers below to match.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/YAMLConverters.h"
#include "OloEngine/Navigation/OffMeshLink.h"
#include "OloEngine/Renderer/LOD.h"

#include <yaml-cpp/yaml.h>

#include <functional>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        using ::OloEngine::YAMLUtils::TryReadFiniteF32;

        // --- Mirror of the generator's serialize emit for a std::vector<struct> ---
        // (NavMeshBoundsComponent's `Links` member, element = OffMeshLink).
        void SerializeLinks(YAML::Emitter& out, const std::vector<OffMeshLink>& links)
        {
            out << YAML::Key << "Links" << YAML::Value << YAML::BeginSeq;
            for (auto const& e : links)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Start" << YAML::Value << e.m_Start;
                out << YAML::Key << "End" << YAML::Value << e.m_End;
                out << YAML::Key << "Radius" << YAML::Value << e.m_Radius;
                out << YAML::Key << "Bidirectional" << YAML::Value << e.m_Bidirectional;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }

        // --- Mirror of the generator's deserialize emit for a std::vector<struct> ---
        void DeserializeLinks(const YAML::Node& node, std::vector<OffMeshLink>& links)
        {
            if (auto seqNode = node["Links"]; seqNode && seqNode.IsSequence())
            {
                links.clear();
                for (auto const& e : seqNode)
                {
                    OffMeshLink tmp0{};
                    tmp0.m_Start = e["Start"].as<glm::vec3>(tmp0.m_Start);
                    tmp0.m_End = e["End"].as<glm::vec3>(tmp0.m_End);
                    if (f32 v; TryReadFiniteF32(e["Radius"], v))
                        tmp0.m_Radius = v;
                    tmp0.m_Bidirectional = e["Bidirectional"].as<bool>(tmp0.m_Bidirectional);
                    links.push_back(tmp0);
                }
            }
        }

        // --- Mirror of the generator's emit for a SCALAR nested struct that itself
        // contains a std::vector<struct> --- (LODGroupComponent's `LODGroup m_LODGroup`
        // member: a struct wrapping std::vector<LODLevel> + f32 Bias). The one-level-
        // deeper recursion (component → LODGroup → Levels[i]) is exactly what the
        // generator produces; here it is inlined as one sub-map.
        void SerializeLODGroup(YAML::Emitter& out, const LODGroup& g)
        {
            out << YAML::Key << "LODGroup" << YAML::Value << YAML::BeginMap;
            out << YAML::Key << "Levels" << YAML::Value << YAML::BeginSeq;
            for (auto const& e : g.Levels)
            {
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

        void DeserializeLODGroup(const YAML::Node& node, LODGroup& g)
        {
            if (auto sub0 = node["LODGroup"]; sub0)
            {
                if (auto seqNode1 = sub0["Levels"]; seqNode1 && seqNode1.IsSequence())
                {
                    g.Levels.clear();
                    for (auto const& e1 : seqNode1)
                    {
                        LODLevel tmp1{};
                        tmp1.MeshHandle = e1["MeshHandle"].as<u64>(static_cast<u64>(tmp1.MeshHandle));
                        if (f32 v; TryReadFiniteF32(e1["MaxDistance"], v))
                            tmp1.MaxDistance = v;
                        tmp1.TriangleCount = e1["TriangleCount"].as<u32>(tmp1.TriangleCount);
                        g.Levels.push_back(tmp1);
                    }
                }
                if (f32 v; TryReadFiniteF32(sub0["Bias"], v))
                    g.Bias = v;
            }
        }

        std::string EmitToString(const std::function<void(YAML::Emitter&)>& body)
        {
            YAML::Emitter out;
            out << YAML::BeginMap;
            body(out);
            out << YAML::EndMap;
            return out.c_str();
        }
    } // namespace

    // A std::vector<struct> round-trips as a YAML sequence of sub-maps, one key per
    // element field, in order.
    TEST(NestedStructSerializerCodegen, VectorOfStructRoundTrips)
    {
        std::vector<OffMeshLink> links;
        links.emplace_back(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(4.0f, 5.0f, 6.0f), 0.75f, true);
        links.emplace_back(glm::vec3(-1.5f, 0.0f, 9.0f), glm::vec3(2.0f, 2.0f, 2.0f), 1.25f, false);

        const std::string yaml = EmitToString([&](YAML::Emitter& out)
                                              { SerializeLinks(out, links); });

        std::vector<OffMeshLink> roundtripped;
        DeserializeLinks(YAML::Load(yaml), roundtripped);

        ASSERT_EQ(roundtripped.size(), links.size());
        for (sizet i = 0; i < links.size(); ++i)
            EXPECT_EQ(roundtripped[i], links[i]) << "link " << i;
    }

    // A scalar nested struct (which itself contains a vector-of-struct) round-trips
    // as a nested sub-map — the two-level recursion the generator emits.
    TEST(NestedStructSerializerCodegen, NestedStructWithVectorRoundTrips)
    {
        LODGroup group;
        group.Bias = 1.5f;
        group.Levels.emplace_back(AssetHandle(1234), 10.0f, 500u);
        group.Levels.emplace_back(AssetHandle(5678), 50.0f, 120u);

        const std::string yaml = EmitToString([&](YAML::Emitter& out)
                                              { SerializeLODGroup(out, group); });

        LODGroup roundtripped;
        roundtripped.Bias = 0.0f; // deliberately different from default so the read is observable
        DeserializeLODGroup(YAML::Load(yaml), roundtripped);

        EXPECT_EQ(roundtripped, group);
        ASSERT_EQ(roundtripped.Levels.size(), 2u);
        EXPECT_EQ(static_cast<u64>(roundtripped.Levels[0].MeshHandle), 1234u);
        EXPECT_EQ(roundtripped.Levels[1].TriangleCount, 120u);
    }

    // A missing sub-map key keeps the destination's constructor defaults (nothing is
    // cleared / overwritten) — matches the generated `if (auto sub = …; sub)` guard.
    TEST(NestedStructSerializerCodegen, MissingNestedKeyKeepsDefaults)
    {
        LODGroup dest;
        dest.Bias = 3.0f;
        dest.Levels.emplace_back(AssetHandle(9), 1.0f, 1u);

        // A YAML map with no "LODGroup" key at all.
        DeserializeLODGroup(YAML::Load("{ Unrelated: 1 }"), dest);

        EXPECT_FLOAT_EQ(dest.Bias, 3.0f);
        ASSERT_EQ(dest.Levels.size(), 1u); // vector untouched (no IsSequence branch entered)
    }

    // A non-finite float element field is rejected (keeps the element's default),
    // and a malformed sequence element is still constructed from whatever keys it has
    // — the generator validates floats via TryReadFiniteF32.
    TEST(NestedStructSerializerCodegen, NonFiniteFloatKeepsElementDefault)
    {
        const char* yaml = R"(
Links:
  - Start: [0, 0, 0]
    End: [1, 1, 1]
    Radius: .nan
    Bidirectional: false
)";
        std::vector<OffMeshLink> links;
        DeserializeLinks(YAML::Load(yaml), links);

        ASSERT_EQ(links.size(), 1u);
        // Radius was NaN → rejected → stays at OffMeshLink's constructor default (0.6).
        EXPECT_FLOAT_EQ(links[0].m_Radius, 0.6f);
        EXPECT_FALSE(links[0].m_Bidirectional);
    }
} // namespace OloEngine::Tests
