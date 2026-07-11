// OLO_TEST_LAYER: shaderpipe
//
// CPU mirrors of the froxel volumetric fog math introduced by issue #435
// (FroxelFogScatter.comp / FroxelFogIntegrate.comp / the froxel branch of
// PostProcess_Fog.glsl). Distance/height fog formulas are pinned by the
// sibling FogMathTest.cpp; the frame itself is pinned by
// VolumetricFogVisualEvidenceTest.cpp (L8). Here we pin, without a GL
// context:
//   - the fog volume's exponential depth-slice placement and its inverse
//     (the fragment's W-coordinate fetch) agreeing slice-for-slice,
//   - Hillaire's energy-conserving step integration (energy bounds,
//     transmittance monotonicity, homogeneous-medium closed form),
//   - the dual-lobe Henyey-Greenstein phase function (normalisation over the
//     sphere, forward-scatter dominance),
//   - the temporal reprojection's clip.w == view-depth identity that the
//     scatter pass relies on for the previous-frame slice coordinate.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    // Mirrors VolumetricFogPass's volume constants (kept literal here so a
    // silent constant change in the pass shows up as a test edit, not a
    // silent behavior shift).
    constexpr u32 kVolumeDepth = 64;

    // Mirrors FroxelFogScatter.comp's slice placement:
    //   viewDepth(z) = near * exp2(log2(far/near) * (z + 0.5)/D)   (centre)
    f32 SliceCentreDepth(u32 slice, u32 depthCount, f32 nearP, f32 farP)
    {
        const f32 logFN = std::log2(farP / nearP);
        return nearP * std::exp2(logFN * (static_cast<f32>(slice) + 0.5f) / static_cast<f32>(depthCount));
    }

    // Mirrors PostProcess_Fog.glsl's froxel fetch coordinate:
    //   w = clamp(log2(viewDepth/near) / log2(far/near), 0, 1)
    f32 FetchW(f32 viewDepth, f32 nearP, f32 farP)
    {
        const f32 logFN = std::log2(farP / nearP);
        return std::clamp(std::log2(std::max(viewDepth, nearP) / nearP) / logFN, 0.0f, 1.0f);
    }

    // Mirrors henyeyGreensteinPhase in FogCommon.glsl
    f32 HenyeyGreensteinPhase(f32 cosTheta, f32 g)
    {
        const f32 g2 = g * g;
        const f32 denom = 1.0f + g2 - 2.0f * g * cosTheta;
        return (1.0f - g2) / (4.0f * std::numbers::pi_v<f32> * denom * std::sqrt(denom));
    }

    // Mirrors dualLobeHGPhase in FroxelFogScatter.comp
    f32 DualLobeHGPhase(f32 cosTheta, f32 g)
    {
        const f32 forwardLobe = HenyeyGreensteinPhase(cosTheta, g);
        const f32 backLobe = HenyeyGreensteinPhase(cosTheta, -g * 0.3f);
        return std::lerp(backLobe, forwardLobe, 0.7f);
    }

    struct IntegrationResult
    {
        glm::vec3 Accumulated{ 0.0f };
        f32 Transmittance = 1.0f;
    };

    // Mirrors FroxelFogIntegrate.comp's per-slice Hillaire step
    IntegrationResult IntegrateSlices(const std::vector<glm::vec4>& scatterSlices,
                                      const std::vector<f32>& stepLengths)
    {
        IntegrationResult result;
        for (sizet i = 0; i < scatterSlices.size(); ++i)
        {
            const glm::vec3 inscatter(scatterSlices[i]);
            const f32 extinction = std::max(scatterSlices[i].w, 1e-5f);
            const f32 stepTransmittance = std::exp(-extinction * stepLengths[i]);
            const glm::vec3 integScatter = inscatter * (1.0f - stepTransmittance) / extinction;
            result.Accumulated += result.Transmittance * integScatter;
            result.Transmittance *= stepTransmittance;
        }
        return result;
    }
} // namespace

// =============================================================================
// Slice placement <-> fetch round trip
// =============================================================================

TEST(VolumetricFogMath, SliceCentreFetchesBackToOwnSlice)
{
    // The centre depth of every slice, run through the fragment's W formula,
    // must land inside that slice's W span [z/D, (z+1)/D] — writer (compute)
    // and reader (fragment) agree on the slicing.
    constexpr f32 nearP = 0.1f;
    constexpr f32 farP = 300.0f;

    for (u32 z = 0; z < kVolumeDepth; ++z)
    {
        const f32 depth = SliceCentreDepth(z, kVolumeDepth, nearP, farP);
        const f32 w = FetchW(depth, nearP, farP);
        const f32 sliceLo = static_cast<f32>(z) / kVolumeDepth;
        const f32 sliceHi = static_cast<f32>(z + 1) / kVolumeDepth;
        EXPECT_GE(w, sliceLo - 1e-5f) << "slice " << z;
        EXPECT_LE(w, sliceHi + 1e-5f) << "slice " << z;
    }
}

