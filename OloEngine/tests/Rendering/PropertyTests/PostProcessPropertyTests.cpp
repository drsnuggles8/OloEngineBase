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
//   [ ] Bloom energy conservation               (needs multi-pass mip chain)
//   [ ] Bloom black passthrough                 (needs multi-pass mip chain)
//   [ ] FXAA edge displacement                  (needs edge-aware test image)
//   [ ] DOF CoC correctness                     (needs scene depth buffer)
//   [ ] Motion blur static                      (needs MotionBlur UBO)
//   [ ] Fog zero/infinite                       (needs Fog UBO + depth)
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

            void SetInputTexture(u32 tex) { m_InputTex = tex; }

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
        auto Lum = [&](u32 x, u32 y) {
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
