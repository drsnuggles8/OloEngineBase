#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Terrain/Foliage/FoliageLayer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainLayer.h"

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// =============================================================================
// TerrainGeneratorTest — pure-CPU contracts for the procedural terrain
// generator (issue #113). These pin the deterministic generation math that the
// renderer / serializer / editor all build on, and run in normal CI (no GPU):
//
//   * Height-field generation — determinism, [0,1] range / finiteness, seed
//     sensitivity, and that each shaping knob (ridge, warp, terrace, exponent)
//     keeps the field valid.
//   * Terrace remap — endpoints, monotonicity, identity at steps==0, plateaus.
//   * Auto-material rule evaluation — band membership, slope selection, weight
//     normalization, the "no rule matched → layer 0" fallback, and the byte
//     packing into the two RGBA8 splatmaps.
//
// The GPU side (GenerateSplatmap upload, the textured render) is covered
// separately by TerrainGenerationEvidenceTest.cpp, which SKIPs without a GL
// context. Classification: unit.
// =============================================================================

using namespace OloEngine;

namespace
{
    constexpr f32 kEps = 1e-4f;

    TerrainGenerator::HeightParams MakeParams(i32 seed = 1337, u32 resolution = 64)
    {
        TerrainGenerator::HeightParams p;
        p.Resolution = resolution;
        p.Seed = seed;
        p.Octaves = 5;
        p.Frequency = 3.0f;
        p.Lacunarity = 2.0f;
        p.Persistence = 0.45f;
        return p;
    }

    // Every value present, finite, and inside the normalized [0, 1] band.
    void ExpectNormalizedField(const std::vector<f32>& heights, u32 resolution)
    {
        ASSERT_EQ(heights.size(), static_cast<sizet>(resolution) * resolution);
        for (const f32 h : heights)
        {
            ASSERT_TRUE(std::isfinite(h)) << "height field contains a non-finite value";
            EXPECT_GE(h, 0.0f);
            EXPECT_LE(h, 1.0f);
        }
    }
} // namespace

// ── Height field ────────────────────────────────────────────────────────────

TEST(TerrainGeneratorTest, HeightFieldIsDeterministic)
{
    const auto params = MakeParams();
    std::vector<f32> a;
    std::vector<f32> b;
    TerrainGenerator::GenerateHeightField(a, params);
    TerrainGenerator::GenerateHeightField(b, params);
    // Same parameters → bit-identical field (the precondition for golden renders
    // and reproducible scenes). Vector operator== is element-wise.
    EXPECT_EQ(a, b);
}

TEST(TerrainGeneratorTest, HeightFieldIsNormalizedAndFinite)
{
    const auto params = MakeParams();
    std::vector<f32> heights;
    TerrainGenerator::GenerateHeightField(heights, params);
    ExpectNormalizedField(heights, params.Resolution);

    // A non-trivial field actually spans the range (not a constant plane).
    const auto [minIt, maxIt] = std::minmax_element(heights.begin(), heights.end());
    EXPECT_LE(*minIt, 0.05f);
    EXPECT_GE(*maxIt, 0.95f);
}

TEST(TerrainGeneratorTest, LargeSeedStillProducesVariedTerrain)
{
    // Regression: a naive `seed * k` noise offset overflows f32 precision for
    // large seeds (the editor's Randomize Seed picks any i32), collapsing every
    // sample to one lattice cell → a dead-flat field. The seed must be hashed
    // into a bounded range so any seed yields real relief.
    std::vector<f32> heights;
    TerrainGenerator::GenerateHeightField(heights, MakeParams(20240611));
    ExpectNormalizedField(heights, 64);
    const auto [minIt, maxIt] = std::minmax_element(heights.begin(), heights.end());
    EXPECT_GT(*maxIt - *minIt, 0.5f) << "large seed produced a (near-)flat height field";
}

TEST(TerrainGeneratorTest, DifferentSeedsProduceDifferentTerrain)
{
    std::vector<f32> a;
    std::vector<f32> b;
    TerrainGenerator::GenerateHeightField(a, MakeParams(1));
    TerrainGenerator::GenerateHeightField(b, MakeParams(2));
    ASSERT_EQ(a.size(), b.size());
    EXPECT_NE(a, b);
}

TEST(TerrainGeneratorTest, RidgedAndWarpAndExponentStayValid)
{
    auto params = MakeParams();
    params.Shaping.RidgeBlend = 1.0f;   // pure ridged multifractal
    params.Shaping.WarpStrength = 0.3f; // heavy domain warp
    params.Shaping.WarpFrequency = 3.0f;
    params.Shaping.HeightExponent = 2.5f;
    std::vector<f32> heights;
    TerrainGenerator::GenerateHeightField(heights, params);
    ExpectNormalizedField(heights, params.Resolution);
}

