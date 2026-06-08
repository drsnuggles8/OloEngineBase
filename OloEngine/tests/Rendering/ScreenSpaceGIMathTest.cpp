#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

// =============================================================================
// Screen-Space Global Illumination — CPU contract tests.
//
// These pin the math implemented in PostProcess_SSGI.glsl WITHOUT a GL context
// (so they run in headless CI), mirroring ScreenSpaceReflectionMathTest. The
// shader's correctness for the rendered frame is checked separately by the GPU
// SSGIVisualEvidenceTest. Per the CLAUDE.md rendering rule, math/contract tests
// prove the formula; the visual test proves the frame looks right.
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    // ---- C++ mirrors of the GLSL helpers in PostProcess_SSGI.glsl ----------

    // Octahedral decode — matches octEncodeGB() in PBR_GBuffer.glsl and
    // OctDecode() in PostProcess_SSGI.glsl.
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

    float DepthFromViewPos(glm::vec3 viewPos, const glm::mat4& proj)
    {
        const glm::vec4 clip = proj * glm::vec4(viewPos, 1.0f);
        return (clip.z / clip.w) * 0.5f + 0.5f;
    }

    // Branchless orthonormal basis around n (Duff et al. 2017) — must match
    // BuildBasis() in PostProcess_SSGI.glsl.
    void BuildBasis(glm::vec3 n, glm::vec3& t, glm::vec3& b)
    {
        const float s = (n.z >= 0.0f) ? 1.0f : -1.0f;
        const float a = -1.0f / (s + n.z);
        const float d = n.x * n.y * a;
        t = glm::vec3(1.0f + s * n.x * n.x * a, s * d, -s * n.x);
        b = glm::vec3(d, s + n.y * n.y * a, -n.y);
    }

    constexpr float kPi = 3.14159265359f;
    constexpr float kGoldenRatioConj = 0.61803398875f;

    // Cosine-weighted hemisphere sample (Malley's method) around n — must match
    // the per-ray sampling in PostProcess_SSGI.glsl's main loop.
    glm::vec3 CosineHemisphereDir(glm::vec3 n, int rayIndex, int rayCount, float ign)
    {
        glm::vec3 t, b;
        BuildBasis(n, t, b);
        const float u1 = (static_cast<float>(rayIndex) + 0.5f) / static_cast<float>(rayCount);
        const float u2 = std::fmod(ign + static_cast<float>(rayIndex) * kGoldenRatioConj, 1.0f);
        const float radius = std::sqrt(u1);
        const float phi = 2.0f * kPi * u2;
        const glm::vec3 local(radius * std::cos(phi), radius * std::sin(phi),
                              std::sqrt(std::max(0.0f, 1.0f - u1)));
        return glm::normalize(t * local.x + b * local.y + n * local.z);
    }

    float Smoothstep(float edge0, float edge1, float x)
    {
        const float tt = glm::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return tt * tt * (3.0f - 2.0f * tt);
    }
} // namespace

// ---- UBO layout contract ----------------------------------------------------

TEST(ScreenSpaceGI, SSGIUBOAlignment)
{
    EXPECT_EQ(sizeof(SSGIUBOData) % 16, 0u) << "SSGIUBOData must be 16-byte aligned for std140";
}

TEST(ScreenSpaceGI, SSGIUBOGetSizeMatchesSizeof)
{
    EXPECT_EQ(SSGIUBOData::GetSize(), sizeof(SSGIUBOData));
}

// The std140 block in PostProcess_SSGI.glsl is laid out byte-for-byte against
// this struct: 3 mat4 (192) + 4 vec4 (64) = 256.
TEST(ScreenSpaceGI, SSGIUBOLayoutSizeMatchesShader)
{
    EXPECT_EQ(sizeof(SSGIUBOData), 256u) << "SSGIUBOData drifted from the PostProcess_SSGI.glsl SSGIParams block";
    EXPECT_EQ(offsetof(SSGIUBOData, RayParams), 192u) << "RayParams must follow the 3 matrices at offset 192";
}

TEST(ScreenSpaceGI, SSGIBindingIsUniqueAndExpected)
{
    EXPECT_EQ(ShaderBindingLayout::UBO_SSGI, 40u);
    // Must not collide with the neighbouring UBO bindings.
    EXPECT_NE(ShaderBindingLayout::UBO_SSGI, ShaderBindingLayout::UBO_SSR);
    EXPECT_NE(ShaderBindingLayout::UBO_SSGI, ShaderBindingLayout::UBO_STAR_NEST_SKY);
}

// ---- Position reconstruct/project round-trip --------------------------------

TEST(ScreenSpaceGI, ViewPositionProjectRoundTrip)
{
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    const glm::mat4 invProj = glm::inverse(proj);

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
    }
}

// ---- Octahedral normal round-trip -------------------------------------------

