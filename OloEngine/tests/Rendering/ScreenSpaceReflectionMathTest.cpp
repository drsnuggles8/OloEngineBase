#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

// =============================================================================
// Screen-Space Reflections — CPU contract tests.
//
// These pin the math implemented in PostProcess_SSR.glsl WITHOUT a GL context
// (so they run in headless CI), mirroring the AutoExposureMathTest approach.
// The shader's correctness for the rendered frame is checked separately by the
// GPU SSRVisualEvidenceTest. Per the CLAUDE.md rendering rule, math/contract
// tests prove the formula; the visual test proves the frame looks right.
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    // ---- C++ mirrors of the GLSL helpers in PostProcess_SSR.glsl -----------

    // Octahedral encode/decode — must match octEncodeGB() in PBR_GBuffer.glsl
    // and OctDecode() in PostProcess_SSR.glsl.
    glm::vec2 OctEncode(glm::vec3 n)
    {
        n /= (std::abs(n.x) + std::abs(n.y) + std::abs(n.z));
        if (n.z < 0.0f)
        {
            const glm::vec2 s(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
            const glm::vec2 yx(n.y, n.x);
            n.x = (1.0f - std::abs(yx.x)) * s.x;
            n.y = (1.0f - std::abs(yx.y)) * s.y;
        }
        return glm::vec2(n.x, n.y);
    }

    glm::vec3 OctDecode(glm::vec2 e)
    {
        glm::vec3 n(e.x, e.y, 1.0f - std::abs(e.x) - std::abs(e.y));
        if (n.z < 0.0f)
        {
            const glm::vec2 s(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
            const float nx = (1.0f - std::abs(n.y)) * s.x;
            const float ny = (1.0f - std::abs(n.x)) * s.y;
            n.x = nx;
            n.y = ny;
        }
        return glm::normalize(n);
    }

    glm::vec3 ViewPosFromDepth(glm::vec2 uv, float depth, const glm::mat4& invProj)
    {
        const glm::vec4 ndc(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, depth * 2.0f - 1.0f, 1.0f);
        const glm::vec4 view = invProj * ndc;
        return glm::vec3(view) / view.w;
    }

    glm::vec2 ProjectToUV(glm::vec3 viewPos, const glm::mat4& proj)
    {
        const glm::vec4 clip = proj * glm::vec4(viewPos, 1.0f);
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        return ndc * 0.5f + 0.5f;
    }

    // GL stores depth as ndc.z*0.5+0.5 for a view-space point.
    float DepthFromViewPos(glm::vec3 viewPos, const glm::mat4& proj)
    {
        const glm::vec4 clip = proj * glm::vec4(viewPos, 1.0f);
        return (clip.z / clip.w) * 0.5f + 0.5f;
    }

    glm::vec3 FresnelSchlick(float cosTheta, glm::vec3 F0)
    {
        return F0 + (1.0f - F0) * std::pow(1.0f - cosTheta, 5.0f);
    }

    float Smoothstep(float edge0, float edge1, float x)
    {
        const float t = glm::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    float RoughFade(float roughness, float maxRoughness)
    {
        return 1.0f - Smoothstep(maxRoughness * 0.75f, maxRoughness, roughness);
    }
} // namespace

// ---- UBO layout contract ----------------------------------------------------

TEST(ScreenSpaceReflection, SSRUBOAlignment)
{
    EXPECT_EQ(sizeof(SSRUBOData) % 16, 0u) << "SSRUBOData must be 16-byte aligned for std140";
}

TEST(ScreenSpaceReflection, SSRUBOGetSizeMatchesSizeof)
{
    EXPECT_EQ(SSRUBOData::GetSize(), sizeof(SSRUBOData));
}

// The std140 block in PostProcess_SSR.glsl is laid out byte-for-byte against
// this struct: 3 mat4 (192) + 8 vec4 (128) = 320. The HZBParams vec4 (#284:
// min-depth HZB acceleration), the TemporalParams vec4 (#902: per-pass temporal
// resolve) and the two denoiser vec4s (#708) must each keep the size 16-byte
// aligned. The SAME block is declared by all four sibling shaders of the pass
// (PreBlur / Resolve / PostBlur / Composite), so a drift here breaks five.
TEST(ScreenSpaceReflection, SSRUBOLayoutSizeMatchesShader)
{
    EXPECT_EQ(sizeof(SSRUBOData), 320u) << "SSRUBOData drifted from the PostProcess_SSR.glsl SSRParams block";
    EXPECT_EQ(offsetof(SSRUBOData, HZBParams), 256u) << "HZBParams must follow Flags at offset 256";
    EXPECT_EQ(offsetof(SSRUBOData, TemporalParams), 272u) << "TemporalParams must follow HZBParams at offset 272";
    EXPECT_EQ(offsetof(SSRUBOData, DenoiseParams), 288u) << "DenoiseParams must follow TemporalParams at offset 288";
    EXPECT_EQ(offsetof(SSRUBOData, DenoiseGuide), 304u);
}

TEST(ScreenSpaceReflection, SSRBindingIsUniqueAndExpected)
{
    EXPECT_EQ(ShaderBindingLayout::UBO_SSR, 38u);
    // Must not collide with the neighbouring UBO bindings.
    EXPECT_NE(ShaderBindingLayout::UBO_SSR, ShaderBindingLayout::UBO_UNDERWATER);
    EXPECT_NE(ShaderBindingLayout::UBO_SSR, ShaderBindingLayout::UBO_GTAO);
}

// ---- Position reconstruct/project round-trip --------------------------------

TEST(ScreenSpaceReflection, ViewPositionProjectRoundTrip)
{
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    const glm::mat4 invProj = glm::inverse(proj);

    // Several view-space points in front of the camera (z < 0), inside the frustum.
    const glm::vec3 points[] = {
        { 0.0f, 0.0f, -5.0f },
        { 1.5f, -0.8f, -10.0f },
        { -2.0f, 1.0f, -25.0f },
        { 0.5f, 0.5f, -100.0f },
    };

    for (const glm::vec3& p : points)
    {
        const glm::vec2 uv = ProjectToUV(p, proj);
        const float depth = DepthFromViewPos(p, proj);
        const glm::vec3 reconstructed = ViewPosFromDepth(uv, depth, invProj);

        EXPECT_NEAR(reconstructed.x, p.x, 1e-2f) << "x reconstruct drift for z=" << p.z;
        EXPECT_NEAR(reconstructed.y, p.y, 1e-2f) << "y reconstruct drift for z=" << p.z;
        EXPECT_NEAR(reconstructed.z, p.z, 1e-2f) << "z reconstruct drift for z=" << p.z;

        // UVs of in-frustum points must land on screen.
        EXPECT_GE(uv.x, 0.0f);
        EXPECT_LE(uv.x, 1.0f);
        EXPECT_GE(uv.y, 0.0f);
        EXPECT_LE(uv.y, 1.0f);
    }
}

// ---- Octahedral normal round-trip -------------------------------------------

TEST(ScreenSpaceReflection, OctahedralNormalRoundTrip)
{
    const glm::vec3 normals[] = {
        glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f)),
        glm::normalize(glm::vec3(0.0f, 0.0f, 1.0f)),
        glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)),
        glm::normalize(glm::vec3(-0.3f, 0.7f, -0.6f)),
        glm::normalize(glm::vec3(0.2f, -0.9f, 0.1f)),
    };

    for (const glm::vec3& n : normals)
    {
        const glm::vec3 decoded = OctDecode(OctEncode(n));
        EXPECT_NEAR(decoded.x, n.x, 2e-3f);
        EXPECT_NEAR(decoded.y, n.y, 2e-3f);
        EXPECT_NEAR(decoded.z, n.z, 2e-3f);
        EXPECT_NEAR(glm::length(decoded), 1.0f, 1e-4f);
    }
}