TEST(TerrainGeneratorTest, TerraceShapingStaysValidAndDeterministic)
{
    auto params = MakeParams();
    params.Shaping.TerraceSteps = 6;
    params.Shaping.TerraceSharpness = 0.85f;
    std::vector<f32> a;
    std::vector<f32> b;
    TerrainGenerator::GenerateHeightField(a, params);
    TerrainGenerator::GenerateHeightField(b, params);
    ExpectNormalizedField(a, params.Resolution);
    EXPECT_EQ(a, b);
}

// ── Radial island falloff (issue #880) ──────────────────────────────────────
//
// The contract this mask exists for: at full strength EVERY border texel of the
// tile is driven to 0, so a terrain tile meeting an ocean has a shoreline rather
// than a cliff wall where the tile stops. Measured on the Drift island before
// the mask existed, 64.9% of its border was above sea level — none of the other
// shaping knobs can fix that, because they are all uniform over the tile.

namespace
{
    // Largest height found on the tile's outermost ring of texels.
    [[nodiscard]] f32 MaxBorderHeight(const std::vector<f32>& heights, u32 resolution)
    {
        f32 maxBorder = 0.0f;
        const auto at = [&](u32 x, u32 z)
        { return heights[static_cast<sizet>(z) * resolution + x]; };
        for (u32 x = 0; x < resolution; ++x)
        {
            maxBorder = std::max({ maxBorder, at(x, 0), at(x, resolution - 1) });
        }
        for (u32 z = 0; z < resolution; ++z)
        {
            maxBorder = std::max({ maxBorder, at(0, z), at(resolution - 1, z) });
        }
        return maxBorder;
    }
} // namespace

TEST(TerrainGeneratorTest, IslandFalloffIsOffByDefaultAndLeavesTheFieldUntouched)
{
    // The identity guarantee: a default-constructed TerrainHeightShaping must
    // produce the same field it did before the mask existed, bit for bit.
    auto params = MakeParams();
    params.Shaping.RidgeBlend = 0.5f;
    params.Shaping.HeightExponent = 1.15f;
    std::vector<f32> off;
    TerrainGenerator::GenerateHeightField(off, params);

    params.Shaping.IslandFalloff = 0.0f;
    params.Shaping.IslandFalloffRadius = 0.25f; // ignored while the strength is 0
    std::vector<f32> explicitlyOff;
    TerrainGenerator::GenerateHeightField(explicitlyOff, params);
    EXPECT_EQ(off, explicitlyOff);

    // ...and the un-masked field really does keep a high border, which is the
    // defect the mask is here to remove. Without this the test above would pass
    // just as happily on a field that was flat at the edges anyway.
    EXPECT_GT(MaxBorderHeight(off, params.Resolution), 0.25f);
}

TEST(TerrainGeneratorTest, IslandFalloffDrivesEveryBorderTexelToZero)
{
    // Swept over resolutions because the ramp's outer radius is derived from the
    // resolution (it ends on the mid-edge texel CENTRE, the outermost sample that
    // exists); an off-by-half-a-texel there leaves a small positive tail on the
    // border instead of an exact zero.
    for (const u32 resolution : { 32u, 64u, 127u, 128u })
    {
        auto params = MakeParams(4242, resolution);
        params.Shaping.RidgeBlend = 0.5f;
        params.Shaping.WarpStrength = 0.2f;
        params.Shaping.HeightExponent = 1.4f;
        params.Shaping.TerraceSteps = 4; // exercised AFTER the mask: Terrace(0) must be 0
        params.Shaping.IslandFalloff = 1.0f;
        params.Shaping.IslandFalloffRadius = 0.3f;

        std::vector<f32> heights;
        TerrainGenerator::GenerateHeightField(heights, params);
        ExpectNormalizedField(heights, resolution);
        EXPECT_FLOAT_EQ(MaxBorderHeight(heights, resolution), 0.0f)
            << "resolution " << resolution << ": the tile border is not at the base height";

        // Anti-vacuity: the island itself must still be there. A mask that zeroed
        // the whole tile would satisfy the assertion above perfectly.
        const f32 peak = *std::max_element(heights.begin(), heights.end());
        EXPECT_GT(peak, 0.5f) << "resolution " << resolution << ": the mask flattened the whole tile";
    }
}

