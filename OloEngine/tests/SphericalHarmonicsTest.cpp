#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/LightProbeBaker.h"
#include "OloEngine/Renderer/SphericalHarmonics.h"
#include "OloEngine/Scene/Components.h"

#include <glm/gtc/constants.hpp>
#include <vector>

using namespace OloEngine;

// ── SHCoefficients basic operations ──

TEST(SphericalHarmonicsTest, ZeroClearsAllCoefficients)
{
    SHCoefficients sh;
    sh.Coefficients[0] = glm::vec3(1.0f, 2.0f, 3.0f);
    sh.Coefficients[4] = glm::vec3(5.0f);

    sh.Zero();

    for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
    {
        EXPECT_FLOAT_EQ(sh.Coefficients[i].x, 0.0f);
        EXPECT_FLOAT_EQ(sh.Coefficients[i].y, 0.0f);
        EXPECT_FLOAT_EQ(sh.Coefficients[i].z, 0.0f);
    }
}

TEST(SphericalHarmonicsTest, AccumulateAddsCoefficients)
{
    SHCoefficients a;
    a.Zero();
    a.Coefficients[0] = glm::vec3(1.0f, 0.0f, 0.0f);
    a.Coefficients[1] = glm::vec3(0.0f, 2.0f, 0.0f);

    SHCoefficients b;
    b.Zero();
    b.Coefficients[0] = glm::vec3(0.5f, 1.0f, 0.0f);
    b.Coefficients[1] = glm::vec3(0.0f, 0.5f, 3.0f);

    a.Accumulate(b);

    EXPECT_FLOAT_EQ(a.Coefficients[0].x, 1.5f);
    EXPECT_FLOAT_EQ(a.Coefficients[0].y, 1.0f);
    EXPECT_FLOAT_EQ(a.Coefficients[1].y, 2.5f);
    EXPECT_FLOAT_EQ(a.Coefficients[1].z, 3.0f);
}

TEST(SphericalHarmonicsTest, ScaleMultipliesAllCoefficients)
{
    SHCoefficients sh;
    sh.Zero();
    sh.Coefficients[0] = glm::vec3(2.0f, 4.0f, 6.0f);
    sh.Coefficients[3] = glm::vec3(1.0f, 1.0f, 1.0f);

    sh.Scale(0.5f);

    EXPECT_FLOAT_EQ(sh.Coefficients[0].x, 1.0f);
    EXPECT_FLOAT_EQ(sh.Coefficients[0].y, 2.0f);
    EXPECT_FLOAT_EQ(sh.Coefficients[0].z, 3.0f);
    EXPECT_FLOAT_EQ(sh.Coefficients[3].x, 0.5f);
}

// ── GPU Layout roundtrip ──

TEST(SphericalHarmonicsTest, GPULayoutRoundtripPreservesData)
{
    SHCoefficients original;
    for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
    {
        original.Coefficients[i] = glm::vec3(
            static_cast<f32>(i) * 0.1f,
            static_cast<f32>(i) * 0.2f,
            static_cast<f32>(i) * 0.3f);
    }

    std::array<glm::vec4, SH_COEFFICIENT_COUNT> gpuData{};
    original.ToGPULayout(gpuData, 1.0f);

    SHCoefficients restored;
    restored.FromGPULayout(gpuData);

    for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
    {
        EXPECT_FLOAT_EQ(restored.Coefficients[i].x, original.Coefficients[i].x);
        EXPECT_FLOAT_EQ(restored.Coefficients[i].y, original.Coefficients[i].y);
        EXPECT_FLOAT_EQ(restored.Coefficients[i].z, original.Coefficients[i].z);
    }
}

TEST(SphericalHarmonicsTest, GPULayoutValidityFlag)
{
    SHCoefficients sh;
    sh.Zero();
    sh.Coefficients[0] = glm::vec3(1.0f, 2.0f, 3.0f);

    std::array<glm::vec4, SH_COEFFICIENT_COUNT> gpuData{};

    sh.ToGPULayout(gpuData, 1.0f);
    EXPECT_FLOAT_EQ(gpuData[0].w, 1.0f);

    sh.ToGPULayout(gpuData, 0.0f);
    EXPECT_FLOAT_EQ(gpuData[0].w, 0.0f);

    // RGB data preserved regardless of validity flag
    EXPECT_FLOAT_EQ(gpuData[0].x, 1.0f);
    EXPECT_FLOAT_EQ(gpuData[0].y, 2.0f);
    EXPECT_FLOAT_EQ(gpuData[0].z, 3.0f);
}