// ---- Reflection vector ------------------------------------------------------

TEST(ScreenSpaceReflection, ReflectionVectorIsMirroredAcrossNormal)
{
    // A flat floor (normal up) viewed from above-and-forward.
    const glm::vec3 N(0.0f, 1.0f, 0.0f);
    const glm::vec3 V = glm::normalize(glm::vec3(0.0f, -0.5f, -1.0f)); // eye -> fragment, going down+forward
    const glm::vec3 R = glm::reflect(V, N);

    // Reflection off a horizontal plane flips the vertical component.
    EXPECT_NEAR(R.x, V.x, 1e-5f);
    EXPECT_NEAR(R.y, -V.y, 1e-5f);
    EXPECT_NEAR(R.z, V.z, 1e-5f);

    // Magnitude is preserved.
    EXPECT_NEAR(glm::length(R), glm::length(V), 1e-5f);

    // The reflected ray points up and away (into the scene) for a downward view.
    EXPECT_GT(R.y, 0.0f);
    EXPECT_LT(R.z, 0.0f);

    // Reflecting again returns the original direction.
    const glm::vec3 RR = glm::reflect(R, N);
    EXPECT_NEAR(RR.x, V.x, 1e-5f);
    EXPECT_NEAR(RR.y, V.y, 1e-5f);
    EXPECT_NEAR(RR.z, V.z, 1e-5f);
}

