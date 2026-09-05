// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// ScreenSpaceDenoiseMathTest.cpp
//
// CPU contract tests for the SSGI / SSR denoiser chain (issue #708): the guided
// spatial kernel in OloEditor/assets/shaders/include/SpatialDenoise.glsl and the
// quad ray distribution in include/StochasticCommon.glsl.
//
// These pin the FORMULAS without a GL context, so they run in headless CI. The
// rendered frame is checked separately by SSGIVisualEvidenceTest — per the
// CLAUDE.md rendering rule, a math test proves the formula and a visual test
// proves the frame. Sibling of ScreenSpaceGIMathTest, which does the same job
// for the trace itself.
//
// The mirrors below are hand-ported from the GLSL. That is the point: if the
// shader's formula changes and this file does not, the test fails and someone
// has to decide which side was right. Every function names its GLSL original.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <limits>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    // ---- C++ mirrors of include/SpatialDenoise.glsl ------------------------

    constexpr float kSkyViewDepth = 60000.0f; // OLO_DENOISE_SKY_VIEW_DEPTH

    // OloDenoiseIsSky
    bool DenoiseIsSky(float viewDepth)
    {
        return !(viewDepth < kSkyViewDepth);
    }

    // OloDenoiseViewPosition
    glm::vec3 DenoiseViewPosition(const glm::mat4& invProjection, glm::vec2 uv, float viewDepth)
    {
        const glm::vec4 clip(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, -1.0f, 1.0f);
        const glm::vec4 nearPoint = invProjection * clip;
        const glm::vec3 ray = glm::vec3(nearPoint) / nearPoint.w;
        return ray * (viewDepth / std::max(-ray.z, 1.0e-6f));
    }

    // OloDenoisePlaneWeight
    float DenoisePlaneWeight(glm::vec3 centerPositionVS, glm::vec3 centerNormalVS, glm::vec3 samplePositionVS,
                             float relativeTolerance, float centerViewDepth)
    {
        const float tolerance = std::max(relativeTolerance * std::max(centerViewDepth, 1.0e-4f), 1.0e-4f);
        const float planeDistance = std::abs(glm::dot(samplePositionVS - centerPositionVS, centerNormalVS));
        return 1.0f - std::clamp(planeDistance / tolerance, 0.0f, 1.0f);
    }

    // OloDenoiseNormalWeight
    float DenoiseNormalWeight(glm::vec3 centerNormal, glm::vec3 sampleNormal, float power)
    {
        const float cosine = std::max(glm::dot(centerNormal, sampleNormal), 0.0f);
        return std::pow(cosine, std::max(power, 1.0f));
    }

    // OloDenoiseVarianceRadius
    float DenoiseVarianceRadius(float variance, float mean, float minRadius, float maxRadius, float knee)
    {
        const float sigma = std::sqrt(std::max(variance, 0.0f));
        const float relative = sigma / std::max(mean, 1.0e-4f);
        const float t = std::clamp(relative / std::max(knee, 1.0e-4f), 0.0f, 1.0f);
        return glm::mix(minRadius, maxRadius, t);
    }

    // OloDenoiseHistoryWidening
    float DenoiseHistoryWidening(float historyLength, float targetLength)
    {
        return 1.0f - std::clamp(historyLength / std::max(targetLength, 1.0f), 0.0f, 1.0f);
    }

    // OloDenoisePostBlurRadius
    float DenoisePostBlurRadius(float variance, float mean, float historyLength, float minRadius,
                                float maxRadius, float knee, float targetHistoryLength)
    {
        const float varianceRadius = DenoiseVarianceRadius(variance, mean, minRadius, maxRadius, knee);
        const float historyRadius =
            glm::mix(minRadius, maxRadius, DenoiseHistoryWidening(historyLength, targetHistoryLength));
        return std::max(varianceRadius, historyRadius);
    }

    // OloDenoiseTapOffset
    glm::ivec2 DenoiseTapOffset(glm::vec2 offset)
    {
        const glm::vec2 magnitude = glm::max(glm::abs(offset), glm::vec2(1.0f));
        const glm::vec2 signs(offset.x > 0.0f ? 1.0f : (offset.x < 0.0f ? -1.0f : 0.0f),
                              offset.y > 0.0f ? 1.0f : (offset.y < 0.0f ? -1.0f : 0.0f));
        const glm::vec2 stepped = signs * glm::ceil(magnitude - 0.5f);
        return glm::ivec2(static_cast<i32>(stepped.x), static_cast<i32>(stepped.y));
    }

    // OloDenoiseSpecularTapWeight
    float DenoiseSpecularTapWeight(float centerRoughness, float tapRoughness, float maxRoughness)
    {
        if (tapRoughness > maxRoughness)
            return 0.0f;
        const float tolerance = std::max(0.25f * std::max(centerRoughness, 0.02f), 0.02f);
        return 1.0f - std::clamp(std::abs(tapRoughness - centerRoughness) / tolerance, 0.0f, 1.0f);
    }

    // OloDenoiseRoughnessRadius
    float DenoiseRoughnessRadius(float roughness, float maxRadius, float knee)
    {
        const float edge = std::max(knee, 1.0e-4f);
        const float t = std::clamp(roughness / edge, 0.0f, 1.0f);
        return maxRadius * (t * t * (3.0f - 2.0f * t)); // smoothstep(0, knee, roughness)
    }

    // The 2x2 joint-bilateral footprint PostProcess_SSGIComposite.glsl builds.
    struct UpscaleFootprint
    {
        glm::ivec2 Base{ 0 };
        glm::vec4 Weights{ 0.0f };
    };

    UpscaleFootprint ComputeUpscaleFootprint(glm::ivec2 fullTexel, glm::vec2 fullSize, glm::vec2 traceSize)
    {
        const glm::vec2 scale = traceSize / fullSize;
        const glm::vec2 traceCoord = (glm::vec2(fullTexel) + 0.5f) * scale - 0.5f;
        const glm::ivec2 base(static_cast<i32>(std::floor(traceCoord.x)),
                              static_cast<i32>(std::floor(traceCoord.y)));
        const glm::vec2 frac = traceCoord - glm::vec2(base);
        return UpscaleFootprint{ base,
                                 glm::vec4((1.0f - frac.x) * (1.0f - frac.y),
                                           frac.x * (1.0f - frac.y),
                                           (1.0f - frac.x) * frac.y,
                                           frac.x * frac.y) };
    }

    // The stratum index OloSampleQuadDistributed2D hands OloSampleStratified2D,
    // and the resulting dimension-0 value BEFORE the blue-noise jitter (which is
    // a per-pixel offset in [0,1) applied inside the stratum). Mirrors
    // `(sampleIndex * 4 + quadPhase + jitter) / (sampleCount * 4)`.
    //
    // The phase includes the FRAME INDEX. Deriving it from the pixel alone —
    // which the first version did — pins each pixel to one quarter of the
    // hemisphere forever, so its converged value is the mean of that quarter
    // rather than of the whole: a bias no temporal resolve can remove.
    u32 QuadPhase(glm::ivec2 pixel, u32 frameIndex)
    {
        return (static_cast<u32>(pixel.x & 1) + 2u * static_cast<u32>(pixel.y & 1) + frameIndex) & 3u;
    }

    float QuadStratumLow(glm::ivec2 pixel, u32 frameIndex, u32 sampleIndex, u32 sampleCount)
    {
        const u32 widenedIndex = sampleIndex * 4u + QuadPhase(pixel, frameIndex);
        return static_cast<float>(widenedIndex) / static_cast<float>(sampleCount * 4u);
    }

    constexpr float kTolerance = 1.0e-4f;
} // namespace

