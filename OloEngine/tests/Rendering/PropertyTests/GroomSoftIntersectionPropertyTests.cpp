// OLO_TEST_LAYER: L1
#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomSoftIntersection.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomStrandMesh.h"

#include <gtest/gtest.h>
#include <limits>
#include <array>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    TEST(GroomSoftIntersection, CoverageIsBoundedAndMonotonicAcrossContact)
    {
        for (const f32 distance : { 0.00001f, 0.001f, 0.1f, 5.0f })
        {
            f32 previous = 0.0f;
            for (i32 i = -10; i <= 110; ++i)
            {
                const f32 separation = distance * static_cast<f32>(i) / 100.0f;
                const f32 coverage = GroomIntersectionCoverage(separation, distance, 1.0f, 0.25f);
                EXPECT_GE(coverage, previous);
                EXPECT_GE(coverage, 0.0f);
                EXPECT_LE(coverage, 1.0f);
                previous = coverage;
            }
            EXPECT_FLOAT_EQ(GroomIntersectionCoverage(0.5f * distance, distance, 1.0f, 0.25f), 0.5f);
        }
    }

    TEST(GroomSoftIntersection, RootsStayWholeAndTheUnprotectedControlFades)
    {
        for (const f32 t : { 0.0f, 0.01f, 0.1f, 0.25f })
            EXPECT_FLOAT_EQ(GroomIntersectionCoverage(-0.01f, 0.01f, t, 0.25f), 1.0f);
        EXPECT_FLOAT_EQ(GroomIntersectionCoverage(-0.01f, 0.01f, 0.0f, 0.0f), 0.0f);
        EXPECT_FLOAT_EQ(GroomIntersectionCoverage(-0.01f, 0.01f, 1.0f, 1.0f), 1.0f);
        EXPECT_FLOAT_EQ(GroomIntersectionCoverage(0.0f, 0.01f, 0.375f, 0.25f), 0.5f);
    }

    TEST(GroomSoftIntersection, DisabledAndAwayFromContactAreExactIdentity)
    {
        EXPECT_FLOAT_EQ(GroomIntersectionCoverage(-1.0f, 0.0f, 1.0f, 0.25f), 1.0f);
        for (const f32 t : { 0.0f, 0.3f, 0.5f, 1.0f })
            EXPECT_FLOAT_EQ(GroomIntersectionCoverage(0.02f, 0.01f, t, 0.25f), 1.0f);
    }

    TEST(GroomSoftIntersection, AutomaticDistanceScalesWithFibresAndProtectsFineShafts)
    {
        for (const f32 length : { 0.0005f, 0.01f, 0.1f, 1.0f })
        {
            const f32 diameter = length * 0.002f;
            const f32 fade = GroomIntersectionDistance(diameter, length);
            EXPECT_FLOAT_EQ(fade, length * 0.02f);
            EXPECT_FLOAT_EQ(GroomIntersectionDistance(diameter * 10.0f, length * 10.0f), fade * 10.0f);
            EXPECT_LE(GroomIntersectionDistance(length, length), length * 0.05f);
            // A normal shaft above its body is already outside the contact
            // band before the protected-root transition even starts.
            EXPECT_FLOAT_EQ(GroomIntersectionCoverage(length * 0.25f, fade, 0.25f, 0.25f), 1.0f);
        }
    }

    TEST(GroomSoftIntersection, CorruptDistancesCannotPoisonCoverage)
    {
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        EXPECT_FLOAT_EQ(GroomIntersectionDistance(nan, 1.0f), 0.0f);
        EXPECT_FLOAT_EQ(GroomIntersectionDistance(1.0f, nan), 0.0f);
        EXPECT_FLOAT_EQ(GroomIntersectionDistance(-1.0f, 1.0f), 0.0f);
        EXPECT_FLOAT_EQ(GroomIntersectionCoverage(0.0f, nan, 1.0f, 0.25f), 1.0f);
    }
    TEST(GroomSoftIntersection, AutomaticDimensionsFollowAuthoredBaseFibres)
    {
        GroomBuilder builder;
        u16 group = 0;
        std::string reason;
        ASSERT_TRUE(builder.AddGroup("body_undercoat", group, reason));
        GroomCoatGroupDesc desc;
        desc.Role = static_cast<u8>(GroomCoatRole::Undercoat);
        desc.Length = 0.5f;
        desc.Width = 0.5f;
        std::vector<std::string> repairs;
        ASSERT_TRUE(builder.SetGroupCoat(group, desc, repairs));
        const std::array<glm::vec3, 3> points{ glm::vec3(0.0f), glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) };
        const std::array<f32, 3> widths{ 0.001f, 0.001f, 0.001f };
        GroomCurveInput curve;
        curve.Points = points;
        curve.Widths = widths;
        curve.GroupId = group;
        ASSERT_TRUE(builder.AddCurve(curve, reason));
        auto groom = builder.Build(reason);
        ASSERT_TRUE(groom);
        GroomCoatSettings settings;
        settings.Enabled = true;
        settings.Undercoat.Length = 0.1f;
        settings.Undercoat.Width = 0.2f;
        const GroomCoatContext coat{ &settings, groom->GetGroupCoats() };
        const auto dimensions = MeasureGroomIntersectionDimensions(*groom, &coat);
        EXPECT_EQ(dimensions.SampledCurves, 1u);
        EXPECT_NEAR(dimensions.MeanLength, 0.05f, 0.000001f);
        EXPECT_NEAR(dimensions.MeanDiameter, 0.0001f, 0.0000001f);
        EXPECT_NEAR(GroomIntersectionDistance(dimensions.MeanDiameter, dimensions.MeanLength), 0.001f, 0.000001f);

        glm::mat3 anisotropic(1.0f);
        anisotropic[1][1] = 0.01f;
        const auto flattened = MeasureGroomIntersectionDimensions(*groom, &coat, anisotropic);
        EXPECT_NEAR(flattened.MeanLength, 0.0005f, 0.0000001f);
        EXPECT_FALSE(GroomIntersectionMetricMatches(dimensions, anisotropic));
        EXPECT_TRUE(GroomIntersectionMetricMatches(flattened, anisotropic));
        // Rotate 90 degrees about Z without changing the metric/length cache.
        const glm::mat3 rotation(glm::vec3(0, 1, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1));
        EXPECT_TRUE(GroomIntersectionMetricMatches(flattened, rotation * anisotropic));
        const auto tiny = MeasureGroomIntersectionDimensions(*groom, &coat, glm::mat3(0.0001f));
        EXPECT_TRUE(GroomIntersectionMetricMatches(tiny, rotation * glm::mat3(0.0001f)));
        EXPECT_FALSE(GroomIntersectionMetricMatches(tiny, glm::mat3(0.001f)));
        const auto invalid = MeasureGroomIntersectionDimensions(*groom, &coat,
                                                                glm::mat3(std::numeric_limits<f32>::quiet_NaN()));
        EXPECT_FALSE(GroomIntersectionMetricMatches(invalid, glm::mat3(1.0f)));

        settings.LengthJitter = 0.2f;
        const auto jittered = MeasureGroomIntersectionDimensions(*groom, &coat);
        std::vector<GroomStrandVertex> vertices;
        std::vector<u32> indices;
        GroomStrandBuildSettings build;
        build.MaxWidthCompensation = 1.0f;
        BuildGroomStrandMesh(GroomBuildSource::FromAsset(*groom), build, vertices, indices, nullptr, &coat);
        f32 realLength = 0.0f;
        const sizet vertexCount = vertices.size();
        for (sizet i = 0; i < vertexCount; i += 4u)
            realLength += glm::length(vertices[i].Other - vertices[i].Position);
        EXPECT_NEAR(jittered.MeanLength, realLength, 0.000001f);
        settings.RoleVisibilityMask = 0u;
        EXPECT_EQ(MeasureGroomIntersectionDimensions(*groom, &coat).SampledCurves, 0u);
    }
} // namespace OloEngine::Tests
