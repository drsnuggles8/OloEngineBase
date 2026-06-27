#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cmath>
#include <vector>

// =============================================================================
// SSAO — CPU contract tests.
//
// OLO_TEST_LAYER: shaderpipe
//
// These pin the per-tap obscurance math implemented in SSAO.glsl WITHOUT a GL
// context (so they run in headless CI — the GPU SSAOVisualEvidenceTest SKIPs
// there), mirroring ScreenSpaceGIMathTest / ScreenSpaceReflectionMathTest /
// ContactShadowMathTest. Per the CLAUDE.md rendering rule, math/contract tests
// prove the formula; the visual test proves the frame looks right.
//
// The property that matters most here is the bug fix this branch ships: the
// SSAO estimator is normal-oriented, so a tap COPLANAR with the surface (a flat
// floor occluding itself) contributes ZERO occlusion, while a tap that rises
// ABOVE the tangent plane (a wall/crease) DOES occlude. The old horizon variant
// lacked this tangent subtraction and darkened flat ground at any tilt.
//
// The constants below MUST stay in sync with SSAO.glsl (kMinCos, kBiasWindow,
// kNearScale, kStrength, the range falloff band, the bias-floor assembly). If
// you retune the shader, retune here — same as the sibling *MathTest mirrors.
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    // ---- Constants mirrored from SSAO.glsl ----
    constexpr float kMinCos = 0.1f;
    constexpr float kBiasWindow = 0.25f;
    constexpr float kNearScale = 0.3f;
    constexpr float kStrength = 4.0f;

    float Smoothstep(float edge0, float edge1, float x)
    {
        const float t = glm::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // CPU mirror of one spiral tap's obscurance contribution in SSAO.glsl's loop.
    // viewNormal: unit view-space surface normal; toSample = samplePos - viewPos
    // (view-space vector from the shaded pixel to the tap's reconstructed pos).
    float TapObscurance(const glm::vec3& viewNormal, const glm::vec3& toSample, float radius, float bias)
    {
        const float dist2 = glm::dot(toSample, toSample);
        if (dist2 < 1e-8f)
            return 0.0f;
        const float invLen = 1.0f / std::sqrt(dist2);
        const float ndotv = glm::dot(viewNormal, toSample) * invLen; // how far the tap is above the tangent plane (cosine)
        const float radius2 = radius * radius;
        const float rangeFalloff = 1.0f - Smoothstep(radius2 * 0.64f, radius2, dist2);
        const float proximity = 1.0f / (1.0f + dist2 / (kNearScale * radius2));
        const float biasFloor = kMinCos + bias;
        return Smoothstep(biasFloor, biasFloor + kBiasWindow, ndotv) * rangeFalloff * proximity;
    }

    // CPU mirror of SSAO.glsl's final AO assembly: ao = 1 - clamp(kStrength *
    // sum(obscurance) / numSamples, 0, 1). numSamples is the spiral budget, so
    // out-of-range/coplanar taps that contribute 0 still count in the denominator.
    float AssembleAO(const std::vector<glm::vec3>& taps, const glm::vec3& viewNormal, float radius, float bias)
    {
        float occlusion = 0.0f;
        for (const glm::vec3& t : taps)
            occlusion += TapObscurance(viewNormal, t, radius, bias);
        const float n = static_cast<float>(taps.size());
        return 1.0f - glm::clamp(kStrength * occlusion / std::max(n, 1.0f), 0.0f, 1.0f);
    }

    constexpr float kRadius = 0.5f;
    constexpr float kBias = 0.025f;
} // namespace

// A tap that lies in the surface's tangent plane (a neighbouring point on the
// SAME flat surface) is perpendicular to the normal, so dot(N, dir) = 0 — well
// below the bias floor — and contributes ZERO occlusion. This is the fix: a flat
// floor cannot self-occlude.
TEST(SSAOMath, CoplanarTapContributesNoOcclusion)
{
    const glm::vec3 N(0.0f, 0.0f, 1.0f);
    // Several in-plane directions at a fraction of the radius (toSample.z == 0).
    for (const glm::vec3 dir : { glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(-1, 1, 0), glm::vec3(0.7f, -0.7f, 0) })
    {
        const glm::vec3 toSample = glm::normalize(dir) * (0.4f * kRadius);
        EXPECT_NEAR(TapObscurance(N, toSample, kRadius, kBias), 0.0f, 1e-6f)
            << "coplanar tap dir=(" << dir.x << "," << dir.y << "," << dir.z << ") occluded a flat surface";
    }
}

// A tap that rises clearly above the tangent plane (a wall/crease occluder)
// produces positive occlusion.
TEST(SSAOMath, OccluderAboveTangentOccludes)
{
    const glm::vec3 N(0.0f, 0.0f, 1.0f);
    // 45° above the plane, well inside the radius.
    const glm::vec3 toSample = glm::normalize(glm::vec3(0.0f, 0.5f, 0.5f)) * (0.3f * kRadius);
    EXPECT_GT(TapObscurance(N, toSample, kRadius, kBias), 0.0f)
        << "an occluder above the tangent plane produced no occlusion";
}

