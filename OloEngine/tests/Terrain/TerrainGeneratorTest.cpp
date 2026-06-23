#include "OloEnginePCH.h"
#include <gtest/gtest.h>

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