TEST(TerrainGeneratorTest, IslandFalloffRadiusControlsHowMuchOfTheTileIsLand)
{
    // The radius is the island-size knob: a smaller one leaves less of the tile
    // above any given cut. Compared at a fixed cut so this is a statement about
    // the mask, not about the noise.
    constexpr f32 kSeaLevel = 0.2f;
    const auto landFraction = [](f32 radius)
    {
        auto params = MakeParams(99, 96);
        params.Shaping.IslandFalloff = 1.0f;
        params.Shaping.IslandFalloffRadius = radius;
        std::vector<f32> heights;
        TerrainGenerator::GenerateHeightField(heights, params);
        const auto above = std::count_if(heights.begin(), heights.end(), [](f32 h)
                                         { return h > kSeaLevel; });
        return static_cast<f32>(above) / static_cast<f32>(heights.size());
    };

    const f32 small = landFraction(0.15f);
    const f32 large = landFraction(0.40f);
    EXPECT_GT(large, small);
    EXPECT_GT(small, 0.0f) << "a small island is still an island";
}

TEST(TerrainGeneratorTest, IslandFalloffStrengthInterpolatesTowardsTheMask)
{
    // Partial strength is a lerp between the raw field and the fully masked one,
    // so a border texel at half strength sits at half its unmasked height.
    auto params = MakeParams(7, 64);
    std::vector<f32> raw;
    TerrainGenerator::GenerateHeightField(raw, params);

    params.Shaping.IslandFalloff = 0.5f;
    params.Shaping.IslandFalloffRadius = 0.3f;
    std::vector<f32> half;
    TerrainGenerator::GenerateHeightField(half, params);
    ExpectNormalizedField(half, params.Resolution);

    // Corner texel: the mask is exactly 0 there, so half strength halves it.
    const sizet corner = 0;
    EXPECT_NEAR(half[corner], raw[corner] * 0.5f, kEps);
    // ...and the border is emphatically NOT zero at half strength, which is what
    // separates "the strength is honoured" from "the mask is always full".
    EXPECT_GT(MaxBorderHeight(half, params.Resolution), 0.0f);
}

TEST(TerrainGeneratorTest, IslandFalloffSurvivesTheErosionPostPass)
{
    // Erosion runs AFTER the mask, and a droplet that dies on the border deposits
    // its sediment on the very texels the mask drove to the floor. Nothing else in
    // the pipeline looks at that edge, so the guarantee could be silently undone
    // by turning on an unrelated knob — which is the whole reason this case is
    // separate from the sweep above rather than another entry in it.
    auto params = MakeParams(20250880, 64);
    params.Shaping.RidgeBlend = 0.4f;
    params.Shaping.IslandFalloff = 1.0f;
    params.Shaping.IslandFalloffRadius = 0.28f;
    params.ErosionIterations = 4;

    std::vector<f32> heights;
    TerrainGenerator::GenerateHeightField(heights, params);
    ExpectNormalizedField(heights, params.Resolution);
    EXPECT_FLOAT_EQ(MaxBorderHeight(heights, params.Resolution), 0.0f)
        << "the erosion post-pass deposited sediment on the masked tile border";

    // Anti-vacuity twice over: the island is still there, and the erosion pass
    // actually ran (otherwise this asserts the same thing the sweep already does).
    EXPECT_GT(*std::max_element(heights.begin(), heights.end()), 0.5f);

    auto unEroded = params;
    unEroded.ErosionIterations = 0;
    std::vector<f32> baseline;
    TerrainGenerator::GenerateHeightField(baseline, unEroded);
    EXPECT_NE(heights, baseline) << "erosion made no difference, so this test proves nothing";
}