// -----------------------------------------------------------------------------
// Geometry: the reconstruction the whole chain is guided by
// -----------------------------------------------------------------------------

// The chain carries a view DEPTH in the signal's alpha and never re-samples a
// depth buffer, so OloDenoiseViewPosition has to rebuild the exact view-space
// point from a UV plus that depth. If it does not, every plane weight in every
// stage is measured against the wrong point.
TEST(ScreenSpaceDenoiseMath, ViewPositionRoundTripsThroughProjection)
{
    const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    const glm::mat4 invProjection = glm::inverse(projection);

    // A spread of view-space points, all in front of the camera.
    const std::array<glm::vec3, 5> points = {
        glm::vec3(0.0f, 0.0f, -1.0f),
        glm::vec3(0.7f, -0.4f, -3.0f),
        glm::vec3(-2.0f, 1.5f, -12.0f),
        glm::vec3(5.0f, 5.0f, -40.0f),
        glm::vec3(-0.05f, 0.02f, -0.5f),
    };

    for (const glm::vec3& point : points)
    {
        const glm::vec4 clip = projection * glm::vec4(point, 1.0f);
        const glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f;
        const float viewDepth = -point.z;

        const glm::vec3 reconstructed = DenoiseViewPosition(invProjection, uv, viewDepth);

        EXPECT_NEAR(reconstructed.x, point.x, 1.0e-3f) << "point z=" << point.z;
        EXPECT_NEAR(reconstructed.y, point.y, 1.0e-3f) << "point z=" << point.z;
        EXPECT_NEAR(reconstructed.z, point.z, 1.0e-3f) << "point z=" << point.z;
    }
}

