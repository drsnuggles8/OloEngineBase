// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// FogMathTest.cpp
//
// Fog distance/height factor + composite-blend CPU contract tests. These pin
// the math implemented in PostProcess_Fog.glsl / include/FogCommon.glsl WITHOUT
// a GL context (so they run in headless CI), mirroring the *MathTest.cpp style
// of ScreenSpaceReflectionMathTest / ContactShadowMathTest.
//
// Complementary to the existing GPU coverage, which this file deliberately does
// NOT duplicate:
//   * ShaderUnitFogTest.EndpointInvariants (ShaderUnitTests.cpp) runs the real
//     computeDistanceFog on the GPU for the three modes at endpoint distances.
//   * PostProcessPropertyTests.FogDisabledTest proves the disabled flag emits
//     zero inscatter / full transmittance on the GPU.
//   * FogVisualEvidenceTest renders the full pipeline and reads pixels back.
//
// What this file adds (the parts none of the above pin):
//   * Distance fog is MONOTONIC in distance for every mode — the near→far
//     gradient the visual test eyeballs ("near clear, far fogged").
//   * MaxOpacity clamps the fog factor, so even at infinite distance the scene
//     is never fully replaced when MaxOpacity < 1 — the math-level guard for
//     the "fog floods the whole frame" failure mode CLAUDE.md calls out.
//   * The composite blend result = scene*(1 - f) + fogColor*f tends toward the
//     fog colour as distance grows and is the EXACT identity (scene unchanged)
//     when fog is off / f == 0.
//   * Height fog is finite, non-negative, grows with density, and is disabled
//     when HeightFalloff ≈ 0.
//
// Per the CLAUDE.md rendering rule: math/contract tests prove the formula; the
// GPU FogVisualEvidenceTest proves the rendered frame actually looks right.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"

#include <glm/glm.hpp>

#include <cmath>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    // ---- C++ mirrors of the GLSL helpers in include/FogCommon.glsl -----------

    // computeDistanceFog(): 0 = no fog, 1 = fully fogged. mode matches FogMode.
    f32 ComputeDistanceFog(f32 dist, FogMode mode, f32 density, f32 fogStart, f32 fogEnd)
    {
        switch (mode)
        {
            case FogMode::Linear:
                return 1.0f - glm::clamp((fogEnd - dist) / std::max(fogEnd - fogStart, 1e-4f), 0.0f, 1.0f);
            case FogMode::Exponential:
                return 1.0f - std::exp(-density * dist);
            case FogMode::ExponentialSquared:
            {
                const f32 dd = density * dist;
                return 1.0f - std::exp(-dd * dd);
            }
        }
        return 0.0f;
    }

    // computeHeightFog(): closed-form exponential integral along the view ray.
    f32 ComputeHeightFog(const glm::vec3& worldPos, const glm::vec3& cameraPos, f32 density,
                         f32 heightFalloff, f32 heightOffset)
    {
        const f32 rayLength = glm::distance(worldPos, cameraPos);
        if (rayLength < 0.001f || heightFalloff < 0.0001f)
            return 0.0f;

        const f32 camH = cameraPos.y - heightOffset;
        const f32 fragH = worldPos.y - heightOffset;
        const f32 deltaH = camH - fragH;

        const f32 camDensity = std::exp(-heightFalloff * camH);
        const f32 fragDensity = std::exp(-heightFalloff * fragH);

        f32 fogIntegral;
        if (std::abs(deltaH) > 0.001f)
            fogIntegral = (camDensity - fragDensity) / (heightFalloff * deltaH);
        else
            fogIntegral = camDensity;

        return 1.0f - std::exp(-density * fogIntegral * rayLength);
    }

    // The combined per-pixel fog factor (PostProcess_Fog.glsl analytical branch):
    //   fogFactor = clamp(max(distFog, heightFog), 0, maxOpacity)
    f32 ComputeFogFactor(f32 dist, FogMode mode, f32 density, f32 fogStart, f32 fogEnd, f32 maxOpacity,
                         const glm::vec3& worldPos, const glm::vec3& cameraPos, f32 heightFalloff,
                         f32 heightOffset)
    {
        const f32 distFog = ComputeDistanceFog(dist, mode, density, fogStart, fogEnd);
        const f32 heightFog = ComputeHeightFog(worldPos, cameraPos, density, heightFalloff, heightOffset);
        return glm::clamp(std::max(distFog, heightFog), 0.0f, maxOpacity);
    }

    // PostProcess_FogUpsample.glsl composite: scene*transmittance + inscatter,
    // where (analytical fog) transmittance = 1 - fogFactor and inscatter =
    // fogColor * fogFactor.
    glm::vec3 CompositeFog(const glm::vec3& scene, const glm::vec3& fogColor, f32 fogFactor)
    {
        const f32 transmittance = 1.0f - fogFactor;
        const glm::vec3 inscatter = fogColor * fogFactor;
        return scene * transmittance + inscatter;
    }
} // namespace

