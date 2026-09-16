// =============================================================================
// FoliageHabitatClumpingPropertyTests.cpp
//
// Pins the species habitat rules, the clump field, the decorrelated variation
// hash and the ground-contact fix of issue #1254.
//
// All of it runs on FoliagePlacement, the pure-CPU seam split out in #1230 —
// the REAL generator, no GL context, no stand-in. That matters here more than
// usual, because every failure mode this file guards is SILENT in a screenshot:
//
//   * a distribution that is subtly structured still looks like grass;
//   * a habitat band that is off by a feather still looks like a meadow;
//   * a plant whose downhill side floats 3 cm off a slope is invisible until
//     someone stands next to it;
//   * and a new field that quietly MOVES every plant retires every canonical
//     instance id in the layer (#1261) without changing the picture at all.
//
// What each test pins, against the issue's four acceptance criteria:
//
//   AC1 species rules     -> MoistureProxyIsWetLowAndFlatDryHighAndSteep
//                            AltitudeBandGatesPlacementAndFeatherIsGradual
//                            MoistureBandSeparatesTwoSpeciesWithASharedFringe
//                            ExclusionWeightSuppressesWherePainted
//   AC1 clustered varia.  -> ClumpingProducesPatchesNotUniformThinning
//                            ClumpFieldIsAPureDeterministicFunction
//                            SharedClumpGroupPutsTwoSpeciesInTheSamePatches
//                            ClumpScaleInfluenceGrowsBiggerPlantsInPatchCores
//   AC2 no repetition     -> LegacyJitterSitsOnThirtyTwoDiagonals
//                            DecorrelatedVariationRemovesTheJitterLattice
//   AC2 no floating roots -> SlopeSinkLandsTheDownhillEdgeOnTheGround
//                            FlatGroundIsUnaffectedBySlopeSink
//   AC3 determinism       -> RegenerationIsBitIdentical
//   backward compatibility-> ADefaultLayerPlacesExactlyWhereItAlwaysDid
//                            HabitatRulesOffIsExactlyOne
//
// OLO_TEST_LAYER: L1
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Terrain/Foliage/FoliageInstanceRegistry.h"
#include "OloEngine/Terrain/Foliage/FoliagePlacement.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <glm/gtc/constants.hpp>
#include <limits>
#include <set>
#include <unordered_map>
#include <vector>

using namespace OloEngine;
namespace FP = OloEngine::FoliagePlacement;

namespace
{
    constexpr u32 kResolution = 129;
    constexpr f32 kWorldSize = 128.0f;
    constexpr f32 kHeightScale = 40.0f;

    /// A perfectly flat field at `height` (normalized [0, 1]). Every normal is
    /// straight up, so the default 0-45 degree slope gate accepts every cell.
    [[nodiscard]] std::vector<f32> FlatField(f32 height)
    {
        return std::vector<f32>(static_cast<sizet>(kResolution) * kResolution, height);
    }

    /// A field that ramps linearly with X from 0 to `top` (normalized). Gentle
    /// enough that the default slope gate still accepts everywhere, so a test
    /// that bands on ALTITUDE is measuring the altitude band and not the slope.
    [[nodiscard]] std::vector<f32> RampField(f32 top)
    {
        std::vector<f32> heights(static_cast<sizet>(kResolution) * kResolution, 0.0f);
        for (u32 z = 0; z < kResolution; ++z)
        {
            for (u32 x = 0; x < kResolution; ++x)
            {
                const f32 t = static_cast<f32>(x) / static_cast<f32>(kResolution - 1);
                heights[static_cast<sizet>(z) * kResolution + x] = t * top;
            }
        }
        return heights;
    }

    /// A plane tilted about Z by a known angle, expressed as a normalized
    /// height field. Returns the field; `outTanSlope` receives the exact
    /// world-space slope tangent the generator will see.
    [[nodiscard]] std::vector<f32> TiltedField(f32 riseOverRunWorld, f32& outTanSlope)
    {
        outTanSlope = riseOverRunWorld;
        std::vector<f32> heights(static_cast<sizet>(kResolution) * kResolution, 0.0f);
        for (u32 z = 0; z < kResolution; ++z)
        {
            for (u32 x = 0; x < kResolution; ++x)
            {
                // World x at this texel, times the slope, expressed back in
                // normalized height units.
                const f32 worldX = static_cast<f32>(x) / static_cast<f32>(kResolution - 1) * kWorldSize;
                // Centred so the field stays inside [0, 1] for a modest slope.
                const f32 worldY = 0.5f * kHeightScale + (worldX - 0.5f * kWorldSize) * riseOverRunWorld;
                heights[static_cast<sizet>(z) * kResolution + x] = worldY / kHeightScale;
            }
        }
        return heights;
    }

    [[nodiscard]] std::vector<FP::Placement> Generate(const FoliageLayer& layer, u32 layerIndex,
                                                      const std::vector<f32>& heights)
    {
        std::vector<FP::Placement> out;
        FP::GenerateLayer(layer, layerIndex, heights, kResolution, nullptr, kWorldSize, kWorldSize,
                          kHeightScale, out);
        return out;
    }

