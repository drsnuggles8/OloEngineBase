#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// GroomCoatShadowPropertyTests — issue #1248, acceptance criterion 1.
//
// "Prototype density-volume/deep-shadow options against a dense static coat and
//  select using quality, cost and memory evidence."
//
// This file IS that evidence. docs/analysis/groom-coat-self-shadowing-1248.md
// records the decision; every claim it makes is an assertion here, so the
// argument behind the selection fails loudly if it stops being true rather than
// ageing quietly in a merged PR body.
//
// WHAT IS ASSERTED, AND WHAT IS NOT. The claims are ORDERINGS and BANDS, not
// exact floats. An exact float would pin a coincidence: it would fail on a
// different `exp` implementation while a genuinely inverted comparison — the
// thing that would actually change the decision — would still pass a loose
// tolerance around it. So the cases assert "the anisotropic volume beats the
// deep opacity map", "finer is worse past the optimum", "a coarser march is not
// worse", which are the statements the document actually makes.
//
// Everything here is CPU, deterministic and integer-hashed, so it runs on a CI
// machine with no GPU and produces the same numbers there as on the box the
// analysis was written from.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Groom/GroomCoatShadow.h"
#include "Groom/GroomStrandFixture.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::GroomCoatShadow;

namespace
{
    // The authored width scale of the groom scenes (GroomFibreCoat.olo uses
    // 25), which is what makes the reference coats dense enough for the
    // comparison to be about density at all.
    constexpr f32 kSceneWidthScale = 25.0f;
    // A single fibre crossing passes 1/e of the light through it. The
    // comparison's unit, and deliberately NOT the component's authored default
    // (4.0 since #1360): every arm here is evaluated through the same kappa, so
    // what it has to be is a value that leaves the truth in a discriminating
    // range — which TheComparisonRunsInADiscriminatingRangeRatherThanAtBlack
    // asserts rather than assumes.
    constexpr f32 kKappa = 1.0f;

    struct Case
    {
        std::vector<CoatSegment> Segments;
        CoatSegmentGrid Grid;
        std::vector<CoatProbe> Probes;
        std::vector<f32> Truth;
        ReferenceSettings Reference;
        f32 MeanSpacing = 0.0f;
    };

    [[nodiscard]] f32 MeanStrandSpacing(const std::vector<CoatSegment>& segments, u32 curveCount)
    {
        glm::vec3 lo{ 0.0f };
        glm::vec3 hi{ 0.0f };
        if (!CoatSegmentBounds(segments, lo, hi) || curveCount == 0)
        {
            return 0.0f;
        }
        const glm::vec3 extent = hi - lo;
        return std::sqrt((extent.x * extent.z) / static_cast<f32>(curveCount));
    }

    // Builds one coat, its grid, its probes and the ground truth at those
    // probes. Shared so every case below is judged against the same truth
    // rather than each re-deriving one slightly differently.
    [[nodiscard]] Case MakeCase(const Ref<GroomAsset>& groom, const glm::vec3& lightDirection, u32 probeCount = 300u,
                                u32 rays = 96u)
    {
        Case testCase;

        CoatSampleSettings sampleSettings;
        sampleSettings.MaxStrands = 200000u;
        sampleSettings.WidthScale = kSceneWidthScale;
        BuildCoatSegments(*groom, glm::mat4(1.0f), sampleSettings, testCase.Segments);

        testCase.MeanSpacing = MeanStrandSpacing(testCase.Segments, groom->GetCurveCount());

        BuildCoatSegmentGrid(testCase.Segments, 64u, testCase.Grid);
        BuildCoatProbes(testCase.Segments, lightDirection, probeCount, 1248u, testCase.Probes);

        testCase.Reference.Rays = rays;
        // FOUR MEAN SPACINGS. Below one spacing the reference aliases against
        // the coat's own regularity and reports a confident zero — the trap the
        // analysis document tabulates and TheFootprintMustSpanSeveralSpacings
        // pins.
        testCase.Reference.FootprintRadius = testCase.MeanSpacing * 4.0f;

        testCase.Truth =
            EvaluateCoatShadowReference(testCase.Segments, testCase.Probes, kKappa, testCase.Reference,
                                        &testCase.Grid);
        return testCase;
    }

    [[nodiscard]] f64 MeanOf(const std::vector<f32>& values)
    {
        if (values.empty())
        {
            return 0.0;
        }
        f64 sum = 0.0;
        for (const f32 v : values)
        {
            sum += static_cast<f64>(v);
        }
        return sum / static_cast<f64>(values.size());
    }