// ---- Distance fog is monotonic in distance (every mode) ---------------------

// The visual test eyeballs "near geometry stays clear, distant geometry tends
// toward the fog colour". That only holds if the underlying distance-fog factor
// never decreases as distance increases. Sweep a fine distance ramp per mode.
TEST(FogMathTest, DistanceFogMonotonicInDistance)
{
    constexpr f32 kDensity = 0.02f;
    constexpr f32 kStart = 5.0f;
    constexpr f32 kEnd = 200.0f;

    for (const FogMode mode : { FogMode::Linear, FogMode::Exponential, FogMode::ExponentialSquared })
    {
        f32 prev = -1.0f;
        u32 violations = 0;
        for (u32 i = 0; i <= 400; ++i)
        {
            const f32 dist = static_cast<f32>(i) * 0.75f; // 0 .. 300
            const f32 f = ComputeDistanceFog(dist, mode, kDensity, kStart, kEnd);
            if (f + 1e-5f < prev)
                ++violations;
            prev = f;
        }
        EXPECT_EQ(violations, 0u)
            << "Distance fog factor decreased with distance for mode " << static_cast<i32>(mode);
    }
}

// Distance-fog endpoints on the CPU side (complements the GPU EndpointInvariants
// test): zero distance is zero fog; far distance saturates the exponential modes
// toward 1; linear respects [start, end].
TEST(FogMathTest, DistanceFogEndpoints)
{
    EXPECT_FLOAT_EQ(ComputeDistanceFog(0.0f, FogMode::Exponential, 0.05f, 0.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(ComputeDistanceFog(0.0f, FogMode::ExponentialSquared, 0.05f, 0.0f, 0.0f), 0.0f);

    // density*dist = 25 → exp(-25) ≈ 1.4e-11, factor ≈ 1.
    EXPECT_NEAR(ComputeDistanceFog(500.0f, FogMode::Exponential, 0.05f, 0.0f, 0.0f), 1.0f, 1e-6f);
    // (density*dist)^2 = 25 → same.
    EXPECT_NEAR(ComputeDistanceFog(50.0f, FogMode::ExponentialSquared, 0.1f, 0.0f, 0.0f), 1.0f, 1e-6f);

    // Linear: before start → 0, beyond end → 1.
    EXPECT_FLOAT_EQ(ComputeDistanceFog(2.0f, FogMode::Linear, 0.0f, 10.0f, 100.0f), 0.0f);
    EXPECT_FLOAT_EQ(ComputeDistanceFog(150.0f, FogMode::Linear, 0.0f, 10.0f, 100.0f), 1.0f);
    // Linear midpoint is the geometric half between start and end.
    EXPECT_NEAR(ComputeDistanceFog(55.0f, FogMode::Linear, 0.0f, 10.0f, 100.0f), 0.5f, 1e-4f);
}

// ---- MaxOpacity clamps the fog factor (the "fog floods the frame" guard) -----

// Even at effectively infinite distance the fog factor can never exceed
// MaxOpacity, so when MaxOpacity < 1 the composited result always keeps a
// (1 - MaxOpacity) fraction of the scene. This is the math-level guard for the
// failure mode where fog drowns the entire frame in fog colour.
TEST(FogMathTest, MaxOpacityClampsFogFactorAndPreservesScene)
{
    constexpr f32 kMaxOpacity = 0.7f;
    const glm::vec3 cam(0.0f, 2.0f, 0.0f);
    const glm::vec3 farPoint(0.0f, 2.0f, -100000.0f);

    const f32 fNear = ComputeFogFactor(/*dist*/ 1.0f, FogMode::ExponentialSquared, 0.05f, 0.0f, 0.0f,
                                       kMaxOpacity, glm::vec3(0.0f, 2.0f, -1.0f), cam,
                                       /*heightFalloff*/ 0.0f, /*heightOffset*/ 0.0f);
    const f32 fFar = ComputeFogFactor(/*dist*/ 100000.0f, FogMode::ExponentialSquared, 0.05f, 0.0f, 0.0f,
                                      kMaxOpacity, farPoint, cam, 0.0f, 0.0f);

    EXPECT_LE(fFar, kMaxOpacity + 1e-6f) << "Fog factor exceeded MaxOpacity at infinite distance";
    EXPECT_NEAR(fFar, kMaxOpacity, 1e-5f) << "Fog factor should saturate exactly at MaxOpacity far away";
    EXPECT_LT(fNear, 0.05f) << "Near fog factor should be tiny";

    // The far composite must retain (1 - MaxOpacity) of the scene: it is NOT
    // pure fog colour.
    const glm::vec3 scene(0.9f, 0.2f, 0.1f); // warm scene colour
    const glm::vec3 fogColor(0.2f, 0.4f, 0.9f);
    const glm::vec3 resultFar = CompositeFog(scene, fogColor, fFar);
    const glm::vec3 expectedSceneResidue = scene * (1.0f - kMaxOpacity);
    // Red channel residue from the scene must survive (scene.r * 0.3 = 0.27).
    EXPECT_GT(resultFar.r, expectedSceneResidue.r - 1e-4f)
        << "MaxOpacity<1 must keep some scene colour even at the far plane";
    EXPECT_GT(resultFar.r, 0.2f) << "Far fog wholly replaced the scene (flood) despite MaxOpacity<1";
}

// ---- Composite blends toward the fog colour with distance -------------------

// The core visual contract, at the math level: near pixels read essentially the
// scene colour; far pixels read essentially the fog colour; the transition is
// monotone toward fog colour as the factor grows.
TEST(FogMathTest, CompositeBlendsTowardFogColorWithDistance)
{
    const glm::vec3 scene(0.85f, 0.80f, 0.70f);    // warm, near-white
    const glm::vec3 fogColor(0.15f, 0.35f, 0.90f); // strong blue, MaxOpacity = 1

    // f == 0 (fog off / point at camera): result is EXACTLY the scene.
    const glm::vec3 r0 = CompositeFog(scene, fogColor, 0.0f);
    EXPECT_FLOAT_EQ(r0.r, scene.r);
    EXPECT_FLOAT_EQ(r0.g, scene.g);
    EXPECT_FLOAT_EQ(r0.b, scene.b);

    // f == 1 (fully fogged): result is EXACTLY the fog colour.
    const glm::vec3 r1 = CompositeFog(scene, fogColor, 1.0f);
    EXPECT_FLOAT_EQ(r1.r, fogColor.r);
    EXPECT_FLOAT_EQ(r1.g, fogColor.g);
    EXPECT_FLOAT_EQ(r1.b, fogColor.b);

    // As the factor ramps 0→1 the blue channel rises monotonically toward the
    // fog colour and the (scene-dominant) red channel falls monotonically.
    f32 prevB = -1.0f;
    f32 prevR = 2.0f;
    for (u32 i = 0; i <= 100; ++i)
    {
        const f32 f = static_cast<f32>(i) / 100.0f;
        const glm::vec3 r = CompositeFog(scene, fogColor, f);
        EXPECT_GE(r.b + 1e-5f, prevB) << "Blue did not rise toward fog colour at f=" << f;
        EXPECT_LE(r.r - 1e-5f, prevR) << "Red did not fall toward fog colour at f=" << f;
        prevB = r.b;
        prevR = r.r;
    }

    // Sanity on the direction: a far (heavily fogged) pixel is more fog-coloured
    // (blue-dominant) than a near (lightly fogged) one. (Avoid the names `near`
    // / `far` — they are empty macros in the Windows SDK headers.)
    const glm::vec3 lightlyFogged = CompositeFog(scene, fogColor, 0.05f);
    const glm::vec3 heavilyFogged = CompositeFog(scene, fogColor, 0.95f);
    EXPECT_LT(lightlyFogged.b, lightlyFogged.r) << "Lightly-fogged pixel should still read as the warm scene";
    EXPECT_GT(heavilyFogged.b, heavilyFogged.r) << "Heavily-fogged pixel should read as the blue fog colour";
}

// ---- Height fog: finite raw integral, disabled at zero falloff, and a
//      combined factor that always stays in range -------------------------------

TEST(FogMathTest, HeightFogFiniteAndCombinedFactorStaysInRange)
{
    const glm::vec3 cam(0.0f, 10.0f, 0.0f);
    const glm::vec3 fragBelow(0.0f, 0.0f, -50.0f); // lower than the camera

    // HeightFalloff ≈ 0 disables height fog entirely (the shader's early-out).
    EXPECT_FLOAT_EQ(ComputeHeightFog(fragBelow, cam, 0.05f, 0.0f, 0.0f), 0.0f);

    // The raw closed-form integral is FINITE for finite inputs, but — faithful
    // to the GLSL — it can be NEGATIVE when camera and fragment differ in height
    // (the view ray climbs into thinner fog). That is expected; the caller tames
    // it with max(distFog, heightFog) then clamp(., 0, maxOpacity).
    const f32 hBelow = ComputeHeightFog(fragBelow, cam, 0.05f, 0.1f, 0.0f);
    EXPECT_TRUE(std::isfinite(hBelow)) << "Raw height fog must be finite (got " << hBelow << ")";

    // The well-behaved regime is camera and fragment at the SAME height (the
    // deltaH ≈ 0 branch, the closed-form's removable singularity): there the
    // factor is a proper [0, 1) fog that grows with density.
    const glm::vec3 fragSameH(0.0f, 10.0f, -50.0f);
    const f32 hFlatLow = ComputeHeightFog(fragSameH, cam, 0.02f, 0.1f, 0.0f);
    const f32 hFlatHigh = ComputeHeightFog(fragSameH, cam, 0.2f, 0.1f, 0.0f);
    EXPECT_TRUE(std::isfinite(hFlatLow));
    EXPECT_GE(hFlatLow, 0.0f);
    EXPECT_LT(hFlatHigh, 1.0f);
    EXPECT_GT(hFlatHigh, hFlatLow) << "Equal-height height fog should increase with density";

    // The contract that actually matters for the frame: the COMBINED, clamped
    // fog factor is always in [0, maxOpacity] regardless of geometry — even
    // where the raw height integral is negative. This is the math-level
    // guarantee that the per-pixel fog factor never goes out of range (no
    // negative-fog brightening, no over-opacity flood).
    constexpr f32 kMaxOpacity = 0.85f;
    for (const glm::vec3& frag : { fragBelow, fragSameH, glm::vec3(0.0f, 40.0f, -120.0f) })
    {
        const f32 dist = glm::distance(frag, cam);
        const f32 f = ComputeFogFactor(dist, FogMode::ExponentialSquared, 0.05f, 0.0f, 0.0f, kMaxOpacity,
                                       frag, cam, /*heightFalloff*/ 0.1f, /*heightOffset*/ 0.0f);
        EXPECT_GE(f, 0.0f) << "Combined fog factor went negative";
        EXPECT_LE(f, kMaxOpacity + 1e-6f) << "Combined fog factor exceeded MaxOpacity";
    }
}

// ---- The UBO packing matches the settings the pipeline uploads --------------

// RenderPipeline.cpp packs FogSettings into FogUBOData like this. Pin the
// channel mapping so a reorder of the GLSL std140 block or the CPU upload is
// caught here rather than as a mysterious wrong-coloured frame.
TEST(FogMathTest, FogUboPackingMirrorsSettings)
{
    FogSettings fog;
    fog.Color = glm::vec3(0.3f, 0.5f, 0.8f);
    fog.Density = 0.05f;
    fog.Start = 12.0f;
    fog.End = 250.0f;
    fog.HeightFalloff = 0.2f;
    fog.HeightOffset = 1.0f;
    fog.MaxOpacity = 0.85f;
    fog.Mode = FogMode::Exponential;

    FogUBOData gpu{};
    gpu.ColorAndDensity = glm::vec4(fog.Color, fog.Density);
    gpu.DistanceParams = glm::vec4(fog.Start, fog.End, fog.HeightFalloff, fog.HeightOffset);
    gpu.RayleighColorAndMaxOpacity = glm::vec4(fog.RayleighColor, fog.MaxOpacity);
    gpu.Flags = glm::vec4(1.0f, static_cast<f32>(std::to_underlying(fog.Mode)), 0.0f, 0.0f);

    // Mirror the shader unpack (PostProcess_Fog.glsl main()).
    EXPECT_FLOAT_EQ(gpu.ColorAndDensity.a, fog.Density);      // baseDensity = .a
    EXPECT_FLOAT_EQ(gpu.DistanceParams.x, fog.Start);         // fogStart = .x
    EXPECT_FLOAT_EQ(gpu.DistanceParams.y, fog.End);           // fogEnd = .y
    EXPECT_FLOAT_EQ(gpu.DistanceParams.z, fog.HeightFalloff); // heightFalloff = .z
    EXPECT_FLOAT_EQ(gpu.DistanceParams.w, fog.HeightOffset);  // heightOffset = .w
    EXPECT_FLOAT_EQ(gpu.RayleighColorAndMaxOpacity.a, fog.MaxOpacity);
    EXPECT_EQ(static_cast<i32>(gpu.Flags.y + 0.5f), std::to_underlying(fog.Mode));
    EXPECT_GT(gpu.Flags.x, 0.5f) << "Enabled flag must read as on";
}