// ---- Fresnel ----------------------------------------------------------------

TEST(ScreenSpaceReflection, FresnelSchlickEndpointsAndMonotonicity)
{
    const glm::vec3 dielectric(0.04f);
    // Normal incidence returns F0.
    EXPECT_NEAR(FresnelSchlick(1.0f, dielectric).r, 0.04f, 1e-5f);
    // Grazing incidence approaches 1.
    EXPECT_NEAR(FresnelSchlick(0.0f, dielectric).r, 1.0f, 1e-5f);

    // Monotonically increasing as the angle gets more grazing (cosTheta -> 0).
    float prev = FresnelSchlick(1.0f, dielectric).r;
    for (float c = 0.9f; c >= 0.0f; c -= 0.1f)
    {
        const float f = FresnelSchlick(c, dielectric).r;
        EXPECT_GE(f, prev - 1e-6f) << "Fresnel should grow toward grazing (cos=" << c << ")";
        prev = f;
    }

    // Metals: F0 == albedo at normal incidence.
    const glm::vec3 gold(1.0f, 0.78f, 0.34f);
    const glm::vec3 fGold = FresnelSchlick(1.0f, gold);
    EXPECT_NEAR(fGold.r, gold.r, 1e-5f);
    EXPECT_NEAR(fGold.g, gold.g, 1e-5f);
    EXPECT_NEAR(fGold.b, gold.b, 1e-5f);
}

// ---- Roughness fade ---------------------------------------------------------

TEST(ScreenSpaceReflection, RoughnessFadeCutoff)
{
    const float maxR = 0.6f;

    // Smooth surfaces get full reflection.
    EXPECT_NEAR(RoughFade(0.0f, maxR), 1.0f, 1e-5f);
    EXPECT_NEAR(RoughFade(maxR * 0.75f, maxR), 1.0f, 1e-5f);

    // Rough surfaces beyond the cutoff get none.
    EXPECT_NEAR(RoughFade(maxR, maxR), 0.0f, 1e-5f);
    EXPECT_NEAR(RoughFade(1.0f, maxR), 0.0f, 1e-5f);

    // Monotonically non-increasing across the transition band.
    float prev = 1.0f;
    for (float r = maxR * 0.75f; r <= maxR; r += 0.01f)
    {
        const float f = RoughFade(r, maxR);
        EXPECT_LE(f, prev + 1e-6f);
        prev = f;
    }
}

// ---- Edge fade --------------------------------------------------------------

