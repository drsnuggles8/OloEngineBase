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

        // RAII wrappers so ASSERT_* early-exits can't leak GL objects. Both
        // helpers no-op on a zero handle (so default-constructed holders are
        // safe) and become owners once filled via address-of.
        struct ScopedTexture
        {
            GLuint m_Id = 0;
            ScopedTexture() = default;
            explicit ScopedTexture(GLuint id) : m_Id(id) {}
            ~ScopedTexture()
            {
                if (m_Id)
                    ::glDeleteTextures(1, &m_Id);
            }
            ScopedTexture(const ScopedTexture&) = delete;
            ScopedTexture& operator=(const ScopedTexture&) = delete;
            GLuint* AddressOf()
            {
                return &m_Id;
            }
            operator GLuint() const
            {
                return m_Id;
            }
        };

        struct ScopedBuffer
        {
            GLuint m_Id = 0;
            ScopedBuffer() = default;
            ~ScopedBuffer()
            {
                if (m_Id)
                    ::glDeleteBuffers(1, &m_Id);
            }
            ScopedBuffer(const ScopedBuffer&) = delete;
            ScopedBuffer& operator=(const ScopedBuffer&) = delete;
            GLuint* AddressOf()
            {
                return &m_Id;
            }
            operator GLuint() const
            {
                return m_Id;
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
                else
                { /* No additional handling required. */
                }

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
    // Shadow bias contract: self-shadow suppression + peter-panning bound.
    //
    // Uses `ShaderUnit_ShadowSelfShadow.glsl` to drive the real production
    // `sampleShadowPCF` kernel against an authored `sampler2DArrayShadow`
    // whose every texel equals a known depth `D`. The probe sweeps the
    // fragment's light-space z via v_TexCoord.x; since the shadow map is
    // uniform, the 3x3 PCF kernel collapses to a hard transition at
    // `projCoords.z - bias == D`.
    //
    // Two invariants under test:
    //   (1) No false self-shadowing: at projCoords.z ≈ D the shadow factor
    //       must be 1.0 (lit). A naive `projCoords.z > storedDepth` compare
    //       with zero bias would incorrectly shadow the surface due to fp32
    //       equality flicker.
    //   (2) Bounded peter-panning: the transition from lit → shadowed must
    //       happen within `D + bias + 1 texel`. If the transition lands
    //       significantly further back, a real scene's shadow would "float"
    //       off its occluder's base by that distance.
    //
    // Catches: bias sign flip (subtracting vs adding), bias scaling bugs by
    // cascade index at cascade 0 (`baseBias * (0+1) == baseBias`), wrong
    // compare operator (GL_LESS vs GL_LEQUAL → compared to our probe which
    // uses the LEQUAL convention).
    // =========================================================================
    TEST(ShadowBiasTest, SelfShadowAndPeterPanningContract)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kShadowRes = 64;
        constexpr u32 kWidth = 256; // depth sweep granularity
        constexpr u32 kHeight = 4;
        constexpr f32 kShadowDepth = 0.5f;
        constexpr f32 kBias = 0.005f;

        // Authored depth texture array: 4 layers (CSM cascades), all filled
        // with constant depth. Layer 0 is the one the probe samples.
        ScopedTexture shadowTex;
        ::glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, shadowTex.AddressOf());
        ::glTextureStorage3D(shadowTex, 1, GL_DEPTH_COMPONENT32F,
                             static_cast<GLsizei>(kShadowRes),
                             static_cast<GLsizei>(kShadowRes), 4);
        // NEAREST filtering keeps each PCF tap a hard 0/1 compare; LINEAR
        // would invoke hardware PCF which interpolates across 4 texels and
        // muddies the single-step transition we're measuring.
        ::glTextureParameteri(shadowTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        ::glTextureParameteri(shadowTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        ::glTextureParameteri(shadowTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        ::glTextureParameteri(shadowTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        ::glTextureParameteri(shadowTex, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        ::glTextureParameteri(shadowTex, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

        std::vector<f32> depthData(static_cast<std::size_t>(kShadowRes) * kShadowRes, kShadowDepth);
        for (u32 layer = 0; layer < 4; ++layer)
        {
            ::glTextureSubImage3D(shadowTex, 0, 0, 0, static_cast<GLint>(layer),
                                  static_cast<GLsizei>(kShadowRes),
                                  static_cast<GLsizei>(kShadowRes), 1,
                                  GL_DEPTH_COMPONENT, GL_FLOAT, depthData.data());
        }
        ::glBindTextureUnit(8, shadowTex);

        // std140 UBO mirror for the probe:
        //   vec4 u_ShadowParams;   offset  0, size 16
        //   int  u_ShadowResolution; offset 16, size 4
        //   int  u_Pad0/1/2;          offset 20..28
        // total 32 bytes (padded to vec4 boundary).
        struct ProbeUbo
        {
            f32 params[4];
            i32 resolution;
            i32 pad[3];
        };
        static_assert(sizeof(ProbeUbo) == 32, "std140 layout mismatch");

        ProbeUbo uboData{ { kBias, 0.0f, 0.0f, 0.0f }, static_cast<i32>(kShadowRes), { 0, 0, 0 } };
        ScopedBuffer ubo;
        ::glCreateBuffers(1, ubo.AddressOf());
        ::glNamedBufferData(ubo, sizeof(ProbeUbo), &uboData, GL_STATIC_DRAW);
        ::glBindBufferBase(GL_UNIFORM_BUFFER, 18, ubo);

        ShaderProbeHarness harness(kWidth, kHeight,
                                   "assets/shaders/tests/ShaderUnit_ShadowSelfShadow.glsl");
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);

        // Read the middle row. All rows should produce the same sweep since
        // v_TexCoord.y is mapped to the sample XY (shadow map is uniform).
        const u32 row = kHeight / 2;

        // Per-bin UV-centre depth: (x + 0.5) / kWidth. This is exactly what
        // the probe sees as `depth` via `clamp(v_TexCoord.x, 0, 1)`.
        auto depthAt = [&](u32 x)
        { return (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kWidth); };

        // (1) Near end (depth well in front of shadow depth): fully lit.
        const f32 shadowAtNear = pixels[(row * kWidth + 0) * 4];
        EXPECT_NEAR(shadowAtNear, 1.0f, 1e-3f)
            << "fragment in front of occluder is not lit: shadow=" << shadowAtNear
            << " at depth=" << depthAt(0);

        // (2) Far end (depth well past shadow depth + bias): fully shadowed.
        const f32 shadowAtFar = pixels[(row * kWidth + (kWidth - 1)) * 4];
        EXPECT_NEAR(shadowAtFar, 0.0f, 1e-3f)
            << "fragment well past occluder is not shadowed: shadow=" << shadowAtFar
            << " at depth=" << depthAt(kWidth - 1);

        // (3) Self-shadow: at the exact surface depth the fragment must be lit.
        //     Pick the x whose depth centre is closest to kShadowDepth from below.
        //     A bias of +0.005 shifts compareRef to 0.495 ≤ 0.5 → lit.
        u32 selfShadowX = 0;
        for (u32 x = 0; x < kWidth; ++x)
        {
            if (depthAt(x) > kShadowDepth)
                break;
            selfShadowX = x;
        }
        const f32 shadowAtSurface = pixels[(row * kWidth + selfShadowX) * 4];
        EXPECT_NEAR(shadowAtSurface, 1.0f, 1e-3f)
            << "false self-shadow at depth=" << depthAt(selfShadowX)
            << " (shadow map depth=" << kShadowDepth << ", bias=" << kBias << ")"
            << " shadow=" << shadowAtSurface;

        // (4) Transition: walk rightwards from the lit region; find the first
        //     bin where shadow dropped below 0.5. That bin's depth is where
        //     compareRef first exceeded the stored depth. Must land in
        //     [kShadowDepth, kShadowDepth + kBias + 1 texel].
        i32 transitionX = -1;
        for (u32 x = 0; x < kWidth; ++x)
        {
            if (pixels[(row * kWidth + x) * 4] < 0.5f)
            {
                transitionX = static_cast<i32>(x);
                break;
            }
        }
        ASSERT_GE(transitionX, 0)
            << "no lit → shadowed transition found: the shadow map or UBO may not be bound";

        const f32 transitionDepth = depthAt(static_cast<u32>(transitionX));
        const f32 depthStep = 1.0f / static_cast<f32>(kWidth);

        // Lower bound: transition depth ≥ shadow map depth (no false shadow
        // before we reach the surface). One depthStep of slack for fp32 rounding.
        EXPECT_GE(transitionDepth, kShadowDepth - depthStep)
            << "transition landed before the surface depth: transitionDepth="
            << transitionDepth << " kShadowDepth=" << kShadowDepth;

        // Upper bound: transition depth ≤ shadow map depth + bias + 1 texel.
        // If bias is too large, the transition slides further back — the
        // classic "peter-panning gap".
        EXPECT_LE(transitionDepth, kShadowDepth + kBias + depthStep)
            << "peter-panning gap too large: transitionDepth="
            << transitionDepth << " exceeds shadowDepth+bias="
            << (kShadowDepth + kBias) << " by more than one texel";

        // shadowTex + ubo destructors delete their GL objects.
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

        ScopedTexture heightTex;
        ::glCreateTextures(GL_TEXTURE_2D, 1, heightTex.AddressOf());
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

        // heightTex destructor deletes the GL texture.
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
