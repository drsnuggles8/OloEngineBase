#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

// =============================================================================
// Screen-Space Contact Shadows — CPU contract tests.
//
// These pin the math implemented in PostProcess_ContactShadow.glsl WITHOUT a GL
// context (so they run in headless CI), mirroring ScreenSpaceGIMathTest /
// ScreenSpaceReflectionMathTest. The shader's correctness for the rendered frame
// is checked separately by the GPU ContactShadowVisualEvidenceTest. Per the
// CLAUDE.md rendering rule, math/contract tests prove the formula; the visual
// test proves the frame looks right.
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    // ---- C++ mirrors of the GLSL helpers in PostProcess_ContactShadow.glsl ----

    // Octahedral decode — matches octEncodeGB() in PBR_GBuffer.glsl and
    // OctDecode() in PostProcess_ContactShadow.glsl.
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

    float Smoothstep(float edge0, float edge1, float x)
    {
        const float tt = glm::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return tt * tt * (3.0f - 2.0f * tt);
    }

    constexpr float kMinNdotL = 0.01f; // matches MIN_NDOTL in the shader
} // namespace

// ---- UBO layout contract ----------------------------------------------------

TEST(ContactShadow, UBOAlignment)
{
    EXPECT_EQ(sizeof(ContactShadowUBOData) % 16, 0u) << "ContactShadowUBOData must be 16-byte aligned for std140";
}

TEST(ContactShadow, UBOGetSizeMatchesSizeof)
{
    EXPECT_EQ(ContactShadowUBOData::GetSize(), sizeof(ContactShadowUBOData));
}

// The std140 block in PostProcess_ContactShadow.glsl is laid out byte-for-byte
// against this struct: 3 mat4 (192) + 5 vec4 (80) = 272.
TEST(ContactShadow, UBOLayoutSizeMatchesShader)
{
    EXPECT_EQ(sizeof(ContactShadowUBOData), 272u)
        << "ContactShadowUBOData drifted from the PostProcess_ContactShadow.glsl ContactShadowParams block";
    EXPECT_EQ(offsetof(ContactShadowUBOData, LightDirection), 192u)
        << "LightDirection must follow the 3 matrices at offset 192";
    EXPECT_EQ(offsetof(ContactShadowUBOData, RayParams), 208u);
    EXPECT_EQ(offsetof(ContactShadowUBOData, ShadeParams), 224u);
    EXPECT_EQ(offsetof(ContactShadowUBOData, ScreenParams), 240u);
    EXPECT_EQ(offsetof(ContactShadowUBOData, Flags), 256u);
}

TEST(ContactShadow, BindingIsUniqueAndExpected)
{
    EXPECT_EQ(ShaderBindingLayout::UBO_CONTACT_SHADOW, 41u);
    // Must not collide with the neighbouring screen-space post UBO bindings.
    EXPECT_NE(ShaderBindingLayout::UBO_CONTACT_SHADOW, ShaderBindingLayout::UBO_SSR);
    EXPECT_NE(ShaderBindingLayout::UBO_CONTACT_SHADOW, ShaderBindingLayout::UBO_SSGI);
    EXPECT_NE(ShaderBindingLayout::UBO_CONTACT_SHADOW, ShaderBindingLayout::UBO_STAR_NEST_SKY);
}

// ---- Position reconstruct/project round-trip --------------------------------

TEST(ContactShadow, ViewPositionProjectRoundTrip)
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

TEST(ContactShadow, OctahedralNormalRoundTrip)
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

// ---- Toward-light direction -------------------------------------------------

// The CPU packs the world-space direction TOWARD the light as the negation of
// the light's travel direction (DirectionalLightComponent::m_Direction), which
// is what RenderPipeline.cpp uploads into LightDirection.xyz. A sun pointing
// down-and-forward yields a toward-light vector pointing up-and-back.
TEST(ContactShadow, TowardLightIsNegatedTravelDirection)
{
    const glm::vec3 travel = glm::normalize(glm::vec3(0.2f, -0.85f, 0.3f));
    const glm::vec3 toward = glm::normalize(-travel);

    EXPECT_NEAR(glm::length(toward), 1.0f, 1e-5f);
    EXPECT_GT(toward.y, 0.0f) << "a downward sun must yield an upward toward-light vector";
    EXPECT_NEAR(glm::dot(toward, travel), -1.0f, 1e-5f) << "toward-light must be antiparallel to travel";
}

