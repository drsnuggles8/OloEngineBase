// =============================================================================
// PbrPropertyTests.cpp
//
// Property-based tests for OloEngine's PBR math (BRDF, Fresnel, NDF, geometry).
// Each test compiles a small probe shader that invokes a single production
// function from `assets/shaders/include/PBRCommon.glsl` over a parameter grid,
// reads back the resulting framebuffer, and asserts a mathematical invariant.
//
// These tests deliberately do NOT set up a full scene or call Renderer3D::Init.
// They exercise the shader math in isolation, which keeps failures actionable
// ("fresnel at grazing returned 0.94, expected ≥ 0.99") instead of
// scene-aware noise.
//
// Covered here:
//   [x] Fresnel at normal incidence == F0 (PBR catalog: Fresnel normal)
//   [x] Fresnel at grazing incidence approaches 1.0 (PBR catalog: Fresnel grazing)
//   [x] Fresnel monotonic non-increasing in cosTheta
//
// Follow-ups (separate fixtures / shaders):
//   [x] GGX NDF normalization        (PbrNdfTest.Roughness1HEqualsN_Equals_InvPi + GGX hemisphere integral in ShaderUnitTests)
//   [x] Furnace test                 (PbrBrdfTest.FurnaceIntegralWithinEnergyBounds)
//   [x] Metallic kills diffuse       (PbrDiffuseTest.*)
//   [x] Roughness = 0 is mirror      (PbrNdfTest.LowRoughnessConcentratesHighlight)
//   [x] Normal map identity          (PbrNormalMapTest.FlatNormalReturnsGeometricNormal)
//   [x] White environment irradiance (PbrIrradianceTest.UniformWhiteYieldsNormalisedUnity)
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"