TEST(SphericalHarmonicsTest, GPULayoutUnusedWComponentsAreZero)
{
    SHCoefficients sh;
    sh.Zero();
    sh.Coefficients[2] = glm::vec3(5.0f, 6.0f, 7.0f);

    std::array<glm::vec4, SH_COEFFICIENT_COUNT> gpuData{};
    sh.ToGPULayout(gpuData, 1.0f);

    // .w should be zero for all coefficients except the first (validity flag)
    for (u32 i = 1; i < SH_COEFFICIENT_COUNT; ++i)
    {
        EXPECT_FLOAT_EQ(gpuData[i].w, 0.0f);
    }
}

// ── SH Basis function evaluation ──

TEST(SphericalHarmonicsTest, BasisFunctionDCTermIsConstant)
{
    // Y_0^0 (DC term) should be constant regardless of direction
    auto basisPosX = SHBasis::Evaluate(glm::vec3(1.0f, 0.0f, 0.0f));
    auto basisNegY = SHBasis::Evaluate(glm::vec3(0.0f, -1.0f, 0.0f));
    auto basisDiag = SHBasis::Evaluate(glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)));

    EXPECT_FLOAT_EQ(basisPosX[0], SHBasis::Y00);
    EXPECT_FLOAT_EQ(basisNegY[0], SHBasis::Y00);
    EXPECT_FLOAT_EQ(basisDiag[0], SHBasis::Y00);
}

TEST(SphericalHarmonicsTest, BasisFunctionLinearTermsMatchDirection)
{
    glm::vec3 dir = glm::normalize(glm::vec3(1.0f, 0.0f, 0.0f));
    auto basis = SHBasis::Evaluate(dir);

    // Y_1^{-1} ~ y, Y_1^0 ~ z, Y_1^1 ~ x
    EXPECT_FLOAT_EQ(basis[1], SHBasis::Y1n1 * dir.y); // y=0
    EXPECT_FLOAT_EQ(basis[2], SHBasis::Y10 * dir.z);  // z=0
    EXPECT_FLOAT_EQ(basis[3], SHBasis::Y11 * dir.x);  // x=1
}

TEST(SphericalHarmonicsTest, BasisFunctionOppositeDirections)
{
    glm::vec3 dirUp = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 dirDown = glm::vec3(0.0f, -1.0f, 0.0f);

    auto basisUp = SHBasis::Evaluate(dirUp);
    auto basisDown = SHBasis::Evaluate(dirDown);

    // DC term should be equal
    EXPECT_FLOAT_EQ(basisUp[0], basisDown[0]);

    // Linear terms should negate
    EXPECT_FLOAT_EQ(basisUp[1], -basisDown[1]); // Y component
}

// ── Irradiance evaluation ──

TEST(SphericalHarmonicsTest, ConstantLightProducesConstantIrradiance)
{
    // If all SH coefficients encode uniform white light (DC term only),
    // irradiance should be the same in every direction
    SHCoefficients sh;
    sh.Zero();
    sh.Coefficients[0] = glm::vec3(1.0f); // DC term

    glm::vec3 irr1 = SHBasis::EvaluateIrradiance(sh, glm::vec3(1, 0, 0));
    glm::vec3 irr2 = SHBasis::EvaluateIrradiance(sh, glm::vec3(0, 1, 0));
    glm::vec3 irr3 = SHBasis::EvaluateIrradiance(sh, glm::vec3(0, 0, 1));

    EXPECT_NEAR(irr1.x, irr2.x, 1e-5f);
    EXPECT_NEAR(irr1.x, irr3.x, 1e-5f);
    EXPECT_NEAR(irr1.y, irr2.y, 1e-5f);
    EXPECT_NEAR(irr1.z, irr2.z, 1e-5f);
}