// THE REGRESSION THE PLANE TEST EXISTS FOR. A tap on the same flat floor as the
// centre pixel, seen at a grazing angle, differs from it in VIEW DEPTH by metres
// while lying exactly in the centre's tangent plane. A depth-difference test
// throws that tap away — and it is the most valuable tap there is, because it is
// the same surface. Measuring along the normal keeps it at full weight.
TEST(ScreenSpaceDenoiseMath, PlaneWeightKeepsGrazingCoplanarTaps)
{
    // A floor: normal +y in view space, centre 10 units away along -z.
    const glm::vec3 normal(0.0f, 1.0f, 0.0f);
    const glm::vec3 center(0.0f, -2.0f, -10.0f);
    // Same plane (same y), but 6 units further down -z: a 6-unit view-depth gap.
    const glm::vec3 grazingTap(0.0f, -2.0f, -16.0f);

    const float weight = DenoisePlaneWeight(center, normal, grazingTap, kSSGIDenoisePlaneTolerance, 10.0f);
    EXPECT_NEAR(weight, 1.0f, kTolerance)
        << "a coplanar tap must keep full weight no matter how far it is in depth";

    // The wall behind it, at the same view depth as the centre but a metre up
    // out of the plane, is rejected outright: 1 m is far beyond 5% of 10.
    const glm::vec3 wallTap(0.0f, -1.0f, -10.0f);
    EXPECT_NEAR(DenoisePlaneWeight(center, normal, wallTap, kSSGIDenoisePlaneTolerance, 10.0f), 0.0f,
                kTolerance);
}

// The tolerance is RELATIVE to the shading distance, so the same physical
// surface behaves identically near and far. A fixed view-space tolerance would
// be centimetres at 1 m and useless at 100 m.
TEST(ScreenSpaceDenoiseMath, PlaneWeightToleranceScalesWithDepth)
{
    const glm::vec3 normal(0.0f, 0.0f, 1.0f);

    // Half a tolerance out of plane at each distance -> the same 0.5 weight.
    for (const float depth : { 1.0f, 10.0f, 100.0f })
    {
        const glm::vec3 center(0.0f, 0.0f, -depth);
        const float halfTolerance = 0.5f * kSSGIDenoisePlaneTolerance * depth;
        const glm::vec3 tap = center + normal * halfTolerance;
        EXPECT_NEAR(DenoisePlaneWeight(center, normal, tap, kSSGIDenoisePlaneTolerance, depth), 0.5f,
                    1.0e-3f)
            << "depth " << depth;
    }
}

TEST(ScreenSpaceDenoiseMath, NormalWeightFallsOffAndNeverGoesNegative)
{
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    EXPECT_NEAR(DenoiseNormalWeight(up, up, kSSGIDenoiseNormalPower), 1.0f, kTolerance);

    // Monotone decreasing as the tap normal turns away.
    float previous = 1.0f;
    for (const float degrees : { 10.0f, 20.0f, 45.0f, 60.0f, 89.0f })
    {
        const float radians = glm::radians(degrees);
        const glm::vec3 turned(std::sin(radians), std::cos(radians), 0.0f);
        const float weight = DenoiseNormalWeight(up, turned, kSSGIDenoiseNormalPower);
        EXPECT_LT(weight, previous) << degrees << " degrees";
        EXPECT_GE(weight, 0.0f);
        previous = weight;
    }

    // A back-facing tap contributes nothing rather than a negative weight, which
    // would SUBTRACT radiance from the accumulator.
    EXPECT_NEAR(DenoiseNormalWeight(up, -up, kSSGIDenoiseNormalPower), 0.0f, kTolerance);
}

// -----------------------------------------------------------------------------
// The radius guides
// -----------------------------------------------------------------------------