// View-space light direction is the world toward-light direction rotated by the
// view matrix's rotation (mat3), matching `mat3(u_View) * Lworld` in the shader.
// With an identity view the two coincide; a 90° yaw rotates X into -Z.
TEST(ContactShadow, ViewSpaceLightTransform)
{
    const glm::vec3 Lworld = glm::normalize(glm::vec3(1.0f, 0.0f, 0.0f));

    // Identity view: Lview == Lworld.
    {
        const glm::mat4 view(1.0f);
        const glm::vec3 Lview = glm::normalize(glm::mat3(view) * Lworld);
        EXPECT_NEAR(Lview.x, Lworld.x, 1e-5f);
        EXPECT_NEAR(Lview.y, Lworld.y, 1e-5f);
        EXPECT_NEAR(Lview.z, Lworld.z, 1e-5f);
    }

    // View looking down -Z from +Z toward origin (camera at +X looking toward
    // origin would rotate axes); use a known lookAt and verify the transform is
    // length-preserving and consistent with the matrix product.
    {
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f),
                                           glm::vec3(0.0f, 0.0f, 0.0f),
                                           glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 Lview = glm::normalize(glm::mat3(view) * Lworld);
        EXPECT_NEAR(glm::length(Lview), 1.0f, 1e-5f);
        // This view has no rotation that flips X (camera straight down -Z), so
        // world +X stays view +X.
        EXPECT_NEAR(Lview.x, 1.0f, 1e-4f);
    }
}

// ---- N·L early-out ----------------------------------------------------------

// Surfaces facing away from the light (N·L <= MIN_NDOTL) are in form shadow and
// the pass leaves them untouched; surfaces facing the light are candidates for a
// contact shadow.
TEST(ContactShadow, NdotLEarlyOutGate)
{
    const glm::vec3 L = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f)); // straight up

    const glm::vec3 facingLight = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 facingAway = glm::normalize(glm::vec3(0.0f, -1.0f, 0.0f));
    const glm::vec3 grazing = glm::normalize(glm::vec3(1.0f, 0.005f, 0.0f)); // N·L ~= 0.005 < MIN_NDOTL

    EXPECT_GT(glm::dot(facingLight, L), kMinNdotL) << "up-facing surface must be lit (contact-shadow candidate)";
    EXPECT_LE(glm::dot(facingAway, L), kMinNdotL) << "down-facing surface must be gated out (form shadow)";
    EXPECT_LE(glm::dot(grazing, L), kMinNdotL) << "near-perpendicular surface must be gated out";
}

// ---- Linear march loop bound ------------------------------------------------

// The march advances by Stride and stops at MaxDistance or after MaxSteps
// iterations, whichever comes first — mirrors the inner loop in
// PostProcess_ContactShadow.glsl so a regression in either cap is caught.
TEST(ContactShadow, MarchStepsBoundedByDistanceAndStepCap)
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

    // Distance-limited: 1.0 / 0.04 = 25 samples, capped at 24 by the step cap.
    EXPECT_EQ(countSteps(0.04f, 1.0f, 24), 24);
    // Distance-limited below the step cap: 0.5 / 0.04 = 12.5 -> 12 samples.
    EXPECT_EQ(countSteps(0.04f, 0.5f, 64), 12);
}

// ---- Occlusion predicate ----------------------------------------------------

// The hit test compares the ray's view-space depth against the scene surface
// sampled at the ray's screen pixel: delta = (-rayPos.z) - (-sPos.z). The ray is
// occluded when it has passed just BEHIND a visible surface — delta in
// (0, thickness). delta <= 0 means the ray is still in front (no occluder yet);
// delta >= thickness means the surface is the distant background, not a thin
// contact occluder.
TEST(ContactShadow, OcclusionPredicateThicknessWindow)
{
    const float thickness = 0.3f;
    auto isHit = [thickness](float rayViewZ, float surfaceViewZ)
    {
        const float delta = (-rayViewZ) - (-surfaceViewZ);
        return delta > 0.0f && delta < thickness;
    };

    // Ray exactly on the surface: delta == 0 -> not a hit (avoids self-shadow).
    EXPECT_FALSE(isHit(-5.0f, -5.0f));
    // Ray just behind a near surface within the thickness window -> hit.
    EXPECT_TRUE(isHit(-5.2f, -5.0f));   // delta = 0.2 in (0, 0.3)
    // Ray in front of the surface (closer to camera) -> no occluder.
    EXPECT_FALSE(isHit(-4.8f, -5.0f));  // delta = -0.2
    // Surface far in front of the ray (background) -> beyond thickness, ignored.
    EXPECT_FALSE(isHit(-6.0f, -5.0f));  // delta = 1.0 >= 0.3
}

// ---- Distance + edge fades --------------------------------------------------