    [[nodiscard]] CoatShadowError ErrorOfVolume(const Case& testCase, const DensityVolume& volume, bool anisotropic,
                                                f32 stepScale)
    {
        std::vector<f32> got;
        got.reserve(testCase.Probes.size());
        f64 totalSteps = 0.0;
        for (const CoatProbe& probe : testCase.Probes)
        {
            u32 steps = 0;
            const f64 tau = SampleDensityVolume(volume, probe.Position, probe.Direction, anisotropic, stepScale,
                                                &steps);
            totalSteps += static_cast<f64>(steps);
            got.push_back(CoatTransmittance(tau, kKappa));
        }
        return CompareCoatShadow(got, testCase.Truth, totalSteps / static_cast<f64>(testCase.Probes.size()));
    }

    [[nodiscard]] CoatShadowError ErrorOfDeepMap(const Case& testCase, const DeepOpacityMap& map)
    {
        std::vector<f32> got;
        got.reserve(testCase.Probes.size());
        for (const CoatProbe& probe : testCase.Probes)
        {
            got.push_back(CoatTransmittance(SampleDeepOpacityMap(map, probe.Position), kKappa));
        }
        return CompareCoatShadow(got, testCase.Truth, 3.0);
    }

    [[nodiscard]] DensityVolume BuildVolumeAt(const Case& testCase, u32 resolution)
    {
        DensityVolumeSettings settings;
        settings.Resolution = resolution;
        DensityVolume volume;
        BuildDensityVolume(testCase.Segments, settings, volume, nullptr);
        return volume;
    }

    [[nodiscard]] DeepOpacityMap BuildMapAt(const Case& testCase, const glm::vec3& lightDirection, u32 resolution,
                                            u32 layers)
    {
        DeepOpacityMapSettings settings;
        settings.Width = resolution;
        settings.Height = resolution;
        settings.Layers = layers;
        settings.LayerSpan = 0.25f;
        DeepOpacityMap map;
        BuildDeepOpacityMap(testCase.Segments, lightDirection, settings, map, nullptr);
        return map;
    }

    const glm::vec3 kSideLight{ 1.0f, 0.0f, 0.0f };
    const glm::vec3 kObliqueLight = glm::normalize(glm::vec3(0.6f, -0.5f, 0.62f));
} // namespace

// ── The ground truth itself ──────────────────────────────────────────────────

TEST(GroomCoatShadowReference, ALatticeOfParallelCylindersMatchesItsAnalyticCrossingCount)
{
    // The one configuration whose answer can be derived by hand: N columns of
    // parallel cylinders of radius r at pitch p, crossed by a perpendicular
    // ray, give N * 2r/p expected crossings. If the reference cannot reproduce
    // that, nothing measured against it means anything.
    constexpr i32 kN = 40;
    constexpr f32 kExtent = 0.2f;
    constexpr f32 kHeight = 0.1f;
    constexpr f32 kRadius = 0.0006f;

    std::vector<CoatSegment> segments;
    for (i32 i = 0; i < kN; ++i)
    {
        for (i32 k = 0; k < kN; ++k)
        {
            const f32 x = kExtent * (static_cast<f32>(i) / static_cast<f32>(kN - 1) - 0.5f);
            const f32 z = kExtent * (static_cast<f32>(k) / static_cast<f32>(kN - 1) - 0.5f);
            for (i32 p = 0; p < 4; ++p)
            {
                CoatSegment segment;
                segment.A = glm::vec3(x, kHeight * static_cast<f32>(p) / 4.0f, z);
                segment.B = glm::vec3(x, kHeight * static_cast<f32>(p + 1) / 4.0f, z);
                segment.RadiusA = kRadius;
                segment.RadiusB = kRadius;
                segments.push_back(segment);
            }
        }
    }

    const f32 pitch = kExtent / static_cast<f32>(kN - 1);
    const f64 analytic = static_cast<f64>(kN) * (2.0 * kRadius / pitch);

    ReferenceSettings settings;
    settings.Rays = 4096u;
    settings.FootprintRadius = pitch * 4.0f;
    const f64 measured = ReferenceCoatOpticalDepth(segments, glm::vec3(-0.12f, 0.0431f, 0.0013f),
                                                   glm::vec3(1.0f, 0.0f, 0.0f), settings);

    EXPECT_NEAR(measured, analytic, analytic * 0.12) << "measured " << measured << " analytic " << analytic;
}

