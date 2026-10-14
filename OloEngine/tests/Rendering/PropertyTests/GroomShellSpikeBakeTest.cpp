#include "OloEnginePCH.h"
// OLO_TEST_LAYER: L1
#include "../../Groom/GroomShellSpikeBake.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include <gtest/gtest.h>

using namespace OloEngine;
using namespace OloEngine::Tests::GroomShellSpike;

namespace
{
    void CheckBake(bool longCoat)
    {
        // CPU suites can start at either repository root or OloEditor.
        const auto previous = std::filesystem::current_path();
        struct RestoreCwd
        {
            std::filesystem::path Path;
            ~RestoreCwd()
            {
                std::filesystem::current_path(Path);
            }
        } restore{ previous };
        if (std::filesystem::is_directory("OloEditor"))
        {
            std::filesystem::current_path("OloEditor");
        }
        const auto groom = Load(longCoat);
        const auto charts = Charts(longCoat);
        u32 curves = 0;
        for (const auto& chart : charts)
        {
            const auto a = BakeChart(*groom, chart, charts);
            const auto b = BakeChart(*groom, chart, charts);
            ASSERT_EQ(a.Density.size(), MapWidth * MapHeight * HeightSlices);
            ASSERT_EQ(a.LengthDirection.size(), MapWidth * MapHeight);
            EXPECT_EQ(std::memcmp(a.Density.data(), b.Density.data(), a.Density.size() * sizeof(glm::vec2)), 0);
            EXPECT_EQ(std::memcmp(a.LengthDirection.data(), b.LengthDirection.data(), a.LengthDirection.size() * sizeof(glm::vec4)), 0);
            EXPECT_LT(a.MaxRootError, 1.0e-3f);
            f64 sum = 0.0;
            f32 low = 1.0f;
            f32 high = 0.0f;
            for (const auto densityPair : a.Density)
            {
                for (f32 density : { densityPair.x, densityPair.y })
                {
                    ASSERT_TRUE(std::isfinite(density));
                    EXPECT_GE(density, 0.0f);
                    EXPECT_LE(density, 1.0f);
                    sum += density;
                    low = std::min(low, density);
                    high = std::max(high, density);
                }
            }
            EXPECT_GT(sum, 0.0);
            EXPECT_GT(high - low, 1.0e-6f) << "a uniform synthetic alpha cannot measure a real coat";
            for (const auto& map : a.LengthDirection)
            {
                ASSERT_TRUE(std::isfinite(map.x) && std::isfinite(map.y) && std::isfinite(map.z));
                EXPECT_GT(map.x, 0.0f);
                EXPECT_LT(std::abs(map.y), 4.0f);
                EXPECT_LT(std::abs(map.z), 4.0f);
            }
            curves += a.Curves;
        }
        EXPECT_EQ(curves, groom->GetCurveCount());
    }
} // namespace

TEST(GroomShellSpikeBake, ShortCookedCoatHasDeterministicBoundCharts)
{
    CheckBake(false);
}
TEST(GroomShellSpikeBake, LongCookedCoatHasDeterministicBoundCharts)
{
    CheckBake(true);
}

TEST(GroomShellSpikeBake, ChartSelectionSeparatesOverlappingRootUVs)
{
    const auto charts = Charts(true);
    const glm::vec2 uv{ 0.3f, 0.51f };
    EXPECT_GT(glm::length(Surface(charts[1], LocalUV(charts[1], uv)) - Surface(charts[4], LocalUV(charts[4], uv))), 0.02f);
    EXPECT_EQ(Texel({ -0.001f, 0.5f }), Texel({ 0.999f, 0.5f }));
}

TEST(GroomShellSpikeBake, SweptSlabsCarryTheCookedFibreAreaAndDirection)
{
    const auto charts = Charts(false);
    const auto& chart = charts[0];
    const glm::vec2 localUV{ 0.1273f, 0.513f };
    const glm::vec3 root = Surface(chart, localUV);
    const std::array<glm::vec3, 2> points{ root, root + Normal(localUV.x) * 0.01f + glm::vec3(0.0f, 0.0f, 0.006f) };
    const std::array<f32, 2> widths{ 0.0001f, 0.0001f };
    GroomBuilder builder;
    u16 group = 0;
    std::string reason;
    ASSERT_TRUE(builder.AddGroup("/body#0", group, reason)) << reason;
    GroomCurveInput curve;
    curve.Points = points;
    curve.Widths = widths;
    curve.RootUV = { localUV.x, chart.VFrom + localUV.y * (chart.VTo - chart.VFrom) };
    curve.GroupId = group;
    ASSERT_TRUE(builder.AddCurve(curve, reason)) << reason;
    const auto groom = builder.Build(reason);
    ASSERT_TRUE(groom) << reason;
    const auto bake = BakeChart(*groom, chart, charts);
    const auto map = bake.LengthDirection[Texel(localUV)];
    EXPECT_NEAR(map.x, 0.01f, 1.0e-6f);
    EXPECT_NEAR(map.y, 0.0f, 1.0e-5f);
    EXPECT_NEAR(map.z, 0.6f, 1.0e-4f);
    const f64 cellArea = TwoPi * chart.Radius * (chart.To - chart.From) / (MapWidth * MapHeight);
    f64 sweptArea = 0.0;
    for (const auto density : bake.Density)
    {
        sweptArea += (-std::log1p(-static_cast<f64>(density.x)) - std::log1p(-static_cast<f64>(density.y))) * cellArea;
    }
    const f64 expected = 0.0001 * std::sqrt(0.01 * 0.01 + 0.006 * 0.006) + HeightSlices * 0.25 * 3.141592654 * 0.0001 * 0.0001;
    EXPECT_NEAR(sweptArea, expected, expected * 1.0e-4);

    // Same physical fibre with extra control points inside every height slab.
    // The old per-segment endcap sum changed the result with this subdivision.
    std::array<glm::vec3, 129> splitPoints{};
    std::array<f32, 129> splitWidths{};
    for (u32 i = 0; i < splitPoints.size(); ++i)
    {
        splitPoints[i] = glm::mix(points[0], points[1], static_cast<f32>(i) / (splitPoints.size() - 1));
        splitWidths[i] = widths[0];
    }
    GroomBuilder subdivided;
    ASSERT_TRUE(subdivided.AddGroup("/body#0", group, reason));
    curve.Points = splitPoints;
    curve.Widths = splitWidths;
    ASSERT_TRUE(subdivided.AddCurve(curve, reason));
    const auto splitGroom = subdivided.Build(reason);
    ASSERT_TRUE(splitGroom) << reason;
    const auto splitBake = BakeChart(*splitGroom, chart, charts);
    f64 splitArea = 0.0;
    for (const auto density : splitBake.Density)
    {
        splitArea += (-std::log1p(-static_cast<f64>(density.x)) - std::log1p(-static_cast<f64>(density.y))) * cellArea;
    }
    EXPECT_NEAR(splitArea, sweptArea, expected * 1.0e-4);
}
