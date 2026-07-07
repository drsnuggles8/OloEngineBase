#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
// =============================================================================
// Vec3ClampSerializerCodegenTest
//
// Pins the OloHeaderTool scene-serializer codegen for OLO_SERIALIZE(Clamp, Min=…,
// Max=…) on a glm::vec3 field (issue #451's vec3-Clamp slice — the follow-up to
// the scalar-only Clamp slice, #536). tools/OloHeaderTool/main.cpp's
// ParseComponentFields now accepts PropType::Vec3 in its Clamp-eligible set, and
// EmitDeserializeFields emits the plain `.as<glm::vec3>(lhs)` finite-validated
// read followed by a per-component glm::clamp/glm::max/glm::min step — mirroring
// the hand-written SanitizeVec3Clamped idiom used by BuoyancyComponent::
// m_ProbeExtents and NoiseAnimationComponent::RotationAmplitude/
// TranslationAmplitude before this slice migrated them onto the generated path
// (see ComponentSerializerCoverageTest for their live coverage).
//
// The mirror function below accesses the vec3 through a member of a small local
// "holder" struct (`holder.Field`), matching the real generator's `comp.m_Field`
// access shape used throughout this codegen (see ContainerSerializerCodegenTest
// / NestedStructSerializerCodegenTest for the same convention).
//
// If you change EmitDeserializeFields' PropType::Vec3 branch or ApplyVec3Clamp,
// update the mirrored Deserialize* helper below to match.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/YAMLConverters.h"

#include <glm/glm.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>

namespace OloEngine::Tests
{
    namespace
    {
        struct ExtentsHolder
        {
            glm::vec3 Extents{ 0.5f, 0.5f, 0.5f };
        };

        // --- Mirror of EmitDeserializeFields' PropType::Vec3 branch with both
        // Min and Max given (issue #451 vec3-Clamp slice) ---
        void DeserializeExtentsBothBounds(const YAML::Node& node, ExtentsHolder& comp)
        {
            comp.Extents = node["Extents"].as<glm::vec3>(comp.Extents);
            comp.Extents = glm::clamp(comp.Extents, glm::vec3(0.01f), glm::vec3(1000.0f));
        }

        // --- Mirror of EmitDeserializeFields' PropType::Vec3 branch, Min-only ---
        void DeserializeExtentsMinOnly(const YAML::Node& node, ExtentsHolder& comp)
        {
            comp.Extents = node["Extents"].as<glm::vec3>(comp.Extents);
            comp.Extents = glm::max(comp.Extents, glm::vec3(0.0f));
        }

        // --- Mirror of EmitDeserializeFields' PropType::Vec3 branch, Max-only ---
        void DeserializeExtentsMaxOnly(const YAML::Node& node, ExtentsHolder& comp)
        {
            comp.Extents = node["Extents"].as<glm::vec3>(comp.Extents);
            comp.Extents = glm::min(comp.Extents, glm::vec3(10.0f));
        }

        void SerializeExtents(YAML::Emitter& out, const ExtentsHolder& comp)
        {
            out << YAML::Key << "Extents" << YAML::Value << comp.Extents;
        }

        std::string EmitToString(const ExtentsHolder& comp)
        {
            YAML::Emitter out;
            out << YAML::BeginMap;
            SerializeExtents(out, comp);
            out << YAML::EndMap;
            return out.c_str();
        }
    } // namespace

    // An in-range vec3 round-trips unchanged.
    TEST(Vec3ClampSerializerCodegen, InRangeValueRoundTrips)
    {
        ExtentsHolder src;
        src.Extents = { 2.0f, 3.0f, 4.0f };
        const std::string yaml = EmitToString(src);

        ExtentsHolder roundtripped;
        DeserializeExtentsBothBounds(YAML::Load(yaml), roundtripped);
        EXPECT_FLOAT_EQ(roundtripped.Extents.x, 2.0f);
        EXPECT_FLOAT_EQ(roundtripped.Extents.y, 3.0f);
        EXPECT_FLOAT_EQ(roundtripped.Extents.z, 4.0f);
    }

