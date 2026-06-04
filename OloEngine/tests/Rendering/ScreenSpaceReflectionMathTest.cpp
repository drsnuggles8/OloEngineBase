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
}

// ---- UBO layout contract ----------------------------------------------------

TEST(ScreenSpaceReflection, SSRUBOAlignment)
{
    EXPECT_EQ(sizeof(SSRUBOData) % 16, 0u) << "SSRUBOData must be 16-byte aligned for std140";
}

TEST(ScreenSpaceReflection, SSRUBOGetSizeMatchesSizeof)
{
    EXPECT_EQ(SSRUBOData::GetSize(), sizeof(SSRUBOData));
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
    auto edgeFade = [edge](glm::vec2 uv) {
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
    EXPECT_LE(s.SSRMaxSteps, 512);
    EXPECT_GE(s.SSRMaxSteps, 1);
    EXPECT_GE(s.SSRBinarySearchSteps, 0);
    EXPECT_TRUE(std::isfinite(s.SSRIntensity));
    EXPECT_GE(s.SSRIntensity, 0.0f);
    EXPECT_GE(s.SSRMaxRoughness, 0.0f);
    EXPECT_LE(s.SSRMaxRoughness, 1.0f);
    EXPECT_GE(s.SSREdgeFade, 0.0f);
    EXPECT_LE(s.SSREdgeFade, 0.5f);
}
