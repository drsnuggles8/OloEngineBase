#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
// =============================================================================
// ContainerSerializerCodegenTest
//
// Pins the OloHeaderTool scene-serializer codegen for std::unordered_set<E> and
// std::string-keyed std::unordered_map<std::string, V> members (issue #451
// unordered_map/set slice). tools/OloHeaderTool/main.cpp's ParseComponentFields
// classifies these as isSet / isMap SerFields, and
// EmitSerializeFields/EmitDeserializeFields emit a SORTED YAML sequence / SORTED
// YAML mapping — sorted because an unordered container's iteration order is not
// guaranteed stable across runs, unlike std::vector's declaration order, and an
// unsorted emit would break the "serialize -> deserialize -> serialize produces
// identical YAML" determinism the round-trip tests rely on.
//
// PrefabComponent (OloEngine/src/OloEngine/Scene/Components.h) is the only live
// engine component with an unordered_set field today — see
// ComponentRoundTripTest.cpp's PrefabComponentSurvivesYAMLRoundTrip for its live
// coverage, all std::string elements. No live component yet has an
// unordered_map field, or a non-std::string-element set (AssetHandle / enum).
// This test reproduces the generator's EXACT emitted code shape for those
// still-uncompiled cases — an AssetHandle-element set (needs the u64-cast sort
// comparator), an enum-element set, and a std::unordered_map<std::string, f32>
// (the shape MorphTargetComponent::Weights would use if it weren't kept
// hand-written for an unrelated Ref<T> reason) — so they get compile + runtime
// coverage before a real component exercises them.
//
// The mirror functions below deliberately access their container through a
// member of a small local "holder" struct (`holder.Field`), exactly like the
// real generator's `comp.m_Field` — NOT through a bare reference parameter.
// decltype of a class-member-access expression yields the MEMBER's own
// declared type regardless of the enclosing object's constness/value
// category, which is what makes `decltype(comp.m_Field)::value_type` work in
// the real generated code; decltype of a plain reference parameter keeps the
// reference (`const T&`), and `T&::value_type` does not compile.
//
// If you change EmitSerializeFields / EmitDeserializeFields' isSet/isMap
// branches, update the mirrored Serialize*/Deserialize* helpers below to match.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/YAMLConverters.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // A minimal local enum standing in for a real component's `enum class`
        // field — the generator only cares about the field's own type (detected
        // via CollectEnumTypes), not this specific name.
        enum class TestPhase : int
        {
            Idle = 0,
            Running = 1,
            Done = 2,
        };

        // Holder structs mirroring how a real `struct *Component` carries these
        // fields as plain (non-reference) public members.
        struct HandleSetHolder
        {
            std::unordered_set<AssetHandle> Handles;
        };
        struct PhaseSetHolder
        {
            std::unordered_set<TestPhase> Phases;
        };
        struct WeightsHolder
        {
            std::unordered_map<std::string, f32> Weights;
        };

        // --- Mirror of EmitSerializeFields' isSet branch, AssetHandle element ---
        // (needs the explicit u64-cast sort comparator — UUID has no operator<
        // of its own, only an implicit conversion to u64).
        void SerializeHandleSet(YAML::Emitter& out, const HandleSetHolder& comp)
        {
            std::vector<decltype(comp.Handles)::value_type> sorted0(comp.Handles.begin(), comp.Handles.end());
            std::ranges::sort(sorted0, [](auto const& lhs, auto const& rhs)
                              { return static_cast<u64>(lhs) < static_cast<u64>(rhs); });
            out << YAML::Key << "Handles" << YAML::Value << YAML::BeginSeq;
            for (auto const& e : sorted0)
                out << static_cast<u64>(e);
            out << YAML::EndSeq;
        }

        // --- Mirror of EmitDeserializeFields' isSet branch (generic decode path) ---
        void DeserializeHandleSet(const YAML::Node& node, HandleSetHolder& comp)
        {
            if (auto seqNode = node["Handles"]; seqNode && seqNode.IsSequence())
            {
                comp.Handles.clear();
                for (auto const& e : seqNode)
                    if (decltype(comp.Handles)::value_type v{}; ::YAML::convert<decltype(comp.Handles)::value_type>::decode(e, v))
                        comp.Handles.insert(v);
            }
        }

        // --- Mirror of EmitSerializeFields' isSet branch, enum element (native
        // operator< — no cast needed) ---
        void SerializePhaseSet(YAML::Emitter& out, const PhaseSetHolder& comp)
        {
            std::vector<decltype(comp.Phases)::value_type> sorted0(comp.Phases.begin(), comp.Phases.end());
            std::ranges::sort(sorted0);
            out << YAML::Key << "Phases" << YAML::Value << YAML::BeginSeq;
            for (auto const& e : sorted0)
                out << static_cast<int>(e);
            out << YAML::EndSeq;
        }

        // --- Mirror of EmitDeserializeFields' isSet branch (Enum int-decode path) ---
        void DeserializePhaseSet(const YAML::Node& node, PhaseSetHolder& comp)
        {
            if (auto seqNode = node["Phases"]; seqNode && seqNode.IsSequence())
            {
                comp.Phases.clear();
                for (auto const& e : seqNode)
                    if (int ev; ::YAML::convert<int>::decode(e, ev))
                        comp.Phases.insert(static_cast<decltype(comp.Phases)::value_type>(ev));
            }
        }

        // --- Mirror of EmitSerializeFields' isMap branch, std::unordered_map<std::string, f32> ---
        void SerializeWeights(YAML::Emitter& out, const WeightsHolder& comp)
        {
            std::vector<std::string> keys0;
            keys0.reserve(comp.Weights.size());
            for (auto const& entry0 : comp.Weights)
                keys0.push_back(entry0.first);
            std::ranges::sort(keys0);
            out << YAML::Key << "Weights" << YAML::Value << YAML::BeginMap;
            for (auto const& k0 : keys0)
                out << YAML::Key << k0 << YAML::Value << comp.Weights.at(k0);
            out << YAML::EndMap;
        }

        // --- Mirror of EmitDeserializeFields' isMap branch (Float decode path) ---
        void DeserializeWeights(const YAML::Node& node, WeightsHolder& comp)
        {
            if (auto mapNode0 = node["Weights"]; mapNode0 && mapNode0.IsMap())
            {
                comp.Weights.clear();
                for (auto const& entry0 : mapNode0)
                {
                    const std::string k0 = entry0.first.as<std::string>();
                    if (f32 v; ::OloEngine::YAMLUtils::TryReadFiniteF32(entry0.second, v))
                        comp.Weights[k0] = v;
                }
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

    // A std::unordered_set<AssetHandle> round-trips as a YAML sequence, sorted
    // ascending by the handle's underlying u64 value regardless of
    // insertion/iteration order.
    TEST(ContainerSerializerCodegen, AssetHandleSetRoundTripsSortedByValue)
    {
        HandleSetHolder src;
        src.Handles = { AssetHandle(300), AssetHandle(100), AssetHandle(200) };

        const std::string yaml = EmitToString([&](YAML::Emitter& out)
                                              { SerializeHandleSet(out, src); });

        HandleSetHolder roundtripped;
        DeserializeHandleSet(YAML::Load(yaml), roundtripped);
        EXPECT_EQ(roundtripped.Handles, src.Handles);

        const YAML::Node seq = YAML::Load(yaml)["Handles"];
        ASSERT_EQ(seq.size(), 3u);
        EXPECT_EQ(seq[0].as<u64>(), 100u);
        EXPECT_EQ(seq[1].as<u64>(), 200u);
        EXPECT_EQ(seq[2].as<u64>(), 300u);
    }

    // A std::unordered_set<enum> round-trips as a YAML sequence of ints.
    TEST(ContainerSerializerCodegen, EnumSetRoundTrips)
    {
        PhaseSetHolder src;
        src.Phases = { TestPhase::Done, TestPhase::Idle };

        const std::string yaml = EmitToString([&](YAML::Emitter& out)
                                              { SerializePhaseSet(out, src); });

        PhaseSetHolder roundtripped;
        DeserializePhaseSet(YAML::Load(yaml), roundtripped);
        EXPECT_EQ(roundtripped.Phases, src.Phases);
    }

    // A std::unordered_map<std::string, V> round-trips as a genuine YAML mapping,
    // sorted by key regardless of insertion/iteration order.
    TEST(ContainerSerializerCodegen, StringKeyedMapRoundTripsSortedByKey)
    {
        WeightsHolder src;
        src.Weights = { { "Brow", 0.8f }, { "Smile", 0.5f }, { "Blink", 1.0f } };

        const std::string yaml = EmitToString([&](YAML::Emitter& out)
                                              { SerializeWeights(out, src); });

        WeightsHolder roundtripped;
        DeserializeWeights(YAML::Load(yaml), roundtripped);

        ASSERT_EQ(roundtripped.Weights.size(), src.Weights.size());
        for (auto const& [name, weight] : src.Weights)
            EXPECT_FLOAT_EQ(roundtripped.Weights.at(name), weight) << name;

        // "Blink" < "Brow" < "Smile" — sorted, not insertion order.
        const YAML::Node map = YAML::Load(yaml)["Weights"];
        auto it = map.begin();
        EXPECT_EQ(it->first.as<std::string>(), "Blink");
        ++it;
        EXPECT_EQ(it->first.as<std::string>(), "Brow");
        ++it;
        EXPECT_EQ(it->first.as<std::string>(), "Smile");
    }

    // A non-finite map value is rejected (the entry is never inserted — unlike a
    // scalar field there's no pre-existing default to fall back to) while a
    // sibling valid entry still round-trips.
    TEST(ContainerSerializerCodegen, NonFiniteMapValueIsDropped)
    {
        const char* yaml = R"(
Weights:
  Brow: .nan
  Smile: 0.5
)";
        WeightsHolder dest;
        DeserializeWeights(YAML::Load(yaml), dest);

        EXPECT_FALSE(dest.Weights.contains("Brow"));
        ASSERT_TRUE(dest.Weights.contains("Smile"));
        EXPECT_FLOAT_EQ(dest.Weights.at("Smile"), 0.5f);
    }

    // A missing key leaves the destination container untouched (no .clear() /
    // insert happens — matches the generated `if (auto x = …; x && …)` guard).
    TEST(ContainerSerializerCodegen, MissingKeyLeavesContainerAtDefault)
    {
        PhaseSetHolder phases;
        phases.Phases = { TestPhase::Running };
        DeserializePhaseSet(YAML::Load("{ Unrelated: 1 }"), phases);
        ASSERT_EQ(phases.Phases.size(), 1u);
        EXPECT_TRUE(phases.Phases.contains(TestPhase::Running));

        WeightsHolder weights;
        weights.Weights = { { "Existing", 1.0f } };
        DeserializeWeights(YAML::Load("{ Unrelated: 1 }"), weights);
        ASSERT_EQ(weights.Weights.size(), 1u);
        EXPECT_FLOAT_EQ(weights.Weights.at("Existing"), 1.0f);
    }
} // namespace OloEngine::Tests