TEST(TerrainGeneratorTest, FractionalIslandFalloffIsNotAppliedTwiceByTheErosionPass)
{
    // At strength 1 the border ends at 0 whether the post-erosion step multiplies
    // or caps, so the case above cannot tell the two apart. At any FRACTIONAL
    // strength it can: a second multiply leaves an untouched border texel at
    // raw * (1 - strength)^2 instead of raw * (1 - strength). Nothing in the
    // shipped scenes uses a fractional strength, which is exactly why the mistake
    // would have sat there — the field is editable from the editor, Lua and MCP.
    constexpr f32 kStrength = 0.5f;
    auto params = MakeParams(31415, 64);
    params.Shaping.IslandFalloff = kStrength;
    params.Shaping.IslandFalloffRadius = 0.28f;

    std::vector<f32> withoutErosion;
    TerrainGenerator::GenerateHeightField(withoutErosion, params);

    params.ErosionIterations = 4;
    std::vector<f32> withErosion;
    TerrainGenerator::GenerateHeightField(withErosion, params);

    // The border may only be carved DOWN by erosion, never scaled again.
    const u32 resolution = params.Resolution;
    const auto at = [&](const std::vector<f32>& h, u32 x, u32 z)
    { return h[static_cast<sizet>(z) * resolution + x]; };
    bool sawAnyBorderHeight = false;
    for (u32 x = 0; x < resolution; ++x)
    {
        for (const u32 z : { 0u, resolution - 1u })
        {
            const f32 before = at(withoutErosion, x, z);
            EXPECT_LE(at(withErosion, x, z), before + kEps)
                << "border texel (" << x << ", " << z << ") rose above its masked height";
            sawAnyBorderHeight = sawAnyBorderHeight || before > kEps;
        }
    }
    // Anti-vacuity: at half strength the border is NOT zero, so "never rose" is a
    // real constraint rather than a restatement of "it is pinned at the floor".
    EXPECT_TRUE(sawAnyBorderHeight) << "the border is already at zero, so this asserts nothing";
}

TEST(TerrainGeneratorTest, IslandFalloffIsDeterministicAndComparesEqual)
{
    auto params = MakeParams();
    params.Shaping.IslandFalloff = 0.8f;
    params.Shaping.IslandFalloffRadius = 0.28f;
    std::vector<f32> a;
    std::vector<f32> b;
    TerrainGenerator::GenerateHeightField(a, params);
    TerrainGenerator::GenerateHeightField(b, params);
    EXPECT_EQ(a, b);

    // operator== must see both new fields, or an inspector edit to either records
    // no undo step and a Scene::Copy comparison silently reports "unchanged".
    TerrainHeightShaping lhs;
    TerrainHeightShaping rhs;
    EXPECT_TRUE(lhs == rhs);
    rhs.IslandFalloff = 0.5f;
    EXPECT_FALSE(lhs == rhs);
    rhs = lhs;
    rhs.IslandFalloffRadius = 0.11f;
    EXPECT_FALSE(lhs == rhs);
}

// ── Erosion post-pass ───────────────────────────────────────────────────────

TEST(TerrainGeneratorTest, ErosionPostPassIsDeterministic)
{
    // Same seed + iteration count → bit-identical field. This is the whole point
    // of the CPU post-pass: the GPU editor brush races (parallel droplet writes),
    // the generation pass must be reproducible so scenes regenerate identically.
    auto params = MakeParams();
    params.ErosionIterations = 2;
    std::vector<f32> a;
    std::vector<f32> b;
    TerrainGenerator::GenerateHeightField(a, params);
    TerrainGenerator::GenerateHeightField(b, params);
    EXPECT_EQ(a, b);
}

TEST(TerrainGeneratorTest, ErosionStaysNormalizedAndFinite)
{
    auto params = MakeParams();
    params.ErosionIterations = 2;
    std::vector<f32> heights;
    TerrainGenerator::GenerateHeightField(heights, params);
    // Erosion deposits/erodes without bound internally, but the field is clamped
    // back into the [0,1] contract every downstream consumer relies on.
    ExpectNormalizedField(heights, params.Resolution);
}

TEST(TerrainGeneratorTest, ErosionActuallyChangesTheField)
{
    // The gate must do something: an eroded field differs from the same seed's
    // un-eroded field, and ErosionIterations == 0 is exactly the un-eroded field.
    auto base = MakeParams();
    std::vector<f32> plain;
    TerrainGenerator::GenerateHeightField(plain, base);

    auto eroded = base;
    eroded.ErosionIterations = 2;
    std::vector<f32> carved;
    TerrainGenerator::GenerateHeightField(carved, eroded);

    ASSERT_EQ(plain.size(), carved.size());
    EXPECT_NE(plain, carved) << "erosion post-pass left the field unchanged";

    // Zero iterations is the disabled path — identical to never calling erosion.
    auto off = base;
    off.ErosionIterations = 0;
    std::vector<f32> untouched;
    TerrainGenerator::GenerateHeightField(untouched, off);
    EXPECT_EQ(plain, untouched);
}