TEST(SphericalHarmonicsTest, IrradianceIsNonNegative)
{
    // EvaluateIrradiance clamps to zero
    SHCoefficients sh;
    sh.Zero();
    sh.Coefficients[3] = glm::vec3(-100.0f); // Strong negative in one direction

    glm::vec3 irr = SHBasis::EvaluateIrradiance(sh, glm::vec3(1, 0, 0));
    EXPECT_GE(irr.x, 0.0f);
    EXPECT_GE(irr.y, 0.0f);
    EXPECT_GE(irr.z, 0.0f);
}

TEST(SphericalHarmonicsTest, DirectionalLightHigherInLitDirection)
{
    // Simulate light coming from +Y by setting SH coefficients with
    // a strong Y-direction linear term
    SHCoefficients sh;
    sh.Zero();
    sh.Coefficients[0] = glm::vec3(0.5f); // DC
    sh.Coefficients[1] = glm::vec3(1.0f); // Y-direction linear term

    glm::vec3 irrUp = SHBasis::EvaluateIrradiance(sh, glm::vec3(0, 1, 0));
    glm::vec3 irrDown = SHBasis::EvaluateIrradiance(sh, glm::vec3(0, -1, 0));

    EXPECT_GT(irrUp.x, irrDown.x);
}

// ── LightProbeVolumeComponent helpers ──

TEST(LightProbeVolumeComponentTest, TotalProbeCount)
{
    LightProbeVolumeComponent vol;
    vol.m_Resolution = glm::ivec3(4, 3, 2);
    EXPECT_EQ(vol.GetTotalProbeCount(), 24);
}

TEST(LightProbeVolumeComponentTest, TotalProbeCountSingle)
{
    LightProbeVolumeComponent vol;
    vol.m_Resolution = glm::ivec3(1, 1, 1);
    EXPECT_EQ(vol.GetTotalProbeCount(), 1);
}

TEST(LightProbeVolumeComponentTest, GridIndexLinearization)
{
    LightProbeVolumeComponent vol;
    vol.m_Resolution = glm::ivec3(4, 3, 2);

    // Index (0,0,0) should be 0
    EXPECT_EQ(vol.GridIndex(0, 0, 0), 0);

    // Index (1,0,0) should be 1
    EXPECT_EQ(vol.GridIndex(1, 0, 0), 1);

    // Index (0,1,0) should be dimX = 4
    EXPECT_EQ(vol.GridIndex(0, 1, 0), 4);

    // Index (0,0,1) should be dimX * dimY = 12
    EXPECT_EQ(vol.GridIndex(0, 0, 1), 12);

    // Last probe
    EXPECT_EQ(vol.GridIndex(3, 2, 1), 23);
}

TEST(LightProbeVolumeComponentTest, WorldToGridCorners)
{
    LightProbeVolumeComponent vol;
    vol.m_BoundsMin = glm::vec3(0.0f);
    vol.m_BoundsMax = glm::vec3(10.0f);
    vol.m_Resolution = glm::ivec3(3, 3, 3);

    // Min corner maps to grid (0,0,0)
    glm::vec3 gridMin = vol.WorldToGrid(glm::vec3(0.0f));
    EXPECT_NEAR(gridMin.x, 0.0f, 1e-5f);
    EXPECT_NEAR(gridMin.y, 0.0f, 1e-5f);
    EXPECT_NEAR(gridMin.z, 0.0f, 1e-5f);

    // Max corner maps to grid (2,2,2) for resolution 3
    glm::vec3 gridMax = vol.WorldToGrid(glm::vec3(10.0f));
    EXPECT_NEAR(gridMax.x, 2.0f, 1e-5f);
    EXPECT_NEAR(gridMax.y, 2.0f, 1e-5f);
    EXPECT_NEAR(gridMax.z, 2.0f, 1e-5f);

    // Center maps to grid (1,1,1)
    glm::vec3 gridCenter = vol.WorldToGrid(glm::vec3(5.0f));
    EXPECT_NEAR(gridCenter.x, 1.0f, 1e-5f);
    EXPECT_NEAR(gridCenter.y, 1.0f, 1e-5f);
    EXPECT_NEAR(gridCenter.z, 1.0f, 1e-5f);
}

// ── Size / layout static asserts (compile-time verification) ──

TEST(SphericalHarmonicsTest, SizeConstants)
{
    EXPECT_EQ(SH_COEFFICIENT_COUNT, 9u);
    EXPECT_EQ(SH_GPU_FLOATS_PER_PROBE, 36u);
    EXPECT_EQ(SH_GPU_BYTES_PER_PROBE, 144u);
    EXPECT_EQ(sizeof(SHCoefficients), 9u * sizeof(glm::vec3));
}