    /// Index of dispersion (variance / mean) of per-tile counts. A Poisson
    /// (uniform random) scatter sits at ~1; a grid-jittered one below 1; a
    /// clumped one well above.
    [[nodiscard]] f64 TileDispersion(const std::vector<FP::Placement>& placements, f32 tileSize)
    {
        const auto tiles = static_cast<u32>(std::ceil(kWorldSize / tileSize));
        std::vector<u32> counts(static_cast<sizet>(tiles) * tiles, 0u);
        for (const auto& p : placements)
        {
            const auto tx = std::min(static_cast<u32>(p.m_Row.PositionScale.x / tileSize), tiles - 1);
            const auto tz = std::min(static_cast<u32>(p.m_Row.PositionScale.z / tileSize), tiles - 1);
            ++counts[static_cast<sizet>(tz) * tiles + tx];
        }

        f64 mean = 0.0;
        for (const u32 c : counts)
            mean += c;
        mean /= static_cast<f64>(counts.size());
        if (mean <= 0.0)
            return 0.0;

        f64 variance = 0.0;
        for (const u32 c : counts)
            variance += (static_cast<f64>(c) - mean) * (static_cast<f64>(c) - mean);
        variance /= static_cast<f64>(counts.size());
        return variance / mean;
    }

    [[nodiscard]] std::vector<u32> TileCounts(const std::vector<FP::Placement>& placements, u32 tiles)
    {
        const f32 tileSize = kWorldSize / static_cast<f32>(tiles);
        std::vector<u32> counts(static_cast<sizet>(tiles) * tiles, 0u);
        for (const auto& p : placements)
        {
            const auto tx = std::min(static_cast<u32>(p.m_Row.PositionScale.x / tileSize), tiles - 1);
            const auto tz = std::min(static_cast<u32>(p.m_Row.PositionScale.z / tileSize), tiles - 1);
            ++counts[static_cast<sizet>(tz) * tiles + tx];
        }
        return counts;
    }

    [[nodiscard]] f64 Correlation(const std::vector<u32>& a, const std::vector<u32>& b)
    {
        const auto n = static_cast<f64>(a.size());
        f64 ma = 0.0;
        f64 mb = 0.0;
        for (sizet i = 0; i < a.size(); ++i)
        {
            ma += a[i];
            mb += b[i];
        }
        ma /= n;
        mb /= n;

        f64 num = 0.0;
        f64 da = 0.0;
        f64 db = 0.0;
        for (sizet i = 0; i < a.size(); ++i)
        {
            const f64 x = static_cast<f64>(a[i]) - ma;
            const f64 y = static_cast<f64>(b[i]) - mb;
            num += x * y;
            da += x * x;
            db += y * y;
        }
        if (da <= 0.0 || db <= 0.0)
            return 0.0;
        return num / std::sqrt(da * db);
    }

    /// How many DISTINCT values the two jitter draws' difference takes over the
    /// grid. This is the measurement the decorrelation switch exists for: a
    /// small number means every plant in the layer sits on one of that many
    /// diagonals inside its cell, which reads at landscape scale as a repeated
    /// distribution.
    ///
    /// Counted by EXACT float equality, deliberately. Bucketing the difference
    /// first looks more robust and is strictly worse: at 1e-4 buckets there are
    /// only 10 000 of them, so 6400 draws collide by the birthday paradox down
    /// to ~4727 no matter how good the hash is — the bucket count becomes the
    /// measurement. These are deterministic pure functions of integers, so two
    /// equal outputs are genuinely the same offset and there is no float noise
    /// to absorb.
    [[nodiscard]] sizet DistinctJitterOffsets(bool decorrelated, u32 cells)
    {
        constexpr u32 kSeed = 31u; // SeedForLayer(0)
        std::set<f32> seen;
        for (u32 iz = 0; iz < cells; ++iz)
        {
            for (u32 ix = 0; ix < cells; ++ix)
            {
                const f32 jx = decorrelated ? FP::HashCellUnit(ix, iz, kSeed)
                                            : FP::HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), kSeed);
                const f32 jz = decorrelated
                                   ? FP::HashCellUnit(ix, iz, kSeed + 7)
                                   : FP::HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), kSeed + 7);
                f32 d = jz - jx;
                if (d < 0.0f)
                    d += 1.0f;
                seen.insert(d);
            }
        }
        return seen.size();
    }
} // namespace

// ── Backward compatibility: nothing about a pre-#1254 layer moves ───────────

TEST(FoliageHabitatClumping, ADefaultLayerPlacesExactlyWhereItAlwaysDid)
{
    // A layer with no habitat rules, no clumping, no ground offset and the
    // legacy hash must reproduce the pre-#1254 generator EXACTLY — same cells,
    // same jittered position, same scale, same Y. Every new field defaults to
    // off precisely so a scene on disk is untouched, and this is the assertion
    // that says so rather than the comment that claims it.
    FoliageLayer layer; // all #1254 fields at their defaults
    layer.Density = 0.25f;

    const auto heights = FlatField(0.4f);
    const auto placements = Generate(layer, 0, heights);
    ASSERT_FALSE(placements.empty());

    const f32 spacing = FP::SpacingForDensity(layer.Density);
    const u32 seed = FP::SeedForLayer(0);

    for (const auto& p : placements)
    {
        const f32 jx = FP::HashPosition(static_cast<f32>(p.m_CellX), static_cast<f32>(p.m_CellZ), seed);
        const f32 jz = FP::HashPosition(static_cast<f32>(p.m_CellX), static_cast<f32>(p.m_CellZ), seed + 7);
        EXPECT_FLOAT_EQ(p.m_Row.PositionScale.x, (static_cast<f32>(p.m_CellX) + jx) * spacing);
        EXPECT_FLOAT_EQ(p.m_Row.PositionScale.z, (static_cast<f32>(p.m_CellZ) + jz) * spacing);

        // Y is the raw sampled height: no offset, no sink.
        EXPECT_FLOAT_EQ(p.m_Row.PositionScale.y, 0.4f * kHeightScale);

        const f32 scaleRand = FP::HashPosition(static_cast<f32>(p.m_CellX), static_cast<f32>(p.m_CellZ), seed + 3);
        EXPECT_FLOAT_EQ(p.m_Row.PositionScale.w, glm::mix(layer.MinScale, layer.MaxScale, scaleRand));
    }

    // And on flat ground with no mask, EVERY in-bounds cell emits: the
    // suitability is exactly 1, so the stochastic comparison is skipped.
    const auto countPerAxis = static_cast<u32>(std::ceil(kWorldSize / spacing));
    u32 inBounds = 0;
    for (u32 iz = 0; iz < countPerAxis; ++iz)
    {
        for (u32 ix = 0; ix < countPerAxis; ++ix)
        {
            const f32 jx = FP::HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), seed);
            const f32 jz = FP::HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), seed + 7);
            if ((static_cast<f32>(ix) + jx) * spacing < kWorldSize &&
                (static_cast<f32>(iz) + jz) * spacing < kWorldSize)
                ++inBounds;
        }
    }
    EXPECT_EQ(placements.size(), inBounds) << "a rule-free layer must not reject a single in-bounds cell";
}