TEST(GroomCoatShadowReference, TheFootprintMustSpanSeveralSpacingsOrItReportsAConfidentZero)
{
    // The failure this pins is SILENT: a footprint narrower than the coat's
    // pitch fits between the strands, every ray in it misses the same way, and
    // the reference reports a coat that casts no shadow at all. Raising the ray
    // count does not help.
    constexpr i32 kN = 40;
    constexpr f32 kExtent = 0.2f;
    constexpr f32 kRadius = 0.0006f;

    std::vector<CoatSegment> segments;
    for (i32 i = 0; i < kN; ++i)
    {
        for (i32 k = 0; k < kN; ++k)
        {
            const f32 x = kExtent * (static_cast<f32>(i) / static_cast<f32>(kN - 1) - 0.5f);
            const f32 z = kExtent * (static_cast<f32>(k) / static_cast<f32>(kN - 1) - 0.5f);
            CoatSegment segment;
            segment.A = glm::vec3(x, 0.0f, z);
            segment.B = glm::vec3(x, 0.1f, z);
            segment.RadiusA = kRadius;
            segment.RadiusB = kRadius;
            segments.push_back(segment);
        }
    }

    const f32 pitch = kExtent / static_cast<f32>(kN - 1);
    const glm::vec3 origin(-0.12f, 0.0431f, 0.0013f);
    const glm::vec3 direction(1.0f, 0.0f, 0.0f);

    ReferenceSettings tooSmall;
    tooSmall.Rays = 4096u; // deliberately generous: rays are not the problem
    tooSmall.FootprintRadius = pitch * 0.1f;
    EXPECT_DOUBLE_EQ(ReferenceCoatOpticalDepth(segments, origin, direction, tooSmall), 0.0);

    ReferenceSettings wideEnough = tooSmall;
    wideEnough.FootprintRadius = pitch * 4.0f;
    EXPECT_GT(ReferenceCoatOpticalDepth(segments, origin, direction, wideEnough), 5.0);
}

TEST(GroomCoatShadowReference, ARayAlongTheFibresCrossesFarLessThanOneAcrossThem)
{
    // The ground truth must itself be anisotropic, or the anisotropic
    // candidate is being judged by an isotropic judge and its whole advantage
    // is unmeasurable.
    std::vector<CoatSegment> segments;
    for (i32 i = 0; i < 40; ++i)
    {
        for (i32 k = 0; k < 40; ++k)
        {
            const f32 x = 0.2f * (static_cast<f32>(i) / 39.0f - 0.5f);
            const f32 z = 0.2f * (static_cast<f32>(k) / 39.0f - 0.5f);
            CoatSegment segment;
            segment.A = glm::vec3(x, 0.0f, z);
            segment.B = glm::vec3(x, 0.1f, z);
            segment.RadiusA = 0.0006f;
            segment.RadiusB = 0.0006f;
            segments.push_back(segment);
        }
    }

    ReferenceSettings settings;
    settings.Rays = 1024u;
    settings.FootprintRadius = 0.02f;

    const f64 across = ReferenceCoatOpticalDepth(segments, glm::vec3(-0.12f, 0.05f, 0.0013f),
                                                 glm::vec3(1.0f, 0.0f, 0.0f), settings);
    const f64 along = ReferenceCoatOpticalDepth(segments, glm::vec3(0.0013f, -0.02f, 0.0011f),
                                                glm::vec3(0.0f, 1.0f, 0.0f), settings);

    EXPECT_GT(across, 5.0);
    EXPECT_LT(along, across * 0.1) << "across " << across << " along " << along;
}

TEST(GroomCoatShadowReference, TheGridChangesTheCostAndNotTheAnswer)
{
    // The grid may only ADD candidates, never remove a real one, so the two
    // must agree exactly — not within a tolerance. A tolerance here would hide
    // precisely the missed-cell bug the grid can have.
    const auto pelt = Tests::GroomStrandFixture::MakePelt(1200u, 6u);
    ASSERT_TRUE(pelt.Groom) << pelt.FailureReason;

    CoatSampleSettings sampleSettings;
    sampleSettings.WidthScale = kSceneWidthScale;
    std::vector<CoatSegment> segments;
    ASSERT_GT(BuildCoatSegments(*pelt.Groom, glm::mat4(1.0f), sampleSettings, segments), 0u);

    CoatSegmentGrid grid;
    ASSERT_TRUE(BuildCoatSegmentGrid(segments, 32u, grid));

    std::vector<CoatProbe> probes;
    ASSERT_GT(BuildCoatProbes(segments, kSideLight, 40u, 1248u, probes), 0u);

    ReferenceSettings settings;
    settings.Rays = 32u;
    settings.FootprintRadius = 0.02f;

    u32 nonZero = 0;
    for (const CoatProbe& probe : probes)
    {
        const f64 brute = ReferenceCoatOpticalDepth(segments, probe.Position, probe.Direction, settings);
        const f64 gridded = ReferenceCoatOpticalDepthGrid(segments, grid, probe.Position, probe.Direction, settings);
        EXPECT_DOUBLE_EQ(brute, gridded);
        if (brute > 0.0)
        {
            ++nonZero;
        }
    }
    // Or the equality above is forty comparisons of zero against zero.
    EXPECT_GT(nonZero, probes.size() / 2u);
}

// ── The build ────────────────────────────────────────────────────────────────