TEST(ScreenSpaceDenoiseMath, VarianceRadiusIsRelativeToTheMean)
{
    constexpr float kMin = 0.0f;
    constexpr float kMax = 4.0f;
    constexpr float kKnee = kSSGIDenoiseVarianceKnee;

    // A converged pixel takes the minimum radius.
    EXPECT_NEAR(DenoiseVarianceRadius(0.0f, 1.0f, kMin, kMax, kKnee), kMin, kTolerance);

    // At sigma == knee * mean it saturates.
    const float mean = 2.0f;
    const float sigmaAtKnee = kKnee * mean;
    EXPECT_NEAR(DenoiseVarianceRadius(sigmaAtKnee * sigmaAtKnee, mean, kMin, kMax, kKnee), kMax, 1.0e-3f);

    // EXPOSURE INDEPENDENCE, which is the whole reason the guide is relative:
    // scaling the signal scales sigma with it and must not move the radius. A
    // guide on raw variance would widen every filter the moment exposure rose.
    const float scale = 8.0f;
    const float scaledSigma = 0.25f * mean * scale;
    EXPECT_NEAR(DenoiseVarianceRadius(scaledSigma * scaledSigma, mean * scale, kMin, kMax, kKnee),
                DenoiseVarianceRadius((0.25f * mean) * (0.25f * mean), mean, kMin, kMax, kKnee), 1.0e-3f);
}

TEST(ScreenSpaceDenoiseMath, HistoryWideningIsFullOnDisocclusionAndGoneAtTheTarget)
{
    constexpr float kTarget = kSSGIDenoiseTargetHistoryLength;
    EXPECT_NEAR(DenoiseHistoryWidening(0.0f, kTarget), 1.0f, kTolerance);
    EXPECT_NEAR(DenoiseHistoryWidening(kTarget, kTarget), 0.0f, kTolerance);
    EXPECT_NEAR(DenoiseHistoryWidening(kTarget * 4.0f, kTarget), 0.0f, kTolerance);
    EXPECT_NEAR(DenoiseHistoryWidening(kTarget * 0.5f, kTarget), 0.5f, kTolerance);
}

// `max`, not a sum and not a product. Either reason alone is sufficient grounds
// to widen; multiplying them would let a converged-but-noisy region cancel
// against a fresh-but-flat one and leave both under-filtered.
TEST(ScreenSpaceDenoiseMath, PostBlurRadiusTakesTheWiderOfTheTwoReasons)
{
    constexpr float kMin = 0.0f;
    constexpr float kMax = 4.0f;
    constexpr float kKnee = kSSGIDenoiseVarianceKnee;
    constexpr float kTarget = kSSGIDenoiseTargetHistoryLength;

    // Fresh but perfectly flat: the history term alone must still widen fully.
    EXPECT_NEAR(DenoisePostBlurRadius(0.0f, 1.0f, 0.0f, kMin, kMax, kKnee, kTarget), kMax, kTolerance);

    // Converged but noisy: the variance term alone must still widen fully.
    const float sigma = kKnee * 1.0f;
    EXPECT_NEAR(DenoisePostBlurRadius(sigma * sigma, 1.0f, kTarget * 2.0f, kMin, kMax, kKnee, kTarget),
                kMax, 1.0e-3f);

    // Converged AND flat: no filtering at all.
    EXPECT_NEAR(DenoisePostBlurRadius(0.0f, 1.0f, kTarget * 2.0f, kMin, kMax, kKnee, kTarget), kMin,
                kTolerance);

    // A zero maximum radius disables the stage outright at every input, which is
    // the contract SSGIRenderPass relies on to skip the draw.
    EXPECT_NEAR(DenoisePostBlurRadius(1.0f, 0.01f, 0.0f, 0.0f, 0.0f, kKnee, kTarget), 0.0f, kTolerance);
}

// SSR's guide, and the one place the two chains genuinely differ. A mirror must
// come out of the filter untouched however noisy its reflection looks.
TEST(ScreenSpaceDenoiseMath, RoughnessRadiusLeavesAMirrorAlone)
{
    constexpr float kMax = 3.0f;
    constexpr float kKnee = kSSRDenoiseRoughnessKnee;

    EXPECT_NEAR(DenoiseRoughnessRadius(0.0f, kMax, kKnee), 0.0f, kTolerance);
    // SSR_MIN_ROUGHNESS (0.045) is the roughest a "mirror" gets in this engine;
    // it must still be filtered far less than a rough surface.
    EXPECT_LT(DenoiseRoughnessRadius(0.045f, kMax, kKnee), 0.15f * kMax);
    EXPECT_NEAR(DenoiseRoughnessRadius(kKnee, kMax, kKnee), kMax, 1.0e-3f);
    EXPECT_NEAR(DenoiseRoughnessRadius(1.0f, kMax, kKnee), kMax, 1.0e-3f);

    // Monotone, so a roughness gradient across a surface never produces a
    // filter that narrows as the surface gets rougher.
    float previous = -1.0f;
    for (const float roughness : { 0.0f, 0.05f, 0.1f, 0.2f, 0.3f, 0.6f, 1.0f })
    {
        const float radius = DenoiseRoughnessRadius(roughness, kMax, kKnee);
        EXPECT_GE(radius, previous) << "roughness " << roughness;
        previous = radius;
    }
}