// ── Cubemap-to-SH projection (CPU path, used by IBL SH irradiance generation) ──
//
// LightProbeBaker::ProjectToSH is what IBLPrecompute::ProjectCubemapToSH calls
// after reading back the source environment cubemap. The math is independent
// of GPU readback, so these tests live here (no Renderer fixture needed) —
// the same routine is also reused by scene-baked light probes.

namespace
{
    // Build a synthetic cubemap pixel buffer in the same layout LightProbeBaker
    // expects: 6 faces (+X, -X, +Y, -Y, +Z, -Z), each row-major at `resolution`
    // texels per side, RGB packed as glm::vec3.
    std::vector<glm::vec3> BuildConstantCubemap(u32 resolution, glm::vec3 color)
    {
        std::vector<glm::vec3> pixels(static_cast<size_t>(6) * resolution * resolution, color);
        return pixels;
    }

    // Build a cubemap where the +Y face is bright (sky) and the others are dark.
    std::vector<glm::vec3> BuildSkyCubemap(u32 resolution, glm::vec3 skyColor, glm::vec3 floorColor)
    {
        std::vector<glm::vec3> pixels(static_cast<size_t>(6) * resolution * resolution, floorColor);
        const size_t faceTexels = static_cast<size_t>(resolution) * resolution;
        // Face index 2 = +Y in LightProbeBaker's face ordering.
        for (size_t i = 0; i < faceTexels; ++i)
        {
            pixels[2 * faceTexels + i] = skyColor;
        }
        return pixels;
    }
} // namespace

TEST(SHProjectionTest, ConstantCubemapProjectsToDCTermOnly)
{
    // A uniformly-lit cubemap must integrate to a DC-only SH expansion —
    // every higher-frequency basis function integrates to zero over the
    // sphere, so the projection wipes them out. If any L1/L2 coefficient
    // leaks meaningful energy, our solid-angle weights are wrong.
    constexpr u32 resolution = 16;
    const glm::vec3 color(0.5f, 0.7f, 0.9f);

    auto pixels = BuildConstantCubemap(resolution, color);
    SHCoefficients sh = LightProbeBaker::ProjectToSH(pixels, resolution);

    // DC term should encode the uniform irradiance: c_00 = color * 2*sqrt(pi).
    // After Y_0^0 = 1/(2*sqrt(pi)) basis weighting and 4*pi normalisation,
    // c_00 = color * 2*sqrt(pi) ≈ color * 3.5449.
    const f32 expectedDC = 2.0f * std::sqrt(glm::pi<f32>());
    EXPECT_NEAR(sh.Coefficients[0].x, color.x * expectedDC, 1e-3f);
    EXPECT_NEAR(sh.Coefficients[0].y, color.y * expectedDC, 1e-3f);
    EXPECT_NEAR(sh.Coefficients[0].z, color.z * expectedDC, 1e-3f);

    // All higher-order terms should be near-zero (slight bias from finite
    // resolution / discretisation; tolerance accommodates that without
    // letting genuine projection errors pass).
    for (u32 i = 1; i < SH_COEFFICIENT_COUNT; ++i)
    {
        EXPECT_NEAR(sh.Coefficients[i].x, 0.0f, 1e-2f) << "coefficient " << i << " (x)";
        EXPECT_NEAR(sh.Coefficients[i].y, 0.0f, 1e-2f) << "coefficient " << i << " (y)";
        EXPECT_NEAR(sh.Coefficients[i].z, 0.0f, 1e-2f) << "coefficient " << i << " (z)";
    }
}