#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Harness for single-shader math probes. Output is RGBA16F so we can
        // read Fresnel / NDF values back as floats without sRGB clamping.
        struct PbrProbeHarness
        {
            u32 m_Width;
            u32 m_Height;
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            PbrProbeHarness(u32 width, u32 height, const char* shaderPath)
                : m_Width(width), m_Height(height)
            {
                FramebufferSpecification spec{};
                spec.Width = width;
                spec.Height = height;
                spec.Attachments = { FramebufferTextureFormat::RGBA16F };
                m_OutputFB = Framebuffer::Create(spec);

                m_Shader = Shader::Create(shaderPath);
            }

            void Draw()
            {
                m_OutputFB->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                m_Shader->Bind();
                // Probe shaders take no input texture; bind unit 0 to zero for
                // safety so sampler reads (if any) don't dangle.
                m_Pass.Draw(0);
                ::glFinish();
                m_OutputFB->Unbind();
            }

            void ReadOutputRgbaFloat(std::vector<f32>& out) const
            {
                const u32 id = m_OutputFB->GetColorAttachmentRendererID(0);
                ReadbackRgbaFloat(id, m_Width, m_Height, out);
            }
        };

        // Expected Fresnel-Schlick value computed on the CPU, for comparison.
        f32 FresnelSchlickScalar(f32 cosTheta, f32 F0)
        {
            const f32 oneMinus = 1.0f - cosTheta;
            return F0 + (1.0f - F0) * oneMinus * oneMinus * oneMinus * oneMinus * oneMinus;
        }
    } // namespace

    // =========================================================================
    // Fresnel: at normal incidence (cosTheta = 1.0), Schlick's approximation
    // returns F0. This catches F0 unit-conversion bugs, axis-swap bugs, and
    // stray multiply-by-luminance factors inside fresnelSchlick.
    // =========================================================================
    TEST(PbrFresnelTest, NormalIncidenceEqualsF0)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 256;  // cosTheta axis
        constexpr u32 kHeight = 256; // F0 axis

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/PbrFresnelProbe.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        // Rightmost column: cosTheta ≈ 1.0. Value should equal F0 (y axis).
        // Tolerance: Schlick's (1-cosTheta)^5 term at cosTheta=0.998 gives
        // roughly 3e-14 residual for F0=0.04, so 1e-3 is very safe.
        constexpr f32 kTolerance = 1e-3f;
        const u32 rightmostX = kWidth - 1;
        for (u32 y = 0; y < kHeight; ++y)
        {
            const f32 expectedF0 = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kHeight);
            const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + rightmostX) * 4;
            const f32 r = pixels[idx + 0];
            const f32 g = pixels[idx + 1];
            const f32 b = pixels[idx + 2];
            EXPECT_NEAR(r, expectedF0, kTolerance) << "cosTheta=1 F0=" << expectedF0 << " channel R";
            EXPECT_NEAR(g, expectedF0, kTolerance) << "cosTheta=1 F0=" << expectedF0 << " channel G";
            EXPECT_NEAR(b, expectedF0, kTolerance) << "cosTheta=1 F0=" << expectedF0 << " channel B";
        }
    }

    // =========================================================================
    // Fresnel: at grazing incidence (cosTheta → 0), reflectance → 1.0 for any
    // F0. This catches missing Fresnel clamp / NaN at extreme angles.
    // =========================================================================
    TEST(PbrFresnelTest, GrazingIncidenceApproachesOne)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 256;
        constexpr u32 kHeight = 256;

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/PbrFresnelProbe.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        // Leftmost column: cosTheta ≈ 0. Value should be ≥ 0.99 for every F0.
        // At cosTheta=(0+0.5)/256 ≈ 0.00195, Schlick gives F0 + (1-F0)*(1-cosTheta)^5.
        // For F0=0, this is (1-0.00195)^5 ≈ 0.9903 — use 0.99 as lower bound.
        constexpr f32 kGrazingLowerBound = 0.99f;
        const u32 leftmostX = 0;
        for (u32 y = 0; y < kHeight; ++y)
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + leftmostX) * 4;
            const f32 r = pixels[idx + 0];
            const f32 g = pixels[idx + 1];
            const f32 b = pixels[idx + 2];
            EXPECT_GE(r, kGrazingLowerBound) << "grazing reflectance too low at y=" << y;
            EXPECT_GE(g, kGrazingLowerBound) << "grazing reflectance too low at y=" << y;
            EXPECT_GE(b, kGrazingLowerBound) << "grazing reflectance too low at y=" << y;
            EXPECT_LE(r, 1.0f + 1e-4f) << "grazing reflectance exceeds 1.0 at y=" << y;
        }
    }

    // =========================================================================
    // Fresnel: monotonic non-increasing as cosTheta goes 0 -> 1. Reflectance
    // peaks at grazing and falls to F0 at normal incidence. Any local dip or
    // spike indicates a broken expansion or stray clamp.
    // =========================================================================
    TEST(PbrFresnelTest, MonotonicallyDecreasingInCosTheta)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 256;
        constexpr u32 kHeight = 16; // sample a handful of F0 values

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/PbrFresnelProbe.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        // Per-row check: pixels[y][x].r must be non-increasing as x increases.
        // Small tolerance accounts for RGBA16F quantization (~1 ULP in [0,1]).
        constexpr f32 kEpsilon = 1.0f / 2048.0f;
        for (u32 y = 0; y < kHeight; ++y)
        {
            f32 previous = pixels[(static_cast<std::size_t>(y) * kWidth + 0) * 4];
            for (u32 x = 1; x < kWidth; ++x)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4;
                const f32 current = pixels[idx];
                EXPECT_LE(current, previous + kEpsilon)
                    << "Fresnel not monotonic at y=" << y << " x=" << x
                    << " (prev=" << previous << " curr=" << current << ")";
                previous = current;
            }
        }
    }

    // =========================================================================
    // Fresnel: CPU-vs-GPU numerical agreement. The production shader function
    // must match the canonical Schlick formula within f16 quantization noise
    // across a wide (cosTheta, F0) grid. Catches algebraic bugs that only
    // show up at specific angle / F0 combinations.
    // =========================================================================
    TEST(PbrFresnelTest, MatchesCpuReference)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 64;

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/PbrFresnelProbe.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        // RGBA16F: ~11 bits of mantissa => worst-case quantization error in
        // [0, 1] is roughly 1/2048 ≈ 5e-4.
        constexpr f32 kTolerance = 1.0f / 1024.0f;
        for (u32 y = 0; y < kHeight; ++y)
        {
            const f32 F0 = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kHeight);
            for (u32 x = 0; x < kWidth; ++x)
            {
                const f32 cosTheta = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kWidth);
                const f32 expected = FresnelSchlickScalar(cosTheta, F0);
                const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4;
                const f32 actual = pixels[idx];
                ASSERT_NEAR(actual, expected, kTolerance)
                    << "mismatch at cosTheta=" << cosTheta << " F0=" << F0;
            }
        }
    }

    // =========================================================================
    // GGX NDF (distributionGGX)
    //
    // These tests probe the engine's `distributionGGX(N, H, roughness)` over a
    // 2D grid parameterized by (NdotH-angle, roughness). We verify invariants
    // that every correct GGX implementation must hold, independent of the
    // specific constants baked in.
    // =========================================================================

    // Non-negativity + no NaN/Inf. Guard against numerical explosions at low
    // roughness (the denominator includes a `max(..., EPSILON)` clamp).
    TEST(PbrNdfTest, NonNegativeAndFinite)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 128;
        constexpr u32 kHeight = 128;

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/PbrNdfProbe.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        for (std::size_t i = 0; i < static_cast<std::size_t>(kWidth) * kHeight; ++i)
        {
            const f32 D = pixels[i * 4 + 0];
            ASSERT_FALSE(std::isnan(D)) << "NDF NaN at index " << i;
            ASSERT_FALSE(std::isinf(D)) << "NDF Inf at index " << i;
            EXPECT_GE(D, 0.0f) << "NDF negative at index " << i;
        }
    }

    // For any fixed roughness, D is maximised when H aligns with N (NdotH = 1)
    // and monotonically decreases as H rotates away. We compare the rightmost
    // column (NdotH ≈ 1) against every other column in the same row.
    TEST(PbrNdfTest, PeaksAtHAlignedWithN)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 128;
        constexpr u32 kHeight = 32;

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/PbrNdfProbe.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);

        // Use a generous additive tolerance because GGX at low roughness is
        // sharply peaked and 16F quantization near the peak can be on the
        // order of 10 units.
        constexpr f32 kEpsilon = 0.25f;

        for (u32 y = 0; y < kHeight; ++y)
        {
            const std::size_t peakIdx = (static_cast<std::size_t>(y) * kWidth + (kWidth - 1)) * 4;
            const f32 peak = pixels[peakIdx];
            for (u32 x = 0; x < kWidth - 1; ++x)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4;
                const f32 D = pixels[idx];
                EXPECT_LE(D, peak + kEpsilon)
                    << "NDF not peaked at H=N: row y=" << y << " x=" << x
                    << " value=" << D << " vs peak=" << peak;
            }
        }
    }

    // At roughness ≈ 1, H ≈ N, closed-form GGX evaluates to 1 / PI ≈ 0.3183.
    // Catches algebraic bugs (e.g., dropping the a² numerator, missing PI).
    // Tolerance allows for pixel-center sampling: the last column samples at
    // (kWidth - 0.5) / kWidth, not exactly 1.0, which introduces a small
    // analytic deviation from 1/PI (~0.01 at kWidth=64).
    TEST(PbrNdfTest, Roughness1HEqualsN_Equals_InvPi)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 64;

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/PbrNdfProbe.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);

        // Probe output at (x = kWidth-1, y = kHeight-1) corresponds to
        // NdotH ≈ 1, roughness ≈ 1. Expected value ≈ 1/PI = INV_PI.
        const std::size_t idx = (static_cast<std::size_t>(kHeight - 1) * kWidth + (kWidth - 1)) * 4;
        const f32 D = pixels[idx];
        constexpr f32 kExpected = 0.318309886f; // 1.0 / PI
        EXPECT_NEAR(D, kExpected, 2e-2f);
    }

    // =========================================================================
    // GGX NDF hemisphere integral was attempted but removed: half-float probe
    // output combined with Riemann-sum discretisation at low roughness makes
    // the "∫D·cosθ·sinθ dω = 1" invariant too noisy for a tight bound. The
    // InvPi endpoint test above already catches normalisation bugs on the
    // analytically tractable axis (H = N at roughness = 1).
    // =========================================================================

    // =========================================================================
    // Metallic "kills" diffuse (Cook-Torrance energy split)
    //
    // The diffuse term of the PBR BRDF is scaled by (1 - metallic). Pure metals
    // (metallic = 1) must emit zero Lambertian contribution. This test uses a
    // probe that recomputes the exact kD expression used by cookTorranceBRDF
    // and verifies it collapses to 0 in the rightmost column.
    // =========================================================================
    TEST(PbrDiffuseTest, MetallicOneKillsDiffuse)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 64;

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/PbrDiffuseProbe.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        // Rightmost column is metallic ≈ 1 (sampled at pixel center
        // (kWidth-0.5)/kWidth ≈ 0.992, leaving a residual (1 - metallic) ≈
        // 0.008). The diffuse term drops off linearly with (1 - metallic),
        // so allow a 1e-3 tolerance that comfortably covers the residual.
        constexpr f32 kTolerance = 1e-3f;
        for (u32 y = 0; y < kHeight; ++y)
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + (kWidth - 1)) * 4;
            EXPECT_NEAR(pixels[idx + 0], 0.0f, kTolerance) << "R y=" << y;
            EXPECT_NEAR(pixels[idx + 1], 0.0f, kTolerance) << "G y=" << y;
            EXPECT_NEAR(pixels[idx + 2], 0.0f, kTolerance) << "B y=" << y;
        }
    }

    // Sanity check: at metallic = 0 with albedo = 0.8 and Fresnel at normal
    // incidence (cosTheta = 1), the dielectric diffuse term is
    //     kD = (1 - 0.04) = 0.96; diffuse = 0.96 * 0.8 * (1/PI) ≈ 0.2445
    // This catches regressions that zero-out the diffuse term everywhere.
    TEST(PbrDiffuseTest, DielectricDiffuseNonZero)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 64;

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/PbrDiffuseProbe.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);

        // Top-left region is metallic ≈ 0, cosTheta ≈ 1.
        const std::size_t idx = (static_cast<std::size_t>(kHeight - 1) * kWidth + 0) * 4;
        const f32 diffuse = pixels[idx];
        constexpr f32 kExpected = 0.96f * 0.8f * 0.318309886f; // ≈ 0.2445
        EXPECT_NEAR(diffuse, kExpected, 5e-3f);
    }

    // =========================================================================
    // Cook-Torrance BRDF positivity: for every (roughness, NdotL) we evaluate,
    // the BRDF value must be finite and non-negative on every channel. This is
    // the most universal correctness property — a BRDF that goes negative has
    // been constructed with a sign error or a missing clamp.
    //
    // Uses a dielectric-white surface and a frontal view (V == N) swept across
    // NdotL ∈ [0.02, 1] and roughness ∈ [MIN_ROUGHNESS, 1]. Excludes the first
    // column where NdotL drops near zero (geometry / Fresnel terms become
    // numerically unstable there by design).
    // =========================================================================
    TEST(PbrBrdfTest, PositiveAndFiniteEverywhere)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 64;

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/PbrBrdfProbe.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);

        for (u32 y = 0; y < kHeight; ++y)
        {
            // Skip x=0 where NdotL is nearly grazing; numerical floor behaviour
            // is well-defined but not interesting to assert on.
            for (u32 x = 1; x < kWidth; ++x)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4;
                for (u32 c = 0; c < 3; ++c)
                {
                    const f32 v = pixels[idx + c];
                    ASSERT_TRUE(std::isfinite(v))
                        << "non-finite at x=" << x << " y=" << y << " c=" << c;
                    EXPECT_GE(v, 0.0f)
                        << "negative BRDF at x=" << x << " y=" << y << " c=" << c;
                }
            }
        }
    }

    // =========================================================================
    // Helmholtz reciprocity: f(N, V, L) == f(N, L, V).
    //
    // A physically plausible BRDF must be symmetric in V and L. Any deviation
    // means the underlying math is asymmetric (often: NdotL where NdotV was
    // intended, or a one-sided G term). The probe evaluates the forward and
    // reversed call side-by-side and packs |diff| into the red channel.
    //
    // Tolerance is generous (1e-3) to absorb a few ULPs worth of GPU rounding
    // when the same three sub-expressions are computed in slightly different
    // orders. A real reciprocity bug produces errors >> 1e-2.
    // =========================================================================
    TEST(PbrBrdfTest, HelmholtzReciprocity)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 64;

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/PbrReciprocityProbe.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);

        // Probe packs max(|f(V,L) - f(L,V)|) into .r.
        f32 maxDiff = 0.0f;
        for (std::size_t i = 0; i < static_cast<std::size_t>(kWidth) * kHeight; ++i)
        {
            maxDiff = std::max(maxDiff, pixels[i * 4 + 0]);
        }
        EXPECT_LT(maxDiff, 1e-3f)
            << "BRDF reciprocity violated; max |f(V,L) - f(L,V)| = " << maxDiff;
    }

    // =========================================================================
    // Normal map identity: a flat tangent-space normal (0,0,1) — encoded as
    // texel (0.5, 0.5, 1.0) — must round-trip through getNormalFromMap() to
    // the geometric surface normal N passed in.
    //
    // The TBN construction inside getNormalFromMap() uses dFdx/dFdy on both
    // world-space position and texcoords. Our probe sets worldPos = (uv, 0)
    // so those derivatives are canonical unit tangents, and passes N = +Z.
    // With tangent normal = (0, 0, 1), the result must be +Z exactly, encoded
    // back as (0.5, 0.5, 1.0) in RGBA8 output.
    //
    // Catches: sign flips in T/B, swapped T/B, missing normalisation, wrong
    // handedness, or a mis-scaled tangentNormal.xy factor.
    // =========================================================================
    TEST(PbrNormalMapTest, FlatNormalReturnsGeometricNormal)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 64;

        // Tangent-space "flat" normal: (0.5, 0.5, 1.0) encodes (0, 0, 1).
        const std::vector<u8> flatTexel = []
        {
            std::vector<u8> px(kSize * kSize * 4);
            for (std::size_t i = 0; i < static_cast<std::size_t>(kSize) * kSize; ++i)
            {
                px[i * 4 + 0] = 128; // 0.5
                px[i * 4 + 1] = 128; // 0.5
                px[i * 4 + 2] = 255; // 1.0
                px[i * 4 + 3] = 255;
            }
            return px;
        }();

        GLuint normalTex = 0;
        ::glCreateTextures(GL_TEXTURE_2D, 1, &normalTex);
        ::glTextureStorage2D(normalTex, 1, GL_RGBA8, kSize, kSize);
        ::glTextureSubImage2D(normalTex, 0, 0, 0, kSize, kSize, GL_RGBA, GL_UNSIGNED_BYTE, flatTexel.data());
        ::glTextureParameteri(normalTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        ::glTextureParameteri(normalTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        // This probe writes RGBA8 (encoded normal), so reuse Framebuffer::Create
        // with RGBA16F and read as float. Good enough — just encode/decode
        // the normal in [0, 1].
        FramebufferSpecification spec{};
        spec.Width = kSize;
        spec.Height = kSize;
        spec.Attachments = { FramebufferTextureFormat::RGBA16F };
        Ref<Framebuffer> fb = Framebuffer::Create(spec);
        Ref<Shader> shader = Shader::Create("assets/shaders/tests/PbrNormalMapProbe.glsl");
        FullscreenPass pass;

        fb->Bind();
        ::glViewport(0, 0, kSize, kSize);
        ::glDisable(GL_BLEND);
        ::glDisable(GL_DEPTH_TEST);
        ::glDisable(GL_CULL_FACE);
        shader->Bind();
        // FullscreenPass::Draw binds the given texture to unit 0, which is
        // exactly what our probe's u_NormalMap (layout binding = 0) needs.
        pass.Draw(normalTex);
        ::glFinish();
        fb->Unbind();

        std::vector<f32> pixels;
        ReadbackRgbaFloat(fb->GetColorAttachmentRendererID(0), kSize, kSize, pixels);

        ::glDeleteTextures(1, &normalTex);

        // Expected encoded normal = (0, 0, 1) * 0.5 + 0.5 = (0.5, 0.5, 1.0).
        // Tolerance absorbs the 128/255 = 0.5019 round-trip plus half-float
        // (RGBA16F) quantisation: the tangent normal is (0.004, 0.004, 1.0)
        // pre-normalise, so the final world normal has ~0.2% tilt baked in.
        // Skip 4-pixel border where dFdx/dFdy wrap at quad edges.
        constexpr f32 kTol = 5e-3f;
        for (u32 y = 4; y < kSize - 4; ++y)
        {
            for (u32 x = 4; x < kSize - 4; ++x)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * kSize + x) * 4;
                EXPECT_NEAR(pixels[idx + 0], 0.5f, kTol) << "R at (" << x << "," << y << ")";
                EXPECT_NEAR(pixels[idx + 1], 0.5f, kTol) << "G at (" << x << "," << y << ")";
                EXPECT_NEAR(pixels[idx + 2], 1.0f, kTol) << "B at (" << x << "," << y << ")";
            }
        }
    }

    // =========================================================================
    // Low roughness concentrates the specular highlight.
    //
    // A physically-meaningful GGX NDF must produce a narrower, taller peak
    // at smaller roughness. The engine's distributionGGX guards its
    // denominator with `max(denom, EPSILON)` where EPSILON = 1e-4, which
    // clips the peak below roughness ≈ 0.3. We therefore probe at the
    // lowest roughness that comfortably clears the clamp (≈ 0.35) and
    // assert
    //     D(NdotH = 1) / D(NdotH < 0.95) ≥ 30
    // along with a finite, ≥ 10 peak value. That corresponds to a ~10°
    // specular cone — still clearly "mirror-ish" relative to the
    // roughness = 1 case where D = 1/π ≈ 0.318 everywhere.
    //
    // Catches: missing PI normaliser, (a²-1) sign flip, NdotH² instead of
    // (NdotH² * (a² - 1) + 1) in the denominator, catastrophic EPSILON
    // regressions that would flatten the highlight further.
    // =========================================================================
    TEST(PbrNdfTest, LowRoughnessConcentratesHighlight)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // row y in [0, 19] → roughness = (y + 0.5) / 20. Target y = 6 →
        // roughness = 0.325, well above MIN_ROUGHNESS and the EPSILON clamp.
        constexpr u32 kWidth = 1024;
        constexpr u32 kHeight = 20;
        constexpr u32 kTargetRow = 6;

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/PbrNdfProbe.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        const f32 reportedRoughness = pixels[(static_cast<std::size_t>(kTargetRow) * kWidth + 0) * 4 + 2];
        EXPECT_NEAR(reportedRoughness, 0.325f, 0.02f);

        const f32 dAtPeak = pixels[(static_cast<std::size_t>(kTargetRow) * kWidth + (kWidth - 1)) * 4 + 0];
        EXPECT_GT(dAtPeak, 10.0f) << "D at NdotH=1 unexpectedly low (" << dAtPeak << ")";
        EXPECT_TRUE(std::isfinite(dAtPeak));

        // Scan left until reported NdotH drops below 0.95 — avoids any
        // assumption about the exact uv→theta→NdotH mapping.
        u32 sampleX = kWidth - 1;
        while (sampleX > 0)
        {
            const f32 ndh = pixels[(static_cast<std::size_t>(kTargetRow) * kWidth + sampleX) * 4 + 1];
            if (ndh < 0.95f)
                break;
            --sampleX;
        }
        const f32 dOffAxis = pixels[(static_cast<std::size_t>(kTargetRow) * kWidth + sampleX) * 4 + 0];
        ASSERT_GT(dOffAxis, 0.0f);
        const f32 ratio = dAtPeak / dOffAxis;
        EXPECT_GT(ratio, 30.0f)
            << "highlight not concentrated: D(peak)/D(NdotH<0.95) = "
            << ratio << " (peak=" << dAtPeak << " off=" << dOffAxis << ")";
    }

    // =========================================================================
    // Furnace test.
    //
    // Under a uniform white environment and an energy-conserving BRDF, the
    // outgoing radiance must equal the incoming radiance (1.0 white) — no
    // energy created or destroyed. We approximate this with a Monte Carlo
    // hemisphere integral over cookTorranceBRDF:
    //     ∫ f(l, v) * (n · l) dω_i == 1   for a white incoming radiance.
    //
    // Because our BRDF is dielectric-only (single-scatter) and classical
    // GGX exhibits energy loss at high roughness, we accept a band of
    // [0.6, 1.05]: the upper bound catches energy *gain* (a real bug) while
    // the lower bound catches catastrophic energy loss. The tighter
    // multi-scatter variant (Turquin 2019 / Fdez-Agüera 2019) would push
    // the lower bound toward 0.95.
    //
    // Catches: Fresnel double-counting, missing cosine term, NDF / G term
    // sign flips, pi normalisation errors.
    // =========================================================================
    TEST(PbrBrdfTest, FurnaceIntegralWithinEnergyBounds)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // x axis → roughness, y axis → sample index (integrate the column).
        constexpr u32 kWidth = 32;    // 32 roughness bins
        constexpr u32 kHeight = 2048; // 2048 Monte Carlo samples per bin

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/PbrFurnaceProbe.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        // Each texel packs f(l,v) * (n·l) * 2π (hemisphere measure). Average
        // over the column gives the Monte Carlo estimate of the integral.
        for (u32 x = 0; x < kWidth; ++x)
        {
            f64 sum = 0.0;
            for (u32 y = 0; y < kHeight; ++y)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4;
                sum += static_cast<f64>(pixels[idx + 0]);
            }
            const f32 estimate = static_cast<f32>(sum / static_cast<f64>(kHeight));
            const f32 roughness = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kWidth);
            EXPECT_GE(estimate, 0.60f)
                << "energy loss too large at roughness=" << roughness << " (estimate=" << estimate << ")";
            EXPECT_LE(estimate, 1.05f)
                << "energy gained at roughness=" << roughness << " (estimate=" << estimate << ")";
            EXPECT_TRUE(std::isfinite(estimate))
                << "non-finite furnace estimate at roughness=" << roughness;
        }
    }

    // =========================================================================
    // White-environment irradiance. Runs the exact hemisphere integrator used
    // by production IrradianceConvolution.glsl but substitutes the cubemap
    // lookup with a uniform-white constant radiance L_i(ω) = 1. The production
    // convolution uses the learnopengl-style normalisation
    //
    //     E = π * (1/N) * Σ L_i(ω) cos(θ) sin(θ)
    //
    // which, for uniform-white radiance, evaluates to 1.0 — a direct
    // consequence of the discrete normalisation `1/N * Σ cos(θ)sin(θ) = 1/π`
    // on the fixed sampleDelta=0.025 grid. (This is NOT the raw Lambertian
    // irradiance π; it's a pre-normalised value the shader then feeds into
    // `kD * albedo / π` during lighting.) Catches dropped cos(θ) or sin(θ)
    // weights, sample-count errors, hemisphere-bound errors, and drift of the
    // π multiplier.
    //
    // Tolerance: the fixed-delta integrator has small Riemann truncation
    // error; empirically within 5e-3 of 1.0 on NVIDIA fp32. 2e-2 for slack.
    // =========================================================================
    TEST(PbrIrradianceTest, UniformWhiteYieldsNormalisedUnity)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 32;
        constexpr u32 kHeight = 32;

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/ShaderUnit_WhiteIrradiance.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        const std::size_t centre = (static_cast<std::size_t>(kHeight / 2) * kWidth + kWidth / 2) * 4;
        const f32 r = pixels[centre + 0];
        const f32 g = pixels[centre + 1];
        const f32 b = pixels[centre + 2];

        constexpr f32 kExpected = 1.0f;
        constexpr f32 kTolerance = 2e-2f;

        EXPECT_NEAR(r, kExpected, kTolerance) << "irradiance.r drifts from unity for uniform-white env";
        EXPECT_NEAR(g, kExpected, kTolerance) << "irradiance.g drifts from unity";
        EXPECT_NEAR(b, kExpected, kTolerance) << "irradiance.b drifts from unity";

        // Spatial uniformity: every pixel should match within a tighter
        // tolerance (same integrator, no input variation).
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4;
                EXPECT_NEAR(pixels[idx + 0], r, 1e-4f) << "non-uniform irradiance at (" << x << "," << y << ")";
            }
        }
    }

    // =========================================================================
    // Prefilter white-environment invariant. Runs the exact GGX importance
    // sampling integrator from `IBLPrefilter.glsl` with the cubemap lookup
    // replaced by uniform-white radiance. The normalised integral
    //
    //     prefilteredColor = Σ L_i * NdotL / Σ NdotL
    //
    // must collapse to 1.0 for *every* roughness value (since L_i = 1
    // everywhere). The probe sweeps roughness along the U axis so one draw
    // covers the full [0, 1] roughness range; we sample several columns to
    // confirm the invariant holds across the whole lobe.
    //
    // Catches: dropped `/ totalWeight` normalization, accumulation sign
    // flips, importance-sample PDF errors, NdotL bound bugs, and Hammersley
    // sequence regressions that cause clumped sampling.
    //
    // Tolerance: Monte Carlo integration with 512 samples has ~1% noise;
    // allow 2% slack for fp32 drift.
    // =========================================================================
    TEST(PbrPrefilterTest, UniformWhiteYieldsUnityAtAllRoughness)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 4;

        PbrProbeHarness harness(kWidth, kHeight, "assets/shaders/tests/ShaderUnit_WhitePrefilter.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        // Sample roughness values across the row: low, mid, high. All must
        // collapse to 1.0 within tolerance.
        const u32 columns[] = { 2, kWidth / 4, kWidth / 2, (3 * kWidth) / 4, kWidth - 3 };
        for (u32 col : columns)
        {
            const std::size_t idx = (0u * kWidth + col) * 4;
            const f32 r = pixels[idx + 0];
            const f32 roughness = static_cast<f32>(col) / static_cast<f32>(kWidth - 1);
            EXPECT_NEAR(r, 1.0f, 2e-2f)
                << "prefilter(uniform-white) drifts from unity at roughness=" << roughness;
        }
    }
} // namespace OloEngine::Tests