// -----------------------------------------------------------------------------
// The sky sentinel
// -----------------------------------------------------------------------------

// Every stage rejects taps whose alpha is the sky sentinel. The predicate is
// written inverted (`!(d < sentinel)`) so a NaN — which compares false against
// everything — is treated as sky rather than as a valid surface at depth NaN,
// which would propagate the NaN into the accumulator and, once bloom had spread
// it, into a black block.
TEST(ScreenSpaceDenoiseMath, SkyPredicateRejectsTheSentinelAndNaN)
{
    EXPECT_FALSE(DenoiseIsSky(0.5f));
    EXPECT_FALSE(DenoiseIsSky(1000.0f));
    EXPECT_TRUE(DenoiseIsSky(kSkyViewDepth));
    EXPECT_TRUE(DenoiseIsSky(kSkyViewDepth * 2.0f));
    EXPECT_TRUE(DenoiseIsSky(std::numeric_limits<float>::infinity()));
    EXPECT_TRUE(DenoiseIsSky(std::numeric_limits<float>::quiet_NaN()));

    // The sentinel must survive an RGBA16F round trip, or the guard it powers is
    // comparing against a value that is no longer there. 60000 is exactly
    // representable in half (1875 x 32); 70000 would be stored as +Inf.
    EXPECT_LT(kSkyViewDepth, 65504.0f) << "the half-float maximum";
}

// -----------------------------------------------------------------------------
// The guided upscale footprint
// -----------------------------------------------------------------------------

// At full resolution the upscale must be an exact copy, with no separate code
// path to keep correct. This is the property that lets SSGIHalfResolution be a
// pure setting rather than a second shader.
TEST(ScreenSpaceDenoiseMath, UpscaleFootprintDegeneratesAtFullResolution)
{
    const glm::vec2 size(1280.0f, 720.0f);
    for (const glm::ivec2 texel : { glm::ivec2(0, 0), glm::ivec2(1, 0), glm::ivec2(637, 411),
                                    glm::ivec2(1279, 719) })
    {
        const UpscaleFootprint footprint = ComputeUpscaleFootprint(texel, size, size);
        EXPECT_EQ(footprint.Base, texel);
        EXPECT_NEAR(footprint.Weights.x, 1.0f, kTolerance);
        EXPECT_NEAR(footprint.Weights.y, 0.0f, kTolerance);
        EXPECT_NEAR(footprint.Weights.z, 0.0f, kTolerance);
        EXPECT_NEAR(footprint.Weights.w, 0.0f, kTolerance);
    }
}

// THE HALF-TEXEL BUG, which is the one an upscale actually gets wrong: the
// footprint has to be CENTRED on the trace-band point the full-res pixel maps
// to. Dropping either `+ 0.5` shifts the whole indirect layer by half a
// trace-band texel — a uniform, plausible-looking offset that only shows as
// indirect light leaking to one side of every object.
//
// Asserted as the weighted centre of mass of the four taps landing exactly on
// that point, which is the property, rather than by restating the formula.
TEST(ScreenSpaceDenoiseMath, UpscaleFootprintIsCenteredAndSumsToOne)
{
    const glm::vec2 fullSize(1280.0f, 720.0f);
    const glm::vec2 traceSize(640.0f, 360.0f);
    const glm::vec2 scale = traceSize / fullSize;
    const std::array<glm::ivec2, 4> offsets = { glm::ivec2(0, 0), glm::ivec2(1, 0), glm::ivec2(0, 1),
                                                glm::ivec2(1, 1) };

    for (i32 y = 0; y < 8; ++y)
    {
        for (i32 x = 0; x < 8; ++x)
        {
            const UpscaleFootprint footprint = ComputeUpscaleFootprint({ x, y }, fullSize, traceSize);
            const float sum = footprint.Weights.x + footprint.Weights.y + footprint.Weights.z +
                              footprint.Weights.w;
            EXPECT_NEAR(sum, 1.0f, kTolerance) << "pixel " << x << "," << y;

            glm::vec2 centerOfMass(0.0f);
            for (sizet i = 0; i < offsets.size(); ++i)
                centerOfMass += footprint.Weights[static_cast<i32>(i)] *
                                (glm::vec2(footprint.Base + offsets[i]) + 0.5f);

            // Where this full-res pixel's centre lands in trace-band texel
            // coordinates, measured from the texel-centre convention on both
            // sides.
            const glm::vec2 expected = (glm::vec2(x, y) + 0.5f) * scale;
            EXPECT_NEAR(centerOfMass.x, expected.x, 1.0e-4f) << "pixel " << x << "," << y;
            EXPECT_NEAR(centerOfMass.y, expected.y, 1.0e-4f) << "pixel " << x << "," << y;
        }
    }
}