TEST(SHProjectionTest, SkyCubemapProducesPositiveZenithIrradiance)
{
    // A "sky above, dark below" cubemap should reconstruct to higher
    // irradiance when sampled with a normal pointing up (+Y) than with one
    // pointing down (-Y). This catches sign / face-orientation bugs in the
    // texel-to-direction mapping inside ProjectToSH.
    constexpr u32 resolution = 16;
    const glm::vec3 skyColor(1.0f, 1.0f, 1.5f); // Bluish sky
    const glm::vec3 floorColor(0.05f);

    auto pixels = BuildSkyCubemap(resolution, skyColor, floorColor);
    SHCoefficients sh = LightProbeBaker::ProjectToSH(pixels, resolution);

    const glm::vec3 irrUp = SHBasis::EvaluateIrradiance(sh, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 irrDown = SHBasis::EvaluateIrradiance(sh, glm::vec3(0.0f, -1.0f, 0.0f));

    // Up should be visibly brighter than down — the linear band captures the
    // top/bottom asymmetry that the DC term alone cannot.
    EXPECT_GT(irrUp.x, irrDown.x + 0.1f) << "Up should reconstruct brighter than down";
    EXPECT_GT(irrUp.y, irrDown.y + 0.1f);
    EXPECT_GT(irrUp.z, irrDown.z + 0.1f);
}

TEST(SHProjectionTest, ProjectionScalesLinearlyWithInputIntensity)
{
    // SH projection is a linear operator. Doubling the input radiance must
    // double every output coefficient. This is the cheap way to catch any
    // accidentally introduced non-linearity (clamping, gamma, etc.) in the
    // projection path.
    constexpr u32 resolution = 8;
    const glm::vec3 base(0.3f, 0.6f, 0.4f);

    auto pixels1x = BuildConstantCubemap(resolution, base);
    auto pixels2x = BuildConstantCubemap(resolution, base * 2.0f);

    SHCoefficients sh1 = LightProbeBaker::ProjectToSH(pixels1x, resolution);
    SHCoefficients sh2 = LightProbeBaker::ProjectToSH(pixels2x, resolution);

    for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
    {
        EXPECT_NEAR(sh2.Coefficients[i].x, sh1.Coefficients[i].x * 2.0f, 1e-4f);
        EXPECT_NEAR(sh2.Coefficients[i].y, sh1.Coefficients[i].y * 2.0f, 1e-4f);
        EXPECT_NEAR(sh2.Coefficients[i].z, sh1.Coefficients[i].z * 2.0f, 1e-4f);
    }
}

TEST(SHProjectionTest, RejectsEmptyOrUndersizedInput)
{
    // Defensive: caller must not crash on bad inputs. The projection logs
    // an error and returns a zeroed SHCoefficients.
    std::vector<glm::vec3> empty;
    SHCoefficients sh = LightProbeBaker::ProjectToSH(empty, /*resolution=*/8);

    for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
    {
        EXPECT_FLOAT_EQ(sh.Coefficients[i].x, 0.0f);
        EXPECT_FLOAT_EQ(sh.Coefficients[i].y, 0.0f);
        EXPECT_FLOAT_EQ(sh.Coefficients[i].z, 0.0f);
    }
}

TEST(SHProjectionTest, ChannelAsymmetryIsPreservedThroughProjectionAndScaling)
{
    // Regression test for the grayscale-output bug.
    //
    // Symptom: the GPU-rendered SH irradiance cubemap came out R=G=B-exactly
    // for newport_loft.hdr even though the source HDR is full-color. The bug
    // hypothesis is that the CPU SH coefficients themselves collapse to
    // grayscale somewhere along projection → cosine-lobe scaling. This test
    // pins the CPU side: build a strongly colored cubemap, project, apply the
    // same cosine-lobe scaling used by IBLPrecompute::GenerateIrradianceMapFromSH,
    // and verify the output preserves channel asymmetry.
    constexpr u32 resolution = 16;
    // Pure red on +Y face, black elsewhere. The "right" SH expansion has
    // strong R contributions to DC (Coefficients[0].x) and L1-Y (Coefficients[1].x)
    // but ~zero G/B contributions everywhere.
    const glm::vec3 red(1.0f, 0.0f, 0.0f);
    const glm::vec3 black(0.0f);

    auto pixels = BuildSkyCubemap(resolution, red, black);
    SHCoefficients radianceSH = LightProbeBaker::ProjectToSH(pixels, resolution);

    // The raw projected coefficients should themselves be channel-asymmetric.
    // If R, G, B collapse here, every downstream consumer (CPU eval, GPU UBO)
    // sees grayscale.
    EXPECT_GT(radianceSH.Coefficients[0].x, 0.1f) << "DC R should carry the red sky";
    EXPECT_NEAR(radianceSH.Coefficients[0].y, 0.0f, 1e-3f) << "DC G should be ~zero (no green in source)";
    EXPECT_NEAR(radianceSH.Coefficients[0].z, 0.0f, 1e-3f) << "DC B should be ~zero (no blue in source)";

    // Apply the production cosine-lobe scaling.
    constexpr f32 kCosineLobeA0 = 1.0f;
    constexpr f32 kCosineLobeA1 = 2.0f / 3.0f;
    constexpr f32 kCosineLobeA2 = 1.0f / 4.0f;
    SHCoefficients irradianceSH = radianceSH;
    irradianceSH.Coefficients[0] *= kCosineLobeA0;
    for (u32 i = 1; i <= 3; ++i)
        irradianceSH.Coefficients[i] *= kCosineLobeA1;
    for (u32 i = 4; i <= 8; ++i)
        irradianceSH.Coefficients[i] *= kCosineLobeA2;

    // Evaluate at +Y (facing the red sky) — output should be predominantly red.
    const glm::vec3 irrUp = SHBasis::EvaluateIrradiance(irradianceSH, glm::vec3(0.0f, 1.0f, 0.0f));
    EXPECT_GT(irrUp.x, 0.1f) << "+Y irradiance should be visibly red";
    EXPECT_GT(irrUp.x, irrUp.y + 0.05f) << "R should dominate over G";
    EXPECT_GT(irrUp.x, irrUp.z + 0.05f) << "R should dominate over B";
}

TEST(SHProjectionTest, UniformWhiteAfterCosineLobeScalingYieldsUnity)
{
    // Regression test for the brightness convention.
    //
    // The production IrradianceConvolution.glsl shader outputs *normalised*
    // irradiance — for uniform-white L=1 the cubemap stores 1.0, NOT the raw
    // Lambertian integral π (see `PbrIrradianceTest.UniformWhiteYieldsNormalisedUnity`
    // for the convolution side). The SH path in
    // `IBLPrecompute::GenerateIrradianceMapFromSH` must match that convention,
    // so its cosine-lobe constants are (1, 2/3, 1/4) — the Ramamoorthi-Hanrahan
    // analytic values (π, 2π/3, π/4) divided by π.
    //
    // A previous version of the SH path used the raw constants and produced a
    // ~π× brighter irradiance map than the convolution path — visibly washing
    // out diffuse PBR surfaces in the editor. This test catches that regression
    // by replaying the exact CPU-side math on a uniform-white cubemap and
    // expecting SH-evaluation to come out at 1.0 (matching convolution).
    //
    // The constants are duplicated from `IBLPrecompute.cpp` rather than
    // exposed via a public helper; if either side changes, this test fails
    // and points the next reader at the call site.
    constexpr u32 resolution = 16;
    const glm::vec3 white(1.0f);

    auto pixels = BuildConstantCubemap(resolution, white);
    SHCoefficients radianceSH = LightProbeBaker::ProjectToSH(pixels, resolution);

    constexpr f32 kCosineLobeA0 = 1.0f;
    constexpr f32 kCosineLobeA1 = 2.0f / 3.0f;
    constexpr f32 kCosineLobeA2 = 1.0f / 4.0f;

    SHCoefficients irradianceSH = radianceSH;
    irradianceSH.Coefficients[0] *= kCosineLobeA0;
    for (u32 i = 1; i <= 3; ++i)
        irradianceSH.Coefficients[i] *= kCosineLobeA1;
    for (u32 i = 4; i <= 8; ++i)
        irradianceSH.Coefficients[i] *= kCosineLobeA2;

    // Evaluate at several normals. For uniform-white only the DC term matters
    // (others integrate to zero over the sphere), so the result must be
    // direction-independent and equal to the convolution path's 1.0.
    constexpr f32 kExpected = 1.0f;
    constexpr f32 kTolerance = 2e-2f;

    const glm::vec3 sampleDirs[] = {
        glm::vec3(1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f))
    };
    for (const auto& dir : sampleDirs)
    {
        const glm::vec3 irr = SHBasis::EvaluateIrradiance(irradianceSH, dir);
        EXPECT_NEAR(irr.x, kExpected, kTolerance) << "direction " << dir.x << "," << dir.y << "," << dir.z;
        EXPECT_NEAR(irr.y, kExpected, kTolerance);
        EXPECT_NEAR(irr.z, kExpected, kTolerance);
    }
}