TEST(GroomCoatShadowVolume, BinningConservesTheCoatsFibreAreaExactly)
{
    // A build that loses fibre area loses shadow, silently. Nearest-voxel
    // deposit is chosen over a trilinear splat precisely so this is an EXACT
    // identity rather than an approximate one.
    const auto pelt = Tests::GroomStrandFixture::MakePelt(2000u, 6u);
    ASSERT_TRUE(pelt.Groom) << pelt.FailureReason;

    CoatSampleSettings sampleSettings;
    sampleSettings.WidthScale = kSceneWidthScale;
    std::vector<CoatSegment> segments;
    ASSERT_GT(BuildCoatSegments(*pelt.Groom, glm::mat4(1.0f), sampleSettings, segments), 0u);

    f64 expected = 0.0;
    for (const CoatSegment& segment : segments)
    {
        // The deposit walks each segment in sub-steps sampled at their
        // midpoints, so the mass it lays down is length times the MEAN
        // diameter, which for a linear taper is the diameter at the midpoint.
        expected += static_cast<f64>(glm::length(segment.B - segment.A)) *
                    static_cast<f64>(segment.RadiusA + segment.RadiusB);
    }

    DensityVolumeSettings settings;
    settings.Resolution = 48u;
    DensityVolume volume;
    DensityVolumeBuildStats stats;
    ASSERT_TRUE(BuildDensityVolume(segments, settings, volume, &stats));

    EXPECT_NEAR(stats.TotalArealMass, expected, expected * 1.0e-4)
        << "binned " << stats.TotalArealMass << " expected " << expected;
    EXPECT_EQ(stats.SegmentsBinned, segments.size());
    EXPECT_GT(stats.OccupiedVoxels, 0u);
    EXPECT_LT(stats.OccupiedVoxels, stats.TotalVoxels) << "a coat that fills its whole bounding box is not a coat";
}

TEST(GroomCoatShadowVolume, AnEmptyOrDegenerateInputIsRefusedRatherThanBuilt)
{
    DensityVolume volume;
    DeepOpacityMap map;
    CoatSegmentGrid grid;

    EXPECT_FALSE(BuildDensityVolume({}, DensityVolumeSettings{}, volume, nullptr));
    EXPECT_FALSE(volume.IsValid());
    EXPECT_FALSE(BuildDeepOpacityMap({}, kSideLight, DeepOpacityMapSettings{}, map, nullptr));
    EXPECT_FALSE(map.IsValid());
    EXPECT_FALSE(BuildCoatSegmentGrid({}, 32u, grid));
    EXPECT_FALSE(grid.IsValid());

    // A zero light direction has no map to build; a silent identity basis would
    // produce a plausible-looking map of the wrong thing.
    const std::vector<CoatSegment> one{ CoatSegment{ glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 0.01f, 0.01f } };
    EXPECT_FALSE(BuildDeepOpacityMap(one, glm::vec3(0.0f), DeepOpacityMapSettings{}, map, nullptr));
}

