// OLO_TEST_LAYER: unit
//
// The scattering maths behind issue #1241 — the Burley diffusion profile, the
// separable kernel built from it, and the one unit conversion in the feature.
//
// WHAT THIS PINS, AND WHY EACH PART NEEDS PINNING:
//
//   * THE UNIT CHAIN. `ScatterRadiusMM` is MILLIMETRES, the world is METRES, and
//     a screen-space radius is a projected length. A slip anywhere in that chain
//     produces a perfectly plausible image that is simply wrong at a different
//     camera distance, a different field of view or a different resolution —
//     which is the one class of bug a look at a screenshot cannot catch. So the
//     scaling is asserted as a RATIO under each of those changes, not just as a
//     number.
//
//   * `ThicknessScale` IS NOT IN THAT CHAIN. It converts a material's authored
//     thickness into millimetres; it is the transmission knob. Folding it into
//     the radius would make a profile that exaggerates transmission quietly
//     shrink its own blur, and it is the obvious wrong thing to do — so the test
//     says so directly.
//
//   * THE POLAR REARRANGEMENT. `SkinBurleyStripFraction` computes the energy in
//     a vertical strip by integrating over annuli instead of over the strip,
//     which cancels the profile's 1/r pole. That rearrangement is either exactly
//     right or subtly wrong, and the only way to tell is a direct 2D
//     integration of the profile — which this test does.
//
//   * ENERGY. The kernel's weights are what conserve a skin pixel's brightness.
//     A kernel whose channel sums drift from 1 darkens or brightens every head
//     in the frame by a few percent, uniformly, which reads as a lighting change
//     and gets chased in the wrong file.
//
//   * THE VERSION BRANCH. A profile authored against transport version 0 must
//     keep shading as it did when the renderer's diffusion is switched on. The
//     identity kernel is how that is true on the CPU side.
//
// No GPU: all of it is CPU state, which is also what makes it run on the Linux
// CI runners that have no GL context.

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/SkinDiffusion.h"
#include "OloEngine/Renderer/SkinProfile.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>

namespace
{
    using namespace OloEngine;

    // The authored reference head
    // (OloEditor/SandboxProject/Assets/Materials/ReferenceHead.oloskin), at the
    // transport version that diffuses. Spelled out rather than loaded so the
    // test says what it is testing and does not depend on a project on disk.
    [[nodiscard]] SkinProfileParameters ReferenceHead()
    {
        SkinProfileParameters parameters{};
        parameters.EvaluationModel = SkinEvaluationModel::ScreenSpaceDiffusion;
        parameters.ScatterColor = glm::vec3(0.85f, 0.55f, 0.45f);
        parameters.ScatterRadiusMM = glm::vec3(1.55f, 0.80f, 0.55f);
        parameters.ThicknessScale = 1000.0f;
        parameters.SpecularTint = glm::vec3(0.97f, 0.93f, 0.90f);
        EXPECT_TRUE(parameters.Sanitize()) << "the reference head must be in range as authored";
        return parameters;
    }

    // Direct 2D integration of the radial profile over the strip |x| <= a, in
    // polar coordinates on a fine grid. Deliberately the DUMB way round: it
    // integrates R(r) * r over annuli and multiplies by the angular fraction of
    // each annulus that falls inside the strip, measured by sampling. If the
    // analytic rearrangement in SkinBurleyStripFraction and this disagree, one
    // of them is wrong and it is not obvious which — which is exactly why the
    // reference is written independently rather than derived from it.
    [[nodiscard]] f64 BruteForceStripFraction(f64 a, f64 d)
    {
        constexpr i32 kRadialSamples = 8000;
        constexpr i32 kAngularSamples = 2048;
        const f64 rMax = 64.0 * d;
        const f64 dr = rMax / static_cast<f64>(kRadialSamples);

        f64 total = 0.0;
        for (i32 i = 1; i <= kRadialSamples; ++i)
        {
            const f64 r = (static_cast<f64>(i) - 0.5) * dr;
            // R(r) * r, with the 1/r pole already cancelled analytically.
            const f64 density = (std::exp(-r / d) + std::exp(-r / (3.0 * d))) / (8.0 * std::numbers::pi * d);

            i32 inside = 0;
            for (i32 t = 0; t < kAngularSamples; ++t)
            {
                const f64 theta = (2.0 * std::numbers::pi * (static_cast<f64>(t) + 0.5)) /
                                  static_cast<f64>(kAngularSamples);
                if (std::abs(std::cos(theta)) * r <= a)
                    ++inside;
            }
            const f64 fraction = static_cast<f64>(inside) / static_cast<f64>(kAngularSamples);
            total += density * (2.0 * std::numbers::pi) * fraction * dr;
        }
        return total;
    }
} // namespace