// -----------------------------------------------------------------------------
// Ray distribution over the 2x2 quad (issue #708, step 1)
// -----------------------------------------------------------------------------

// The claim the pre-blur cashes in: the four pixels of a quad sample DISJOINT
// strata, so averaging their neighbourhood recovers close to 4x the ray count
// instead of re-averaging near-identical hemispheres.
TEST(ScreenSpaceDenoiseMath, QuadDistributionGivesTheFourPixelsDisjointStrata)
{
    constexpr u32 kRayCount = 4;

    // Must hold in EVERY frame, not just frame 0 — the phase rotates, and a
    // rotation that collided two pixels onto one stratum in some frames would
    // still pass a frame-0-only check.
    for (u32 frame = 0; frame < 8; ++frame)
    {
        std::array<bool, kRayCount * 4> occupied{};
        for (i32 y = 0; y < 2; ++y)
        {
            for (i32 x = 0; x < 2; ++x)
            {
                for (u32 r = 0; r < kRayCount; ++r)
                {
                    const u32 stratum = r * 4u + QuadPhase({ x, y }, frame);
                    ASSERT_LT(stratum, occupied.size());
                    EXPECT_FALSE(occupied[stratum])
                        << "frame " << frame << ": stratum " << stratum
                        << " claimed twice within one quad";
                    occupied[stratum] = true;
                }
            }
        }

        // And together they cover the whole widened grid — no stratum unsampled.
        for (sizet i = 0; i < occupied.size(); ++i)
            EXPECT_TRUE(occupied[i]) << "frame " << frame << ": stratum " << i
                                     << " never sampled by the quad";
    }
}

// Each pixel on its own must STILL be stratified at 1/sampleCount spacing.
// Widening the grid without preserving this would trade the per-pixel
// stratification (which measurably beat the old sampler) for the cross-pixel
// one, rather than getting both.
TEST(ScreenSpaceDenoiseMath, QuadDistributionKeepsPerPixelStratification)
{
    constexpr u32 kRayCount = 8;
    const float expectedSpacing = 1.0f / static_cast<float>(kRayCount);

    for (i32 y = 0; y < 2; ++y)
    {
        for (i32 x = 0; x < 2; ++x)
        {
            for (u32 frame = 0; frame < 4; ++frame)
            {
                for (u32 r = 1; r < kRayCount; ++r)
                {
                    const float spacing = QuadStratumLow({ x, y }, frame, r, kRayCount) -
                                          QuadStratumLow({ x, y }, frame, r - 1, kRayCount);
                    EXPECT_NEAR(spacing, expectedSpacing, kTolerance)
                        << "pixel " << x << "," << y << " frame " << frame << " ray " << r;
                }
                // Every stratum stays inside [0, 1): a sample index that ran off
                // the end would feed a radius outside the hemisphere mapping's
                // domain.
                EXPECT_GE(QuadStratumLow({ x, y }, frame, 0, kRayCount), 0.0f);
                EXPECT_LT(QuadStratumLow({ x, y }, frame, kRayCount - 1, kRayCount), 1.0f);
            }
        }
    }
}