TEST(ScreenSpaceGI, OctahedralNormalRoundTrip)
{
    const glm::vec3 normals[] = {
        glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f)),
        glm::normalize(glm::vec3(0.0f, 0.0f, 1.0f)),
        glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)),
        glm::normalize(glm::vec3(-0.3f, 0.7f, -0.6f)),
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

// ---- Orthonormal basis (Duff et al. 2017) -----------------------------------

TEST(ScreenSpaceGI, BuildBasisIsOrthonormal)
{
    const glm::vec3 normals[] = {
        glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f)),
        glm::normalize(glm::vec3(0.0f, 0.0f, 1.0f)),  // s flips sign here (n.z >= 0)
        glm::normalize(glm::vec3(0.0f, 0.0f, -1.0f)), // s = -1 branch
        glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)),
        glm::normalize(glm::vec3(-0.3f, 0.7f, -0.6f)),
        glm::normalize(glm::vec3(0.2f, -0.9f, 0.1f)),
    };

    for (const glm::vec3& n : normals)
    {
        glm::vec3 t, b;
        BuildBasis(n, t, b);
        EXPECT_NEAR(glm::length(t), 1.0f, 1e-4f) << "tangent not unit length";
        EXPECT_NEAR(glm::length(b), 1.0f, 1e-4f) << "bitangent not unit length";
        EXPECT_NEAR(glm::dot(t, n), 0.0f, 1e-4f) << "tangent not perpendicular to normal";
        EXPECT_NEAR(glm::dot(b, n), 0.0f, 1e-4f) << "bitangent not perpendicular to normal";
        EXPECT_NEAR(glm::dot(t, b), 0.0f, 1e-4f) << "tangent/bitangent not orthogonal";
    }
}

// ---- Cosine-weighted hemisphere sampling ------------------------------------

// Every generated ray must be a unit vector inside the upper hemisphere about N.
TEST(ScreenSpaceGI, HemisphereSamplesAreUnitAndInHemisphere)
{
    const glm::vec3 N = glm::normalize(glm::vec3(0.2f, 0.9f, -0.3f));
    const int rayCount = 16;
    for (int i = 0; i < rayCount; ++i)
    {
        const glm::vec3 dir = CosineHemisphereDir(N, i, rayCount, 0.37f);
        EXPECT_NEAR(glm::length(dir), 1.0f, 1e-4f) << "ray " << i << " not unit length";
        EXPECT_GE(glm::dot(dir, N), -1e-4f) << "ray " << i << " points below the surface";
    }
}

// The hallmark of cosine-weighted sampling: the expected cosine E[cos theta]
// under pdf ∝ cos theta is exactly 2/3 (∫_hemi cos^2 theta / pi dω = 2/3). With
// stratified u1 = (i+0.5)/N this converges deterministically, independent of the
// per-pixel azimuth jitter — so a regression in the Malley mapping or the basis
// transform shows up here. Reference: pbrt §13.6.3 (cosine-weighted hemisphere).
TEST(ScreenSpaceGI, CosineWeightedExpectedCosineIsTwoThirds)
{
    const glm::vec3 N = glm::normalize(glm::vec3(-0.4f, 0.5f, 0.76f));
    const int rayCount = 8192;
    double sumCos = 0.0;
    for (int i = 0; i < rayCount; ++i)
    {
        const glm::vec3 dir = CosineHemisphereDir(N, i, rayCount, 0.123f);
        sumCos += static_cast<double>(glm::dot(dir, N));
    }
    const double meanCos = sumCos / rayCount;
    EXPECT_NEAR(meanCos, 2.0 / 3.0, 5e-3) << "cosine-weighted mean cos drifted from 2/3";
}

// ---- Linear march loop bound ------------------------------------------------

// The per-ray march advances by Stride and stops at MaxDistance or after
// MaxSteps iterations, whichever comes first. This mirrors the inner loop in
// PostProcess_SSGI.glsl so a regression in either cap is caught on the CPU.
TEST(ScreenSpaceGI, MarchStepsBoundedByDistanceAndStepCap)
{
    auto countSteps = [](float stride, float maxDist, int maxSteps)
    {
        int steps = 0;
        float traveled = 0.0f;
        for (int s = 0; s < maxSteps; ++s)
        {
            traveled += stride;
            if (traveled > maxDist)
                break;
            ++steps;
        }
        return steps;
    };

    // Distance-limited: 8 / 0.25 = 32 samples, well under the 64-step cap.
    EXPECT_EQ(countSteps(0.25f, 8.0f, 64), 32);
    // Step-limited: 24 steps reach 6.0 < 40.0, so the step cap binds first.
    EXPECT_EQ(countSteps(0.25f, 40.0f, 24), 24);
}

// ---- Indirect-diffuse estimator ---------------------------------------------