TEST(GroomCoatShadowVolume, ANonFiniteCurveIsDroppedRatherThanPoisoningTheDensity)
{
    // A MIXED set: one good segment and two corrupt ones. This is the case
    // that matters, and an earlier version of this test did not have it -- it
    // used a lone good segment, so it could not see that CoatSegmentBounds
    // took the min/max over the NaN, returned false, and failed the WHOLE
    // build. One bad curve cost the entire coat its shadow, which is the
    // opposite of the drop-the-curve promise BuildCoatSegments makes.
    std::vector<CoatSegment> segments;
    CoatSegment good;
    good.A = glm::vec3(0.0f, 0.0f, 0.0f);
    good.B = glm::vec3(0.0f, 0.1f, 0.0f);
    good.RadiusA = 0.001f;
    good.RadiusB = 0.001f;
    segments.push_back(good);

    CoatSegment corrupt = good;
    corrupt.B = glm::vec3(std::numeric_limits<f32>::quiet_NaN(), 0.1f, 0.0f);
    segments.push_back(corrupt);

    CoatSegment corruptRadius = good;
    corruptRadius.RadiusB = std::numeric_limits<f32>::infinity();
    segments.push_back(corruptRadius);

    DensityVolumeSettings settings;
    settings.Resolution = 8u;
    DensityVolume volume;
    DensityVolumeBuildStats stats;
    ASSERT_TRUE(BuildDensityVolume(segments, settings, volume, &stats))
        << "one corrupt segment failed the whole build instead of being dropped";
    EXPECT_EQ(stats.SegmentsBinned, 1u) << "the good segment was not binned";
    EXPECT_EQ(stats.SegmentsRejected, 2u) << "the corrupt segments were not counted as rejected";
    EXPECT_GT(stats.OccupiedVoxels, 0u) << "the good segment deposited nothing";
    EXPECT_TRUE(std::isfinite(stats.TotalArealMass));
    for (const f32 density : volume.Density)
    {
        EXPECT_TRUE(std::isfinite(density));
    }
    for (const glm::vec3& direction : volume.Direction)
    {
        EXPECT_TRUE(std::isfinite(direction.x) && std::isfinite(direction.y) && std::isfinite(direction.z));
    }
    // And the BOX is finite -- a NaN corner makes every UVW mapping a NaN,
    // which the shader's range test cannot catch because every comparison
    // against a NaN is false.
    EXPECT_TRUE(std::isfinite(volume.BoundsMin.x) && std::isfinite(volume.BoundsMax.x));

    // A non-finite optical depth must read as FULLY LIT, not fully shadowed:
    // the failure mode of a missing occlusion term has to be a bright coat,
    // which is visibly "this did not run", rather than a black one, which is
    // indistinguishable from a correct silhouette.
    EXPECT_FLOAT_EQ(CoatTransmittance(std::numeric_limits<f64>::quiet_NaN(), 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(CoatTransmittance(1.0, std::numeric_limits<f32>::quiet_NaN()), 1.0f);
    EXPECT_FLOAT_EQ(CoatTransmittance(0.0, 1.0f), 1.0f);

    // An INFINITE optical depth also reads fully lit, and that is deliberate
    // rather than a missed `exp(-inf) == 0`. The march cannot produce one from
    // a finite density over a finite span, so an infinity means the volume
    // itself is corrupt — and a corrupt occlusion term has to fail bright,
    // where it is visible as "this did not run", not black, where it is
    // indistinguishable from a correct silhouette.
    EXPECT_FLOAT_EQ(CoatTransmittance(std::numeric_limits<f64>::infinity(), 1.0f), 1.0f);

    // A merely LARGE depth is a real measurement and does reach zero, so the
    // rule above costs nothing where it matters.
    //
    // HOW FAST it reaches zero changed with #1360 and the new number is the
    // point, not an adjusted tolerance. The shipped form is
    // exp(-tau * (1 - exp(-kappa))), so at kappa = 1 each expected crossing
    // costs 1 - 1/e = 0.632 of an e-fold rather than a whole one, and the floor
    // as kappa grows is exp(-tau): a coat of perfectly opaque fibres still
    // passes light wherever the footprint crossed nothing, which for a Poisson
    // crossing count is exactly P(N = 0). Both are asserted, because the floor
    // is the half that would silently stop being true if the generating
    // function were replaced by something that merely looks like it.
    EXPECT_LT(CoatTransmittance(90.0, 1.0f), 1.0e-24f);
    EXPECT_GT(CoatTransmittance(90.0, 1.0f), 0.0f);
    EXPECT_NEAR(CoatTransmittance(8.0, 16.0f), std::exp(-8.0f), 1.0e-6f);
}

// ── The comparison the decision rests on ─────────────────────────────────────

TEST(GroomCoatShadowComparison, TheComparisonRunsInADiscriminatingRangeRatherThanAtBlack)
{
    // THE GUARD ON EVERY OTHER CASE IN THIS FILE. At a high enough density
    // every candidate agrees at T = 0, and a comparison taken there measures
    // nothing while looking like it measured everything — the same shape as
    // #1246's alpha-to-coverage case failing with two identical numbers.
    for (const glm::vec3& light : { kSideLight, kObliqueLight })
    {
        const auto pelt = Tests::GroomStrandFixture::MakePelt(20000u, 8u);
        ASSERT_TRUE(pelt.Groom) << pelt.FailureReason;
        const Case testCase = MakeCase(pelt.Groom, light);
        ASSERT_FALSE(testCase.Truth.empty());

        const f64 mean = MeanOf(testCase.Truth);
        const f32 lowest = *std::min_element(testCase.Truth.begin(), testCase.Truth.end());
        const f32 highest = *std::max_element(testCase.Truth.begin(), testCase.Truth.end());

        EXPECT_GT(mean, 0.02) << "the coat is saturated at black; nothing below can discriminate";
        EXPECT_LT(mean, 0.60) << "the coat is barely shadowed; nothing below is being tested";
        EXPECT_LT(lowest, 0.05) << "no probe is deep in the coat";
        EXPECT_GT(highest, 0.80) << "no probe is at the coat's surface";
    }
}

TEST(GroomCoatShadowComparison, EverySelectedCandidateBeatsDoingNothingByAnOrderOfMagnitude)
{
    const auto pelt = Tests::GroomStrandFixture::MakePelt(20000u, 8u);
    ASSERT_TRUE(pelt.Groom) << pelt.FailureReason;
    const Case testCase = MakeCase(pelt.Groom, kSideLight);

    const DensityVolume volume = BuildVolumeAt(testCase, 64u);
    ASSERT_TRUE(volume.IsValid());

    const CoatShadowError none =
        CompareCoatShadow(std::vector<f32>(testCase.Probes.size(), 1.0f), testCase.Truth, 0.0);
    const CoatShadowError anisotropic = ErrorOfVolume(testCase, volume, true, 3.0f);

    EXPECT_GT(none.MeanAbs, 0.70) << "the control should be badly wrong; that is the point of the feature";
    EXPECT_LT(anisotropic.MeanAbs, none.MeanAbs * 0.1);
}

TEST(GroomCoatShadowComparison, TheAnisotropicVolumeBeatsTheIsotropicOneOnEveryCoatAndLight)
{
    // The direction channel is the anisotropic mode's entire extra cost — five
    // times the memory — so it has to earn it everywhere, not on average.
    const auto pelt = Tests::GroomStrandFixture::MakePelt(20000u, 8u);
    const auto scalp = Tests::GroomStrandFixture::MakeScalp(20000u, 12u);
    ASSERT_TRUE(pelt.Groom) << pelt.FailureReason;
    ASSERT_TRUE(scalp.Groom) << scalp.FailureReason;

    for (const Ref<GroomAsset>& groom : { pelt.Groom, scalp.Groom })
    {
        for (const glm::vec3& light : { kSideLight, kObliqueLight })
        {
            const Case testCase = MakeCase(groom, light);
            const DensityVolume volume = BuildVolumeAt(testCase, 64u);
            ASSERT_TRUE(volume.IsValid());

            const CoatShadowError isotropic = ErrorOfVolume(testCase, volume, false, 3.0f);
            const CoatShadowError anisotropic = ErrorOfVolume(testCase, volume, true, 3.0f);

            EXPECT_LT(anisotropic.MeanAbs, isotropic.MeanAbs)
                << "iso " << isotropic.MeanAbs << " aniso " << anisotropic.MeanAbs;
        }
    }
}

TEST(GroomCoatShadowComparison, TheAnisotropicVolumeBeatsTheBestDeepOpacityMap)
{
    // THE SELECTION ITSELF. The deep map is compared at 128^2 x 8, which is
    // the resolution its OWN sweep found best — comparing it at a resolution
    // past its optimum is how a bake-off flatters the answer it started with,
    // and the first run of this comparison did exactly that at 512^2.
    const auto pelt = Tests::GroomStrandFixture::MakePelt(20000u, 8u);
    ASSERT_TRUE(pelt.Groom) << pelt.FailureReason;

    for (const glm::vec3& light : { kSideLight, kObliqueLight })
    {
        const Case testCase = MakeCase(pelt.Groom, light);
        const DensityVolume volume = BuildVolumeAt(testCase, 64u);
        const DeepOpacityMap map = BuildMapAt(testCase, light, 128u, 8u);
        ASSERT_TRUE(volume.IsValid());
        ASSERT_TRUE(map.IsValid());

        const CoatShadowError anisotropic = ErrorOfVolume(testCase, volume, true, 3.0f);
        const CoatShadowError deep = ErrorOfDeepMap(testCase, map);

        EXPECT_LT(anisotropic.MeanAbs, deep.MeanAbs)
            << "aniso " << anisotropic.MeanAbs << " deep " << deep.MeanAbs;
        // And the deep map is a real competitor rather than a straw man: if it
        // ever scores worse than this, it has been mis-built rather than
        // out-performed, and the selection stops being evidence.
        EXPECT_LT(deep.MeanAbs, 0.10) << "the rejected candidate is performing too badly to be a fair comparison";
        // Its cost advantage is the reason it was worth measuring at all.
        EXPECT_LT(map.GpuBytes(), volume.GpuBytes());
    }
}

// ── The two findings ─────────────────────────────────────────────────────────

TEST(GroomCoatShadowComparison, FinerIsWorseForTheDensityVolumePastItsOptimum)
{
    // Counter-intuitive and measured: below about two strand spacings a voxel
    // holds one strand or none, so the "average" it stores is a sample and the
    // volume aliases against the coat. Spending more memory past the optimum
    // does not waste it — it makes the shadow worse.
    const auto pelt = Tests::GroomStrandFixture::MakePelt(20000u, 8u);
    ASSERT_TRUE(pelt.Groom) << pelt.FailureReason;
    const Case testCase = MakeCase(pelt.Groom, kSideLight);

    const DensityVolume coarse = BuildVolumeAt(testCase, 64u);
    const DensityVolume fine = BuildVolumeAt(testCase, 128u);
    ASSERT_TRUE(coarse.IsValid());
    ASSERT_TRUE(fine.IsValid());

    const CoatShadowError coarseError = ErrorOfVolume(testCase, coarse, true, 3.0f);
    const CoatShadowError fineError = ErrorOfVolume(testCase, fine, true, 3.0f);

    EXPECT_LT(coarseError.MeanAbs, fineError.MeanAbs)
        << "64^3 " << coarseError.MeanAbs << " vs 128^3 " << fineError.MeanAbs;
    EXPECT_GT(fine.GpuBytes(), coarse.GpuBytes() * 4ull) << "and it cost eight times the memory to be worse";

    // The optimum tracks the coat's own spacing rather than being a magic
    // number: a 64^3 voxel here is about two mean spacings.
    const glm::vec3 voxel = coarse.VoxelSize();
    const f32 voxelLength = std::min({ voxel.x, voxel.y, voxel.z });
    EXPECT_GT(voxelLength, testCase.MeanSpacing) << "the selected voxel is finer than the coat it averages";
}

TEST(GroomCoatShadowComparison, FinerIsWorseForTheDeepOpacityMapTooAndByMore)
{
    const auto pelt = Tests::GroomStrandFixture::MakePelt(20000u, 8u);
    ASSERT_TRUE(pelt.Groom) << pelt.FailureReason;
    const Case testCase = MakeCase(pelt.Groom, kSideLight);

    const DeepOpacityMap coarse = BuildMapAt(testCase, kSideLight, 128u, 8u);
    const DeepOpacityMap fine = BuildMapAt(testCase, kSideLight, 512u, 8u);
    ASSERT_TRUE(coarse.IsValid());
    ASSERT_TRUE(fine.IsValid());

    const CoatShadowError coarseError = ErrorOfDeepMap(testCase, coarse);
    const CoatShadowError fineError = ErrorOfDeepMap(testCase, fine);

    // Measured at roughly three times worse, which is why the first run of the
    // comparison scored the deep map a third as good as it actually is.
    EXPECT_LT(coarseError.MeanAbs * 1.5, fineError.MeanAbs)
        << "128^2 " << coarseError.MeanAbs << " vs 512^2 " << fineError.MeanAbs;
}

TEST(GroomCoatShadowComparison, ACoarserMarchIsNotWorseAndCostsAThirdOfTheTaps)
{
    // The volume's error is dominated by a systematic bias, not by quadrature,
    // so refining the step buys nothing. Asserted on BOTH coats, because a
    // step that happens to suit one is a coincidence.
    const auto pelt = Tests::GroomStrandFixture::MakePelt(20000u, 8u);
    const auto scalp = Tests::GroomStrandFixture::MakeScalp(20000u, 12u);
    ASSERT_TRUE(pelt.Groom) << pelt.FailureReason;
    ASSERT_TRUE(scalp.Groom) << scalp.FailureReason;

    for (const Ref<GroomAsset>& groom : { pelt.Groom, scalp.Groom })
    {
        const Case testCase = MakeCase(groom, kObliqueLight);
        const DensityVolume volume = BuildVolumeAt(testCase, 64u);
        ASSERT_TRUE(volume.IsValid());

        const CoatShadowError fine = ErrorOfVolume(testCase, volume, true, 1.0f);
        const CoatShadowError selected = ErrorOfVolume(testCase, volume, true, 3.0f);

        EXPECT_LE(selected.MeanAbs, fine.MeanAbs * 1.05)
            << "step 3 " << selected.MeanAbs << " vs step 1 " << fine.MeanAbs;
        EXPECT_LT(selected.MeanSamples, fine.MeanSamples * 0.45)
            << "step 3 " << selected.MeanSamples << " taps vs step 1 " << fine.MeanSamples;
    }
}

TEST(GroomCoatShadowComparison, AMarchFarCoarserThanTheSelectedStepDoesDegrade)
{
    // Pairs with the case above: without it, "coarser is not worse" reads as
    // an invitation to march once, and the measured cliff past four voxels
    // would be undocumented.
    const auto pelt = Tests::GroomStrandFixture::MakePelt(20000u, 8u);
    ASSERT_TRUE(pelt.Groom) << pelt.FailureReason;
    const Case testCase = MakeCase(pelt.Groom, kSideLight);
    const DensityVolume volume = BuildVolumeAt(testCase, 64u);
    ASSERT_TRUE(volume.IsValid());

    const CoatShadowError selected = ErrorOfVolume(testCase, volume, true, 3.0f);
    const CoatShadowError tooCoarse = ErrorOfVolume(testCase, volume, true, 8.0f);

    EXPECT_GT(tooCoarse.MeanAbs, selected.MeanAbs * 2.0)
        << "step 3 " << selected.MeanAbs << " vs step 8 " << tooCoarse.MeanAbs;
}

TEST(GroomCoatShadowComparison, TheVolumeUnderShadowsRatherThanOverShadowing)
{
    // The sign of the bias is a design fact, not a detail: a positive bias
    // would mean the coat reads BRIGHTER than the truth, which is the "no
    // depth" failure the whole feature exists to remove. Under-shadowing is
    // the safe direction and is what the march-step choice trades against.
    const auto pelt = Tests::GroomStrandFixture::MakePelt(20000u, 8u);
    ASSERT_TRUE(pelt.Groom) << pelt.FailureReason;
    const Case testCase = MakeCase(pelt.Groom, kSideLight);
    const DensityVolume volume = BuildVolumeAt(testCase, 64u);
    ASSERT_TRUE(volume.IsValid());

    const CoatShadowError selected = ErrorOfVolume(testCase, volume, true, 3.0f);
    EXPECT_LT(selected.Bias, 0.02) << "bias " << selected.Bias;
    EXPECT_GT(selected.Bias, -0.10) << "bias " << selected.Bias;
}

// ── Shadow LOD ───────────────────────────────────────────────────────────────

TEST(GroomCoatShadowLod, TheResolutionRequestIsMonotoneInApparentSize)
{
    // A coat that gets smaller must never ask for MORE resolution. That
    // monotonicity is what stops a LOD oscillation before hysteresis is even
    // involved.
    const CoatLodPolicy policy;
    u32 previous = 0;
    for (i32 i = 0; i <= 40; ++i)
    {
        const f32 pixelSize = 1024.0f * std::pow(0.85f, static_cast<f32>(i));
        const u32 step = SelectCoatLodStep(policy, pixelSize);
        EXPECT_GE(step, previous) << "pixelSize " << pixelSize;
        EXPECT_LE(step, policy.MaxLodSteps);
        previous = step;
    }
}

TEST(GroomCoatShadowLod, TheResolutionNeverFallsBelowItsOwnFloor)
{
    CoatLodPolicy policy;
    policy.BaseResolution = 64u;
    policy.MaxLodSteps = 8u;
    policy.MinResolution = 8u;
    for (u32 step = 0; step <= 12u; ++step)
    {
        EXPECT_GE(CoatLodResolution(policy, step), policy.MinResolution) << "step " << step;
    }
    EXPECT_EQ(CoatLodResolution(policy, 0u), 64u);
    EXPECT_EQ(CoatLodResolution(policy, 1u), 32u);
    EXPECT_EQ(CoatLodResolution(policy, 2u), 16u);
}

TEST(GroomCoatShadowLod, ADegenerateApparentSizeAsksForTheCoarsestLodRatherThanTheFinest)
{
    // A zero or non-finite pixel size means "this coat is not on screen in any
    // measurable way". Answering LOD 0 there would make an off-screen coat the
    // most expensive thing in the frame.
    const CoatLodPolicy policy;
    EXPECT_EQ(SelectCoatLodStep(policy, 0.0f), policy.MaxLodSteps);
    EXPECT_EQ(SelectCoatLodStep(policy, -1.0f), policy.MaxLodSteps);
    EXPECT_EQ(SelectCoatLodStep(policy, std::numeric_limits<f32>::quiet_NaN()), policy.MaxLodSteps);
}

TEST(GroomCoatShadowLod, HysteresisRefinesImmediatelyAndCoarsensOnlyOnceStable)
{
    // A coat straddling a LOD boundary must not rebuild its representation
    // every frame — that is the "uncontrolled flicker" criterion's failure
    // mode showing up as a rebuild counter before it shows up as a picture.
    EXPECT_EQ(ApplyCoatLodHysteresis(2u, 1u, 0u, 3u), 1u) << "refining is immediate";
    EXPECT_EQ(ApplyCoatLodHysteresis(1u, 2u, 0u, 3u), 1u) << "coarsening waits";
    EXPECT_EQ(ApplyCoatLodHysteresis(1u, 2u, 2u, 3u), 1u);
    EXPECT_EQ(ApplyCoatLodHysteresis(1u, 2u, 3u, 3u), 2u) << "and is taken once stable";
    EXPECT_EQ(ApplyCoatLodHysteresis(2u, 2u, 0u, 3u), 2u) << "no change is no change";
}

TEST(GroomCoatShadowLod, AnOscillatingRequestCannotRebuildEveryFrame)
{
    // The actual scenario: a coat sitting exactly on a boundary, alternating
    // between two requests. Without hysteresis this is one rebuild per frame
    // forever.
    u32 current = 1u;
    u32 framesStable = 0;
    u32 changes = 0;
    u32 lastRequest = current;
    for (u32 frame = 0; frame < 200u; ++frame)
    {
        const u32 requested = (frame % 2u) == 0u ? 1u : 2u;
        framesStable = requested == lastRequest ? framesStable + 1u : 0u;
        lastRequest = requested;

        const u32 next = ApplyCoatLodHysteresis(current, requested, framesStable, 3u);
        if (next != current)
        {
            ++changes;
        }
        current = next;
    }
    // The coarser request never holds for three frames, so it is never taken;
    // the finer one is taken once, immediately, and then it is already current.
    EXPECT_LE(changes, 1u) << "the representation rebuilt " << changes << " times on an oscillating request";
}
