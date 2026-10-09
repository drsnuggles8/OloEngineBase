// OLO_TEST_LAYER: L1
#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RayTracing/VegetationPolicy.h"
#include "OloEngine/Renderer/RayTracing/VegetationSurfaceCache.h"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <vector>

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

    // --- #1354: complete representations before the TLAS is withheld -------

    namespace
    {
        using Tier = VegetationPolicy::CastingTier;
        using GroupCost = VegetationPolicy::GroupCost;
        using CastingCost = VegetationPolicy::CastingGroupCost;

        // A mesh group and its card, as fractions of the limits: the mesh
        // costs `meshShare`ths of each, the card a 64th of the mesh.
        CastingCost MeshGroup(u64 meshShare, bool wasRequested = false)
        {
            const GroupCost mesh{ VegetationPolicy::GeometryBytes / meshShare, VegetationPolicy::AccelerationStructureBytes / meshShare,
                                  VegetationPolicy::VerticesPerFrame / meshShare, VegetationPolicy::TrianglesPerFrame / meshShare, 1u };
            const GroupCost card{ mesh.GeometryBytes / 64u, mesh.AccelerationBytes / 64u, mesh.RefreshVertices / 64u,
                                  mesh.RefreshTriangles / 64u, 1u };
            return { mesh, card, true, wasRequested };
        }
    } // namespace

    // A caster that does not fit at its requested tier is traced at its
    // complete lower-cost one instead of being refused: every plant stays in
    // the shadow rays' scene. The requested tier is a disc around the camera.
    TEST(VegetationCastingPlan, ACasterThatDoesNotFitIsTracedAsItsCardNotRefused)
    {
        // Six quarter-budget meshes: three fit after the cards, the rest are cards.
        std::array<CastingCost, 6> groups{};
        groups.fill(MeshGroup(4u));
        std::array<Tier, 6> tiers{};
        const auto plan = VegetationPolicy::ChooseCastingTiers(groups, tiers);
        EXPECT_EQ(tiers, (std::array{ Tier::Requested, Tier::Requested, Tier::Requested, Tier::Fallback, Tier::Fallback, Tier::Fallback }));
        EXPECT_EQ(plan.Out, 0u) << "a caster was refused while its card fitted";
        EXPECT_EQ(plan.Requested, 3u);
        EXPECT_EQ(plan.Fallbacks, 3u);
        EXPECT_NE(plan.Pressure, RayTracing::VegetationPressure::None) << "a fallback must name what forced it";
        const GroupCost upgrade = VegetationPolicy::Difference(groups[0].Requested, groups[0].Fallback);
        EXPECT_EQ(plan.Recovery.RefreshVertices, 3u * upgrade.RefreshVertices) << "recovery is the upgrades left undone";

        // Everything fits: everything is requested and nothing is under pressure.
        std::array<CastingCost, 2> two{ MeshGroup(16u), MeshGroup(16u) };
        std::array<Tier, 2> twoTiers{};
        const auto easy = VegetationPolicy::ChooseCastingTiers(two, twoTiers);
        EXPECT_EQ(twoTiers, (std::array{ Tier::Requested, Tier::Requested }));
        EXPECT_EQ(easy.Pressure, RayTracing::VegetationPressure::None);
        EXPECT_EQ(easy.Recovery.RefreshBuilds, 0u);
    }

    // A tier's WORK is its full refresh times how often it refreshes in steady
    // state: every frame for a detailed group, once per error deadline for a
    // proxy. Charged at a full refresh every frame, the IntegratedRenderer
    // meadow (1,681 casting groups, more builds than a frame has) was withheld
    // for good although its staggered proxies fit with room to spare.
    TEST(VegetationCastingPlan, APlanChargesEachTiersSteadyRefreshDemand)
    {
        CastingCost workHeavy = MeshGroup(4u);
        workHeavy.Requested.GeometryBytes = 1u; // memory never binds here
        workHeavy.Requested.AccelerationBytes = 1u;
        workHeavy.Fallback.GeometryBytes = 1u;
        workHeavy.Fallback.AccelerationBytes = 1u;
        std::array<CastingCost, 8> groups{};
        groups.fill(workHeavy);
        std::array<Tier, 8> tiers{};
        const auto everyFrame = VegetationPolicy::ChooseCastingTiers(groups, tiers);
        EXPECT_EQ(everyFrame.Out, 0u);
        EXPECT_LE(everyFrame.DemandVertices, static_cast<f64>(VegetationPolicy::VerticesPerFrame));
        EXPECT_LE(everyFrame.DemandTriangles, static_cast<f64>(VegetationPolicy::TrianglesPerFrame));
        EXPECT_LE(everyFrame.DemandBuilds, static_cast<f64>(VegetationPolicy::UpdatesPerFrame));
        EXPECT_EQ(everyFrame.Pressure, RayTracing::VegetationPressure::FrameWork);
        EXPECT_LT(everyFrame.Requested, 8u);

        // The same groups as proxies refreshing every fourth frame: all fit.
        for (CastingCost& group : groups)
            group.RequestedRate = group.FallbackRate = 0.25f;
        const auto staggered = VegetationPolicy::ChooseCastingTiers(groups, tiers);
        EXPECT_EQ(staggered.Requested, 8u) << "a staggered proxy was charged a full refresh every frame";
        EXPECT_EQ(staggered.Pressure, RayTracing::VegetationPressure::None);

        // More groups than a frame has builds: every frame, the tail is out;
        // staggered, every one is in.
        std::vector<CastingCost> crowd(VegetationPolicy::UpdatesPerFrame + 600u, CastingCost{ { 1u, 1u, 4u, 2u, 1u }, {}, false, false });
        std::vector<Tier> crowdTiers(crowd.size());
        EXPECT_EQ(VegetationPolicy::ChooseCastingTiers(crowd, crowdTiers).Out, 600u);
        for (CastingCost& group : crowd)
            group.RequestedRate = 0.2f;
        const auto amortized = VegetationPolicy::ChooseCastingTiers(crowd, crowdTiers);
        EXPECT_EQ(amortized.Out, 0u) << "builds that refresh one frame in five were charged every frame";
        EXPECT_NEAR(amortized.DemandBuilds, 0.2 * static_cast<f64>(crowd.size()), 1e-3);

        // The build count binds on its own: one build per part, many parts.
        CastingCost manyParts{ { 1u, 1u, 1u, 1u, VegetationPolicy::UpdatesPerFrame / 2u }, { 1u, 1u, 1u, 1u, 1u }, true, false };
        std::array<CastingCost, 3> three{ manyParts, manyParts, manyParts };
        std::array<Tier, 3> threeTiers{};
        const auto builds = VegetationPolicy::ChooseCastingTiers(three, threeTiers);
        EXPECT_LE(builds.DemandBuilds, static_cast<f64>(VegetationPolicy::UpdatesPerFrame));
        EXPECT_EQ(threeTiers[2], Tier::Fallback);
    }

    // The steady refresh rate: a detailed group every frame, a proxy once per
    // deadline (0.25 m of motion), and nothing while the wind clock is paused.
    TEST(VegetationCastingPlan, ARefreshRateFollowsTheProxyDeadline)
    {
        constexpr f32 velocity = 5.0f; // deadline 0.05 s
        EXPECT_FLOAT_EQ(VegetationPolicy::RefreshRate(true, velocity, 0.01f), 1.0f);
        EXPECT_FLOAT_EQ(VegetationPolicy::RefreshRate(false, velocity, 0.0125f), 0.25f);
        EXPECT_FLOAT_EQ(VegetationPolicy::RefreshRate(false, velocity, 0.2f), 1.0f) << "a frame longer than the deadline refreshes every frame";
        EXPECT_FLOAT_EQ(VegetationPolicy::RefreshRate(false, velocity, 0.0f), 0.0f);
        EXPECT_FLOAT_EQ(VegetationPolicy::RefreshRate(false, std::numeric_limits<f32>::infinity(), 0.01f), 1.0f);
    }

    // When even the cheapest complete tier does not fit, the casters cannot be
    // complete. Admission stops at the first misfit (no far group slips in
    // behind a near one), the rest are OUT, and the plan says why and what
    // recovery would cost: the explicit technique fallback, never a silent gap.
    TEST(VegetationCastingPlan, WhenNoTierFitsTheCastersAreLeftOutAndCounted)
    {
        const CastingCost cardOnly{ { VegetationPolicy::GeometryBytes / 3u, 0u, 0u, 0u, 1u }, {}, false, false };
        std::array<CastingCost, 4> groups{ cardOnly, cardOnly, cardOnly, cardOnly };
        std::array<Tier, 4> tiers{};
        const auto plan = VegetationPolicy::ChooseCastingTiers(groups, tiers);
        EXPECT_EQ(tiers, (std::array{ Tier::Requested, Tier::Requested, Tier::Requested, Tier::Out }));
        EXPECT_EQ(plan.Out, 1u);
        EXPECT_EQ(plan.Pressure, RayTracing::VegetationPressure::GeometryMemory);
        EXPECT_EQ(plan.Recovery.GeometryBytes, cardOnly.Requested.GeometryBytes);

        // The resident-group cap is a limit like any other.
        std::vector<CastingCost> many(VegetationPolicy::ResidentGroups + 2u, CastingCost{ { 1u, 1u, 1u, 1u, 0u }, {}, false, false });
        std::vector<Tier> manyTiers(many.size());
        const auto crowded = VegetationPolicy::ChooseCastingTiers(many, manyTiers);
        EXPECT_EQ(crowded.Out, 2u);
        EXPECT_EQ(crowded.Pressure, RayTracing::VegetationPressure::ResidentGroups);
    }

    // Hysteresis on the tier: a group that was NOT at its requested tier last
    // frame is upgraded only with a margin of every budget left over, so a
    // group whose upgrade just fits does not flip, and rebuild, every frame.
    // One that held the tier keeps it while it still fits exactly.
    TEST(VegetationCastingPlan, AnUpgradeAtTheMarginNeedsHeadroomUnlessTheGroupHeldItsTier)
    {
        // One card group and one mesh group whose upgrade leaves less than
        // the margin of the geometry budget free.
        const u64 margin = VegetationPolicy::GeometryBytes / VegetationPolicy::UpgradeMarginDivisor;
        CastingCost atTheMargin{ { VegetationPolicy::GeometryBytes - margin / 2u, 0u, 0u, 0u, 1u }, { 1u, 0u, 0u, 0u, 1u }, true, false };
        std::array<CastingCost, 1> group{ atTheMargin };
        std::array<Tier, 1> tier{};
        VegetationPolicy::ChooseCastingTiers(group, tier);
        EXPECT_EQ(tier[0], Tier::Fallback) << "an upgrade into the margin would flip back next frame";

        group[0].WasRequested = true;
        VegetationPolicy::ChooseCastingTiers(group, tier);
        EXPECT_EQ(tier[0], Tier::Requested) << "a group that held its tier lost it while it still fitted";
    }

    // Reflection-only groups charge the resident-group cap as well: a group
    // the planner admits past it would be refused by the cache, and a refusal
    // withholds the TLAS from every reflection ray.
    TEST(VegetationCastingPlan, ReflectionTiersStopAtTheResidentGroupCap)
    {
        using RTier = VegetationPolicy::ReflectionTier;
        const VegetationPolicy::ReflectionGroupCost tiny{ 1u, 1u, 0u, 0u };
        std::array<VegetationPolicy::ReflectionGroupCost, 4> groups{ tiny, tiny, tiny, tiny };
        std::array<RTier, 4> tiers{};
        VegetationPolicy::ChooseReflectionTiers(groups, 0u, 0u, tiers, VegetationPolicy::ResidentGroups - 2u);
        EXPECT_EQ(tiers, (std::array{ RTier::Card, RTier::Card, RTier::Out, RTier::Out }));
    }

    // Reduced cadence within a DECLARED bound: a caster holds a snapshot only
    // inside the proxies' deadline (shadow error 0.25 m), a reflection-only
    // group inside the looser reflection bound, and neither past it.
    TEST(VegetationCastingPlan, AHeldSnapshotStaysInsideItsDeclaredErrorBound)
    {
        constexpr f32 velocity = 10.0f; // proxy deadline 25 ms
        EXPECT_TRUE(VegetationPolicy::CanHoldSnapshot(1.02f, 1.0f, velocity, /*castsShadows*/ true));
        EXPECT_FALSE(VegetationPolicy::CanHoldSnapshot(1.03f, 1.0f, velocity, true)) << "a caster held past 0.25 m of error";
        EXPECT_TRUE(VegetationPolicy::CanHoldSnapshot(1.09f, 1.0f, velocity, false));
        EXPECT_FALSE(VegetationPolicy::CanHoldSnapshot(1.11f, 1.0f, velocity, false)) << "a reflection held past 1 m of error";
        EXPECT_FALSE(VegetationPolicy::CanHoldSnapshot(2.5f, 1.0f, 0.0f, false)) << "a still plant held forever";
        EXPECT_FALSE(VegetationPolicy::CanHoldSnapshot(0.5f, 1.0f, velocity, false)) << "time ran backwards";
        EXPECT_FALSE(VegetationPolicy::CanHoldSnapshot(1.0f, 1.0f, std::numeric_limits<f32>::infinity(), false));
        EXPECT_FALSE(VegetationPolicy::CanHoldSnapshot(std::numeric_limits<f32>::quiet_NaN(), 1.0f, 1.0f, true));
    }

    // The technique's hysteresis: a failure takes effect at once; recovery
    // waits for RecoveryFrames complete frames in a row, so demand flickering
    // at the budget degrades the technique once instead of every other frame.
    TEST(VegetationCastingPlan, RecoveryWaitsForAStableRunOfCompleteFrames)
    {
        u32 clean = VegetationPolicy::RecoveryFrames;
        EXPECT_TRUE(VegetationPolicy::Recovered(true, clean)) << "nothing has failed: publish at once";
        EXPECT_FALSE(VegetationPolicy::Recovered(false, clean));
        for (u32 frame = 1u; frame < VegetationPolicy::RecoveryFrames; ++frame)
            EXPECT_FALSE(VegetationPolicy::Recovered(true, clean)) << "published after " << frame << " complete frames";
        EXPECT_TRUE(VegetationPolicy::Recovered(true, clean));

        // Alternating frames never come back.
        u32 flicker = VegetationPolicy::RecoveryFrames;
        u32 published = 0u;
        for (u32 frame = 0u; frame < 64u; ++frame)
            published += VegetationPolicy::Recovered(frame % 2u == 1u, flicker) ? 1u : 0u;
        EXPECT_EQ(published, 0u) << "the technique oscillated with the demand";
    }

    TEST(VegetationCastingPlan, EveryPressureSourceHasAName)
    {
        for (u32 i = 0u; i < static_cast<u32>(RayTracing::VegetationPressure::Count); ++i)
            EXPECT_STRNE(RayTracing::ToString(static_cast<RayTracing::VegetationPressure>(i)), "unknown");
        RayTracing::VegetationSurfaceStats stats;
        EXPECT_EQ(stats.DominantPressure(), RayTracing::VegetationPressure::None);
        stats.Pressure[static_cast<sizet>(RayTracing::VegetationPressure::FrameWork)] = 2u;
        stats.Pressure[static_cast<sizet>(RayTracing::VegetationPressure::GeometryMemory)] = 5u;
        EXPECT_EQ(stats.DominantPressure(), RayTracing::VegetationPressure::GeometryMemory);
    }

    // The order-free plant term: a group's share of the representation
    // signature is the same however its plants are split into groups.
    TEST(VegetationCastingPlan, ThePlantSetSumDoesNotSeeHowPlantsAreGrouped)
    {
        u64 whole = 0u;
        for (u64 id = 100u; id < 164u; ++id)
            whole += RayTracing::VegetationPlantTerm(id);
        u64 left = 0u, right = 0u;
        for (u64 id = 100u; id < 120u; ++id)
            left += RayTracing::VegetationPlantTerm(id);
        for (u64 id = 120u; id < 164u; ++id)
            right += RayTracing::VegetationPlantTerm(id);
        EXPECT_EQ(left + right, whole);
        EXPECT_NE(RayTracing::VegetationPlantTerm(1u), RayTracing::VegetationPlantTerm(2u));
    }
} // namespace OloEngine::Tests