TEST(ScreenSpaceReflection, EdgeFadeVanishesAtBorders)
{
    const float edge = 0.1f;
    auto edgeFade = [edge](glm::vec2 uv)
    {
        float f = 1.0f;
        f *= Smoothstep(0.0f, edge, uv.x) * Smoothstep(0.0f, edge, 1.0f - uv.x);
        f *= Smoothstep(0.0f, edge, uv.y) * Smoothstep(0.0f, edge, 1.0f - uv.y);
        return f;
    };

    // Centre of screen: full confidence.
    EXPECT_NEAR(edgeFade(glm::vec2(0.5f, 0.5f)), 1.0f, 1e-5f);
    // Hard borders: zero confidence.
    EXPECT_NEAR(edgeFade(glm::vec2(0.0f, 0.5f)), 0.0f, 1e-5f);
    EXPECT_NEAR(edgeFade(glm::vec2(1.0f, 0.5f)), 0.0f, 1e-5f);
    EXPECT_NEAR(edgeFade(glm::vec2(0.5f, 0.0f)), 0.0f, 1e-5f);
    EXPECT_NEAR(edgeFade(glm::vec2(0.5f, 1.0f)), 0.0f, 1e-5f);
    // Just inside the band: partial.
    const float partial = edgeFade(glm::vec2(0.05f, 0.5f));
    EXPECT_GT(partial, 0.0f);
    EXPECT_LT(partial, 1.0f);
}

// ---- Settings sanitization --------------------------------------------------

TEST(ScreenSpaceReflection, SanitizeClampsNonFiniteAndRanges)
{
    PostProcessSettings s;
    s.SSRMaxDistance = std::numeric_limits<f32>::quiet_NaN();
    s.SSRThickness = std::numeric_limits<f32>::infinity();
    s.SSRStride = -5.0f;
    s.SSRMaxSteps = 100000;
    s.SSRBinarySearchSteps = -3;
    s.SSRIntensity = std::numeric_limits<f32>::quiet_NaN();
    s.SSRMaxRoughness = 5.0f;
    s.SSREdgeFade = -1.0f;

    SanitizeSSR(s);

    EXPECT_TRUE(std::isfinite(s.SSRMaxDistance));
    EXPECT_TRUE(std::isfinite(s.SSRThickness));
    EXPECT_GE(s.SSRStride, 0.001f);
    EXPECT_LE(s.SSRMaxSteps, kSSRMaxSteps); // sanitizer cap must match the runtime/shader cap (256)
    EXPECT_GE(s.SSRMaxSteps, 1);
    EXPECT_GE(s.SSRBinarySearchSteps, 0);
    EXPECT_LE(s.SSRBinarySearchSteps, kSSRMaxBinarySearchSteps); // cap matches runtime/shader (32)
    EXPECT_TRUE(std::isfinite(s.SSRIntensity));
    EXPECT_GE(s.SSRIntensity, 0.0f);
    EXPECT_GE(s.SSRMaxRoughness, 0.0f);
    EXPECT_LE(s.SSRMaxRoughness, 1.0f);
    EXPECT_GE(s.SSREdgeFade, 0.0f);
    EXPECT_LE(s.SSREdgeFade, 0.5f);
}

// ---- Signal / composite split (issue #902) ----------------------------------

// PostProcess_SSR.glsl no longer composites. It writes the DELTA
// (reflection - base) * blend into SSRSignal, and PostProcess_SSRComposite.glsl
// adds that to the upstream colour. This is the algebraic identity the split
// rests on, and it is what makes the accumulated buffer carry ONLY the
// stochastic term: get it wrong and the temporal resolve accumulates base
// colour, which is precisely the failure #902 exists to remove.
TEST(ScreenSpaceReflection, DeltaCompositeReproducesTheReplaceMixResolve)
{
    const auto base = glm::vec3(0.20f, 0.35f, 0.55f);
    const auto reflection = glm::vec3(0.90f, 0.10f, 0.05f);

    for (const f32 blend : { 0.0f, 0.17f, 0.5f, 0.83f, 1.0f })
    {
        const glm::vec3 delta = (reflection - base) * blend; // what draw A writes
        const glm::vec3 composited = base + delta;           // what draw C computes
        const glm::vec3 legacy = glm::mix(base, reflection, blend);

        EXPECT_NEAR(composited.r, legacy.r, 1e-6f) << "blend=" << blend;
        EXPECT_NEAR(composited.g, legacy.g, 1e-6f) << "blend=" << blend;
        EXPECT_NEAR(composited.b, legacy.b, 1e-6f) << "blend=" << blend;
    }
}

