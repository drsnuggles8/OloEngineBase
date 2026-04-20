// =============================================================================
// ShadowTerrainPropertyTests.cpp
//
// Layer-1 property tests for the shadow and terrain subsystems. Shader math
// is exercised in isolation via dedicated probe shaders (under
// `OloEditor/assets/shaders/tests/`) so we don't need the full cascaded
// shadow pipeline or tessellated terrain patch path. The bugs these tests
// catch are mathematical — control-flow in `calculateCascadedShadowFactorCSM`
// and the derivative kernel in `Terrain_PBR` — not pipeline wiring.
//
// Covered here:
//   [x] Shadow bounds: out-of-frustum short-circuits to "lit"
//   [x] Shadow bounds: past max shadow distance short-circuits to "lit"
//   [x] Shadow cascade: correct index selected for sweep of view-space depths
//   [x] Terrain: flat heightmap yields exact (0, 1, 0) normals
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
#include <array>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Reuse PbrProbeHarness-style construction here without sharing code
        // (keeps test file independent; harness is ~30 lines).
        struct ShaderProbeHarness
        {
            u32 m_Width;
            u32 m_Height;
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            ShaderProbeHarness(u32 width, u32 height, const char* shaderPath)
                : m_Width(width), m_Height(height)
            {
                FramebufferSpecification spec{};
                spec.Width = width;
                spec.Height = height;
                spec.Attachments = { FramebufferTextureFormat::RGBA16F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create(shaderPath);
            }

            void Draw(u32 boundTex = 0)
            {
                m_OutputFB->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                m_Shader->Bind();
                m_Pass.Draw(boundTex);
                ::glFinish();
                m_OutputFB->Unbind();
            }

            void ReadRgbaFloat(std::vector<f32>& out) const
            {
                const u32 id = m_OutputFB->GetColorAttachmentRendererID(0);
                ReadbackRgbaFloat(id, m_Width, m_Height, out);
            }
        };
    } // namespace

    // =========================================================================
    // Shadow: the bounds-and-cascade control-flow in
    // `calculateCascadedShadowFactorCSM` must short-circuit to "lit" (= 1.0
    // in production; encoded as 0.25 for past-max and 0.50 for out-of-frustum
    // in the probe) whenever the fragment is outside the shadowed region.
    //
    // The test fixture sweeps -viewDepth from 0 → 1.2 * maxShadowDistance on
    // X (past 100 world units = past max), and pushes projCoords.x out of
    // bounds on the top half of the framebuffer. Every short-circuit
    // classification must match expectation.
    //
    // Catches: boundary-condition flips (e.g. `> 1.0` changed to `>= 1.0`),
    // missing max-distance clamp, swapped cascade-plane tests.
    // =========================================================================
    TEST(ShadowBoundsTest, BoundaryCasesShortCircuit)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 32;

        ShaderProbeHarness harness(kWidth, kHeight,
                                   "assets/shaders/tests/ShaderUnit_ShadowBounds.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        // MAX_SHADOW_DISTANCE = 100. The probe maps X to -viewDepth ∈
        // [0, 120]. Past x corresponding to -viewDepth > 100 must emit the
        // past-max sentinel (0.25).
        //
        // Top half (y > kHeight/2) is out-of-frustum (projCoords.x = 2.0)
        // regardless of viewDepth. The past-max check runs BEFORE the
        // frustum check, so:
        //    past-max AND out-of-frustum  → 0.25 (past-max wins)
        //    in-range AND out-of-frustum  → 0.50
        //    in-range AND in-frustum      → 0.75
        //    past-max AND in-frustum      → 0.25

        // Threshold: pixel center UV (x + 0.5) / kWidth times 120 > 100
        // means the probe computed past-max. We bucket pixels as either
        // "definitely past-max", "definitely in-range", or a thin border
        // around the transition (skipped from hard assertions — the border
        // can shift by one pixel depending on floating-point rasterisation).
        constexpr f32 kMaxDist = 100.0f;
        constexpr f32 kSweep = 120.0f;

        u32 mismatches = 0;
        u32 borderSkipped = 0;
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4;
                const f32 sentinel = pixels[idx + 0];

                const f32 uvX = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kWidth);
                const f32 viewDepthMag = uvX * kSweep;

                // Skip a 2-pixel-wide transition band around the past-max boundary.
                if (std::abs(viewDepthMag - kMaxDist) < (kSweep / static_cast<f32>(kWidth)))
                {
                    ++borderSkipped;
                    continue;
                }

                const bool pastMax = viewDepthMag > kMaxDist;
                const bool outOfFrustum = (y >= kHeight / 2);

                f32 expected = 0.75f;
                if (pastMax)
                    expected = 0.25f;
                else if (outOfFrustum)
                    expected = 0.50f;

                if (std::abs(sentinel - expected) > 0.02f)
                {
                    if (mismatches < 8)
                    {
                        ADD_FAILURE() << "at (" << x << "," << y << "): sentinel=" << sentinel
                                      << " expected=" << expected
                                      << " viewDepthMag=" << viewDepthMag
                                      << " pastMax=" << pastMax << " outOfFrustum=" << outOfFrustum;
                    }
                    ++mismatches;
                }
            }
        }
        EXPECT_EQ(mismatches, 0u)
            << mismatches << " pixels mis-classified the shadow short-circuit path";
    }

    // =========================================================================
    // Shadow: cascade index selection is `for i in [0..4): if -viewDepth < CASCADE_PLANES[i] pick i`.
    // Cascade planes in the probe are (10, 25, 50, 100) world units.
    // The probe encodes cascade = ((index + 1) / 5) so:
    //   depth ∈ [0,10)    → index 0 → 0.20
    //   depth ∈ [10,25)   → index 1 → 0.40
    //   depth ∈ [25,50)   → index 2 → 0.60
    //   depth ∈ [50,100]  → index 3 → 0.80
    //   depth > 100       → index = -1 (past-max early-out) → 0.00
    //
    // Catches: cascade boundaries off-by-one, wrong CASCADE_PLANES swizzle.
    // =========================================================================
    TEST(ShadowBoundsTest, CascadeIndexSweepIsCorrect)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 240; // many samples along x
        constexpr u32 kHeight = 2;  // only in-frustum rows matter

        ShaderProbeHarness harness(kWidth, kHeight,
                                   "assets/shaders/tests/ShaderUnit_ShadowBounds.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadRgbaFloat(pixels);

        // Sample along the bottom row (in-frustum).
        struct Band
        {
            f32 minDepth, maxDepth;
            f32 expectedCascade;
        };
        const Band bands[] = {
            { 0.5f, 9.0f, 0.20f },   // cascade 0
            { 11.0f, 24.0f, 0.40f }, // cascade 1
            { 26.0f, 49.0f, 0.60f }, // cascade 2
            { 51.0f, 99.0f, 0.80f }, // cascade 3
        };

        for (const Band& b : bands)
        {
            // Pick midpoint of band in pixel coords.
            const f32 depthMid = 0.5f * (b.minDepth + b.maxDepth);
            const u32 x = static_cast<u32>(std::floor((depthMid / 120.0f) * kWidth));
            const std::size_t idx = (0u * kWidth + x) * 4;
            const f32 cascadeEncoded = pixels[idx + 1];
            EXPECT_NEAR(cascadeEncoded, b.expectedCascade, 0.01f)
                << "depth=" << depthMid << " expected cascade " << b.expectedCascade
                << " got " << cascadeEncoded;
        }
    }

    // =========================================================================
    // Terrain: a flat heightmap (all texels equal) must produce exact
    // (0, 1, 0) world-space normals via the production Terrain_PBR
    // 4-tap central-difference kernel. Any non-zero dX or dZ indicates
    // a derivative-kernel bug or an axis-sign error.
    //
    // Test setup: create a 32×32 R8 texture with all texels = 128 (= 0.5
    // after normalisation). hL/hR/hD/hU all equal → dX = dZ = 0 →
    // normalize(vec3(0, 1, 0)) = (0, 1, 0).
    //
    // Catches: off-by-one in texel offsets, swapped x/z derivatives, wrong
    // normalisation factor, accidental dependency on heightScale.
    // =========================================================================
    TEST(TerrainHeightmapTest, FlatHeightmapProducesUpNormal)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kRes = 32;

        // Build a flat R8 heightmap of constant value 128.
        std::vector<u8> flatHeights(static_cast<std::size_t>(kRes) * kRes, 128);

        GLuint heightTex = 0;
        ::glCreateTextures(GL_TEXTURE_2D, 1, &heightTex);
        ::glTextureStorage2D(heightTex, 1, GL_R8, kRes, kRes);
        ::glTextureSubImage2D(heightTex, 0, 0, 0, kRes, kRes, GL_RED, GL_UNSIGNED_BYTE, flatHeights.data());
        ::glTextureParameteri(heightTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        ::glTextureParameteri(heightTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        ::glTextureParameteri(heightTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        ::glTextureParameteri(heightTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        ShaderProbeHarness harness(64, 64,
                                   "assets/shaders/tests/ShaderUnit_TerrainFlatHeightmap.glsl");
        harness.Draw(static_cast<u32>(heightTex));

        std::vector<f32> pixels;
        harness.ReadRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(64) * 64 * 4);

        // Every pixel must be (0, 1, 0) ±epsilon. We skip the 1-pixel border
        // because CLAMP_TO_EDGE sampling at the image edges can introduce
        // tiny asymmetries (edge texel replicated on one side, true neighbour
        // on the other). Interior pixels have the full 4-tap kernel.
        u32 violations = 0;
        for (u32 y = 1; y < 63; ++y)
        {
            for (u32 x = 1; x < 63; ++x)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * 64 + x) * 4;
                const f32 nx = pixels[idx + 0];
                const f32 ny = pixels[idx + 1];
                const f32 nz = pixels[idx + 2];

                if (std::abs(nx) > 1e-3f || std::abs(ny - 1.0f) > 1e-3f || std::abs(nz) > 1e-3f)
                {
                    if (violations < 4)
                    {
                        ADD_FAILURE() << "at (" << x << "," << y << ") normal=("
                                      << nx << "," << ny << "," << nz
                                      << ") expected (0, 1, 0)";
                    }
                    ++violations;
                }
            }
        }
        EXPECT_EQ(violations, 0u) << violations << " interior pixels had non-up normals";

        ::glDeleteTextures(1, &heightTex);
    }

    // =========================================================================
    // Splatmap channel isolation. Exercises the blend math that maps a
    // 4-channel splatmap texel (r, g, b, a) onto 4 layers of a
    // sampler2DArray. The probe shader (ShaderUnit_SplatmapChannel.glsl)
    // does exactly:
    //     blended = w.r*layer0 + w.g*layer1 + w.b*layer2 + w.a*layer3
    //
    // We author a 1x1 sampler2DArray with 4 distinct solid-color layers and
    // a 1x1 splatmap texel carrying the per-test weight vector. For each
    // isolated channel we expect the output to equal exactly that layer's
    // color. For a 50/50 mix of two layers we expect the linear blend.
    //
    // Catches: swizzle inversions (w.rgba ↔ layer 0..3 mismapping),
    // array-layer index off-by-one, renormalisation bugs, and broken
    // sampler2DArray binding wiring (binding 20, matches Terrain_PBR).
    // =========================================================================
    namespace
    {
        // Creates a 1x1 sampler2DArray with 4 layers, each filled with a
        // single solid RGBA float color.
        u32 CreateSolidArray4(const std::array<std::array<f32, 4>, 4>& layers)
        {
            GLuint id = 0;
            ::glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &id);
            ::glTextureStorage3D(id, 1, GL_RGBA32F, 1, 1, 4);
            ::glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            ::glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            ::glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            ::glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            for (u32 layer = 0; layer < 4; ++layer)
            {
                ::glTextureSubImage3D(id, 0,
                                      0, 0, static_cast<GLint>(layer),
                                      1, 1, 1,
                                      GL_RGBA, GL_FLOAT,
                                      layers[layer].data());
            }
            return static_cast<u32>(id);
        }

        u32 CreateSolidSplat(f32 r, f32 g, f32 b, f32 a)
        {
            GLuint id = 0;
            ::glCreateTextures(GL_TEXTURE_2D, 1, &id);
            ::glTextureStorage2D(id, 1, GL_RGBA32F, 1, 1);
            ::glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            ::glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            ::glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            ::glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            f32 weights[4] = { r, g, b, a };
            ::glTextureSubImage2D(id, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, weights);
            return static_cast<u32>(id);
        }
    } // namespace

    TEST(TerrainSplatmapTest, ChannelIsolationMapsToCorrectLayer)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kSize = 8;

        // Four distinct layer colors. Keep them well-separated so a
        // swizzle bug produces a big visible delta.
        const std::array<std::array<f32, 4>, 4> layerColors = { {
            { { 1.0f, 0.0f, 0.0f, 1.0f } }, // layer 0: red
            { { 0.0f, 1.0f, 0.0f, 1.0f } }, // layer 1: green
            { { 0.0f, 0.0f, 1.0f, 1.0f } }, // layer 2: blue
            { { 1.0f, 1.0f, 0.0f, 1.0f } }, // layer 3: yellow
        } };

        const u32 layerArray = CreateSolidArray4(layerColors);
        ::glBindTextureUnit(20, static_cast<GLuint>(layerArray));

        ShaderProbeHarness harness(kSize, kSize, "assets/shaders/tests/ShaderUnit_SplatmapChannel.glsl");

        // Test cases: (splat weights) → (expected RGB).
        struct Case
        {
            const char* name;
            std::array<f32, 4> weights;
            std::array<f32, 3> expected;
        };
        const Case cases[] = {
            { "channel0 isolates layer 0 (red)", { 1.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } },
            { "channel1 isolates layer 1 (green)", { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
            { "channel2 isolates layer 2 (blue)", { 0.0f, 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
            { "channel3 isolates layer 3 (yellow)", { 0.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 0.0f } },
            { "50/50 layer0+layer1 == olive", { 0.5f, 0.5f, 0.0f, 0.0f }, { 0.5f, 0.5f, 0.0f } },
        };

        for (const auto& tc : cases)
        {
            const u32 splat = CreateSolidSplat(tc.weights[0], tc.weights[1],
                                               tc.weights[2], tc.weights[3]);
            ::glBindTextureUnit(24, static_cast<GLuint>(splat));

            harness.Draw(0);

            std::vector<f32> pixels;
            harness.ReadRgbaFloat(pixels);
            ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kSize) * kSize * 4);

            // Sample centre pixel; all pixels should be uniform since input is 1x1.
            const std::size_t centre = (static_cast<std::size_t>(kSize / 2) * kSize + kSize / 2) * 4;
            EXPECT_NEAR(pixels[centre + 0], tc.expected[0], 1e-4f) << tc.name << " (R)";
            EXPECT_NEAR(pixels[centre + 1], tc.expected[1], 1e-4f) << tc.name << " (G)";
            EXPECT_NEAR(pixels[centre + 2], tc.expected[2], 1e-4f) << tc.name << " (B)";

            ::glDeleteTextures(1, &splat);
        }

        ::glDeleteTextures(1, &layerArray);
    }
} // namespace OloEngine::Tests
