// OLO_TEST_LAYER: L1
#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Foliage/FoliageWind.h"
#include "OloEngine/Terrain/Foliage/FoliageInstanceRegistry.h"
#include <gtest/gtest.h>
#include <glm/gtx/component_wise.hpp>
#include <limits>

namespace OloEngine::Tests
{
    TEST(FoliageWindContract, BoundsCoverCappedFieldAndHierarchicalModes)
    {
        for (f32 strength : { 0.0f, 0.3f, 2.0f, 20.0f })
        {
            for (f32 branch : { 0.0f, 0.5f, 1.0f })
            {
                for (f32 leaf : { 0.0f, 0.5f, 1.0f })
                {
                    const auto weights = SanitizeFoliageWind(0.5f, branch, leaf);
                    FoliageBoundsProfile profile;
                    profile.m_WindDisplacement = FoliageWindMaximumDisplacement(strength, weights);
                    const auto box = FoliageInstanceBounds({ 10.0f, 2.0f, -3.0f }, 2.0f, 4.0f, profile);
                    // Triangle inequality for independent, adversarial phases.
                    const f32 extent = strength * (2.0f + std::sqrt(1.25f) * (0.3f * branch + 0.13f * leaf));
                    EXPECT_LE(10.0f + 1.0f + extent, box.Max.x + 1e-5f);
                    EXPECT_GE(2.0f - extent, box.Min.y - 1e-5f);
                    EXPECT_LE(10.0f + extent, box.Max.y + 1e-5f);
                    EXPECT_GE(-3.0f - 1.0f - extent, box.Min.z - 1e-5f);
                }
            }
        }
    }

    TEST(FoliageWindContract, ObliqueImpostorCornersStayInsideBounds)
    {
        constexpr f32 radius = 2.0f;
        const f32 extent = FoliageImpostorBoundsRadius(radius);
        for (f32 yaw : { 0.0f, 0.4f, 0.8f, 1.2f })
        {
            for (f32 pitch : { -0.7f, 0.0f, 0.7f })
            {
                const glm::vec3 z(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
                const auto x = glm::normalize(glm::cross(glm::vec3(0, 1, 0), z));
                const auto y = glm::cross(z, x);
                for (f32 sx : { -1.0f, 1.0f })
                    for (f32 sy : { -1.0f, 1.0f })
                    {
                        const auto corner = radius * (sx * x + sy * y);
                        EXPECT_LE(glm::compMax(glm::abs(corner)), extent + 1e-5f);
                    }
            }
        }
        EXPECT_GE(FoliageWindMaximumDisplacement(1.0f, glm::vec4(0), 100.0f), 100.0f);
    }

    TEST(FoliageWindContract, WindEditsPublishUpdatedBoundsWithoutRephasingIdentity)
    {
        FoliageInstanceRegistry registry;
        FoliageLayer layer;
        FoliageBoundsProfile profile;
        FoliageInstanceData row{};
        row.PositionScale = { 1.0f, 0.0f, 1.0f, 1.0f };
        row.RotationHeight.y = 2.0f;
        const auto generate = [&]()
        {
            registry.BeginGeneration({ layer });
            registry.BeginLayer(0, layer, 1236, 1.0f, 16.0f, 16.0f, FoliageRepresentation::MeshCard, false, profile);
            registry.AddInstance(1, 1, row, 0);
            registry.EndLayer();
            registry.EndGeneration();
        };
        generate();
        const auto id = registry.GetRecords()[0].m_Id;
        const auto initial = registry.GetGeneration();
        profile.m_WindDisplacement = 5.0f;
        generate();
        EXPECT_GT(registry.GetGeneration(), initial);
        EXPECT_EQ(registry.GetRecords()[0].m_Id, id);
        EXPECT_FLOAT_EQ(registry.GetRecords()[0].m_LocalBounds.Min.x, -4.5f);
        const auto boundsGeneration = registry.GetGeneration();
        layer.WindStiffness = 0.5f; // changes deformation without changing the conservative envelope
        generate();
        EXPECT_GT(registry.GetGeneration(), boundsGeneration);
        EXPECT_EQ(registry.GetRecords()[0].m_Id, id);
    }

    TEST(FoliageWindContract, IdentityPhaseIsStableAndDesynchronised)
    {
        for (u64 id = 1; id < 1000; ++id)
        {
            const auto phase = FoliageWindPhase(id);
            EXPECT_TRUE(Math::BitwiseEqual(phase, FoliageWindPhase(id)));
            EXPECT_GE(phase, 0.0f);
            EXPECT_LT(phase, 6.2831853f);
            EXPECT_GT(std::abs(phase - FoliageWindPhase(id + 1)), 1e-6f);
        }
    }

    TEST(FoliageWindContract, PauseResetAndRegenerationSeedHistoryOnce)
    {
        FoliageWindHistory history;
        history.Advance(4.0f, 3.0f);
        EXPECT_FLOAT_EQ(history.PreviousTime, 4.0f);
        history.Advance(5.0f, 4.0f);
        EXPECT_FLOAT_EQ(history.PreviousTime, 4.0f);
        history.Advance(5.0f, 5.0f);
        EXPECT_FLOAT_EQ(history.PreviousTime, history.Time);
        history.Advance(0.0f, 5.0f);
        EXPECT_FLOAT_EQ(history.PreviousTime, 0.0f);
        history.Reset();
        history.Advance(6.0f, 5.0f);
        EXPECT_FLOAT_EQ(history.PreviousTime, 6.0f);
        history.Advance(7.0f, 6.0f);
        EXPECT_FLOAT_EQ(history.PreviousTime, 6.0f);
    }

    TEST(FoliageWindContract, InvalidWeightsCannotReachShaderOrBounds)
    {
        const auto weights = SanitizeFoliageWind(std::numeric_limits<f32>::quiet_NaN(), -2.0f, 8.0f);
        EXPECT_FLOAT_EQ(weights.x, 0.0f);
        EXPECT_FLOAT_EQ(weights.y, 0.0f);
        EXPECT_FLOAT_EQ(weights.z, 1.0f);
        for (const f32 invalid : { std::numeric_limits<f32>::quiet_NaN(),
                                   std::numeric_limits<f32>::infinity(),
                                   -std::numeric_limits<f32>::infinity() })
        {
            const auto sanitized = SanitizeFoliageWind(invalid, invalid, invalid);
            EXPECT_FLOAT_EQ(sanitized.x, 0.0f);
            EXPECT_FLOAT_EQ(sanitized.y, 0.0f);
            EXPECT_FLOAT_EQ(sanitized.z, 0.0f);
        }
    }
} // namespace OloEngine::Tests