TEST(VolumetricFogMath, FetchWClampsOutsideVolume)
{
    constexpr f32 nearP = 0.1f;
    constexpr f32 farP = 300.0f;
    EXPECT_FLOAT_EQ(FetchW(0.01f, nearP, farP), 0.0f);   // closer than near
    EXPECT_FLOAT_EQ(FetchW(5000.0f, nearP, farP), 1.0f); // beyond far (sky)
    // Monotonic within range
    f32 prev = -1.0f;
    for (f32 d = nearP; d <= farP; d *= 1.4f)
    {
        const f32 w = FetchW(d, nearP, farP);
        EXPECT_GT(w, prev);
        prev = w;
    }
}

// =============================================================================
// Hillaire energy-conserving integration
// =============================================================================

TEST(VolumetricFogMath, TransmittanceDecreasesMonotonically)
{
    // Any positive extinction must strictly reduce transmittance per slice.
    std::vector<glm::vec4> slices(16, glm::vec4(0.5f, 0.5f, 0.5f, 0.2f));
    std::vector<f32> steps(16, 1.0f);

    f32 prevTransmittance = 1.0f;
    IntegrationResult running;
    for (sizet i = 0; i < slices.size(); ++i)
    {
        running = IntegrateSlices({ slices.begin(), slices.begin() + static_cast<std::ptrdiff_t>(i + 1) },
                                  { steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(i + 1) });
        EXPECT_LT(running.Transmittance, prevTransmittance);
        EXPECT_GT(running.Transmittance, 0.0f);
        prevTransmittance = running.Transmittance;
    }
}

TEST(VolumetricFogMath, HomogeneousMediumMatchesClosedForm)
{
    // For a homogeneous medium the accumulated in-scatter has the closed form
    //   L * (1 - exp(-sigma * totalLen)) / sigma    (with L = inscatter rate)
    // and transmittance exp(-sigma * totalLen). The slice-wise integration
    // must reproduce both to float accuracy — this is the property that makes
    // slice count a quality knob rather than a brightness knob.
    constexpr f32 sigma = 0.13f;
    const glm::vec3 inscatterRate(0.8f, 0.6f, 0.4f);
    constexpr f32 totalLen = 12.0f;
    constexpr u32 sliceCount = 48;

    std::vector<glm::vec4> slices(sliceCount, glm::vec4(inscatterRate, sigma));
    std::vector<f32> steps(sliceCount, totalLen / sliceCount);
    const auto result = IntegrateSlices(slices, steps);

    const f32 expectedTransmittance = std::exp(-sigma * totalLen);
    const glm::vec3 expectedAccumulated = inscatterRate * (1.0f - expectedTransmittance) / sigma;

    EXPECT_NEAR(result.Transmittance, expectedTransmittance, 1e-4f);
    EXPECT_NEAR(result.Accumulated.x, expectedAccumulated.x, 1e-3f);
    EXPECT_NEAR(result.Accumulated.y, expectedAccumulated.y, 1e-3f);
    EXPECT_NEAR(result.Accumulated.z, expectedAccumulated.z, 1e-3f);
}

TEST(VolumetricFogMath, ZeroDensityIsIdentity)
{
    // Empty froxels must contribute nothing and leave transmittance at 1
    // (extinction floors at 1e-5, so the step is ~identity, not a divide-by-0).
    std::vector<glm::vec4> slices(32, glm::vec4(0.0f));
    std::vector<f32> steps(32, 2.0f);
    const auto result = IntegrateSlices(slices, steps);
    EXPECT_NEAR(result.Transmittance, 1.0f, 1e-3f);
    EXPECT_NEAR(result.Accumulated.x, 0.0f, 1e-5f);
}

TEST(VolumetricFogMath, IntegrationIsEnergyBounded)
{
    // The accumulated in-scatter of a medium emitting L per unit density can
    // never exceed L/sigma * (1 - finalTransmittance) — no step formulation
    // may create energy.
    constexpr f32 sigma = 0.4f;
    const glm::vec3 rate(2.0f);
    std::vector<glm::vec4> slices(64, glm::vec4(rate, sigma));
    std::vector<f32> steps(64, 0.7f);
    const auto result = IntegrateSlices(slices, steps);

    const glm::vec3 bound = rate / sigma * (1.0f - result.Transmittance);
    EXPECT_LE(result.Accumulated.x, bound.x + 1e-4f);
    EXPECT_GE(result.Transmittance, 0.0f);
    EXPECT_LE(result.Transmittance, 1.0f);
}

// =============================================================================
// Phase functions
// =============================================================================

