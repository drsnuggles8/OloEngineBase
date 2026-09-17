// OLO_TEST_LAYER: L1
#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RayTracing/VegetationPolicy.h"

#include <gtest/gtest.h>

namespace OloEngine::Tests
{
    using RayTracing::VegetationFrameBudget;
    using RayTracing::VegetationPolicy;

    TEST(VegetationPolicy, SnapshotCannotSurviveResetReverseTimeOrItsErrorDeadline)
    {
        // The maximum declared error is reached at 25 ms for this velocity.
        EXPECT_TRUE(VegetationPolicy::CanReuseSnapshot(1.024f, 1.0f, 10.0f, true));
        EXPECT_FALSE(VegetationPolicy::CanReuseSnapshot(1.026f, 1.0f, 10.0f, true));
        EXPECT_FALSE(VegetationPolicy::CanReuseSnapshot(0.99f, 1.0f, 10.0f, true));
        EXPECT_FALSE(VegetationPolicy::CanReuseSnapshot(1.0f, 1.0f, 10.0f, false));
        EXPECT_FALSE(VegetationPolicy::CanReuseSnapshot(std::numeric_limits<f32>::infinity(), 1.0f, 10.0f, true));
        EXPECT_FALSE(VegetationPolicy::CanReuseSnapshot(1.0f, 1.0f, std::numeric_limits<f32>::quiet_NaN(), true));
        EXPECT_NEAR(VegetationPolicy::ProxyAgeLimit(0.0f), 0.05f, 1e-6f);
        EXPECT_NEAR(VegetationPolicy::ProxyAgeLimit(100.0f), 0.0025f, 1e-6f);
    }

    TEST(VegetationPolicy, ReservationRefusalLeavesCapacityForSmallerGroups)
    {
        VegetationFrameBudget budget;
        EXPECT_FALSE(budget.Reserve(std::numeric_limits<u64>::max(), 1u));
        EXPECT_TRUE(budget.Reserve(100u, 20u));
        EXPECT_FALSE(budget.Reserve(1u, std::numeric_limits<u64>::max()));
        EXPECT_EQ(budget.Updates, 1u);
        EXPECT_EQ(budget.Vertices, 100u);
        EXPECT_EQ(budget.Triangles, 20u);
        EXPECT_TRUE(budget.Reserve(VegetationPolicy::VerticesPerFrame - 100u,
                                   VegetationPolicy::TrianglesPerFrame - 20u));
        EXPECT_FALSE(budget.Reserve(1u, 0u));
        EXPECT_FALSE(budget.Reserve(0u, 1u));
    }

    TEST(VegetationPolicy, UpdateCountIsBoundedIndependentlyOfVertexAndTriangleCost)
    {
        VegetationFrameBudget budget;
        for (u32 i = 0; i < VegetationPolicy::UpdatesPerFrame; ++i)
            ASSERT_TRUE(budget.Reserve(4u, 2u));
        EXPECT_FALSE(budget.Reserve(4u, 2u));
        EXPECT_EQ(budget.Updates, VegetationPolicy::UpdatesPerFrame);
    }

    TEST(VegetationPolicy, WindRateHandlesFieldCapLegacyModesAndWorldScale)
    {
        const auto rate = [](bool hierarchy, bool field, f32 norm)
        {
            return VegetationPolicy::WindVelocityBound(2.0f, 1.1f, 0.7f, 0.8f,
                                                       hierarchy, field, 8.0f, 0.6f, 0.4f, norm);
        };
        EXPECT_GT(rate(true, true, 1.0f), 0.0f);
        EXPECT_NEAR(rate(true, true, 3.0f), 3.0f * rate(true, true, 1.0f), 1e-5f);
        EXPECT_GT(rate(true, false, 1.0f), 0.0f);
        EXPECT_GT(rate(false, false, 1.0f), 0.0f);
        EXPECT_TRUE(std::isinf(rate(true, true, -1.0f)));
        EXPECT_TRUE(std::isinf(VegetationPolicy::WindVelocityBound(1.0f, 1.0f, 2.0f, 0.0f,
                                                                   true, true, 8.0f, 0.0f, 1.0f, 1.0f)));
    }
} // namespace OloEngine::Tests