TEST(TerrainGeneratorTest, ApplyErosionStandaloneIsDeterministicAndGuarded)
{
    constexpr u32 kRes = 48;
    auto params = MakeParams(1337, kRes);
    std::vector<f32> field;
    TerrainGenerator::GenerateHeightField(field, params);

    const ErosionParams erosion; // defaults (namespace-scope struct, like TerrainLayerRule)

    // Two independent runs on copies of the same field → identical results.
    std::vector<f32> a = field;
    std::vector<f32> b = field;
    TerrainGenerator::ApplyErosion(a, kRes, 2, erosion, /*seed*/ 99);
    TerrainGenerator::ApplyErosion(b, kRes, 2, erosion, /*seed*/ 99);
    EXPECT_EQ(a, b);
    ExpectNormalizedField(a, kRes);

    // A different seed carves a different field.
    std::vector<f32> c = field;
    TerrainGenerator::ApplyErosion(c, kRes, 2, erosion, /*seed*/ 1234);
    EXPECT_NE(a, c);

    // Guards: zero iterations and a mismatched buffer are no-ops (not crashes).
    std::vector<f32> noop = field;
    TerrainGenerator::ApplyErosion(noop, kRes, 0, erosion, 99);
    EXPECT_EQ(noop, field);

    std::vector<f32> wrongSize(kRes * kRes + 1, 0.5f);
    const std::vector<f32> before = wrongSize;
    TerrainGenerator::ApplyErosion(wrongSize, kRes, 2, erosion, 99);
    EXPECT_EQ(wrongSize, before);
}

// ── Terrace remap ─────────────────────────────────────────────────────────

TEST(TerrainGeneratorTest, TerraceEndpointsAndIdentity)
{
    // steps == 0 is identity.
    EXPECT_NEAR(TerrainGenerator::Terrace(0.42f, 0, 0.5f), 0.42f, kEps);

    // Endpoints are preserved for any step count.
    EXPECT_NEAR(TerrainGenerator::Terrace(0.0f, 5, 0.6f), 0.0f, kEps);
    EXPECT_NEAR(TerrainGenerator::Terrace(1.0f, 5, 0.6f), 1.0f, kEps);
}

TEST(TerrainGeneratorTest, TerraceIsMonotonicAndBounded)
{
    f32 prev = -1.0f;
    for (int i = 0; i <= 200; ++i)
    {
        const f32 x = static_cast<f32>(i) / 200.0f;
        const f32 y = TerrainGenerator::Terrace(x, 5, 0.8f);
        EXPECT_GE(y, 0.0f);
        EXPECT_LE(y, 1.0f);
        EXPECT_GE(y, prev - kEps) << "Terrace must be monotonic non-decreasing at x=" << x;
        prev = y;
    }
}

TEST(TerrainGeneratorTest, TerraceProducesFlatPlateaus)
{
    // With high sharpness the ramp should spend most of its length on flat
    // plateaus (consecutive samples that barely change).
    int flatSamples = 0;
    constexpr int kSamples = 500;
    f32 prev = TerrainGenerator::Terrace(0.0f, 4, 0.95f);
    for (int i = 1; i <= kSamples; ++i)
    {
        const f32 x = static_cast<f32>(i) / static_cast<f32>(kSamples);
        const f32 y = TerrainGenerator::Terrace(x, 4, 0.95f);
        if (std::fabs(y - prev) < 1e-3f)
            ++flatSamples;
        prev = y;
    }
    EXPECT_GT(flatSamples, kSamples / 2) << "expected the terraced ramp to be mostly flat plateaus";
}

// ── Auto-material rule evaluation ───────────────────────────────────────────

TEST(TerrainGeneratorTest, RuleWeightPeaksInsideBandAndZeroOutside)
{
    TerrainLayerRule rule;
    rule.LayerIndex = 1;
    rule.MinHeight = 0.3f;
    rule.MaxHeight = 0.6f;
    rule.HeightBlend = 0.05f;
    rule.MinSlopeDeg = 0.0f;
    rule.MaxSlopeDeg = 90.0f;
    rule.SlopeBlend = 0.0f;
    rule.Strength = 1.0f;

    EXPECT_NEAR(TerrainGenerator::EvaluateRuleWeight(0.45f, 0.0f, rule), 1.0f, kEps); // centre of band
    EXPECT_NEAR(TerrainGenerator::EvaluateRuleWeight(0.0f, 0.0f, rule), 0.0f, kEps);  // well below
    EXPECT_NEAR(TerrainGenerator::EvaluateRuleWeight(1.0f, 0.0f, rule), 0.0f, kEps);  // well above
}