// -----------------------------------------------------------------------------
// The profile itself
// -----------------------------------------------------------------------------

TEST(SkinDiffusionTest, BurleyShapeIsBoundedBelowOnEveryLegalAlbedo)
{
    // `s` divides the authored mean free path. SkinBurleyScalingMM relies on it
    // never reaching zero, and the claim in that function's comment is that the
    // cubic term is non-negative so `s >= 1.85 - albedo`. If that stopped being
    // true the division would blow up for some authored colour and nothing else
    // in the feature would notice until a frame went white.
    for (i32 step = 0; step <= 100; ++step)
    {
        const f32 albedo = static_cast<f32>(step) / 100.0f;
        const f32 shape = SkinBurleyShapeFromAlbedo(albedo);
        EXPECT_TRUE(std::isfinite(shape));
        EXPECT_GT(shape, 0.8f) << "albedo " << albedo;
    }
}

TEST(SkinDiffusionTest, BurleyCdfIsMonotoneAndSaturates)
{
    constexpr f32 d = 1.0f;
    f32 previous = 0.0f;
    for (i32 step = 1; step <= 200; ++step)
    {
        const f32 r = static_cast<f32>(step) * 0.25f;
        const f32 cdf = SkinBurleyCdf(r, d);
        EXPECT_GE(cdf, previous) << "r " << r;
        previous = cdf;
    }
    EXPECT_NEAR(SkinBurleyCdf(400.0f, d), 1.0f, 1.0e-5f);
    // The pole is a returned zero, not an infinity: the kernel integrates
    // annuli and the annulus at the origin has no area.
    EXPECT_FLOAT_EQ(SkinBurleyProfile(0.0f, d), 0.0f);
    EXPECT_FLOAT_EQ(SkinBurleyCdf(0.0f, d), 0.0f);
}

TEST(SkinDiffusionTest, StripFractionMatchesDirectTwoDimensionalIntegration)
{
    // The one claim in the file that cannot be checked by inspection.
    constexpr f64 d = 1.549;
    for (const f64 a : { 0.1, 0.5, 1.0, 3.0, 8.0, 18.6 })
    {
        const f64 analytic = static_cast<f64>(SkinBurleyStripFraction(static_cast<f32>(a), static_cast<f32>(d)));
        const f64 brute = BruteForceStripFraction(a, d);
        EXPECT_NEAR(analytic, brute, 2.0e-3) << "strip half-width " << a;
    }
}