// Closer occluders cast a stronger contact shadow; the strength uses a SQUARED
// falloff to zero at the max ray length — distFade = (1 - clamp(traveled/maxDist))^2
// — so the shadow stays tight to the contact and the faint grazing far end fades
// out smoothly (a linear cutoff leaves stochastic hard-edge speckle on long rays).
TEST(ContactShadow, DistanceFadeFavoursCloseHits)
{
    const float maxDist = 1.0f;
    auto distFade = [maxDist](float traveled)
    {
        const float f = 1.0f - glm::clamp(traveled / maxDist, 0.0f, 1.0f);
        return f * f;
    };

    EXPECT_NEAR(distFade(0.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(distFade(maxDist), 0.0f, 1e-6f);
    EXPECT_GT(distFade(0.1f), distFade(0.9f)) << "a closer occluder must darken more than a distant one";
    // The squared curve falls off faster than linear past the contact: at the
    // midpoint the strength is a quarter, not a half.
    EXPECT_NEAR(distFade(0.5f * maxDist), 0.25f, 1e-5f) << "midpoint must be the squared (0.25), not linear (0.5), value";
}

TEST(ContactShadow, EdgeFadeVanishesAtBorders)
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

// ---- Multiplicative composite -----------------------------------------------

// Contact shadows OCCLUDE direct light (vs SGGI's add or SSR's replace/mix): the
// shadow factor multiplies the lit colour. out = base * (1 - occlusion*intensity),
// which can only darken (never brighten), is clamped, and at zero occlusion is a
// no-op.
TEST(ContactShadow, CompositeIsMultiplicativeDarkening)
{
    const glm::vec3 base(0.4f, 0.6f, 0.8f);

    auto composite = [&](float occlusion, float intensity)
    {
        const float shadowFactor = 1.0f - glm::clamp(occlusion * intensity, 0.0f, 1.0f);
        return base * shadowFactor;
    };

    // No occlusion -> unchanged.
    {
        const glm::vec3 out = composite(0.0f, 1.0f);
        EXPECT_NEAR(out.r, base.r, 1e-6f);
        EXPECT_NEAR(out.g, base.g, 1e-6f);
        EXPECT_NEAR(out.b, base.b, 1e-6f);
    }
    // Half occlusion at full intensity -> halve the lit colour.
    {
        const glm::vec3 out = composite(0.5f, 1.0f);
        EXPECT_NEAR(out.r, base.r * 0.5f, 1e-6f);
        EXPECT_LT(out.r, base.r) << "contact shadow must darken, never brighten";
    }
    // Full occlusion at full intensity -> black.
    {
        const glm::vec3 out = composite(1.0f, 1.0f);
        EXPECT_NEAR(out.r, 0.0f, 1e-6f);
    }
    // Over-driven product is clamped so the colour never goes negative.
    {
        const glm::vec3 out = composite(2.0f, 4.0f);
        EXPECT_NEAR(out.r, 0.0f, 1e-6f);
        EXPECT_GE(out.r, 0.0f);
    }
    // Intensity scales the darkening: lower intensity -> brighter result.
    {
        const glm::vec3 weak = composite(1.0f, 0.25f);
        const glm::vec3 strong = composite(1.0f, 1.0f);
        EXPECT_GT(weak.r, strong.r) << "lower intensity must leave more light";
    }
}

// ---- Settings sanitization --------------------------------------------------

TEST(ContactShadow, SanitizeClampsNonFiniteAndRanges)
{
    PostProcessSettings s;
    s.ContactShadowIntensity = std::numeric_limits<f32>::quiet_NaN();
    s.ContactShadowMaxDistance = std::numeric_limits<f32>::infinity();
    s.ContactShadowThickness = -5.0f;
    s.ContactShadowStride = -1.0f;
    s.ContactShadowMaxSteps = 100000;
    s.ContactShadowBias = std::numeric_limits<f32>::quiet_NaN();
    s.ContactShadowEdgeFade = -1.0f;

    SanitizeContactShadow(s);

    EXPECT_TRUE(std::isfinite(s.ContactShadowIntensity));
    EXPECT_GE(s.ContactShadowIntensity, 0.0f);
    EXPECT_LE(s.ContactShadowIntensity, 1.0f);
    EXPECT_TRUE(std::isfinite(s.ContactShadowMaxDistance));
    EXPECT_GE(s.ContactShadowThickness, 0.001f);
    EXPECT_GE(s.ContactShadowStride, 0.001f);
    EXPECT_LE(s.ContactShadowMaxSteps, kContactShadowMaxSteps); // cap matches the runtime/shader (128)
    EXPECT_GE(s.ContactShadowMaxSteps, 1);
    EXPECT_TRUE(std::isfinite(s.ContactShadowBias));
    EXPECT_GE(s.ContactShadowBias, 0.0f);
    EXPECT_GE(s.ContactShadowEdgeFade, 0.0f);
    EXPECT_LE(s.ContactShadowEdgeFade, 0.5f);
}