TEST(TerrainGeneratorTest, SlopeBandSelectsRule)
{
    TerrainLayerRule cliff;
    cliff.LayerIndex = 2;
    cliff.MinHeight = 0.0f;
    cliff.MaxHeight = 1.0f;
    cliff.HeightBlend = 0.0f;
    cliff.MinSlopeDeg = 32.0f;
    cliff.MaxSlopeDeg = 90.0f;
    cliff.SlopeBlend = 0.0f;
    cliff.Strength = 1.0f;

    EXPECT_NEAR(TerrainGenerator::EvaluateRuleWeight(0.5f, 60.0f, cliff), 1.0f, kEps); // steep → on
    EXPECT_NEAR(TerrainGenerator::EvaluateRuleWeight(0.5f, 5.0f, cliff), 0.0f, kEps);  // flat → off
}

TEST(TerrainGeneratorTest, DefaultRulesAssignExpectedLayers)
{
    const auto rules = TerrainGenerator::MakeDefaultRules();
    std::array<f32, MAX_TERRAIN_LAYERS> w{};

    // Low gentle ground → grass (layer 1) dominates.
    TerrainGenerator::EvaluateLayerWeights(0.30f, 5.0f, rules, w);
    EXPECT_GT(w[1], w[0]);
    EXPECT_GT(w[1], w[2]);
    EXPECT_GT(w[1], w[3]);

    // Steep slope at mid altitude → rock (layer 2) dominates.
    TerrainGenerator::EvaluateLayerWeights(0.40f, 65.0f, rules, w);
    EXPECT_GT(w[2], w[1]);

    // High gentle ground → snow (layer 3) dominates.
    TerrainGenerator::EvaluateLayerWeights(0.92f, 5.0f, rules, w);
    EXPECT_GT(w[3], w[1]);
}

TEST(TerrainGeneratorTest, LayerWeightsAreNormalized)
{
    const auto rules = TerrainGenerator::MakeDefaultRules();
    std::array<f32, MAX_TERRAIN_LAYERS> w{};
    TerrainGenerator::EvaluateLayerWeights(0.30f, 5.0f, rules, w);

    f32 sum = 0.0f;
    for (const f32 v : w)
        sum += v;
    EXPECT_NEAR(sum, 1.0f, kEps);
}

TEST(TerrainGeneratorTest, NoMatchingRuleFallsBackToLayerZero)
{
    std::vector<TerrainLayerRule> rules;
    TerrainLayerRule narrow;
    narrow.LayerIndex = 3;
    narrow.MinHeight = 0.40f;
    narrow.MaxHeight = 0.50f;
    narrow.HeightBlend = 0.0f;
    narrow.MinSlopeDeg = 0.0f;
    narrow.MaxSlopeDeg = 10.0f;
    narrow.SlopeBlend = 0.0f;
    rules.push_back(narrow);

    std::array<f32, MAX_TERRAIN_LAYERS> w{};
    TerrainGenerator::EvaluateLayerWeights(0.95f, 80.0f, rules, w); // outside the only rule
    EXPECT_NEAR(w[0], 1.0f, kEps);
    EXPECT_NEAR(w[3], 0.0f, kEps);
}

TEST(TerrainGeneratorTest, PackLayerWeightsQuantizesToBothSplatmaps)
{
    std::array<f32, MAX_TERRAIN_LAYERS> w{};
    w[0] = 1.0f; // layer 0 → splatmap 0, R
    w[5] = 0.5f; // layer 5 → splatmap 1, G

    std::array<u8, 4> s0{};
    std::array<u8, 4> s1{};
    TerrainGenerator::PackLayerWeights(w, s0.data(), s1.data());

    EXPECT_EQ(s0[0], 255);
    EXPECT_EQ(s0[1], 0);
    EXPECT_EQ(s0[2], 0);
    EXPECT_EQ(s0[3], 0);
    EXPECT_EQ(s1[0], 0);
    EXPECT_EQ(s1[1], 128); // round(0.5 * 255)
}

// ── Foliage auto-population ─────────────────────────────────────────────────
// The rule → FoliageLayer mapping that closes the loop: the same height/slope
// rules that paint the splatmap also emit matching foliage. Pure CPU, so these
// pin the mapping in CI; the actual on-screen vegetation is checked in the
// editor (rendering-adjacent change).