// A tap whose cosine is just under the bias floor is rejected (tangent bias);
// a tap just above it occludes. Guards the self-occlusion threshold.
TEST(SSAOMath, BiasFloorRejectsNearTangentTaps)
{
    const glm::vec3 N(0.0f, 0.0f, 1.0f);
    const float biasFloor = kMinCos + kBias; // 0.125
    const float dist = 0.3f * kRadius;

    // Build toSample with a chosen cosine c against N: pick angle so dot/len = c.
    const auto tapWithCosine = [&](float c) -> glm::vec3
    {
        const float s = std::sqrt(std::max(0.0f, 1.0f - c * c));
        return glm::vec3(s, 0.0f, c) * dist; // |toSample| = dist, dot(N,·)/len = c
    };

    EXPECT_NEAR(TapObscurance(N, tapWithCosine(biasFloor - 0.03f), kRadius, kBias), 0.0f, 1e-6f)
        << "a tap below the bias floor was counted as an occluder";
    EXPECT_GT(TapObscurance(N, tapWithCosine(biasFloor + kBiasWindow + 0.05f), kRadius, kBias), 0.0f)
        << "a tap well above the bias floor was not counted";
}

// The world-space range falloff discards taps beyond the radius, so a
// foreshortened far tap cannot inject phantom occlusion.
TEST(SSAOMath, TapBeyondRadiusIsIgnored)
{
    const glm::vec3 N(0.0f, 0.0f, 1.0f);
    // A strongly-above-plane direction, but placed past the radius.
    const glm::vec3 dir = glm::normalize(glm::vec3(0.0f, 0.6f, 0.8f));
    EXPECT_GT(TapObscurance(N, dir * (0.5f * kRadius), kRadius, kBias), 0.0f) << "near occluder should count";
    EXPECT_NEAR(TapObscurance(N, dir * (1.2f * kRadius), kRadius, kBias), 0.0f, 1e-6f)
        << "occluder beyond the radius should be range-culled";
}

// Proximity weighting: at the same direction, a NEAR occluder outweighs a FAR
// one (so a tight crease dominates over the broad floor).
TEST(SSAOMath, NearOccluderOutweighsFar)
{
    const glm::vec3 N(0.0f, 0.0f, 1.0f);
    const glm::vec3 dir = glm::normalize(glm::vec3(0.0f, 0.6f, 0.8f));
    const float nearObsc = TapObscurance(N, dir * (0.25f * kRadius), kRadius, kBias);
    const float farObsc = TapObscurance(N, dir * (0.75f * kRadius), kRadius, kBias);
    EXPECT_GT(nearObsc, farObsc) << "near occluder (" << nearObsc << ") did not outweigh far (" << farObsc << ")";
}

// Assembly contract — THE FIX: a full disk of taps all lying on the SAME flat
// surface (coplanar with the normal) yields AO == 1 (unoccluded). The old
// horizon math drove this well below 1, darkening flat ground.
TEST(SSAOMath, FlatSurfaceDiskIsUnoccluded)
{
    const glm::vec3 N(0.0f, 0.0f, 1.0f);
    std::vector<glm::vec3> taps;
    for (int i = 0; i < 32; ++i)
    {
        const float a = (static_cast<float>(i) + 0.5f) / 32.0f * 6.2831853f * 7.0f;
        const float r = std::sqrt((static_cast<float>(i) + 0.5f) / 32.0f) * kRadius;
        taps.emplace_back(std::cos(a) * r, std::sin(a) * r, 0.0f); // all in the tangent plane
    }
    EXPECT_NEAR(AssembleAO(taps, N, kRadius, kBias), 1.0f, 1e-5f)
        << "a flat surface self-occluded (AO < 1) — the over-darkening regression";
}

// Assembly contract: when a chunk of the disk lands on a wall ABOVE the tangent
// plane (a contact crease), AO drops clearly below 1.
TEST(SSAOMath, ContactCreaseIsOccluded)
{
    const glm::vec3 N(0.0f, 0.0f, 1.0f);
    std::vector<glm::vec3> taps;
    for (int i = 0; i < 32; ++i)
    {
        const float a = (static_cast<float>(i) + 0.5f) / 32.0f * 6.2831853f * 7.0f;
        const float r = std::sqrt((static_cast<float>(i) + 0.5f) / 32.0f) * kRadius;
        // Half the taps (those in +y) ride up a wall: give them a +N component.
        const float up = (std::sin(a) > 0.0f) ? 0.55f * r : 0.0f;
        taps.emplace_back(std::cos(a) * r, std::sin(a) * r, up);
    }
    const float ao = AssembleAO(taps, N, kRadius, kBias);
    EXPECT_LT(ao, 0.9f) << "a contact crease did not occlude (AO=" << ao << ")";
    EXPECT_GE(ao, 0.0f) << "AO must stay clamped to [0,1]";
}