TEST(FoliageHabitatClumping, HabitatRulesOffIsExactlyOne)
{
    // The suitability of a default layer is the multiplicative identity, on
    // flat ground and on any slope inside its band. If this ever returns 0.999
    // the stochastic comparison wakes up and starts deleting plants from every
    // scene on disk.
    const FoliageLayer layer;
    EXPECT_FLOAT_EQ(FP::HabitatSuitability(layer, 0.0f, 0.0f, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(FP::HabitatSuitability(layer, 900.0f, 0.99f, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(FP::HabitatSuitability(layer, 12.0f, 0.3f, 0.75f), 1.0f);

    // Outside the slope band it is still a hard zero, exactly as before.
    EXPECT_FLOAT_EQ(FP::HabitatSuitability(layer, 12.0f, 0.3f, 0.5f), 0.0f);
}

// ── AC1: species rules ──────────────────────────────────────────────────────

TEST(FoliageHabitatClumping, MoistureProxyIsWetLowAndFlatDryHighAndSteep)
{
    // The documented shape of the proxy, so a future edit to the formula has to
    // be deliberate: low + flat is fully wet, high + steep is fully dry, and
    // each axis alone gets you halfway.
    EXPECT_FLOAT_EQ(FP::MoistureAt(0.0f, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(FP::MoistureAt(1.0f, 0.70710678f), 0.0f); // 45 degrees: runoff begins
    EXPECT_NEAR(FP::MoistureAt(1.0f, 1.0f), 0.5f, 1e-5f);     // flat but high
    EXPECT_NEAR(FP::MoistureAt(0.0f, 0.70710678f), 0.5f, 1e-5f); // low but steep

    // Monotone in both inputs — the property an author bands on.
    EXPECT_GT(FP::MoistureAt(0.2f, 1.0f), FP::MoistureAt(0.8f, 1.0f));
    EXPECT_GT(FP::MoistureAt(0.5f, 1.0f), FP::MoistureAt(0.5f, 0.8f));

    // A non-finite input must not produce a non-finite weight; it would make
    // every downstream comparison false and empty the layer silently.
    EXPECT_TRUE(std::isfinite(FP::MoistureAt(std::numeric_limits<f32>::quiet_NaN(), 1.0f)));
}

TEST(FoliageHabitatClumping, AltitudeBandGatesPlacementAndFeatherIsGradual)
{
    const auto heights = RampField(0.8f); // 0 .. 32 world units across X

    FoliageLayer banded;
    banded.Density = 0.25f;
    banded.UseAltitudeBand = true;
    banded.MinAltitude = 8.0f;
    banded.MaxAltitude = 20.0f;

    const auto placements = Generate(banded, 0, heights);
    ASSERT_FALSE(placements.empty()) << "the band accepted nothing at all";

    for (const auto& p : placements)
    {
        EXPECT_GE(p.m_Row.PositionScale.y, 8.0f - 1e-3f) << "a plant below the band's floor";
        EXPECT_LE(p.m_Row.PositionScale.y, 20.0f + 1e-3f) << "a plant above the band's ceiling";
    }

    // A feather cannot widen the band — it only softens its inside — so it
    // places STRICTLY FEWER plants, all still inside the same bounds.
    FoliageLayer feathered = banded;
    feathered.AltitudeFeather = 4.0f;
    const auto softened = Generate(feathered, 0, heights);
    EXPECT_LT(softened.size(), placements.size())
        << "a feathered band must thin its own edges, not widen them";
    for (const auto& p : softened)
    {
        EXPECT_GE(p.m_Row.PositionScale.y, 8.0f - 1e-3f);
        EXPECT_LE(p.m_Row.PositionScale.y, 20.0f + 1e-3f);
    }

    // And the thinning is at the EDGES: the outer quarter of the band loses a
    // larger fraction than the core does.
    const auto fractionIn = [](const std::vector<FP::Placement>& ps, f32 lo, f32 hi)
    {
        sizet n = 0;
        for (const auto& p : ps)
            if (p.m_Row.PositionScale.y >= lo && p.m_Row.PositionScale.y <= hi)
                ++n;
        return n;
    };
    const f64 edgeKept = static_cast<f64>(fractionIn(softened, 8.0f, 11.0f)) /
                         std::max<f64>(1.0, static_cast<f64>(fractionIn(placements, 8.0f, 11.0f)));
    const f64 coreKept = static_cast<f64>(fractionIn(softened, 13.0f, 15.0f)) /
                         std::max<f64>(1.0, static_cast<f64>(fractionIn(placements, 13.0f, 15.0f)));
    EXPECT_LT(edgeKept, coreKept) << "the feather thinned the core as much as the edge, so it is not a feather";
}

TEST(FoliageHabitatClumping, MoistureBandSeparatesTwoSpeciesWithASharedFringe)
{
    // Two species banded on overlapping moisture ranges over the same ramp:
    // the wet one owns the low ground, the dry one the high ground, and there
    // is a band in between where BOTH appear. That fringe is what acceptance
    // criterion 2 means by "transition across habitat boundaries" — without it
    // the two species meet on a line.
    const auto heights = RampField(1.0f);

    FoliageLayer wet;
    wet.Density = 0.25f;
    wet.UseMoisture = true;
    wet.MinMoisture = 0.55f;
    wet.MaxMoisture = 1.0f;
    wet.MoistureFeather = 0.12f;

    FoliageLayer dry = wet;
    dry.MinMoisture = 0.0f;
    dry.MaxMoisture = 0.68f;

    const auto wetPlants = Generate(wet, 0, heights);
    const auto dryPlants = Generate(dry, 1, heights);
    ASSERT_FALSE(wetPlants.empty());
    ASSERT_FALSE(dryPlants.empty());

    // On a ramp rising with X, moisture FALLS with X, so the wet species owns
    // low X and the dry species high X.
    const auto meanX = [](const std::vector<FP::Placement>& ps)
    {
        f64 sum = 0.0;
        for (const auto& p : ps)
            sum += p.m_Row.PositionScale.x;
        return sum / static_cast<f64>(ps.size());
    };
    EXPECT_LT(meanX(wetPlants), meanX(dryPlants)) << "the wet species did not settle on the wetter ground";

    // Both present in the overlap, neither present at the far end of the other's
    // range — a real boundary, not a hard partition and not a total overlap.
    const auto countInStrip = [](const std::vector<FP::Placement>& ps, f32 lo, f32 hi)
    {
        sizet n = 0;
        for (const auto& p : ps)
            if (p.m_Row.PositionScale.x >= lo && p.m_Row.PositionScale.x < hi)
                ++n;
        return n;
    };
    const f32 mid = kWorldSize * 0.5f;
    EXPECT_GT(countInStrip(wetPlants, mid - 12.0f, mid + 12.0f), 0u) << "no wet plants in the fringe";
    EXPECT_GT(countInStrip(dryPlants, mid - 12.0f, mid + 12.0f), 0u) << "no dry plants in the fringe";
    EXPECT_EQ(countInStrip(wetPlants, kWorldSize - 8.0f, kWorldSize), 0u)
        << "the wet species reached the driest ground";
    EXPECT_EQ(countInStrip(dryPlants, 0.0f, 4.0f), 0u) << "the dry species reached the wettest ground";
}

TEST(FoliageHabitatClumping, ExclusionWeightSuppressesWherePainted)
{
    // Unpainted ground is untouched; the threshold is where suppression becomes
    // total; in between it is a ramp, so a painted edge feathers.
    EXPECT_FLOAT_EQ(FP::ExclusionWeight(0.0f, 0.5f), 1.0f);
    EXPECT_FLOAT_EQ(FP::ExclusionWeight(0.5f, 0.5f), 0.0f);
    EXPECT_FLOAT_EQ(FP::ExclusionWeight(1.0f, 0.5f), 0.0f);
    EXPECT_NEAR(FP::ExclusionWeight(0.25f, 0.5f), 0.5f, 1e-6f);

    // A zero threshold cannot suppress anything — and must not divide by zero.
    EXPECT_FLOAT_EQ(FP::ExclusionWeight(1.0f, 0.0f), 1.0f);
    EXPECT_TRUE(std::isfinite(FP::ExclusionWeight(std::numeric_limits<f32>::quiet_NaN(), 0.5f)));
}

// ── AC1: deterministic clustered variation ──────────────────────────────────

TEST(FoliageHabitatClumping, ClumpFieldIsAPureDeterministicFunction)
{
    for (int i = 0; i < 64; ++i)
    {
        const f32 x = static_cast<f32>(i) * 3.7f;
        const f32 z = static_cast<f32>(i) * -1.9f;
        const f32 a = FP::ClumpField(x, z, 12.0f, 1234u);
        const f32 b = FP::ClumpField(x, z, 12.0f, 1234u);
        EXPECT_FLOAT_EQ(a, b);
        EXPECT_GE(a, 0.0f);
        EXPECT_LE(a, 1.0f);
    }

    // A zero or negative patch size is a division by zero in the field, and a
    // NaN there empties the layer silently rather than loudly.
    EXPECT_TRUE(std::isfinite(FP::ClumpField(10.0f, 10.0f, 0.0f, 7u)));
    EXPECT_TRUE(std::isfinite(FP::ClumpField(10.0f, 10.0f, -5.0f, 7u)));
    EXPECT_TRUE(std::isfinite(FP::ClumpField(10.0f, 10.0f, std::numeric_limits<f32>::quiet_NaN(), 7u)));

    // Different seeds are different fields — otherwise every species clumps
    // identically and "clump group" would mean nothing.
    EXPECT_NE(FP::ClumpField(20.0f, 20.0f, 12.0f, 1u), FP::ClumpField(20.0f, 20.0f, 12.0f, 2u));
}

TEST(FoliageHabitatClumping, ClumpingProducesPatchesNotUniformThinning)
{
    const auto heights = FlatField(0.4f);

    FoliageLayer uniform;
    uniform.Density = 1.0f;

    FoliageLayer clumped = uniform;
    clumped.ClumpStrength = 0.9f;
    clumped.ClumpScale = 16.0f;
    clumped.ClumpFalloff = 1.5f;

    const auto uniformPlants = Generate(uniform, 0, heights);
    const auto clumpedPlants = Generate(clumped, 0, heights);
    ASSERT_FALSE(clumpedPlants.empty());

    // Clumping can only multiply suitability DOWN, which is the fact the
    // inspector warns authors about.
    EXPECT_LT(clumpedPlants.size(), uniformPlants.size());

    // The real contract: the survivors are in PATCHES. A grid-jittered scatter
    // is more regular than Poisson (dispersion below 1); a clumped one is far
    // above it. Measured at half the patch size, where patch structure shows.
    const f64 uniformDispersion = TileDispersion(uniformPlants, 8.0f);
    const f64 clumpedDispersion = TileDispersion(clumpedPlants, 8.0f);
    std::printf("[foliage-clump] dispersion uniform %.3f  clumped %.3f\n", uniformDispersion, clumpedDispersion);

    EXPECT_LT(uniformDispersion, 1.0) << "the unclumped control is not a near-uniform scatter, so the "
                                         "comparison below proves nothing. A grid-jittered scatter is "
                                         "SUB-Poisson — more regular than random — which is what makes "
                                         "the super-Poisson bound below meaningful.";
    // The absolute bound is the load-bearing one: a dispersion above 1 is
    // super-Poisson, i.e. genuinely clustered. The relative bound alone would
    // pass trivially, because the control sits near zero.
    EXPECT_GT(clumpedDispersion, 1.5)
        << "clumping thinned the layer without clustering it — it is acting as a density multiplier, "
           "not as a patch field";
    EXPECT_GT(clumpedDispersion, uniformDispersion * 3.0);
}

TEST(FoliageHabitatClumping, SharedClumpGroupPutsTwoSpeciesInTheSamePatches)
{
    const auto heights = FlatField(0.4f);

    FoliageLayer a;
    a.Density = 1.0f;
    a.ClumpStrength = 0.95f;
    a.ClumpScale = 16.0f;
    a.ClumpFalloff = 1.5f;
    a.ClumpGroup = 3; // a shared field

    FoliageLayer b = a; // same group

    FoliageLayer independent = a;
    independent.ClumpGroup = -1; // its own field, derived from the layer seed

    // Layer INDEX differs, so the jitter and threshold streams differ too —
    // the only thing these share is the clump field.
    const auto shareA = TileCounts(Generate(a, 0, heights), 16);
    const auto shareB = TileCounts(Generate(b, 1, heights), 16);
    const auto own = TileCounts(Generate(independent, 1, heights), 16);

    const f64 sharedCorr = Correlation(shareA, shareB);
    const f64 independentCorr = Correlation(shareA, own);
    std::printf("[foliage-clump] tile correlation shared %.3f  independent %.3f\n", sharedCorr, independentCorr);

    EXPECT_GT(sharedCorr, 0.6) << "two species in one clump group did not co-occur — the group seed is not "
                                  "reaching the field";
    EXPECT_LT(independentCorr, sharedCorr * 0.6)
        << "a species with its own clump group tracked the shared one just as closely, so the group is "
           "not selecting anything";
}

TEST(FoliageHabitatClumping, ClumpScaleInfluenceGrowsBiggerPlantsInPatchCores)
{
    const auto heights = FlatField(0.4f);

    FoliageLayer layer;
    layer.Density = 1.0f;
    layer.MinScale = 1.0f;
    layer.MaxScale = 1.0f; // remove the ordinary scale randomisation
    layer.ClumpStrength = 0.6f;
    layer.ClumpScale = 16.0f;
    layer.ClumpScaleInfluence = 1.0f;

    const auto plants = Generate(layer, 0, heights);
    ASSERT_FALSE(plants.empty());

    // With MinScale == MaxScale the ONLY thing that can move a plant's scale is
    // the clump influence, so scale is a direct readout of the field.
    f32 lo = std::numeric_limits<f32>::max();
    f32 hi = std::numeric_limits<f32>::lowest();
    for (const auto& p : plants)
    {
        lo = std::min(lo, p.m_Row.PositionScale.w);
        hi = std::max(hi, p.m_Row.PositionScale.w);
        // mix(1, 0.5 + clump, 1) with clump in [0, 1] -> [0.5, 1.5]
        EXPECT_GE(p.m_Row.PositionScale.w, 0.5f - 1e-4f);
        EXPECT_LE(p.m_Row.PositionScale.w, 1.5f + 1e-4f);
    }
    EXPECT_GT(hi - lo, 0.1f) << "clump scale influence produced no size variation at all";

    // And it is OFF by default: influence 0 leaves the authored scale alone.
    FoliageLayer flat = layer;
    flat.ClumpScaleInfluence = 0.0f;
    for (const auto& p : Generate(flat, 0, heights))
        EXPECT_FLOAT_EQ(p.m_Row.PositionScale.w, 1.0f);
}

// ── AC2: no identical repeated distributions ────────────────────────────────

TEST(FoliageHabitatClumping, LegacyJitterSitsOnThirtyTwoDiagonals)
{
    // The defect DecorrelatedVariation exists to fix, measured rather than
    // asserted from the code. HashPosition XORs the seed in BEFORE its single
    // multiply-and-shift, so two draws whose seeds differ by 7 differ by a
    // near-constant offset: over an 80x80 grid the difference of the two jitter
    // draws takes only ~32 distinct values, which puts every plant in the layer
    // on one of 32 diagonals inside its own cell.
    //
    // This test is here so that a future "harmless" change to HashPosition is
    // recognised as a change to every foliage scene on disk.
    const sizet legacy = DistinctJitterOffsets(/*decorrelated=*/false, 80);
    std::printf("[foliage-hash] legacy distinct jitter offsets over 80x80: %zu\n", legacy);
    EXPECT_EQ(legacy, 32u) << "HashPosition's structure changed — every placement on disk moved with it";
}

TEST(FoliageHabitatClumping, DecorrelatedVariationRemovesTheJitterLattice)
{
    const sizet legacy = DistinctJitterOffsets(/*decorrelated=*/false, 80);
    const sizet mixed = DistinctJitterOffsets(/*decorrelated=*/true, 80);
    std::printf("[foliage-hash] decorrelated distinct jitter offsets over 80x80: %zu\n", mixed);

    EXPECT_GT(mixed, legacy * 50u) << "the avalanche hash did not break the diagonal lattice";
    // 6400 draws from the 2^24 values HashCellUnit can return collide only by
    // the birthday paradox: the expected distinct count is 6398.8, so anything
    // below ~6350 is structure in the hash rather than chance.
    EXPECT_GT(mixed, 6350u) << "6400 cells produced far fewer than 6400 distinct offsets — the "
                               "replacement hash has structure of its own";

    // HashCell must also avalanche across the SEED, which is the specific thing
    // HashPosition does not do: adjacent seeds at the same cell are independent.
    for (u32 cell = 0; cell < 32; ++cell)
    {
        const u32 a = FP::HashCell(cell, cell, 31u);
        const u32 b = FP::HashCell(cell, cell, 32u);
        EXPECT_NE(a, b);
        // At least a quarter of the bits should flip between adjacent seeds.
        EXPECT_GE(std::popcount(a ^ b), 8);
    }
}

// ── AC2: no floating roots ──────────────────────────────────────────────────

TEST(FoliageHabitatClumping, SlopeSinkLandsTheDownhillEdgeOnTheGround)
{
    // The bug, stated as geometry: a plant meets the ground at exactly one
    // point, its origin. On a slope the ground under its downhill edge is
    // lower, so that edge is in the air by (drop across its own half-width).
    // A sink factor of 1 is exactly that drop.
    f32 tanSlope = 0.0f;
    const auto heights = TiltedField(0.35f, tanSlope); // ~19 degrees, inside the default gate

    FoliageLayer floating;
    floating.Density = 0.25f;
    floating.MinScale = 2.0f;
    floating.MaxScale = 2.0f; // half-width 1.0 world unit

    FoliageLayer sunk = floating;
    sunk.SlopeSinkFactor = 1.0f;

    const auto without = Generate(floating, 0, heights);
    const auto with = Generate(sunk, 0, heights);
    ASSERT_EQ(without.size(), with.size()) << "the sink changed WHICH cells emit; it must only move Y";

    // Same plants, same XZ — the sink is a Y-only edit, which is what lets the
    // registry keep every id through it.
    for (sizet i = 0; i < with.size(); ++i)
    {
        EXPECT_FLOAT_EQ(with[i].m_Row.PositionScale.x, without[i].m_Row.PositionScale.x);
        EXPECT_FLOAT_EQ(with[i].m_Row.PositionScale.z, without[i].m_Row.PositionScale.z);
    }

    // The measured drop. upDot for a plane of slope t is 1/sqrt(1+t^2).
    const f32 upDot = 1.0f / std::sqrt(1.0f + tanSlope * tanSlope);
    const f32 expectedSink = tanSlope * 0.5f * 2.0f; // tan * halfWidth, halfWidth = 0.5 * scale

    // The normal is a CENTRAL DIFFERENCE, and SampleHeight clamps its
    // coordinates at the field border — so within one texel of x = 0 or
    // x = worldSize one arm of the difference collapses onto the centre and the
    // measured gradient is half the real one. Those plants have a genuinely
    // different sampled slope, not a tolerance problem, so they are excluded by
    // position rather than absorbed into a wider bound. (kResolution texels
    // across kWorldSize is a hair under one world unit per texel; 2 is a
    // comfortable margin.)
    constexpr f32 kBorderMargin = 2.0f;
    f64 worstError = 0.0;
    sizet compared = 0;
    for (sizet i = 0; i < with.size(); ++i)
    {
        const f32 x = with[i].m_Row.PositionScale.x;
        if (x < kBorderMargin || x > kWorldSize - kBorderMargin)
            continue;
        const f32 drop = without[i].m_Row.PositionScale.y - with[i].m_Row.PositionScale.y;
        worstError = std::max(worstError, std::abs(static_cast<f64>(drop - expectedSink)));
        ++compared;
    }
    std::printf("[foliage-sink] upDot %.4f  expected sink %.4f  worst error %.5f over %zu plants\n",
                static_cast<f64>(upDot), static_cast<f64>(expectedSink), worstError, compared);
    ASSERT_GT(compared, 100u) << "the border exclusion removed almost everything, so the bound below "
                                 "is not measuring the interior";
    EXPECT_LT(worstError, 0.01) << "the sink is not the drop across the plant's own half-width";

    EXPECT_FLOAT_EQ(FP::GroundSinkFor(0.0f, upDot, 2.0f), 0.0f) << "factor 0 must be exactly the off switch";
    EXPECT_LT(FP::GroundSinkFor(1.0f, upDot, 2.0f), 0.0f) << "the sink must be downward";
    EXPECT_TRUE(std::isfinite(FP::GroundSinkFor(1.0f, 0.0f, 2.0f)))
        << "a vertical face must not sink a plant to infinity";
}

TEST(FoliageHabitatClumping, FlatGroundIsUnaffectedBySlopeSink)
{
    const auto heights = FlatField(0.4f);

    FoliageLayer layer;
    layer.Density = 0.25f;
    const auto without = Generate(layer, 0, heights);

    FoliageLayer sunk = layer;
    sunk.SlopeSinkFactor = 1.0f;
    const auto with = Generate(sunk, 0, heights);

    ASSERT_EQ(without.size(), with.size());
    for (sizet i = 0; i < with.size(); ++i)
        EXPECT_FLOAT_EQ(with[i].m_Row.PositionScale.y, without[i].m_Row.PositionScale.y)
            << "flat ground has no drop to compensate, so the sink must be exactly zero there";

    // The constant offset, by contrast, applies everywhere.
    FoliageLayer raised = layer;
    raised.GroundOffset = 0.25f;
    const auto lifted = Generate(raised, 0, heights);
    ASSERT_EQ(lifted.size(), without.size());
    for (sizet i = 0; i < lifted.size(); ++i)
        EXPECT_FLOAT_EQ(lifted[i].m_Row.PositionScale.y, without[i].m_Row.PositionScale.y + 0.25f);
}

// ── AC3: determinism ────────────────────────────────────────────────────────

TEST(FoliageHabitatClumping, RegenerationIsBitIdentical)
{
    // Everything on at once. Determinism is not a nicety here: the registry's
    // placement signature assumes it, so a generator that wobbles by one ulp
    // retires and re-issues ids every regeneration.
    f32 tanSlope = 0.0f;
    const auto heights = TiltedField(0.2f, tanSlope);

    FoliageLayer layer;
    layer.Density = 0.5f;
    layer.SlopeFeather = 8.0f;
    layer.UseAltitudeBand = true;
    layer.MinAltitude = 4.0f;
    layer.MaxAltitude = 32.0f;
    layer.AltitudeFeather = 3.0f;
    layer.UseMoisture = true;
    layer.MinMoisture = 0.2f;
    layer.MaxMoisture = 0.95f;
    layer.MoistureFeather = 0.1f;
    layer.ClumpStrength = 0.7f;
    layer.ClumpScale = 20.0f;
    layer.ClumpFalloff = 1.3f;
    layer.ClumpScaleInfluence = 0.5f;
    layer.ClumpGroup = 2;
    layer.GroundOffset = -0.05f;
    layer.SlopeSinkFactor = 0.8f;
    layer.DecorrelatedVariation = true;

    const auto first = Generate(layer, 3, heights);
    const auto second = Generate(layer, 3, heights);
    ASSERT_FALSE(first.empty());
    ASSERT_EQ(first.size(), second.size());

    for (sizet i = 0; i < first.size(); ++i)
    {
        EXPECT_EQ(first[i].m_CellX, second[i].m_CellX);
        EXPECT_EQ(first[i].m_CellZ, second[i].m_CellZ);
        EXPECT_EQ(std::memcmp(&first[i].m_Row, &second[i].m_Row, sizeof(FoliageInstanceData)), 0)
            << "placement " << i << " is not bit-identical across two runs";
    }

    // And every value it produced is finite — a NaN position is invisible on
    // the CPU and a missing plant on the GPU.
    for (const auto& p : first)
    {
        EXPECT_TRUE(std::isfinite(p.m_Row.PositionScale.x));
        EXPECT_TRUE(std::isfinite(p.m_Row.PositionScale.y));
        EXPECT_TRUE(std::isfinite(p.m_Row.PositionScale.z));
        EXPECT_TRUE(std::isfinite(p.m_Row.PositionScale.w));
        EXPECT_GT(p.m_Row.PositionScale.w, 0.0f);
        EXPECT_TRUE(std::isfinite(p.m_Row.RotationHeight.x));
        EXPECT_TRUE(std::isfinite(p.m_Row.RotationHeight.y));
    }
}

// ── AC3: regeneration updates only the affected canonical instances ─────────

namespace
{
    /// The driver loop from FoliageRenderer::GenerateInstances, minus the GPU
    /// upload — the same ~12 lines FoliageInstanceIdentityPropertyTests uses.
    /// Keep all three in step.
    void Regenerate(FoliageInstanceRegistry& registry, const std::vector<FoliageLayer>& layers,
                    const std::vector<f32>& heights)
    {
        registry.BeginGeneration(layers);

        std::vector<FP::Placement> placements;
        for (u32 layerIdx = 0; layerIdx < static_cast<u32>(layers.size()); ++layerIdx)
        {
            const auto& layer = layers[layerIdx];
            if (!layer.Enabled || layer.Density <= 0.0f)
                continue;

            FP::GenerateLayer(layer, layerIdx, heights, kResolution, nullptr, kWorldSize, kWorldSize,
                              kHeightScale, placements);

            registry.BeginLayer(layerIdx, layer, FP::SeedForLayer(layerIdx),
                                FP::SpacingForDensity(layer.Density), kWorldSize, kWorldSize,
                                FoliageRepresentation::MeshCard, false);

            for (u32 row = 0; row < static_cast<u32>(placements.size()); ++row)
                registry.AddInstance(placements[row].m_CellX, placements[row].m_CellZ, placements[row].m_Row, row);
            registry.EndLayer();
        }

        registry.EndGeneration();
    }

    [[nodiscard]] std::set<FoliageInstanceId> LiveIds(const FoliageInstanceRegistry& registry)
    {
        std::set<FoliageInstanceId> ids;
        for (const auto& record : registry.GetRecords())
            ids.insert(record.m_Id);
        return ids;
    }
} // namespace

TEST(FoliageHabitatClumping, TighteningAHabitatBandRetiresOnlyThePlantsThatFallOutside)
{
    // The incremental contract. A habitat rule gates WHETHER a cell emits; it
    // does not move anything. So tightening one must retire exactly the plants
    // it excluded and leave every survivor its id — the registry's placement
    // signature deliberately does not cover these fields.
    FoliageInstanceRegistry registry;
    const auto heights = RampField(1.0f);

    FoliageLayer wide;
    wide.Name = "Grass";
    wide.Density = 0.5f;
    wide.UseAltitudeBand = true;
    wide.MinAltitude = 0.0f;
    wide.MaxAltitude = 40.0f;

    Regenerate(registry, { wide }, heights);
    ASSERT_GT(registry.GetRecords().size(), 100u);
    const auto before = LiveIds(registry);

    FoliageLayer narrow = wide;
    narrow.MaxAltitude = 20.0f;
    Regenerate(registry, { narrow }, heights);

    const auto after = LiveIds(registry);
    EXPECT_LT(after.size(), before.size()) << "the tighter band excluded nothing";
    EXPECT_GT(after.size(), 0u);

    // Every surviving id is one that already existed: no plant was re-issued.
    for (const FoliageInstanceId id : after)
        EXPECT_TRUE(before.contains(id)) << "a plant inside the unchanged part of the band got a NEW id";
    EXPECT_TRUE(registry.GetLastDelta().m_Added.empty())
        << "tightening a habitat band added plants, so it is moving them, not gating them";
    EXPECT_EQ(registry.GetLastDelta().m_Retired.size(), before.size() - after.size());
}

TEST(FoliageHabitatClumping, EnablingClumpingKeepsTheIdsOfEveryPlantItLeftStanding)
{
    FoliageInstanceRegistry registry;
    const auto heights = FlatField(0.4f);

    FoliageLayer plain;
    plain.Name = "Grass";
    plain.Density = 0.5f;

    Regenerate(registry, { plain }, heights);
    const auto before = LiveIds(registry);
    ASSERT_GT(before.size(), 100u);

    FoliageLayer clumped = plain;
    clumped.ClumpStrength = 0.8f;
    clumped.ClumpScale = 16.0f;
    Regenerate(registry, { clumped }, heights);

    const auto after = LiveIds(registry);
    EXPECT_LT(after.size(), before.size());
    for (const FoliageInstanceId id : after)
        EXPECT_TRUE(before.contains(id)) << "clumping re-issued an id for a plant that did not move";
    EXPECT_TRUE(registry.GetLastDelta().m_Added.empty());
}

TEST(FoliageHabitatClumping, DecorrelatedVariationRetiresEveryIdInThatLayer)
{
    // The other side of the same coin, and the reason the flag IS in the
    // placement signature: it re-draws the jitter, so every plant is somewhere
    // else. Those are different plants in different places, and keeping their
    // ids would be exactly the silent lie FoliageInstanceRegistry exists to
    // prevent.
    FoliageInstanceRegistry registry;
    const auto heights = FlatField(0.4f);

    FoliageLayer legacy;
    legacy.Name = "Grass";
    legacy.Density = 0.5f;

    Regenerate(registry, { legacy }, heights);
    const auto before = LiveIds(registry);
    ASSERT_GT(before.size(), 100u);

    FoliageLayer decorrelated = legacy;
    decorrelated.DecorrelatedVariation = true;
    Regenerate(registry, { decorrelated }, heights);

    const auto after = LiveIds(registry);
    ASSERT_FALSE(after.empty());
    for (const FoliageInstanceId id : after)
        EXPECT_FALSE(before.contains(id)) << "a plant that MOVED kept its canonical id";
    EXPECT_EQ(registry.GetLastDelta().m_Retired.size(), before.size());
    EXPECT_EQ(registry.GetLastDelta().m_Survived, 0u);
}

TEST(FoliageHabitatClumping, GroundSinkIsAnAttributeEditNotAnIdentityChange)
{
    // Y is an attribute, not part of the cell -> XZ mapping, so sinking a layer
    // updates every record and retires nothing. If this ever starts retiring,
    // the sink has leaked into the placement signature.
    FoliageInstanceRegistry registry;
    f32 tanSlope = 0.0f;
    const auto heights = TiltedField(0.3f, tanSlope);

    FoliageLayer flat;
    flat.Name = "Grass";
    flat.Density = 0.5f;

    Regenerate(registry, { flat }, heights);
    const auto before = LiveIds(registry);
    ASSERT_GT(before.size(), 100u);

    FoliageLayer sunk = flat;
    sunk.SlopeSinkFactor = 1.0f;
    Regenerate(registry, { sunk }, heights);

    EXPECT_EQ(LiveIds(registry), before) << "sinking a layer changed which plants exist";
    EXPECT_TRUE(registry.GetLastDelta().m_Retired.empty());
    EXPECT_EQ(registry.GetLastDelta().m_Updated, before.size())
        << "every plant moved in Y, so every record must be reported as updated";
}