namespace
{
    // A FoliageLayer is well-formed: finite floats, ordered ranges, a fade that
    // begins before the cull distance, and a real splat channel.
    void ExpectValidFoliageLayer(const FoliageLayer& layer)
    {
        EXPECT_FALSE(layer.Name.empty());
        // A foliage billboard alpha-tests against its albedo cutout; without one
        // it renders as solid/garbage quads, so the preset must supply a texture.
        EXPECT_FALSE(layer.AlbedoPath.empty()) << "emitted foliage needs an albedo cutout to render as blades";
        EXPECT_GT(layer.Density, 0.0f);
        EXPECT_GE(layer.SplatmapChannel, 0);
        EXPECT_LT(layer.SplatmapChannel, 8);

        EXPECT_TRUE(std::isfinite(layer.MinSlopeAngle));
        EXPECT_TRUE(std::isfinite(layer.MaxSlopeAngle));
        EXPECT_GE(layer.MinSlopeAngle, 0.0f);
        EXPECT_LE(layer.MaxSlopeAngle, 90.0f);
        EXPECT_LE(layer.MinSlopeAngle, layer.MaxSlopeAngle);

        EXPECT_TRUE(std::isfinite(layer.Density));
        EXPECT_TRUE(std::isfinite(layer.MinScale));
        EXPECT_TRUE(std::isfinite(layer.MaxScale));
        EXPECT_LE(layer.MinScale, layer.MaxScale);
        EXPECT_TRUE(std::isfinite(layer.MinHeight));
        EXPECT_TRUE(std::isfinite(layer.MaxHeight));
        EXPECT_LE(layer.MinHeight, layer.MaxHeight);

        EXPECT_TRUE(std::isfinite(layer.ViewDistance));
        EXPECT_TRUE(std::isfinite(layer.FadeStartDistance));
        EXPECT_GT(layer.ViewDistance, 0.0f);
        EXPECT_LT(layer.FadeStartDistance, layer.ViewDistance) << "fade must begin before the cull distance";

        EXPECT_TRUE(std::isfinite(layer.BaseColor.x));
        EXPECT_TRUE(std::isfinite(layer.BaseColor.y));
        EXPECT_TRUE(std::isfinite(layer.BaseColor.z));
        EXPECT_TRUE(std::isfinite(layer.WindStrength));
        EXPECT_TRUE(std::isfinite(layer.WindSpeed));
    }
} // namespace

TEST(TerrainGeneratorTest, DefaultFoliageLayersAreVegetatedAndWellFormed)
{
    const auto layers = TerrainGenerator::MakeDefaultFoliageLayers();
    ASSERT_FALSE(layers.empty()) << "default biome must emit some vegetation";

    bool sawGrassChannel = false;
    for (const auto& layer : layers)
    {
        ExpectValidFoliageLayer(layer);
        if (layer.SplatmapChannel == 1) // grass material layer
            sawGrassChannel = true;
    }
    EXPECT_TRUE(sawGrassChannel) << "default biome should grow foliage on the grass layer (channel 1)";
}

TEST(TerrainGeneratorTest, DefaultFoliageEqualsMappingOfDefaultRules)
{
    // The convenience preset is exactly the rule-driven mapping applied to the
    // default rules — not a separate hand-authored list that could drift.
    const auto preset = TerrainGenerator::MakeDefaultFoliageLayers();
    const auto fromRules = TerrainGenerator::MakeFoliageLayersFromRules(TerrainGenerator::MakeDefaultRules());
    ASSERT_EQ(preset.size(), fromRules.size());
    for (sizet i = 0; i < preset.size(); ++i)
        EXPECT_TRUE(preset[i] == fromRules[i]) << "preset layer " << i << " differs from the rule mapping";
}

TEST(TerrainGeneratorTest, FoliageInheritsPlacementBandFromMatchingRule)
{
    // The placement mask is taken from the material rule, so vegetation lands
    // exactly on the band the splatmap paints. A rule with a tight slope band
    // clamps the emitted foliage's slope band the same way.
    std::vector<TerrainLayerRule> rules;
    TerrainLayerRule grass;
    grass.LayerIndex = 1; // grass layer → grass + wildflowers profiles
    grass.MinSlopeDeg = 4.0f;
    grass.MaxSlopeDeg = 12.0f; // tighter than either profile's own ceiling
    rules.push_back(grass);

    const auto layers = TerrainGenerator::MakeFoliageLayersFromRules(rules);
    ASSERT_FALSE(layers.empty());
    for (const auto& layer : layers)
    {
        EXPECT_EQ(layer.SplatmapChannel, 1) << "foliage must read the layer its rule paints";
        EXPECT_GE(layer.MinSlopeAngle, 4.0f - kEps) << "min slope inherited from the rule";
        EXPECT_LE(layer.MaxSlopeAngle, 12.0f + kEps) << "rule's slope ceiling must clamp the foliage band";
    }
}