TEST(VolumetricFogMath, HenyeyGreensteinIntegratesToOneOverSphere)
{
    // ∫ phase dΩ = 2π ∫ phase(cosθ) sinθ dθ must equal 1 for any g — the
    // phase function redistributes energy, never creates it. Numeric
    // integration over 4096 steps pins the (1/4π) normalisation constant.
    for (const f32 g : { 0.0f, 0.3f, 0.76f, -0.4f })
    {
        constexpr int kSteps = 4096;
        f64 integral = 0.0;
        for (int i = 0; i < kSteps; ++i)
        {
            const f64 theta = (static_cast<f64>(i) + 0.5) / kSteps * std::numbers::pi;
            const f64 cosTheta = std::cos(theta);
            const f64 phase = HenyeyGreensteinPhase(static_cast<f32>(cosTheta), g);
            integral += 2.0 * std::numbers::pi * phase * std::sin(theta) * (std::numbers::pi / kSteps);
        }
        EXPECT_NEAR(integral, 1.0, 5e-3) << "g = " << g;
    }
}

TEST(VolumetricFogMath, DualLobePhaseFavoursForwardScattering)
{
    // With a positive anisotropy the blended dual-lobe phase must still put
    // more energy in the forward direction (cosθ = 1) than backward — the
    // property that makes fog glow around lights you look toward.
    constexpr f32 g = 0.6f;
    EXPECT_GT(DualLobeHGPhase(1.0f, g), DualLobeHGPhase(-1.0f, g));
    EXPECT_GT(DualLobeHGPhase(1.0f, g), DualLobeHGPhase(0.0f, g));
    // And it stays finite/positive everywhere.
    for (f32 c = -1.0f; c <= 1.0f; c += 0.125f)
    {
        const f32 p = DualLobeHGPhase(c, g);
        EXPECT_TRUE(std::isfinite(p));
        EXPECT_GT(p, 0.0f);
    }
}

// =============================================================================
// Temporal reprojection support identity
// =============================================================================

TEST(VolumetricFogMath, PerspectiveClipWEqualsViewDepth)
{
    // FroxelFogScatter.comp derives the PREVIOUS frame's slice coordinate
    // from prevClip.w, relying on the GL perspective convention that clip.w
    // equals the positive view-space depth. Pin that identity for a
    // representative projection + a sweep of view positions.
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);

    for (const f32 depth : { 0.2f, 1.0f, 10.0f, 100.0f, 400.0f })
    {
        const glm::vec4 viewPos(3.0f, -2.0f, -depth, 1.0f); // forward is -Z
        const glm::vec4 clip = proj * viewPos;
        EXPECT_NEAR(clip.w, depth, depth * 1e-5f);
    }
}

// =============================================================================
// Scatter-shader phase call-site convention
// =============================================================================

TEST(VolumetricFogMath, ScatterShaderUsesForwardPhaseConventionForClusterLights)
{
    // The dual-lobe HG phase itself is pinned above; this anchors the CALL
    // SITE in FroxelFogScatter.comp. With viewDir = camera->froxel and
    // L = froxel->light, the scattering cosine is dot(incident travel,
    // outgoing direction) = dot(-L, -viewDir) = dot(viewDir, L) - so the
    // forward lobe peaks when the camera looks TOWARD a fog-shrouded light.
    // The negated form dot(viewDir, -L) shipped once during #435 review and
    // put the anisotropic halo on the wrong side of every clustered light
    // (the sun tap's dot(viewDir, -sunDir) is NOT the same convention:
    // sunDir there is the sun's travel direction, not a froxel->light vector).
    namespace fs = std::filesystem;
    fs::path shaderPath;
    for (const auto& candidate : { fs::path("OloEditor/assets/shaders"),
                                   fs::current_path() / "OloEditor/assets/shaders",
                                   fs::current_path().parent_path() / "OloEditor/assets/shaders",
                                   fs::path("assets/shaders") })
    {
        if (fs::exists(candidate / "compute" / "FroxelFogScatter.comp"))
        {
            shaderPath = candidate / "compute" / "FroxelFogScatter.comp";
            break;
        }
    }
    ASSERT_FALSE(shaderPath.empty()) << "FroxelFogScatter.comp not found from cwd " << fs::current_path();

    std::ifstream in(shaderPath);
    ASSERT_TRUE(in.is_open());
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();

    EXPECT_NE(source.find("dualLobeHGPhase(dot(viewDir, L)"), std::string::npos)
        << "cluster-light phase call site changed - re-derive the scattering "
           "cosine convention before accepting (forward lobe must face the light)";
    EXPECT_EQ(source.find("dualLobeHGPhase(dot(viewDir, -L)"), std::string::npos)
        << "cluster-light phase cosine is negated - the anisotropic halo "
           "renders on the wrong side of every clustered light";
}