    // Each component is clamped independently into [Min, Max] — mirrors
    // SanitizeVec3Clamped's per-component behavior.
    TEST(Vec3ClampSerializerCodegen, OutOfRangeComponentsClampIndependently)
    {
        const char* yaml = R"(Extents: [-5.0, 2000.0, 500.0])";

        ExtentsHolder dest;
        DeserializeExtentsBothBounds(YAML::Load(yaml), dest);

        EXPECT_FLOAT_EQ(dest.Extents.x, 0.01f);   // -5.0 clamped up to Min
        EXPECT_FLOAT_EQ(dest.Extents.y, 1000.0f); // 2000.0 clamped down to Max
        EXPECT_FLOAT_EQ(dest.Extents.z, 500.0f);  // already in range
    }

    // A missing key keeps the pre-existing (constructor-default) value, same as
    // the plain (non-Clamp) glm::vec3 path — the clamp step never runs on an
    // absent node because DecodeVec3 already left the fallback in place.
    TEST(Vec3ClampSerializerCodegen, MissingKeyKeepsDefault)
    {
        ExtentsHolder dest;
        DeserializeExtentsBothBounds(YAML::Load("{ Unrelated: 1 }"), dest);
        EXPECT_FLOAT_EQ(dest.Extents.x, 0.5f);
        EXPECT_FLOAT_EQ(dest.Extents.y, 0.5f);
        EXPECT_FLOAT_EQ(dest.Extents.z, 0.5f);
    }

    // Min-only clamps up but never down (glm::max broadcast).
    TEST(Vec3ClampSerializerCodegen, MinOnlyClampsUpNotDown)
    {
        const char* yaml = R"(Extents: [-1.0, 5.0, -0.001])";
        ExtentsHolder dest;
        DeserializeExtentsMinOnly(YAML::Load(yaml), dest);
        EXPECT_FLOAT_EQ(dest.Extents.x, 0.0f);
        EXPECT_FLOAT_EQ(dest.Extents.y, 5.0f);
        EXPECT_FLOAT_EQ(dest.Extents.z, 0.0f);
    }

    // Max-only clamps down but never up (glm::min broadcast).
    TEST(Vec3ClampSerializerCodegen, MaxOnlyClampsDownNotUp)
    {
        const char* yaml = R"(Extents: [20.0, 5.0, 9.999])";
        ExtentsHolder dest;
        DeserializeExtentsMaxOnly(YAML::Load(yaml), dest);
        EXPECT_FLOAT_EQ(dest.Extents.x, 10.0f);
        EXPECT_FLOAT_EQ(dest.Extents.y, 5.0f);
        EXPECT_FLOAT_EQ(dest.Extents.z, 9.999f);
    }

    // A non-finite component falls back to the WHOLE pre-existing vector (not a
    // per-component fallback) before the clamp step runs — DecodeVec3 rejects the
    // whole node if any component is non-finite, matching every other plain
    // glm::vec3 field's behavior; the Clamp annotation only adds a range step on
    // top, it does not change the finite-fallback semantics.
    TEST(Vec3ClampSerializerCodegen, NonFiniteComponentFallsBackToWholeDefault)
    {
        const char* yaml = R"(Extents: [.nan, 5.0, 3.0])";
        ExtentsHolder dest;
        dest.Extents = { 7.0f, 7.0f, 7.0f };
        DeserializeExtentsBothBounds(YAML::Load(yaml), dest);

        // Whole-vector fallback kept (7,7,7), then clamped into [0.01, 1000].
        EXPECT_FLOAT_EQ(dest.Extents.x, 7.0f);
        EXPECT_FLOAT_EQ(dest.Extents.y, 7.0f);
        EXPECT_FLOAT_EQ(dest.Extents.z, 7.0f);
    }
} // namespace OloEngine::Tests