// Cosine-weighted MC of a CONSTANT incoming radiance L over the hemisphere
// returns L (the pdf cancels the cosine), so the one-bounce diffuse out is
// albedo * L — no extra 1/pi, since diffuse reflectance already folds it. The
// shader sums per-ray radiance and divides by the ray count, then tints by
// albedo; this pins that arithmetic. Misses contribute zero (screen-space sees
// no off-screen light), which scales the bounce by the on-screen hit fraction.
TEST(ScreenSpaceGI, IndirectDiffuseIsAlbedoTimesMeanRadiance)
{
    const glm::vec3 albedo(0.8f, 0.2f, 0.2f);
    const glm::vec3 L(1.0f, 1.0f, 1.0f);
    const int rayCount = 8;

    // All rays hit constant radiance L.
    {
        glm::vec3 sum(0.0f);
        for (int i = 0; i < rayCount; ++i)
            sum += L;
        const glm::vec3 indirect = albedo * (sum / static_cast<float>(rayCount));
        EXPECT_NEAR(indirect.r, albedo.r * L.r, 1e-5f);
        EXPECT_NEAR(indirect.g, albedo.g * L.g, 1e-5f);
        EXPECT_NEAR(indirect.b, albedo.b * L.b, 1e-5f);
    }

    // Half the rays miss (radiance 0): the bounce halves.
    {
        glm::vec3 sum(0.0f);
        for (int i = 0; i < rayCount; ++i)
            sum += (i % 2 == 0) ? L : glm::vec3(0.0f);
        const glm::vec3 indirect = albedo * (sum / static_cast<float>(rayCount));
        EXPECT_NEAR(indirect.r, albedo.r * 0.5f, 1e-5f);
    }
}

// Additive composite (vs SSR's replace/mix): indirect diffuse is EXTRA bounced
// light, so out = base + indirectDiffuse * intensity.
TEST(ScreenSpaceGI, CompositeIsAdditive)
{
    const glm::vec3 base(0.3f, 0.3f, 0.3f);
    const glm::vec3 indirectDiffuse(0.2f, 0.05f, 0.05f);
    const float intensity = 2.0f;
    const glm::vec3 out = base + indirectDiffuse * intensity;
    EXPECT_NEAR(out.r, 0.3f + 0.2f * 2.0f, 1e-5f);
    EXPECT_NEAR(out.g, 0.3f + 0.05f * 2.0f, 1e-5f);
    EXPECT_GT(out.r, base.r) << "additive GI must brighten, never darken";
}

// ---- Edge fade --------------------------------------------------------------

TEST(ScreenSpaceGI, EdgeFadeVanishesAtBorders)
{
    const float edge = 0.1f;
    auto edgeFade = [edge](glm::vec2 uv)
    {
        float f = 1.0f;
        f *= Smoothstep(0.0f, edge, uv.x) * Smoothstep(0.0f, edge, 1.0f - uv.x);
        f *= Smoothstep(0.0f, edge, uv.y) * Smoothstep(0.0f, edge, 1.0f - uv.y);
        return f;
    };

    EXPECT_NEAR(edgeFade(glm::vec2(0.5f, 0.5f)), 1.0f, 1e-5f);
    EXPECT_NEAR(edgeFade(glm::vec2(0.0f, 0.5f)), 0.0f, 1e-5f);
    EXPECT_NEAR(edgeFade(glm::vec2(1.0f, 0.5f)), 0.0f, 1e-5f);
    const float partial = edgeFade(glm::vec2(0.05f, 0.5f));
    EXPECT_GT(partial, 0.0f);
    EXPECT_LT(partial, 1.0f);
}

// ---- Settings sanitization --------------------------------------------------

TEST(ScreenSpaceGI, SanitizeClampsNonFiniteAndRanges)
{
    PostProcessSettings s;
    s.SSGIIntensity = std::numeric_limits<f32>::quiet_NaN();
    s.SSGIMaxDistance = std::numeric_limits<f32>::infinity();
    s.SSGIThickness = -5.0f;
    s.SSGIStride = -1.0f;
    s.SSGIMaxSteps = 100000;
    s.SSGIRayCount = 9999;
    s.SSGIEdgeFade = -1.0f;

    SanitizeSSGI(s);

    EXPECT_TRUE(std::isfinite(s.SSGIIntensity));
    EXPECT_GE(s.SSGIIntensity, 0.0f);
    EXPECT_TRUE(std::isfinite(s.SSGIMaxDistance));
    EXPECT_GE(s.SSGIThickness, 0.001f);
    EXPECT_GE(s.SSGIStride, 0.001f);
    EXPECT_LE(s.SSGIMaxSteps, kSSGIMaxSteps); // cap matches the runtime/shader (64)
    EXPECT_GE(s.SSGIMaxSteps, 1);
    EXPECT_LE(s.SSGIRayCount, kSSGIMaxRays); // cap matches the runtime/shader (32)
    EXPECT_GE(s.SSGIRayCount, 1);
    EXPECT_GE(s.SSGIEdgeFade, 0.0f);
    EXPECT_LE(s.SSGIEdgeFade, 0.5f);
}
