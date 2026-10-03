// OLO_TEST_LAYER: L1
#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RayTracing/VegetationPolicy.h"
#include "OloEngine/Renderer/RayTracing/VegetationSurfaceCache.h"

#include <gtest/gtest.h>

#include <array>
#include <limits>

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

    // #1533: a layer that casts no raster shadow is masked out of shadow rays, so
    // only a reflection ray can read it. Traced for shadows alone, the showcase
    // dog's non-casting lawn overran the budget and withheld the whole TLAS.
    TEST(VegetationPolicy, ALayerThatCastsNoShadowIsTracedOnlyForReflections)
    {
        EXPECT_FALSE(VegetationPolicy::TracesLayer(/*castsShadows*/ false, /*reflectionsReadVegetation*/ false));
        EXPECT_TRUE(VegetationPolicy::TracesLayer(false, true));
        EXPECT_TRUE(VegetationPolicy::TracesLayer(true, false));
        EXPECT_TRUE(VegetationPolicy::TracesLayer(true, true));
    }

    // #1533: a layer that casts no shadow enters the ray-traced scene as cards,
    // nearest first, while they fit what the casting layers left -- geometry
    // AND acceleration structures -- and then the nearest card groups become
    // the mesh while the difference fits. Each pass ends at its first misfit,
    // so each tier is a disc around the camera; a card-only group (beyond the
    // mesh distance) is no misfit for the upgrade pass.
    TEST(VegetationPolicy, ReflectionOnlyGroupsAreCardsNearestFirstAndTheNearestBecomeTheMesh)
    {
        using Tier = VegetationPolicy::ReflectionTier;
        using Cost = VegetationPolicy::ReflectionGroupCost;
        const u64 g = VegetationPolicy::GeometryBytes / 16u;
        const u64 a = VegetationPolicy::AccelerationStructureBytes / 16u;
        const Cost meshGroup{ g, a, 4u * g, 4u * a }; // the mesh tier costs three more sixteenths
        const Cost cardOnly{ g, a, 0u, 0u };
        const std::array<Cost, 6> nearestFirst{ meshGroup, meshGroup, cardOnly, meshGroup, meshGroup, cardOnly };
        std::array<Tier, 6> tiers{};

        // Six cards leave ten sixteenths: three upgrades fit, the fourth does not.
        VegetationPolicy::ChooseReflectionTiers(nearestFirst, 0u, 0u, tiers);
        EXPECT_EQ(tiers, (std::array{ Tier::Mesh, Tier::Mesh, Tier::Card, Tier::Mesh, Tier::Card, Tier::Card }));

        // The acceleration-structure room binds on its own: the geometry would fit everything.
        VegetationPolicy::ChooseReflectionTiers(nearestFirst, 0u, VegetationPolicy::AccelerationStructureBytes - 2u * a, tiers);
        EXPECT_EQ(tiers, (std::array{ Tier::Card, Tier::Card, Tier::Out, Tier::Out, Tier::Out, Tier::Out }));

        // A card that does not fit ends admission: no far small group slips in behind it.
        const std::array<Cost, 3> bigThenSmall{ cardOnly, Cost{ VegetationPolicy::GeometryBytes, 0u, 0u, 0u }, cardOnly };
        std::array<Tier, 3> three{};
        VegetationPolicy::ChooseReflectionTiers(bigThenSmall, 0u, 0u, three);
        EXPECT_EQ(three, (std::array{ Tier::Card, Tier::Out, Tier::Out }));

        // Casting layers that took a whole budget leave nothing.
        VegetationPolicy::ChooseReflectionTiers(nearestFirst, VegetationPolicy::GeometryBytes, 0u, tiers);
        EXPECT_EQ(tiers, (std::array<Tier, 6>{}));
        VegetationPolicy::ChooseReflectionTiers(nearestFirst, std::numeric_limits<u64>::max(), std::numeric_limits<u64>::max(), tiers);
        EXPECT_EQ(tiers, (std::array<Tier, 6>{}));
        VegetationPolicy::ChooseReflectionTiers({}, 0u, 0u, std::span<Tier>{});
    }

    // #1533: last frame's unrecorded builds are spent before any refresh, so
    // refreshes plus retries never ask the backend for more than one frame's
    // budget. A debt larger than the frame saturates rather than wrapping.
    TEST(VegetationPolicy, ABuildDebtIsSpentBeforeAnyRefresh)
    {
        VegetationFrameBudget budget;
        budget.Charge({ 1u, 4u, VegetationPolicy::TrianglesPerFrame - 2u });
        EXPECT_TRUE(budget.Reserve(4u, 2u)) << "the room the debt left";
        EXPECT_FALSE(budget.Reserve(4u, 1u)) << "a refresh spent budget the retries need";

        VegetationFrameBudget overdrawn;
        overdrawn.Charge({ std::numeric_limits<u32>::max(), std::numeric_limits<u64>::max(), std::numeric_limits<u64>::max() });
        EXPECT_EQ(overdrawn.Updates, VegetationPolicy::UpdatesPerFrame);
        EXPECT_EQ(overdrawn.Vertices, VegetationPolicy::VerticesPerFrame);
        EXPECT_EQ(overdrawn.Triangles, VegetationPolicy::TrianglesPerFrame);
        EXPECT_FALSE(overdrawn.Reserve(0u, 0u));

        // One build per part, as the backend counts them.
        VegetationFrameBudget parts;
        parts.Charge({ VegetationPolicy::UpdatesPerFrame - 2u, 0u, 0u });
        EXPECT_FALSE(parts.Reserve(1u, 1u, 3u));
        EXPECT_TRUE(parts.Reserve(1u, 1u, 2u));
    }

    // #1533: what each kind of gap costs. A reflection-only group left out
    // nearest first costs nothing (it is counted); a refused one costs the
    // frame's completeness, which withholds the TLAS from reflections; only a
    // refused CASTING group costs what shadow rays need.
    TEST(VegetationPolicy, OnlyACastingGapCostsWhatShadowRaysNeed)
    {
        RayTracing::VegetationSurfaceCache cache;
        cache.SetEnabled(true);
        cache.BeginFrame();
        cache.CountBeyondReflectionBudget(3u, 96u, 12.5f, 0.8f);
        EXPECT_TRUE(cache.GetStats().Complete) << "a group left out nearest first is not a refusal";
        EXPECT_TRUE(cache.GetStats().CastersComplete);
        EXPECT_EQ(cache.GetStats().Refused, 0u);
        EXPECT_EQ(cache.GetStats().BeyondReflectionBudget, 3u);
        EXPECT_EQ(cache.GetStats().PlantsBeyondReflectionBudget, 96u);
        EXPECT_FLOAT_EQ(cache.GetStats().ReflectionReach, 12.5f);
        EXPECT_FLOAT_EQ(cache.GetStats().ReflectionDetailReach, 0.8f);

        cache.Refuse(/*castsShadows*/ false);
        RayTracing::VegetationSurfaceInput invalid; // no rest stream: refused by Queue
        invalid.CastShadows = false;
        cache.Queue(invalid);
        EXPECT_FALSE(cache.GetStats().Complete);
        EXPECT_TRUE(cache.GetStats().CastersComplete) << "a reflection-only refusal took shadow rays away";
        EXPECT_EQ(cache.GetStats().Refused, 2u);

        invalid.CastShadows = true;
        cache.Queue(invalid);
        EXPECT_FALSE(cache.GetStats().CastersComplete) << "a missing caster would leak light through it";

        cache.BeginFrame();
        EXPECT_TRUE(cache.GetStats().Complete);
        EXPECT_TRUE(cache.GetStats().CastersComplete);
        EXPECT_EQ(cache.GetStats().BeyondReflectionBudget, 0u);
        EXPECT_FLOAT_EQ(cache.GetStats().ReflectionReach, 0.0f);
        EXPECT_FLOAT_EQ(cache.GetStats().ReflectionDetailReach, 0.0f);
        cache.Shutdown();
    }
} // namespace OloEngine::Tests