TEST(SkinDiffusionTest, StripFractionIsMonotoneAndBounded)
{
    constexpr f32 d = 0.8f;
    f32 previous = 0.0f;
    for (i32 step = 1; step <= 100; ++step)
    {
        const f32 a = static_cast<f32>(step) * 0.2f;
        const f32 strip = SkinBurleyStripFraction(a, d);
        EXPECT_GE(strip, previous);
        EXPECT_LE(strip, 1.0f);
        previous = strip;
    }
    EXPECT_FLOAT_EQ(SkinBurleyStripFraction(0.0f, d), 0.0f);
    EXPECT_FLOAT_EQ(SkinBurleyStripFraction(1.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(SkinBurleyStripFraction(std::numeric_limits<f32>::quiet_NaN(), d), 0.0f);
}

TEST(SkinDiffusionTest, RedScattersFurtherThanBlue)
{
    // The whole reason a skin profile is per-channel. If this inverts, the head
    // reads as blue-lit rather than as flesh and every other number in the file
    // can still be right.
    const glm::vec3 d = SkinBurleyScalingMM(ReferenceHead());
    EXPECT_GT(d.x, d.y);
    EXPECT_GT(d.y, d.z);
}

// -----------------------------------------------------------------------------
// The kernel
// -----------------------------------------------------------------------------

TEST(SkinDiffusionTest, KernelConservesEnergyPerChannel)
{
    for (const SkinDiffusionQuality quality :
         { SkinDiffusionQuality::Low, SkinDiffusionQuality::Medium, SkinDiffusionQuality::High })
    {
        const SkinDiffusionKernel kernel = BuildSkinDiffusionKernel(ReferenceHead(), quality);
        ASSERT_EQ(kernel.TapCount, GetSkinDiffusionTapCount(quality));

        glm::vec3 sums(0.0f);
        for (u32 tap = 0; tap < kernel.TapCount; ++tap)
        {
            const glm::vec4& entry = kernel.Taps[tap];
            EXPECT_TRUE(std::isfinite(entry.x));
            EXPECT_GE(entry.y, 0.0f);
            EXPECT_GE(entry.z, 0.0f);
            EXPECT_GE(entry.w, 0.0f);
            sums += glm::vec3(entry.y, entry.z, entry.w);
        }
        EXPECT_NEAR(sums.x, 1.0f, 1.0e-5f) << ToString(quality);
        EXPECT_NEAR(sums.y, 1.0f, 1.0e-5f) << ToString(quality);
        EXPECT_NEAR(sums.z, 1.0f, 1.0e-5f) << ToString(quality);
    }
}

TEST(SkinDiffusionTest, KernelIsSymmetricAndCentred)
{
    // An asymmetric kernel SHIFTS the image rather than blurring it, and a
    // sub-pixel shift of the diffuse half against the sharp specular half is
    // exactly the artefact this feature exists to avoid.
    const SkinDiffusionKernel kernel = BuildSkinDiffusionKernel(ReferenceHead(), SkinDiffusionQuality::Medium);
    const u32 half = kernel.TapCount / 2u;
    EXPECT_NEAR(kernel.Taps[half].x, 0.0f, 1.0e-6f);
    for (u32 i = 0; i < half; ++i)
    {
        const glm::vec4& low = kernel.Taps[i];
        const glm::vec4& high = kernel.Taps[kernel.TapCount - 1u - i];
        EXPECT_NEAR(low.x, -high.x, 1.0e-5f) << "tap " << i;
        EXPECT_NEAR(low.y, high.y, 1.0e-6f) << "tap " << i;
        EXPECT_NEAR(low.z, high.z, 1.0e-6f) << "tap " << i;
        EXPECT_NEAR(low.w, high.w, 1.0e-6f) << "tap " << i;
    }
}

TEST(SkinDiffusionTest, BlueIsConcentratedNearerTheCentreThanRed)
{
    // The per-channel colour response, stated as the thing an image shows: red
    // bleeds out of a shadow terminator and blue stays put. It is carried
    // entirely by the WEIGHTS, because all three channels share one set of tap
    // offsets — so the property to assert is that blue's weight falls off faster.
    const SkinDiffusionKernel kernel = BuildSkinDiffusionKernel(ReferenceHead(), SkinDiffusionQuality::High);
    const u32 half = kernel.TapCount / 2u;

    // THE CENTRE TAP'S WEIGHT, not a fraction within some window. The obvious
    // measure — "how much of each channel's energy lies within half the
    // support?" — SATURATES: the support is sized to RED, so green and blue both
    // report 1.000 there and the assertion that blue beats green would compare
    // two equal numbers. The centre weight is strictly ordered across the whole
    // range of authored profiles, because it is the one number the profile's
    // width acts on most directly.
    const glm::vec4& centre = kernel.Taps[half];
    EXPECT_GT(centre.w, centre.z) << "blue must be more concentrated than green";
    EXPECT_GT(centre.z, centre.y) << "green must be more concentrated than red";

    // And red really does reach further: some of its energy is out past half the
    // support, where blue's is not.
    glm::vec3 outer(0.0f);
    for (u32 tap = 0; tap < kernel.TapCount; ++tap)
    {
        if (std::abs(kernel.Taps[tap].x) > 0.5f)
            outer += glm::vec3(kernel.Taps[tap].y, kernel.Taps[tap].z, kernel.Taps[tap].w);
    }
    EXPECT_GT(outer.x, outer.z) << "red must carry more energy past half the support than blue";
    EXPECT_GT(outer.x, 0.005f) << "red carries no energy at all past half the support — the support is mis-sized";
}

TEST(SkinDiffusionTest, VersionZeroProfileBuildsTheIdentityKernel)
{
    // ADR 0024's rule, on the CPU side: turning the renderer's diffusion on must
    // not restate a profile authored against the older transport.
    SkinProfileParameters parameters = ReferenceHead();
    parameters.EvaluationModel = SkinEvaluationModel::DiffuseSpecularSplit;

    const SkinDiffusionKernel kernel = BuildSkinDiffusionKernel(parameters, SkinDiffusionQuality::High);
    EXPECT_TRUE(kernel.IsIdentity());
    EXPECT_EQ(kernel.TapCount, 1u);
    EXPECT_FLOAT_EQ(kernel.SupportRadiusMM, 0.0f);
    EXPECT_FLOAT_EQ(kernel.Taps[0].y, 1.0f);
    EXPECT_FLOAT_EQ(kernel.Taps[0].z, 1.0f);
    EXPECT_FLOAT_EQ(kernel.Taps[0].w, 1.0f);
}

TEST(SkinDiffusionTest, SmallestLegalRadiusStillProducesAFiniteKernel)
{
    // The authored floor is 1e-3 mm, which is far below a texel at any framing:
    // the profile fits entirely inside the centre tap, and a naive
    // normalisation would divide by zero and blur the channel to black.
    SkinProfileParameters parameters = ReferenceHead();
    parameters.ScatterRadiusMM = glm::vec3(kMinSkinScatterRadiusMM);
    ASSERT_TRUE(parameters.Sanitize());

    const SkinDiffusionKernel kernel = BuildSkinDiffusionKernel(parameters, SkinDiffusionQuality::Medium);
    glm::vec3 sums(0.0f);
    for (u32 tap = 0; tap < kernel.TapCount; ++tap)
    {
        EXPECT_TRUE(std::isfinite(kernel.Taps[tap].y));
        EXPECT_TRUE(std::isfinite(kernel.Taps[tap].z));
        EXPECT_TRUE(std::isfinite(kernel.Taps[tap].w));
        sums += glm::vec3(kernel.Taps[tap].y, kernel.Taps[tap].z, kernel.Taps[tap].w);
    }
    EXPECT_NEAR(sums.x, 1.0f, 1.0e-4f);
    EXPECT_NEAR(sums.y, 1.0f, 1.0e-4f);
    EXPECT_NEAR(sums.z, 1.0f, 1.0e-4f);
}

// -----------------------------------------------------------------------------
// The unit chain
// -----------------------------------------------------------------------------

TEST(SkinDiffusionTest, MillimetresConvertThroughTheEngineMetreConvention)
{
    // One world unit is one metre, so a millimetre is a thousandth. Asserted as
    // a NAMED constant relationship rather than as a magic 0.001 in the radius
    // expression, because the shader mirrors this exact number and the two
    // drifting apart is invisible in any single frame.
    EXPECT_FLOAT_EQ(kSkinMillimetresPerWorldUnit, 1000.0f);
    EXPECT_FLOAT_EQ(kSkinWorldUnitsPerMillimetre * kSkinMillimetresPerWorldUnit, 1.0f);

    // A 1000 mm radius is one world unit, and at one world unit of depth through
    // a 90-degree vertical field of view (P[1][1] == 1) on a 1000-pixel-tall
    // target it subtends exactly half the viewport height.
    EXPECT_FLOAT_EQ(SkinDiffusionRadiusPixels(1000.0f, 1.0f, 1.0f, 1000.0f),
                    std::min(500.0f, kMaxSkinDiffusionRadiusPixels));
}

TEST(SkinDiffusionTest, ScreenRadiusFollowsDepthResolutionAndFieldOfView)
{
    // The third acceptance criterion, as three ratios. Every value here is kept
    // well under kMaxSkinDiffusionRadiusPixels so the clamp does not mask a
    // scaling error — a test that measured a clamped radius would pass whatever
    // the maths did.
    constexpr f32 radiusMM = 2.0f;
    constexpr f32 baseDepth = 1.0f;
    constexpr f32 baseScale = 1.5f;
    constexpr f32 baseHeight = 1080.0f;

    const f32 base = SkinDiffusionRadiusPixels(radiusMM, baseDepth, baseScale, baseHeight);
    ASSERT_GT(base, 0.0f);
    ASSERT_LT(base, kMaxSkinDiffusionRadiusPixels);

    // Twice as far away is half the radius.
    EXPECT_NEAR(SkinDiffusionRadiusPixels(radiusMM, baseDepth * 2.0f, baseScale, baseHeight), base * 0.5f, 1.0e-4f);
    // Twice the vertical resolution is twice the radius — the blur covers the
    // same part of the FACE, which is what "follows resolution" has to mean.
    EXPECT_NEAR(SkinDiffusionRadiusPixels(radiusMM, baseDepth, baseScale, baseHeight * 2.0f), base * 2.0f, 1.0e-4f);
    // A narrower field of view (a larger P[1][1]) magnifies, so the radius grows.
    EXPECT_NEAR(SkinDiffusionRadiusPixels(radiusMM, baseDepth, baseScale * 2.0f, baseHeight), base * 2.0f, 1.0e-4f);
    // And the radius is linear in the authored millimetres.
    EXPECT_NEAR(SkinDiffusionRadiusPixels(radiusMM * 2.0f, baseDepth, baseScale, baseHeight), base * 2.0f, 1.0e-4f);
}

TEST(SkinDiffusionTest, ThicknessScaleDoesNotChangeTheScreenRadius)
{
    // The trap this feature was most likely to fall into. `ThicknessScale`
    // converts a MATERIAL's thickness into millimetres; it is not a second
    // opinion about how long a millimetre is. If it ever enters the radius
    // chain, a profile that exaggerates transmission quietly shrinks its own
    // blur — and its lower bound is 0, so the reciprocal that would need is not
    // even defined.
    SkinProfileParameters identity = ReferenceHead();
    SkinProfileParameters exaggerated = ReferenceHead();
    exaggerated.ThicknessScale = 4000.0f;
    SkinProfileParameters zeroed = ReferenceHead();
    zeroed.ThicknessScale = kMinSkinThicknessScale;
    ASSERT_TRUE(exaggerated.Sanitize());
    ASSERT_TRUE(zeroed.Sanitize());

    const f32 identitySupport = SkinDiffusionSupportRadiusMM(identity);
    EXPECT_FLOAT_EQ(SkinDiffusionSupportRadiusMM(exaggerated), identitySupport);
    EXPECT_FLOAT_EQ(SkinDiffusionSupportRadiusMM(zeroed), identitySupport);
    EXPECT_GT(identitySupport, 0.0f);
}

TEST(SkinDiffusionTest, ScreenRadiusIsBoundedAndRejectsDegenerateInput)
{
    // The fourth acceptance criterion's "bounded artifacts": a head against the
    // near plane must make the blur STOP WIDENING, not make the frame time
    // triple. And nothing non-finite may leave this function, because the value
    // goes straight into a uniform block.
    EXPECT_FLOAT_EQ(SkinDiffusionRadiusPixels(200.0f, 1.0e-4f, 2.0f, 2160.0f), kMaxSkinDiffusionRadiusPixels);
    EXPECT_FLOAT_EQ(SkinDiffusionRadiusPixels(2.0f, -1.0f, 1.5f, 1080.0f), 0.0f);
    EXPECT_FLOAT_EQ(SkinDiffusionRadiusPixels(2.0f, 1.0f, 1.5f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(SkinDiffusionRadiusPixels(std::numeric_limits<f32>::quiet_NaN(), 1.0f, 1.5f, 1080.0f), 0.0f);
    EXPECT_FLOAT_EQ(SkinDiffusionRadiusPixels(2.0f, std::numeric_limits<f32>::infinity(), 1.5f, 1080.0f), 0.0f);
}

TEST(SkinDiffusionTest, SupportRadiusCoversTheWidestChannel)
{
    // The tap offsets are shared, so the support has to be the widest channel's
    // — a support sized to the average would truncate red's tail, which is the
    // part of the profile that makes skin read as skin.
    const SkinProfileParameters parameters = ReferenceHead();
    const glm::vec3 d = SkinBurleyScalingMM(parameters);
    const f32 support = SkinDiffusionSupportRadiusMM(parameters);

    EXPECT_NEAR(SkinBurleyCdf(support, d.x), kSkinDiffusionSupportFraction, 1.0e-3f);
    EXPECT_GT(SkinBurleyCdf(support, d.y), kSkinDiffusionSupportFraction);
    EXPECT_GT(SkinBurleyCdf(support, d.z), kSkinDiffusionSupportFraction);
}

TEST(SkinDiffusionTest, TheGeneralisedTransportReducesToBurley)
{
    // THE CLAIM SkinDiffusion.h MAKES, ASSERTED RATHER THAN ASSUMED. #1368
    // opened up the two constants Burley fixed at 0.25 and 3, and the whole
    // argument for doing it that way — rather than as a second profile beside
    // the first — is that version 1 is then the SPECIAL CASE and not a separate
    // code path. That is only true if the generalised form reproduces the
    // original to the bit, so this is where it is checked.
    for (const f32 d : { 0.25f, 1.0f, 3.5f })
    {
        for (const f32 r : { 0.05f, 0.5f, 2.0f, 9.0f, 40.0f })
        {
            EXPECT_NEAR(SkinTransportCdf(r, d, 0.25f, 3.0f), SkinBurleyCdf(r, d), 1.0e-6f)
                << "r = " << r << ", d = " << d;
        }
        for (const f32 fraction : { 0.25f, 0.5f, 0.9f, kSkinDiffusionSupportFraction })
        {
            EXPECT_NEAR(SkinTransportRadiusForFraction(fraction, d, 0.25f, 3.0f),
                        SkinBurleyRadiusForFraction(fraction, d), 1.0e-3f * d)
                << "fraction = " << fraction << ", d = " << d;
        }
    }
}

TEST(SkinDiffusionTest, TheInversionBracketHoldsWhenTheRateRatioIsBelowOne)
{
    // A RATIO BELOW 1 MAKES THE PLAIN `d` TERM THE SLOW ONE, and a bracket
    // scaled by the ratio is then too SHORT — bisection converges on its own
    // upper bound and hands that back as the answer, silently, wrong by
    // whatever was asked for.
    //
    // Neither shipped model can reach this (Burley passes 3, version 6 passes
    // 4), so nothing in the engine was wrong. The function is public and takes
    // an arbitrary positive ratio, which is enough reason to pin it: the next
    // caller is the one that would have found out.
    //
    // Checked against the closed form, which exists when the mixture weight is
    // 1: CDF(r) = 1 - e^{-r/d}, so the fraction f is reached at -d ln(1 - f).
    constexpr f32 kD = 1.0f;
    for (const f32 ratio : { 0.1f, 0.25f, 0.5f, 1.0f, 3.0f })
    {
        for (const f32 fraction : { 0.9f, 0.99f, 0.999f })
        {
            const f32 produced = SkinTransportRadiusForFraction(fraction, kD, 1.0f, ratio);
            const f32 expected = -kD * std::log(1.0f - fraction);
            EXPECT_NEAR(produced, expected, 0.01f * expected)
                << "ratio = " << ratio << ", fraction = " << fraction << ": the bracket is too short";
        }
    }

    // And the CDF really does reach what the radius claims, for both shipped
    // ratios and a below-one one — the property the bracket exists to deliver.
    for (const f32 ratio : { 0.25f, 3.0f, 4.0f })
    {
        const f32 r = SkinTransportRadiusForFraction(kSkinDiffusionSupportFraction, kD, 0.5f, ratio);
        EXPECT_NEAR(SkinTransportCdf(r, kD, 0.5f, ratio), kSkinDiffusionSupportFraction, 1.0e-3f)
            << "ratio = " << ratio;
    }
}