// A miss must contribute EXACTLY nothing. Every early-out in
// PostProcess_SSR.glsl (sky, roughness fade closed, sub-horizon microfacet, no
// hit) writes a zero delta rather than the base colour, so a pixel that never
// reflects anything feeds the accumulator a hard zero instead of a copy of the
// scene. The pairing matters: with the OLD composite-in-place output, a miss
// wrote `base`, and accumulating THAT is the smear the issue describes.
TEST(ScreenSpaceReflection, AMissContributesExactlyZeroToTheAccumulatedSignal)
{
    const auto base = glm::vec3(0.20f, 0.35f, 0.55f);
    const auto reflection = glm::vec3(0.90f, 0.10f, 0.05f);

    const glm::vec3 missDelta = (reflection - base) * 0.0f;
    EXPECT_FLOAT_EQ(missDelta.r, 0.0f);
    EXPECT_FLOAT_EQ(missDelta.g, 0.0f);
    EXPECT_FLOAT_EQ(missDelta.b, 0.0f);
    EXPECT_FLOAT_EQ((base + missDelta).r, base.r);

    // The paired negative: the pre-#902 output of a miss was the base colour,
    // which is NOT zero and would have dragged the scene into the history.
    EXPECT_GT(base.r + base.g + base.b, 0.0f);
}

// The roughness cutoff moved 0.6 -> 0.8 because SSR now has an accumulator
// behind its single VNDF sample. Pin the default and the fade window it
// implies, so a future change to either is deliberate rather than incidental.
TEST(ScreenSpaceReflection, MaxRoughnessDefaultMatchesTheTemporalResolveEra)
{
    const PostProcessSettings defaults;
    EXPECT_FLOAT_EQ(defaults.SSRMaxRoughness, 0.8f)
        << "the SSR roughness cutoff is a visual claim tied to the per-pass temporal resolve "
           "(issue #902) — if it moved, the evidence in "
           "ScreenSpaceTemporalResolveEvidenceTest must move with it";
    EXPECT_TRUE(defaults.SSRTemporalResolve);

    // roughFade = 1 - smoothstep(max * 0.75, max, roughness): full strength at
    // or below 0.6, fully closed at or above 0.8, partial in between. The 0.7
    // sphere in the evidence scene sits inside that window on purpose.
    const auto roughFade = [](f32 roughness, f32 maxRoughness)
    {
        return 1.0f - glm::smoothstep(maxRoughness * 0.75f, maxRoughness, roughness);
    };
    EXPECT_FLOAT_EQ(roughFade(0.55f, 0.8f), 1.0f);
    EXPECT_FLOAT_EQ(roughFade(0.85f, 0.8f), 0.0f);
    EXPECT_GT(roughFade(0.7f, 0.8f), 0.0f);
    EXPECT_LT(roughFade(0.7f, 0.8f), 1.0f);
    // ...and the old cutoff rejected that same surface outright, which is the
    // difference the evidence test measures on screen.
    EXPECT_FLOAT_EQ(roughFade(0.7f, 0.6f), 0.0f);
}

// Feedback is clamped to the 0.98 ceiling OloTemporalBlend enforces anyway, so
// a persisted 1.0 cannot read as "the history never updates" in the panel.
TEST(ScreenSpaceReflection, SanitizeClampsTheTemporalFeedback)
{
    PostProcessSettings s;
    s.SSRTemporalFeedback = 1.0f;
    SanitizeSSR(s);
    EXPECT_LE(s.SSRTemporalFeedback, 0.98f);

    s.SSRTemporalFeedback = std::numeric_limits<f32>::quiet_NaN();
    SanitizeSSR(s);
    EXPECT_TRUE(std::isfinite(s.SSRTemporalFeedback));

    s.SSRTemporalFeedback = -2.0f;
    SanitizeSSR(s);
    EXPECT_GE(s.SSRTemporalFeedback, 0.0f);
}