// THE BIAS FIX, and the reason the phase is not a pure function of the pixel.
//
// A phase pinned to the pixel's parity means a pixel samples the same quarter of
// the hemisphere in every frame, so what it converges to is the mean of THAT
// QUARTER, not of the hemisphere — a bias, which is invisible to every noise
// metric and which no amount of temporal accumulation removes. Rotating by the
// frame index makes each pixel visit all four phases over four frames, so its
// own temporal average is unbiased.
TEST(ScreenSpaceDenoiseMath, EveryPixelVisitsEveryQuadPhaseOverFourFrames)
{
    for (i32 y = 0; y < 2; ++y)
    {
        for (i32 x = 0; x < 2; ++x)
        {
            std::array<bool, 4> seen{};
            for (u32 frame = 0; frame < 4; ++frame)
                seen[QuadPhase({ x, y }, frame)] = true;
            for (sizet phase = 0; phase < seen.size(); ++phase)
                EXPECT_TRUE(seen[phase]) << "pixel " << x << "," << y << " never sampled phase "
                                         << phase << " in four frames — its converged value is the "
                                                     "mean of a subset of the hemisphere, which is a bias";
        }
    }

    // Position still only matters modulo 2: the phase is a quad property, so two
    // pixels in the same quad position share it (in any given frame).
    for (u32 frame = 0; frame < 4; ++frame)
    {
        EXPECT_EQ(QuadPhase({ 0, 0 }, frame), QuadPhase({ 64, 128 }, frame));
        EXPECT_EQ(QuadPhase({ 1, 0 }, frame), QuadPhase({ 101, 200 }, frame));
        EXPECT_EQ(QuadPhase({ 0, 1 }, frame), QuadPhase({ 8, 9 }, frame));
        EXPECT_EQ(QuadPhase({ 1, 1 }, frame), QuadPhase({ 777, 333 }, frame));
    }
}

// -----------------------------------------------------------------------------
// The tap pattern
// -----------------------------------------------------------------------------

// THE DEFECT THIS PINS: `ivec2(round(offset))` puts three or four of the eight
// Poisson taps back on the CENTRE at a radius of 1-2 texels, because the disc
// points have magnitudes from 0.16 to 0.95. The centre is then counted four or
// five times, the kernel is far weaker than "8 taps" suggests, and nothing about
// the code says so. It shipped as the default pre-blur radius for a while.
TEST(ScreenSpaceDenoiseMath, EveryDiscTapLandsOffTheCentreTexel)
{
    // The real disc, at the radii the chain actually uses.
    const std::array<glm::vec2, 8> poisson = {
        glm::vec2(-0.4706069f, -0.4427112f),
        glm::vec2(-0.9057375f, +0.3003471f),
        glm::vec2(-0.3487388f, +0.4037880f),
        glm::vec2(+0.1023042f, +0.6439373f),
        glm::vec2(+0.5699277f, +0.3513750f),
        glm::vec2(+0.2939128f, -0.1131226f),
        glm::vec2(+0.7836658f, -0.4208784f),
        glm::vec2(+0.1564120f, -0.8198990f),
    };

    for (const float radius : { 0.5f, 1.0f, 2.0f, 4.0f })
    {
        for (const glm::vec2& point : poisson)
        {
            const glm::ivec2 tap = DenoiseTapOffset(point * radius);
            EXPECT_NE(tap, glm::ivec2(0, 0))
                << "radius " << radius << " tap (" << point.x << "," << point.y
                << ") collapsed onto the centre texel, which would weight the centre twice";
        }
    }
}

// The offset must keep the disc's DIRECTION — pushing a tap off the centre is
// only legitimate if it goes the way the pattern pointed.
TEST(ScreenSpaceDenoiseMath, TapOffsetPreservesDirection)
{
    for (const glm::vec2 offset : { glm::vec2(0.3f, 0.2f), glm::vec2(-0.3f, 0.2f),
                                    glm::vec2(0.3f, -0.2f), glm::vec2(-0.9f, -0.4f),
                                    glm::vec2(3.4f, -2.6f) })
    {
        const glm::ivec2 tap = DenoiseTapOffset(offset);
        if (offset.x > 0.0f)
            EXPECT_GT(tap.x, 0);
        if (offset.x < 0.0f)
            EXPECT_LT(tap.x, 0);
        if (offset.y > 0.0f)
            EXPECT_GT(tap.y, 0);
        if (offset.y < 0.0f)
            EXPECT_LT(tap.y, 0);
    }

    // A large offset must still round normally rather than being snapped to 1.
    EXPECT_EQ(DenoiseTapOffset({ 3.4f, -2.6f }), glm::ivec2(3, -3));
}

// -----------------------------------------------------------------------------
// SSR's specular tap rejection
// -----------------------------------------------------------------------------

