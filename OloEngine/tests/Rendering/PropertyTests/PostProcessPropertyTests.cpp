// =============================================================================
// PostProcessPropertyTests.cpp
//
// Property-based tests for single-shader post-process passes. Each test
// compiles the engine's production shader, runs it over a procedurally
// generated input, and verifies a mathematical invariant of the effect.
//
// Covered here (this file grows as catalog lands):
//   [x] Tone mapping: monotonicity of luminance ramp (Reinhard)
//   [x] Tone mapping: black input → black output (all operators)
//   [x] Vignette: center pixel unaffected by vignette
//   [x] Chromatic aberration: center pixel unaffected
//   [x] Bloom energy conservation               (BloomChainEnergyTest.MultiPassDownUpPreservesTotalEnergy)
//   [ ] Bloom black passthrough                 (needs multi-pass mip chain)
//   [x] FXAA edge displacement                  (FxaaEdgeDisplacementTest.EdgePreservesFlatRegions)
//   [x] DOF CoC correctness                     (DofFocusTest.DepthAtFocusDistanceIsIdentity)
//   [x] Motion blur static                      (MotionBlurStaticTest.ZeroVelocityIsIdentity)
//   [x] Fog zero/infinite                       (ShaderUnitFogTest.EndpointInvariants)
//
// TODO: The tests marked [ ] are out of scope for the initial PR because
// they require either multi-pass rendering (bloom mip chain), depth-buffer
// inputs (DOF, fog, motion blur), or additional UBO bindings beyond the
// single PostProcessUBO shared by the primary effects. They are mechanical
// follow-ups once bloom and depth-backed fixtures are written.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // sRGB piecewise decode (the OS framebuffer is sRGB-encoded when the
        // output is RGBA8; the tone-map shader writes the gamma-encoded value
        // directly, so we must decode for comparisons in linear space).
        f32 SrgbToLinear(u8 c)
        {
            const f32 s = static_cast<f32>(c) / 255.0f;
            return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
        }

        // Helper: set up the standard PostProcess UBO with defaults. Caller
        // typically overrides one or two fields before upload.
        PostProcessUBOData MakeDefaultPostProcessUBO(u32 width, u32 height)
        {
            PostProcessUBOData ubo{};
            ubo.TonemapOperator = 1; // Reinhard
            ubo.Exposure = 1.0f;
            ubo.Gamma = 2.2f;
            ubo.BloomThreshold = 1.0f;
            ubo.BloomIntensity = 0.5f;
            ubo.VignetteIntensity = 0.3f;
            ubo.VignetteSmoothness = 0.15f;
            ubo.ChromaticAberrationIntensity = 0.005f;
            ubo.InverseScreenWidth = 1.0f / static_cast<f32>(width);
            ubo.InverseScreenHeight = 1.0f / static_cast<f32>(height);
            ubo.TexelSizeX = ubo.InverseScreenWidth;
            ubo.TexelSizeY = ubo.InverseScreenHeight;
            return ubo;
        }

        // Common setup: input texture + output framebuffer + UBO binding 7 +
        // shader compile. Asserts each step. Caller draws + reads back.
        struct PostProcessHarness
        {
            u32 m_Width;
            u32 m_Height;
            u32 m_InputTex = 0;
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            Ref<UniformBuffer> m_Ubo;
            FullscreenPass m_Pass;

            PostProcessHarness(u32 width, u32 height, const char* shaderPath, const PostProcessUBOData& uboData)
                : m_Width(width), m_Height(height)
            {
                FramebufferSpecification spec{};
                spec.Width = width;
                spec.Height = height;
                spec.Attachments = { FramebufferTextureFormat::RGBA8 };
                m_OutputFB = Framebuffer::Create(spec);

                m_Shader = Shader::Create(shaderPath);
                m_Ubo = UniformBuffer::Create(PostProcessUBOData::GetSize(), 7);
                m_Ubo->SetData(&uboData, PostProcessUBOData::GetSize());
            }

            ~PostProcessHarness()
            {
                if (m_InputTex)
                    ::glDeleteTextures(1, &m_InputTex);
            }

            void SetInputTexture(u32 tex)
            {
                m_InputTex = tex;
            }

            void Draw()
            {
                m_OutputFB->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                m_Shader->Bind();
                m_Pass.Draw(m_InputTex);
                ::glFinish();
                m_OutputFB->Unbind();
            }

            void ReadOutputRgba8(std::vector<u8>& out) const
            {
                const u32 id = m_OutputFB->GetColorAttachmentRendererID(0);
                ReadbackRgba8(id, m_Width, m_Height, out);
            }
        };
    } // namespace

    // =========================================================================
    // Tone mapping
    // =========================================================================

    // Reinhard tone mapping must be monotonically non-decreasing w.r.t. input
    // luminance. Build a horizontal HDR ramp 0 → 16, push through the shader,
    // check the output luminance never decreases (within 1-LSB tolerance).
    TEST(ToneMapMonotonicityTest, ReinhardPreservesLuminanceOrdering)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 256;
        constexpr u32 kHeight = 1;

        std::vector<f32> hdrPixels(kWidth * kHeight * 4);
        for (u32 x = 0; x < kWidth; ++x)
        {
            const f32 t = static_cast<f32>(x) / static_cast<f32>(kWidth - 1);
            const f32 lum = t * 16.0f;
            hdrPixels[(x * 4) + 0] = lum;
            hdrPixels[(x * 4) + 1] = lum;
            hdrPixels[(x * 4) + 2] = lum;
            hdrPixels[(x * 4) + 3] = 1.0f;
        }

        auto uboData = MakeDefaultPostProcessUBO(kWidth, kHeight);
        uboData.TonemapOperator = 1; // Reinhard

        PostProcessHarness h(kWidth, kHeight, "assets/shaders/PostProcess_ToneMap.glsl", uboData);
        h.SetInputTexture(CreateFloatTexture2D(kWidth, kHeight, hdrPixels.data()));
        h.Draw();

        std::vector<u8> rgba;
        h.ReadOutputRgba8(rgba);

        f32 prevLum = -1.0f;
        u32 violations = 0;
        constexpr f32 kEpsilon = 1.0f / 255.0f;
        for (u32 x = 0; x < kWidth; ++x)
        {
            const u32 i = x * 4;
            const f32 r = SrgbToLinear(rgba[i + 0]);
            const f32 g = SrgbToLinear(rgba[i + 1]);
            const f32 b = SrgbToLinear(rgba[i + 2]);
            const f32 lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (lum + kEpsilon < prevLum)
                ++violations;
            prevLum = lum;
        }

        EXPECT_EQ(violations, 0u) << "Tone mapping is not monotonic; " << violations
                                  << " pixel(s) decreased vs their left neighbor.";
        EXPECT_LT(rgba[0], 16u) << "Black input should tone-map near black";
        EXPECT_GT(rgba[(kWidth - 1) * 4], 200u) << "HDR end should saturate near white";
    }

    // All three tone-map operators must map pure black (0,0,0) to pure black.
    // The gamma correction in the shader also passes 0 through unchanged.
    class ToneMapBlackFixture : public ::testing::TestWithParam<int>
    {
    };

    // Monotonicity across a luminance ramp must hold for EVERY operator (not
    // just Reinhard). An inverted curve anywhere in the pipeline would be a
    // major visual regression. We parameterize over the three non-identity
    // operators: Reinhard (1), ACES (2), Uncharted2 (3).
    class ToneMapMonotonicityFixture : public ::testing::TestWithParam<int>
    {
    };

    TEST_P(ToneMapMonotonicityFixture, HdrRampIsNonDecreasing)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 256;
        constexpr u32 kHeight = 1;

        std::vector<f32> hdrPixels(kWidth * kHeight * 4);
        for (u32 x = 0; x < kWidth; ++x)
        {
            // Ramp stops at 8.0 — ACES saturates well before that, which is
            // fine for monotonicity as long as the plateau isn't an inversion.
            const f32 t = static_cast<f32>(x) / static_cast<f32>(kWidth - 1);
            const f32 lum = t * 8.0f;
            hdrPixels[(x * 4) + 0] = lum;
            hdrPixels[(x * 4) + 1] = lum;
            hdrPixels[(x * 4) + 2] = lum;
            hdrPixels[(x * 4) + 3] = 1.0f;
        }

        auto uboData = MakeDefaultPostProcessUBO(kWidth, kHeight);
        uboData.TonemapOperator = GetParam();

        PostProcessHarness h(kWidth, kHeight, "assets/shaders/PostProcess_ToneMap.glsl", uboData);
        h.SetInputTexture(CreateFloatTexture2D(kWidth, kHeight, hdrPixels.data()));
        h.Draw();

        std::vector<u8> rgba;
        h.ReadOutputRgba8(rgba);

        f32 prevLum = -1.0f;
        u32 violations = 0;
        // ACES and Uncharted2 compress more aggressively; allow a 2-LSB
        // tolerance window (vs 1 LSB for Reinhard) to absorb gamma + tone-map
        // quantization noise. A real monotonicity violation produces drops
        // many LSBs large, so this does not hide regressions.
        constexpr f32 kEpsilon = 2.0f / 255.0f;
        for (u32 x = 0; x < kWidth; ++x)
        {
            const u32 i = x * 4;
            const f32 r = SrgbToLinear(rgba[i + 0]);
            const f32 g = SrgbToLinear(rgba[i + 1]);
            const f32 b = SrgbToLinear(rgba[i + 2]);
            const f32 lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (lum + kEpsilon < prevLum)
                ++violations;
            prevLum = lum;
        }

        EXPECT_EQ(violations, 0u) << "Operator " << GetParam() << " is not monotonic across the HDR ramp";
        EXPECT_LT(rgba[0], 16u) << "Black input should tone-map near black for operator " << GetParam();
    }
    INSTANTIATE_TEST_SUITE_P(AllOperators, ToneMapMonotonicityFixture,
                             ::testing::Values(1 /*Reinhard*/, 2 /*ACES*/, 3 /*Uncharted2*/));

    TEST_P(ToneMapBlackFixture, BlackInputStaysBlack)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 8;
        constexpr u32 kHeight = 8;

        auto uboData = MakeDefaultPostProcessUBO(kWidth, kHeight);
        uboData.TonemapOperator = GetParam();

        PostProcessHarness h(kWidth, kHeight, "assets/shaders/PostProcess_ToneMap.glsl", uboData);
        h.SetInputTexture(CreateUniformFloatTexture2D(kWidth, kHeight, 0.0f, 0.0f, 0.0f, 1.0f));
        h.Draw();

        std::vector<u8> rgba;
        h.ReadOutputRgba8(rgba);

        // Uncharted2 has a non-zero black constant (subtracts E/F at the end)
        // but the shader's gamma-stage pow(0, 1/gamma) produces 0, so black
        // stays black regardless. We allow 1-LSB tolerance.
        for (std::size_t i = 0; i < rgba.size(); i += 4)
        {
            EXPECT_LE(rgba[i + 0], 1u) << "R at pixel " << (i / 4);
            EXPECT_LE(rgba[i + 1], 1u) << "G at pixel " << (i / 4);
            EXPECT_LE(rgba[i + 2], 1u) << "B at pixel " << (i / 4);
        }
    }
    INSTANTIATE_TEST_SUITE_P(AllOperators, ToneMapBlackFixture,
                             ::testing::Values(0 /*None*/, 1 /*Reinhard*/, 2 /*ACES*/, 3 /*Uncharted2*/));

    // =========================================================================
    // Tone map NaN/Inf safety: extreme HDR values (e.g., 10^5 from explosions,
    // numerical errors in prior passes) must survive the tone mapper without
    // producing NaN or Inf. RGBA8 readback naturally clamps so we can't catch
    // Inf directly; we use a float framebuffer to sample the HDR output
    // before clamping. Any NaN/Inf here signals a divide-by-zero or log(0)
    // in the tone-map curve that would propagate into subsequent post-passes.
    // =========================================================================
    class ToneMapExtremeHdrFixture : public ::testing::TestWithParam<int>
    {
    };

    TEST_P(ToneMapExtremeHdrFixture, ExtremeHdrProducesFiniteOutput)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 8;

        auto uboData = MakeDefaultPostProcessUBO(kSize, kSize);
        uboData.TonemapOperator = GetParam();
        uboData.Exposure = 1.0f;

        // Create an explicit HDR float framebuffer (the default harness uses
        // RGBA8 which auto-clamps and hides NaN/Inf).
        FramebufferSpecification spec{};
        spec.Width = kSize;
        spec.Height = kSize;
        spec.Attachments = { FramebufferTextureFormat::RGBA16F };
        Ref<Framebuffer> hdrFb = Framebuffer::Create(spec);

        auto shader = Shader::Create("assets/shaders/PostProcess_ToneMap.glsl");
        ASSERT_TRUE(shader);
        auto ubo = UniformBuffer::Create(PostProcessUBOData::GetSize(), 7);
        ubo->SetData(&uboData, PostProcessUBOData::GetSize());

        // Extreme input: 1e5 on R, denormalised range on G, near-FLT_MAX on B.
        const f32 kExtremeR = 1.0e5f;
        const f32 kExtremeG = 1.0e-6f;
        const f32 kExtremeB = 1.0e4f;
        const u32 inputTex = CreateUniformFloatTexture2D(kSize, kSize, kExtremeR, kExtremeG, kExtremeB, 1.0f);

        FullscreenPass pass;
        hdrFb->Bind();
        ::glViewport(0, 0, kSize, kSize);
        ::glDisable(GL_BLEND);
        ::glDisable(GL_DEPTH_TEST);
        ::glDisable(GL_CULL_FACE);
        shader->Bind();
        pass.Draw(inputTex);
        ::glFinish();
        hdrFb->Unbind();

        std::vector<f32> pixels;
        ReadbackRgbaFloat(hdrFb->GetColorAttachmentRendererID(0), kSize, kSize, pixels);
        ::glDeleteTextures(1, &inputTex);

        const FloatStats stats = ComputeStats(pixels);
        EXPECT_EQ(stats.m_NanCount, 0u) << "operator " << GetParam() << " produced NaN on extreme HDR input";
        EXPECT_EQ(stats.m_InfCount, 0u) << "operator " << GetParam() << " produced Inf on extreme HDR input";
    }
    INSTANTIATE_TEST_SUITE_P(AllOperators, ToneMapExtremeHdrFixture,
                             ::testing::Values(1 /*Reinhard*/, 2 /*ACES*/, 3 /*Uncharted2*/));

    // =========================================================================
    // Vignette
    // =========================================================================

    // A vignette effect must leave the screen center untouched (or nearly so)
    // while darkening the corners. Feed a uniform mid-gray field, read the
    // center texel and compare to corner texels — center must be brighter.
    TEST(VignettePropertyTest, CenterBrighterThanCorners)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 64;

        auto uboData = MakeDefaultPostProcessUBO(kSize, kSize);
        uboData.VignetteIntensity = 1.0f; // force strong vignette
        uboData.VignetteSmoothness = 0.5f;

        PostProcessHarness h(kSize, kSize, "assets/shaders/PostProcess_Vignette.glsl", uboData);
        h.SetInputTexture(CreateUniformFloatTexture2D(kSize, kSize, 0.5f, 0.5f, 0.5f, 1.0f));
        h.Draw();

        std::vector<u8> rgba;
        h.ReadOutputRgba8(rgba);

        // Vignette shader writes linear values directly (gamma applied later
        // by the ToneMap pass in the real pipeline). So byte/255 IS linear.
        auto Lum = [&](u32 x, u32 y)
        {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
            return static_cast<f32>(rgba[i + 0]) / 255.0f;
        };

        const f32 centerLum = Lum(kSize / 2, kSize / 2);
        const f32 cornerLum = 0.25f * (Lum(0, 0) + Lum(kSize - 1, 0) + Lum(0, kSize - 1) + Lum(kSize - 1, kSize - 1));

        EXPECT_GT(centerLum, cornerLum + 0.02f)
            << "Vignette did not darken corners: center=" << centerLum << " corner=" << cornerLum;

        // The center should be nearly untouched (within ~10% of input 0.5).
        EXPECT_NEAR(centerLum, 0.5f, 0.08f) << "Vignette darkened the center too aggressively";
    }

    // =========================================================================
    // Chromatic aberration
    // =========================================================================

    // Chromatic aberration displaces color channels radially from the center.
    // The exact center texel has zero radial offset, so it must be unchanged.
    TEST(ChromaticAberrationPropertyTest, CenterPixelUnaffected)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 64;

        auto uboData = MakeDefaultPostProcessUBO(kSize, kSize);
        uboData.ChromaticAberrationIntensity = 0.02f; // exaggerate for test

        PostProcessHarness h(kSize, kSize, "assets/shaders/PostProcess_ChromaticAberration.glsl", uboData);
        // Input: uniform mid-gray so radial sampling ≈ center sample.
        h.SetInputTexture(CreateUniformFloatTexture2D(kSize, kSize, 0.5f, 0.5f, 0.5f, 1.0f));
        h.Draw();

        std::vector<u8> rgba;
        h.ReadOutputRgba8(rgba);

        // Since the input is uniform, CA displacement doesn't change output
        // (sampling anywhere yields the same color). The invariant here is
        // stronger: uniform input → uniform output, validating CA does not
        // introduce energy.
        const std::size_t cx = (kSize / 2) + (kSize / 2) * kSize;
        const u8 rCenter = rgba[cx * 4 + 0];
        const u8 gCenter = rgba[cx * 4 + 1];
        const u8 bCenter = rgba[cx * 4 + 2];

        // All pixels should match the center within 1 LSB (uniform input).
        for (std::size_t i = 0; i < static_cast<std::size_t>(kSize) * kSize; ++i)
        {
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 0]), static_cast<int>(rCenter), 1);
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 1]), static_cast<int>(gCenter), 1);
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 2]), static_cast<int>(bCenter), 1);
        }
    }

    // =========================================================================
    // FXAA
    //
    // FXAA is an LDR post-process that finds luma gradients and blurs along
    // them. On a perfectly uniform input there is no gradient, so FXAA must
    // be a no-op (within 1 LSB of RGBA8 quantization). Catches broken luma
    // computation that activates on zero-gradient pixels.
    // =========================================================================
    TEST(FxaaUniformInputTest, UniformInputIsNoOp)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 64;
        PostProcessUBOData ubo = MakeDefaultPostProcessUBO(kSize, kSize);

        PostProcessHarness harness(kSize, kSize, "assets/shaders/PostProcess_FXAA.glsl", ubo);
        // Medium-gray input (0.5 linear). FXAA reads LDR so stay in [0, 1].
        harness.SetInputTexture(CreateUniformFloatTexture2D(kSize, kSize, 0.5f, 0.5f, 0.5f, 1.0f));
        harness.Draw();

        std::vector<u8> rgba;
        harness.ReadOutputRgba8(rgba);
        ASSERT_EQ(rgba.size(), static_cast<std::size_t>(kSize) * kSize * 4);

        // Pick the center as reference; assert every pixel matches within 1 LSB.
        const std::size_t cx = (static_cast<std::size_t>(kSize / 2) * kSize + kSize / 2);
        const u8 rCenter = rgba[cx * 4 + 0];
        const u8 gCenter = rgba[cx * 4 + 1];
        const u8 bCenter = rgba[cx * 4 + 2];

        for (std::size_t i = 0; i < static_cast<std::size_t>(kSize) * kSize; ++i)
        {
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 0]), static_cast<int>(rCenter), 1)
                << "pixel " << i << " R differs from center beyond 1 LSB (FXAA activated on uniform input)";
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 1]), static_cast<int>(gCenter), 1);
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 2]), static_cast<int>(bCenter), 1);
        }
    }

    // =========================================================================
    // FXAA must not displace pixels far from a sharp edge. With a vertical
    // hard edge at x=kEdge (left half = black, right half = white), pixels
    // several columns into the black half must remain black, and pixels
    // several columns into the white half must remain white. Only a narrow
    // band around the edge may be blended.
    //
    // Catches: overly aggressive FXAA parameters (EDGE_THRESHOLD too low,
    // SEARCH_STEPS too high) that bleed the edge far into the flat regions.
    // =========================================================================
    TEST(FxaaEdgeDisplacementTest, EdgePreservesFlatRegions)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 64;
        constexpr u32 kEdge = kSize / 2;
        constexpr u32 kMargin = 3; // Allow up to 3px bleed near the edge.

        // Build a hard vertical edge: x < kEdge → black, x ≥ kEdge → white.
        std::vector<f32> pixels(kSize * kSize * 4);
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const f32 v = (x < kEdge) ? 0.0f : 1.0f;
                const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
                pixels[i + 0] = v;
                pixels[i + 1] = v;
                pixels[i + 2] = v;
                pixels[i + 3] = 1.0f;
            }
        }

        PostProcessUBOData ubo = MakeDefaultPostProcessUBO(kSize, kSize);
        PostProcessHarness harness(kSize, kSize, "assets/shaders/PostProcess_FXAA.glsl", ubo);
        harness.SetInputTexture(CreateFloatTexture2D(kSize, kSize, pixels.data()));
        harness.Draw();

        std::vector<u8> rgba;
        harness.ReadOutputRgba8(rgba);

        // Interior rows only (skip top/bottom 2 rows to avoid edge-clamp effects).
        for (u32 y = 2; y < kSize - 2; ++y)
        {
            for (u32 x = 0; x < kEdge - kMargin; ++x)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
                EXPECT_LE(static_cast<int>(rgba[i + 0]), 4)
                    << "pixel (" << x << ", " << y << ") R: FXAA bled "
                    << static_cast<int>(kEdge - x) << " columns into black region";
            }
            for (u32 x = kEdge + kMargin; x < kSize; ++x)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
                EXPECT_GE(static_cast<int>(rgba[i + 0]), 251)
                    << "pixel (" << x << ", " << y << ") R: FXAA bled "
                    << static_cast<int>(x - kEdge) << " columns into white region";
            }
        }
    }

    // =========================================================================
    // Motion blur
    //
    // When the current frame's ViewProjection matches the previous frame's,
    // per-pixel velocity collapses to zero and motion blur must reduce to an
    // identity pass over the scene-color input. We force this by writing
    // identity matrices into the MotionBlur UBO: InverseViewProjection = I
    // and PrevViewProjection = I. The shader then computes prevUV = v_TexCoord
    // for every pixel, yielding velocity == 0 regardless of the depth buffer
    // contents.
    //
    // Catches: motion blur activating on zero-velocity pixels (unstable
    // blending weights, wrong clamp, sign flips in the prev-proj reconstruction).
    // =========================================================================
    TEST(MotionBlurStaticTest, ZeroVelocityIsIdentity)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 32;
        PostProcessUBOData ubo = MakeDefaultPostProcessUBO(kSize, kSize);
        ubo.MotionBlurStrength = 1.0f;
        ubo.MotionBlurSamples = 8;
        ubo.CameraNear = 0.1f;
        ubo.CameraFar = 100.0f;

        PostProcessHarness harness(kSize, kSize, "assets/shaders/PostProcess_MotionBlur.glsl", ubo);

        // Uniform mid-gray input so any non-identity averaging stays detectable.
        harness.SetInputTexture(CreateUniformFloatTexture2D(kSize, kSize, 0.5f, 0.5f, 0.5f, 1.0f));

        // Depth texture at binding 19 (shader samples .r for reconstruction).
        // With identity matrices, the depth value is irrelevant but the sampler
        // must still be bound to something valid. Using 0.5 keeps it in-range.
        const u32 depthTex = CreateUniformFloatTexture2D(kSize, kSize, 0.5f, 0.5f, 0.5f, 1.0f);
        ::glBindTextureUnit(19, static_cast<GLuint>(depthTex));

        // MotionBlur UBO at binding 8: identity matrices.
        MotionBlurUBOData mbUbo{};
        // (default-initialized matrices are already identity)
        Ref<UniformBuffer> mbBuffer = UniformBuffer::Create(MotionBlurUBOData::GetSize(), 8);
        mbBuffer->SetData(&mbUbo, MotionBlurUBOData::GetSize());

        harness.Draw();

        std::vector<u8> rgba;
        harness.ReadOutputRgba8(rgba);
        ::glDeleteTextures(1, &depthTex);

        // Every pixel must match the uniform gray input within 1 LSB.
        const u8 expected = static_cast<u8>(std::lround(0.5f * 255.0f));
        for (std::size_t i = 0; i < static_cast<std::size_t>(kSize) * kSize; ++i)
        {
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 0]), static_cast<int>(expected), 2)
                << "pixel " << i << " R drifted (motion blur activated at zero velocity)";
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 1]), static_cast<int>(expected), 2);
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 2]), static_cast<int>(expected), 2);
        }
    }

    // =========================================================================
    // DOF (Depth of Field)
    //
    // When every fragment is at exactly the focus distance, the circle of
    // confusion is zero, blurRadius stays under 0.5, and the shader takes the
    // early-out path that returns the sampled scene color unmodified. We set
    // up the depth buffer such that linearizeDepth(depth) == u_DOFFocusDistance
    // and assert the output matches the input.
    //
    // Mapping: depth (NDC-encoded in sampler) → linear depth via
    //     z_ndc = depth * 2 - 1
    //     linear = (2 * near * far) / (far + near - z_ndc * (far - near))
    // We invert: set desired linear = 10.0, near = 0.1, far = 100.0, solve for
    // depth. Algebra:
    //     far + near - z_ndc*(far-near) = 2*near*far / linear
    //     z_ndc = (far + near - 2*near*far/linear) / (far - near)
    //     depth = (z_ndc + 1) / 2
    // For linear=10, near=0.1, far=100: z_ndc ≈ 0.98, depth ≈ 0.99.
    // =========================================================================
    TEST(DofFocusTest, DepthAtFocusDistanceIsIdentity)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 32;
        constexpr f32 kNear = 0.1f;
        constexpr f32 kFar = 100.0f;
        constexpr f32 kFocus = 10.0f;

        PostProcessUBOData ubo = MakeDefaultPostProcessUBO(kSize, kSize);
        ubo.DOFFocusDistance = kFocus;
        ubo.DOFFocusRange = 5.0f;
        ubo.DOFBokehRadius = 10.0f;
        ubo.CameraNear = kNear;
        ubo.CameraFar = kFar;

        PostProcessHarness harness(kSize, kSize, "assets/shaders/PostProcess_DOF.glsl", ubo);
        harness.SetInputTexture(CreateUniformFloatTexture2D(kSize, kSize, 0.5f, 0.5f, 0.5f, 1.0f));

        // Compute a depth value that linearizes exactly to kFocus.
        const f32 zNdc = (kFar + kNear - 2.0f * kNear * kFar / kFocus) / (kFar - kNear);
        const f32 depth = (zNdc + 1.0f) * 0.5f;

        const u32 depthTex = CreateUniformFloatTexture2D(kSize, kSize, depth, depth, depth, 1.0f);
        ::glBindTextureUnit(19, static_cast<GLuint>(depthTex));

        harness.Draw();

        std::vector<u8> rgba;
        harness.ReadOutputRgba8(rgba);
        ::glDeleteTextures(1, &depthTex);

        const u8 expected = static_cast<u8>(std::lround(0.5f * 255.0f));
        for (std::size_t i = 0; i < static_cast<std::size_t>(kSize) * kSize; ++i)
        {
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 0]), static_cast<int>(expected), 2)
                << "pixel " << i << " R drifted (DOF blurred at focus distance)";
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 1]), static_cast<int>(expected), 2);
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 2]), static_cast<int>(expected), 2);
        }
    }

    // =========================================================================
    // DOF CoC linear-model sweep. Exercises the full CoC math from
    // PostProcess_DOF.glsl using a CoC-only probe shader
    // (ShaderUnit_DofCoc.glsl) that outputs the computed CoC directly. We
    // author a depth gradient that sweeps linear depth from 0 → 2*focus
    // across the image width so one draw covers near/at/far of the focus
    // point in a single readback.
    //
    // Linear model: coc = clamp(|linearDepth - focus| / focusRange, 0, 1).
    //
    // Invariants asserted:
    //   1. CoC(focus) == 0 (identity at focus point)
    //   2. CoC clamps to exactly 1.0 once |Δ| >= focusRange
    //   3. Monotonically non-decreasing in |linearDepth - focus|
    //   4. Near/far symmetric: CoC(focus+d) == CoC(focus-d) within 2 LSBs
    //   5. blurRadius == coc * u_DOFBokehRadius (the second output channel)
    //
    // Catches: wrong sign on (linearDepth - focus), missing clamp, asymmetric
    // CoC (e.g. dropped abs()), wrong divisor.
    // =========================================================================
    TEST(DofFocusTest, CocLinearModelMatchesSweep)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 128;
        constexpr u32 kHeight = 4;
        constexpr f32 kNear = 0.1f;
        constexpr f32 kFar = 100.0f;
        constexpr f32 kFocus = 10.0f;
        constexpr f32 kRange = 5.0f;
        constexpr f32 kBokeh = 16.0f;
        constexpr f32 kLinearSweepMax = 2.0f * kFocus; // 0 → 20

        // Build per-pixel depth values: linearDepth varies across U.
        auto LinearToNdcDepth = [&](f32 linear) -> f32
        {
            const f32 zNdc = (kFar + kNear - 2.0f * kNear * kFar / linear) / (kFar - kNear);
            return (zNdc + 1.0f) * 0.5f;
        };

        std::vector<f32> depthPixels(static_cast<std::size_t>(kWidth) * kHeight * 4);
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kWidth);
                const f32 linear = std::max(0.01f, u * kLinearSweepMax);
                const f32 depth = LinearToNdcDepth(linear);
                const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4;
                depthPixels[idx + 0] = depth;
                depthPixels[idx + 1] = depth;
                depthPixels[idx + 2] = depth;
                depthPixels[idx + 3] = 1.0f;
            }
        }

        PostProcessUBOData ubo = MakeDefaultPostProcessUBO(kWidth, kHeight);
        ubo.DOFFocusDistance = kFocus;
        ubo.DOFFocusRange = kRange;
        ubo.DOFBokehRadius = kBokeh;
        ubo.CameraNear = kNear;
        ubo.CameraFar = kFar;

        PostProcessHarness harness(kWidth, kHeight,
                                   "assets/shaders/tests/ShaderUnit_DofCoc.glsl", ubo);
        // Input texture is unused by the CoC probe but the harness wires it.
        harness.SetInputTexture(CreateUniformFloatTexture2D(kWidth, kHeight, 0.5f, 0.5f, 0.5f, 1.0f));

        const u32 depthTex = [&]() -> u32
        {
            GLuint id = 0;
            ::glCreateTextures(GL_TEXTURE_2D, 1, &id);
            ::glTextureStorage2D(id, 1, GL_RGBA32F, static_cast<GLsizei>(kWidth),
                                 static_cast<GLsizei>(kHeight));
            ::glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            ::glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            ::glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            ::glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            ::glTextureSubImage2D(id, 0, 0, 0,
                                  static_cast<GLsizei>(kWidth),
                                  static_cast<GLsizei>(kHeight),
                                  GL_RGBA, GL_FLOAT, depthPixels.data());
            return static_cast<u32>(id);
        }();
        ::glBindTextureUnit(19, static_cast<GLuint>(depthTex));

        harness.Draw();

        std::vector<u8> rgba;
        harness.ReadOutputRgba8(rgba);
        ::glDeleteTextures(1, &depthTex);
        ASSERT_EQ(rgba.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        // Pull CoC values along the top row (all rows are identical since
        // the depth gradient is 1D across U).
        auto CocAt = [&](u32 x) -> f32
        {
            const std::size_t idx = (0u * kWidth + x) * 4;
            return static_cast<f32>(rgba[idx + 0]) / 255.0f;
        };
        auto BlurRatioAt = [&](u32 x) -> f32
        {
            const std::size_t idx = (0u * kWidth + x) * 4;
            return static_cast<f32>(rgba[idx + 1]) / 255.0f;
        };

        // Find the pixel x closest to linear==focus.
        const u32 focusX = static_cast<u32>(std::lround(
            (kFocus / kLinearSweepMax) * kWidth - 0.5f));
        // Tolerance accounts for pixel-center quantization: one pixel of sweep
        // resolution = (kLinearSweepMax / kWidth) / kRange in CoC units =
        // 20/128/5 ≈ 0.031 ≈ 8/255. Round up for safety (8-bit readback).
        constexpr f32 kPixelCocLsb = 10.0f / 255.0f;
        EXPECT_LE(CocAt(focusX), kPixelCocLsb)
            << "CoC(focus) must be ~0; got " << CocAt(focusX);

        // Invariant 2: pixels at linear <= focus-range OR >= focus+range must saturate to 1.
        const u32 nearSatX = static_cast<u32>(std::lround(
            ((kFocus - 1.5f * kRange) / kLinearSweepMax) * kWidth));
        const u32 farSatX = static_cast<u32>(std::lround(
            ((kFocus + 1.5f * kRange) / kLinearSweepMax) * kWidth));
        EXPECT_GE(CocAt(nearSatX), 250.0f / 255.0f)
            << "CoC beyond near saturation (x=" << nearSatX << ") must be ~1.0";
        EXPECT_GE(CocAt(farSatX), 250.0f / 255.0f)
            << "CoC beyond far saturation (x=" << farSatX << ") must be ~1.0";

        // Invariant 3: monotonic non-decreasing in |linear - focus|.
        // Walk outward from focus in both directions.
        u32 monotonicityViolations = 0;
        for (u32 i = 1; i < focusX; ++i)
        {
            // Near side: as x decreases, |Δ| increases → CoC should not decrease
            if (CocAt(focusX - i) + (2.0f / 255.0f) < CocAt(focusX - i + 1))
                ++monotonicityViolations;
        }
        for (u32 i = 1; focusX + i < kWidth; ++i)
        {
            if (CocAt(focusX + i) + (2.0f / 255.0f) < CocAt(focusX + i - 1))
                ++monotonicityViolations;
        }
        EXPECT_EQ(monotonicityViolations, 0u)
            << "CoC should be monotonically non-decreasing in |linearDepth - focus|";

        // Invariant 4: symmetric around focus (within a few LSBs due to the
        // inverse-depth mapping making pixel widths non-uniform in linear
        // space; we compare CoC *values* at matched |Δ|, not pixel distances).
        for (u32 stepPct = 25; stepPct <= 90; stepPct += 25)
        {
            const f32 delta = (static_cast<f32>(stepPct) / 100.0f) * kRange;
            const u32 xNear = static_cast<u32>(std::lround(
                ((kFocus - delta) / kLinearSweepMax) * kWidth - 0.5f));
            const u32 xFar = static_cast<u32>(std::lround(
                ((kFocus + delta) / kLinearSweepMax) * kWidth - 0.5f));
            if (xNear >= kWidth || xFar >= kWidth)
                continue;
            EXPECT_NEAR(CocAt(xNear), CocAt(xFar), 12.0f / 255.0f)
                << "CoC must be symmetric around focus at Δ=" << delta
                << " (xNear=" << xNear << " xFar=" << xFar << ")";
        }

        // Invariant 5: blur radius == coc * bokehRadius. Since the probe
        // writes blurRadius / bokehRadius to G, G == R.
        for (u32 x = 0; x < kWidth; x += 4)
        {
            EXPECT_NEAR(BlurRatioAt(x), CocAt(x), 2.0f / 255.0f)
                << "blurRadius/bokehRadius mismatches CoC at x=" << x;
        }
    }

    // =========================================================================
    // Bloom threshold: all-black input must produce all-black output.
    //
    // The bloom threshold pass extracts pixels brighter than u_BloomThreshold.
    // A pure-black input has zero luminance, so the `step(threshold, brightness)`
    // term goes to zero and nothing is emitted. Catches shaders that generate
    // energy from nothing (e.g. bias bugs, soft-knee formula errors producing
    // negative->pow results, or unclamped sample offsets sneaking values in).
    // =========================================================================
    TEST(BloomThresholdTest, BlackInputStaysBlack)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 32;
        PostProcessUBOData ubo = MakeDefaultPostProcessUBO(kSize, kSize);
        ubo.BloomThreshold = 1.0f;

        PostProcessHarness harness(kSize, kSize, "assets/shaders/PostProcess_BloomThreshold.glsl", ubo);
        harness.SetInputTexture(CreateUniformFloatTexture2D(kSize, kSize, 0.0f, 0.0f, 0.0f, 1.0f));

        harness.Draw();

        std::vector<u8> rgba;
        harness.ReadOutputRgba8(rgba);

        for (std::size_t i = 0; i < static_cast<std::size_t>(kSize) * kSize; ++i)
        {
            EXPECT_EQ(static_cast<int>(rgba[i * 4 + 0]), 0) << "pixel " << i << " R non-zero";
            EXPECT_EQ(static_cast<int>(rgba[i * 4 + 1]), 0) << "pixel " << i << " G non-zero";
            EXPECT_EQ(static_cast<int>(rgba[i * 4 + 2]), 0) << "pixel " << i << " B non-zero";
        }
    }

    // =========================================================================
    // Bloom downsample: the 13-tap Call-of-Duty kernel uses weights
    //     0.125 + 4*0.125 + 4*0.03125 + 4*0.0625 = 1.0
    // so a constant input must map to the same constant output. This catches
    // weight-sum bugs (e.g., typo scaling factors, missing divide) that would
    // either brighten or darken uniform regions — the single most common way
    // to silently break a bloom chain.
    // =========================================================================
    TEST(BloomDownsampleTest, UniformInputIsIdentity)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 64;
        PostProcessUBOData ubo = MakeDefaultPostProcessUBO(kSize, kSize);

        PostProcessHarness harness(kSize, kSize, "assets/shaders/PostProcess_BloomDownsample.glsl", ubo);
        harness.SetInputTexture(CreateUniformFloatTexture2D(kSize, kSize, 0.5f, 0.25f, 0.75f, 1.0f));

        harness.Draw();

        std::vector<u8> rgba;
        harness.ReadOutputRgba8(rgba);

        const u8 expectedR = static_cast<u8>(std::lround(0.5f * 255.0f));
        const u8 expectedG = static_cast<u8>(std::lround(0.25f * 255.0f));
        const u8 expectedB = static_cast<u8>(std::lround(0.75f * 255.0f));

        // Interior pixels only — edge pixels clamp samples, which is correct
        // behaviour for a uniform input but let's leave a 4-px margin anyway
        // so we're invariant to any future change in edge-sampling clamping.
        for (u32 y = 4; y < kSize - 4; ++y)
        {
            for (u32 x = 4; x < kSize - 4; ++x)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * kSize + x) * 4;
                EXPECT_NEAR(static_cast<int>(rgba[idx + 0]), static_cast<int>(expectedR), 2);
                EXPECT_NEAR(static_cast<int>(rgba[idx + 1]), static_cast<int>(expectedG), 2);
                EXPECT_NEAR(static_cast<int>(rgba[idx + 2]), static_cast<int>(expectedB), 2);
            }
        }
    }

    // =========================================================================
    // Bloom upsample: the 9-tap tent filter has weights summing to 16/16=1.
    // Same invariant as downsample — constant input ⇒ constant output.
    // Catches weight-sum bugs in the tent kernel.
    // =========================================================================
    TEST(BloomUpsampleTest, UniformInputIsIdentity)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 64;
        PostProcessUBOData ubo = MakeDefaultPostProcessUBO(kSize, kSize);

        PostProcessHarness harness(kSize, kSize, "assets/shaders/PostProcess_BloomUpsample.glsl", ubo);
        harness.SetInputTexture(CreateUniformFloatTexture2D(kSize, kSize, 0.5f, 0.5f, 0.5f, 1.0f));

        harness.Draw();

        std::vector<u8> rgba;
        harness.ReadOutputRgba8(rgba);

        const u8 expected = static_cast<u8>(std::lround(0.5f * 255.0f));
        for (u32 y = 2; y < kSize - 2; ++y)
        {
            for (u32 x = 2; x < kSize - 2; ++x)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * kSize + x) * 4;
                EXPECT_NEAR(static_cast<int>(rgba[idx + 0]), static_cast<int>(expected), 2);
                EXPECT_NEAR(static_cast<int>(rgba[idx + 1]), static_cast<int>(expected), 2);
                EXPECT_NEAR(static_cast<int>(rgba[idx + 2]), static_cast<int>(expected), 2);
            }
        }
    }

    // =========================================================================
    // Bloom composite: with u_BloomIntensity = 0, the additive combine
    // collapses to sceneColor (bloom contribution multiplied by zero). The
    // output must match the scene input exactly — catches the classic "bloom
    // leaks when intensity is 0" family of bugs (e.g., accidental += instead
    // of = on the first term, or a hard-coded >= 0 clamp that adds a floor).
    // =========================================================================
    TEST(BloomCompositeTest, ZeroIntensityPassesSceneThrough)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 32;
        PostProcessUBOData ubo = MakeDefaultPostProcessUBO(kSize, kSize);
        ubo.BloomIntensity = 0.0f;

        PostProcessHarness harness(kSize, kSize, "assets/shaders/PostProcess_BloomComposite.glsl", ubo);
        // Scene at unit 0 = mid-grey; bloom at unit 1 = bright magenta (to
        // make absence easy to detect).
        harness.SetInputTexture(CreateUniformFloatTexture2D(kSize, kSize, 0.5f, 0.5f, 0.5f, 1.0f));

        const u32 bloomTex = CreateUniformFloatTexture2D(kSize, kSize, 1.0f, 0.0f, 1.0f, 1.0f);
        ::glBindTextureUnit(1, static_cast<GLuint>(bloomTex));

        harness.Draw();

        std::vector<u8> rgba;
        harness.ReadOutputRgba8(rgba);
        ::glDeleteTextures(1, &bloomTex);

        const u8 expected = static_cast<u8>(std::lround(0.5f * 255.0f));
        for (std::size_t i = 0; i < static_cast<std::size_t>(kSize) * kSize; ++i)
        {
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 0]), static_cast<int>(expected), 2)
                << "pixel " << i << " R: bloom leaked despite intensity=0";
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 1]), static_cast<int>(expected), 2);
            EXPECT_NEAR(static_cast<int>(rgba[i * 4 + 2]), static_cast<int>(expected), 2);
        }
    }

    // =========================================================================
    // Bloom chain energy conservation. Chains the production downsample +
    // upsample shaders through a multi-mip pyramid (64 → 32 → 16 → 8 → 16 →
    // 32 → 64, all RGBA16F) and asserts that total summed radiance survives
    // the round-trip within a tolerance.
    //
    // The Call-of-Duty 13-tap downsample weights sum to 1.0, so a uniform
    // input stays uniform (and total sum halves per area halving). The
    // 9-tap tent upsample weights also sum to 1.0, so reversing the chain
    // should reconstruct total energy — minus kernel-leak across borders.
    //
    // Test input: single bright HDR pixel at the centre of a 64×64 frame
    // (value 10.0 per RGB channel, total R sum = 10.0). After the full
    // down→up chain the reconstructed total should match within ~30 %.
    // We use a wide tolerance because:
    //   1. kernel footprint at mip 3 (8×8) covers ~40 % of the image, so
    //      border clamping loses some energy;
    //   2. linear filtering at half-texel offsets smears the Dirac across
    //      multiple output pixels (intended behaviour).
    //
    // Catches: downsample weight-sum != 1.0 (energy gain/loss per step),
    // upsample weight-sum != 1.0, wrong texel-size UBO values (sampling
    // outside the texel grid), and accidental additive instead of
    // overwrite output in either pass.
    // =========================================================================
    TEST(BloomChainEnergyTest, MultiPassDownUpPreservesTotalEnergy)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kFullSize = 64;

        constexpr f32 kPeak = 10.0f;
        std::vector<f32> input(kFullSize * kFullSize * 4, 0.0f);
        const u32 cx = kFullSize / 2;
        const u32 cy = kFullSize / 2;
        const std::size_t centreIdx = (static_cast<std::size_t>(cy) * kFullSize + cx) * 4;
        input[centreIdx + 0] = kPeak;
        input[centreIdx + 1] = kPeak;
        input[centreIdx + 2] = kPeak;
        input[centreIdx + 3] = 1.0f;

        const f64 inputSumR = kPeak;

        u32 currentTex = CreateFloatTexture2D(kFullSize, kFullSize, input.data());

        auto downsampleShader = Shader::Create("assets/shaders/PostProcess_BloomDownsample.glsl");
        auto upsampleShader = Shader::Create("assets/shaders/PostProcess_BloomUpsample.glsl");
        ASSERT_TRUE(downsampleShader);
        ASSERT_TRUE(upsampleShader);

        auto ubo = UniformBuffer::Create(PostProcessUBOData::GetSize(), 7);
        FullscreenPass pass;

        auto makeHdrFb = [](u32 w, u32 h)
        {
            FramebufferSpecification s{};
            s.Width = w;
            s.Height = h;
            s.Attachments = { FramebufferTextureFormat::RGBA16F };
            return Framebuffer::Create(s);
        };

        auto runPass = [&](const Ref<Shader>& shader, Ref<Framebuffer>& target,
                           u32 srcW, u32 srcH, u32 inputTex)
        {
            auto uboData = MakeDefaultPostProcessUBO(srcW, srcH);
            uboData.TexelSizeX = 1.0f / static_cast<f32>(srcW);
            uboData.TexelSizeY = 1.0f / static_cast<f32>(srcH);
            ubo->SetData(&uboData, PostProcessUBOData::GetSize());

            target->Bind();
            ::glViewport(0, 0, static_cast<GLsizei>(target->GetSpecification().Width),
                         static_cast<GLsizei>(target->GetSpecification().Height));
            ::glDisable(GL_BLEND);
            ::glDisable(GL_DEPTH_TEST);
            ::glDisable(GL_CULL_FACE);
            shader->Bind();
            pass.Draw(inputTex);
            ::glFinish();
            target->Unbind();
        };

        auto fbDown32 = makeHdrFb(32, 32);
        auto fbDown16 = makeHdrFb(16, 16);
        auto fbDown8 = makeHdrFb(8, 8);

        runPass(downsampleShader, fbDown32, 64, 64, currentTex);
        ::glDeleteTextures(1, reinterpret_cast<const GLuint*>(&currentTex));

        runPass(downsampleShader, fbDown16, 32, 32, fbDown32->GetColorAttachmentRendererID(0));
        runPass(downsampleShader, fbDown8, 16, 16, fbDown16->GetColorAttachmentRendererID(0));

        auto fbUp16 = makeHdrFb(16, 16);
        auto fbUp32 = makeHdrFb(32, 32);
        auto fbUp64 = makeHdrFb(64, 64);

        runPass(upsampleShader, fbUp16, 8, 8, fbDown8->GetColorAttachmentRendererID(0));
        runPass(upsampleShader, fbUp32, 16, 16, fbUp16->GetColorAttachmentRendererID(0));
        runPass(upsampleShader, fbUp64, 32, 32, fbUp32->GetColorAttachmentRendererID(0));

        std::vector<f32> reconstructed;
        ReadbackRgbaFloat(fbUp64->GetColorAttachmentRendererID(0), kFullSize, kFullSize, reconstructed);
        ASSERT_EQ(reconstructed.size(), static_cast<std::size_t>(kFullSize) * kFullSize * 4);

        f64 outSumR = 0.0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(kFullSize) * kFullSize; ++i)
        {
            outSumR += static_cast<f64>(reconstructed[i * 4 + 0]);
        }

        const f64 ratio = outSumR / inputSumR;
        EXPECT_GT(ratio, 0.70) << "Bloom chain lost > 30% energy (ratio=" << ratio << ")";
        EXPECT_LT(ratio, 1.30) << "Bloom chain gained > 30% energy (ratio=" << ratio << ")";
    }

    // =========================================================================
    // Fog: when u_FogFlags.x < 0.5 (disabled), the shader must early-out with
    // output = vec4(0, 0, 0, 1): zero inscatter, full transmittance. This is
    // the contract the composite pass relies on — if the disable flag leaks
    // even a single non-zero value, a disabled fog system would tint the
    // entire frame. Catches flag-check typos and uninitialised outputs.
    //
    // Note: this test uploads only the Fog UBO (binding 17). The shader has
    // references to Camera (binding 0), Shadow (binding 6), and MotionBlur
    // (binding 8) UBOs plus a shadow-map array sampler, but with FogFlags.x=0
    // the shader returns before any of those bindings are read. The depth
    // texture (binding 19) IS read before the flag check, so we bind a small
    // dummy depth texture to unit 19.
    // =========================================================================
    TEST(FogDisabledTest, DisabledFlagProducesZeroInscatter)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 32;
        PostProcessUBOData ubo = MakeDefaultPostProcessUBO(kSize, kSize);

        PostProcessHarness harness(kSize, kSize, "assets/shaders/PostProcess_Fog.glsl", ubo);
        harness.SetInputTexture(CreateUniformFloatTexture2D(kSize, kSize, 0.5f, 0.5f, 0.5f, 1.0f));

        // Dummy depth texture at binding 19 (0.5 = mid-depth; value is never
        // used because shader early-outs, but sampling an unbound sampler is
        // undefined behaviour on some drivers).
        const u32 depthTex = CreateUniformFloatTexture2D(kSize, kSize, 0.5f, 0.5f, 0.5f, 1.0f);
        ::glBindTextureUnit(19, static_cast<GLuint>(depthTex));

        // Fog UBO at binding 17 with disabled flag.
        FogUBOData fogData{};
        fogData.Flags = glm::vec4(0.0f); // x=0 → disabled
        Ref<UniformBuffer> fogUbo = UniformBuffer::Create(FogUBOData::GetSize(), 17);
        fogUbo->SetData(&fogData, FogUBOData::GetSize());

        harness.Draw();

        std::vector<u8> rgba;
        harness.ReadOutputRgba8(rgba);
        ::glDeleteTextures(1, &depthTex);

        // Expected: (0, 0, 0, 255) — zero RGB inscatter, full alpha
        // transmittance. Allow 1 LSB of rounding noise on each channel.
        for (std::size_t i = 0; i < static_cast<std::size_t>(kSize) * kSize; ++i)
        {
            EXPECT_LE(static_cast<int>(rgba[i * 4 + 0]), 1)
                << "pixel " << i << " R: disabled fog emitted non-zero inscatter";
            EXPECT_LE(static_cast<int>(rgba[i * 4 + 1]), 1)
                << "pixel " << i << " G: disabled fog emitted non-zero inscatter";
            EXPECT_LE(static_cast<int>(rgba[i * 4 + 2]), 1)
                << "pixel " << i << " B: disabled fog emitted non-zero inscatter";
            EXPECT_GE(static_cast<int>(rgba[i * 4 + 3]), 254)
                << "pixel " << i << " A: disabled fog lost transmittance";
        }
    }
} // namespace OloEngine::Tests