TEST(TerrainGeneratorTest, ProfileSlopeCeilingClampsPermissiveRule)
{
    // The reverse direction: a wide-open rule (0..90°) must still be tightened
    // by the profile's own slope ceiling so grass never climbs onto cliffs.
    std::vector<TerrainLayerRule> rules;
    TerrainLayerRule grass;
    grass.LayerIndex = 1;
    grass.MinSlopeDeg = 0.0f;
    grass.MaxSlopeDeg = 90.0f;
    rules.push_back(grass);

    const auto layers = TerrainGenerator::MakeFoliageLayersFromRules(rules);
    ASSERT_FALSE(layers.empty());
    for (const auto& layer : layers)
        EXPECT_LT(layer.MaxSlopeAngle, 90.0f) << "profile slope ceiling must cap a permissive rule";
}

TEST(TerrainGeneratorTest, FoliageOnlyEmittedForLayersWithRules)
{
    // No rules → no vegetation (nothing painted to grow on).
    EXPECT_TRUE(TerrainGenerator::MakeFoliageLayersFromRules({}).empty());

    // A rule set that only paints rock (layer 2) has no vegetation profile, so
    // it emits nothing — bare cliffs stay bare.
    std::vector<TerrainLayerRule> rockOnly;
    TerrainLayerRule rock;
    rock.LayerIndex = 2;
    rockOnly.push_back(rock);
    EXPECT_TRUE(TerrainGenerator::MakeFoliageLayersFromRules(rockOnly).empty());

    // A rule set that only paints grass (layer 1) emits the grass-layer
    // profiles (grass + wildflowers) but no sand dune grass (layer 0 unpainted).
    std::vector<TerrainLayerRule> grassOnly;
    TerrainLayerRule grass;
    grass.LayerIndex = 1;
    grassOnly.push_back(grass);
    const auto layers = TerrainGenerator::MakeFoliageLayersFromRules(grassOnly);
    EXPECT_FALSE(layers.empty());
    for (const auto& layer : layers)
        EXPECT_EQ(layer.SplatmapChannel, 1) << "only the painted (grass) layer should carry foliage";
}

// ── Visual evidence (manual) ────────────────────────────────────────────────
// DISABLED so it never runs in CI (writes files); run on demand with
//   OloEngine-Tests.exe --gtest_also_run_disabled_tests \
//       --gtest_filter=TerrainGeneratorTest.DISABLED_DumpErosionHeightmapPNGs
// to eyeball that the post-pass carves dendritic drainage channels rather than
// just perturbing the noise. Writes grayscale heightmaps (plus a signed
// erosion/deposition diff) to OloEditor/assets/tests/visual/.
namespace
{
    void WriteGrayscalePNG(const std::string& path, const std::vector<f32>& field, u32 res)
    {
        std::vector<std::uint8_t> px(static_cast<sizet>(res) * res);
        for (sizet i = 0; i < px.size(); ++i)
            px[i] = static_cast<std::uint8_t>(std::lround(std::clamp(field[i], 0.0f, 1.0f) * 255.0f));
        stbi_write_png(path.c_str(), static_cast<int>(res), static_cast<int>(res), 1, px.data(), static_cast<int>(res));
    }
} // namespace

TEST(TerrainGeneratorTest, DISABLED_DumpErosionHeightmapPNGs)
{
    constexpr u32 kRes = 256;
    auto params = MakeParams(1337, kRes);
    params.Octaves = 7;
    params.Shaping.RidgeBlend = 0.55f; // some mountains so channels are visible

    std::vector<f32> plain;
    TerrainGenerator::GenerateHeightField(plain, params);

    params.ErosionIterations = 4;
    std::vector<f32> eroded;
    TerrainGenerator::GenerateHeightField(eroded, params);

    // Signed diff centered at 0.5: darker = eroded away, brighter = deposited.
    std::vector<f32> diff(plain.size());
    for (sizet i = 0; i < diff.size(); ++i)
        diff[i] = 0.5f + std::clamp((eroded[i] - plain[i]) * 6.0f, -0.5f, 0.5f);

    const std::string dir = "assets/tests/visual/";
    WriteGrayscalePNG(dir + "terrain_erosion_before.png", plain, kRes);
    WriteGrayscalePNG(dir + "terrain_erosion_after.png", eroded, kRes);
    WriteGrayscalePNG(dir + "terrain_erosion_diff.png", diff, kRes);

    SUCCEED() << "wrote terrain_erosion_{before,after,diff}.png to " << dir;
}