// The SSR trace early-outs to a ZERO delta above its roughness cutoff while
// still writing a valid depth and normal to the guide. A purely geometric filter
// therefore averages those zeros in as if they were real, and a polished patch
// next to a rough one on the SAME coplanar floor gets a dark band the width of
// the blur radius — every geometric test passes, which is what makes it hard to
// see in code.
TEST(ScreenSpaceDenoiseMath, SpecularTapWeightRejectsWhatTheTraceExcluded)
{
    constexpr float kMaxRoughness = 0.8f;

    // A tap past the trace's cutoff contributed nothing by construction.
    EXPECT_NEAR(DenoiseSpecularTapWeight(0.5f, 0.9f, kMaxRoughness), 0.0f, kTolerance);
    EXPECT_NEAR(DenoiseSpecularTapWeight(0.5f, 1.0f, kMaxRoughness), 0.0f, kTolerance);

    // A tap on the same lobe is kept at full weight.
    EXPECT_NEAR(DenoiseSpecularTapWeight(0.5f, 0.5f, kMaxRoughness), 1.0f, kTolerance);

    // A mirror rejects a rough neighbour far more aggressively than a rough
    // surface rejects a slightly rougher one — the tolerance scales with the
    // centre's own lobe width, which is what keeps a sharp reflection sharp
    // right up to a material seam.
    const float mirrorRejectsRough = DenoiseSpecularTapWeight(0.02f, 0.20f, kMaxRoughness);
    const float roughAcceptsRough = DenoiseSpecularTapWeight(0.60f, 0.68f, kMaxRoughness);
    EXPECT_NEAR(mirrorRejectsRough, 0.0f, kTolerance);
    EXPECT_GT(roughAcceptsRough, 0.4f);

    // Never negative: a negative weight would SUBTRACT a neighbour's reflection.
    for (const float tap : { 0.0f, 0.1f, 0.3f, 0.5f, 0.79f })
        EXPECT_GE(DenoiseSpecularTapWeight(0.3f, tap, kMaxRoughness), 0.0f);
}

// -----------------------------------------------------------------------------
// The settings contract
// -----------------------------------------------------------------------------

// The sanitizer is the backstop for values arriving from a scene file, a script
// or an MCP write. A NaN radius reaching the shader makes `radius > 0.0` false
// and SILENTLY disables the stage, which is the failure mode this repo's
// no-silent-fallbacks rule exists to prevent.
TEST(ScreenSpaceDenoiseMath, SanitizersClampTheDenoiseRadii)
{
    PostProcessSettings settings;
    settings.SSGIPreBlurRadius = std::numeric_limits<f32>::quiet_NaN();
    settings.SSGIPostBlurRadius = 1.0e9f;
    settings.SSRPreBlurRadius = -5.0f;
    settings.SSRPostBlurRadius = std::numeric_limits<f32>::infinity();

    SanitizeSSGI(settings);
    SanitizeSSR(settings);

    EXPECT_TRUE(std::isfinite(settings.SSGIPreBlurRadius));
    EXPECT_FLOAT_EQ(settings.SSGIPreBlurRadius, 1.0f) << "a NaN must fall back to the default, not to 0";
    EXPECT_FLOAT_EQ(settings.SSGIPostBlurRadius, kScreenSpaceMaxDenoiseRadius);
    EXPECT_FLOAT_EQ(settings.SSRPreBlurRadius, 0.0f);
    EXPECT_FLOAT_EQ(settings.SSRPostBlurRadius, 3.0f) << "an Inf must fall back to the default";
}

// The chain's defaults must all be ON, because the acceptance criteria for
// issue #708 are about the shipped configuration and not about a feature nobody
// turns on. If a default is deliberately flipped, this test is where to say so.
TEST(ScreenSpaceDenoiseMath, DenoiserChainIsOnByDefault)
{
    const PostProcessSettings settings;
    EXPECT_TRUE(settings.SSGIHalfResolution);
    EXPECT_TRUE(settings.SSGIRayDistribution);
    EXPECT_GT(settings.SSGIPreBlurRadius, 0.0f);
    EXPECT_GT(settings.SSGIPostBlurRadius, 0.0f);
    EXPECT_GT(settings.SSRPreBlurRadius, 0.0f);
    EXPECT_GT(settings.SSRPostBlurRadius, 0.0f);
    // The spatial and temporal halves must agree about what "the same surface"
    // is, or a tap the pre-blur accepted is one the resolve rejects.
    EXPECT_FLOAT_EQ(kSSGIDenoisePlaneTolerance, 0.05f)
        << "kept equal to SSGI_DEPTH_TOLERANCE in PostProcess_SSGIResolve.glsl";
}
