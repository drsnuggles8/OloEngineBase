// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// BloomMathTest.cpp
//
// Bloom threshold soft-knee + additive-composite CPU contract tests. These pin
// the math implemented in PostProcess_BloomThreshold.glsl and
// PostProcess_BloomComposite.glsl WITHOUT a GL context (headless CI), mirroring
// the *MathTest.cpp style of ScreenSpaceReflectionMathTest.
//
// Complementary to the existing GPU bloom coverage, which this file does NOT
// duplicate (PostProcessPropertyTests.cpp):
//   * BloomThresholdTest.BlackInputStaysBlack — black extracts nothing.
//   * BloomDownsampleTest / BloomUpsampleTest — kernel weights sum to 1.
//   * BloomCompositeTest.ZeroIntensityPassesSceneThrough — intensity 0 = scene.
//   * BloomChainEnergyTest — multi-mip down/up energy conservation.
//
// What this file adds: the THRESHOLD EXTRACTION CURVE direction (which the GPU
// tests only probe at the trivial black case), namely —
//   * Sub-threshold pixels are fully suppressed (the soft-knee gate).
//   * Above-threshold HDR pixels are extracted and the extracted amount grows
//     monotonically with brightness until the knee saturates, after which the
//     bright pixel passes through unmodified.
//   * The additive composite preserves the bloom colour (a green source bleeds
//     green) and scales linearly with intensity — the math behind the visual
//     test's "coloured halo around a bright source" contract.
//
// Per the CLAUDE.md rendering rule: math/contract tests prove the formula; the
// GPU BloomVisualEvidenceTest proves the rendered frame actually looks right.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cmath>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    // Rec. 709 luma weights, matching the dot() in PostProcess_BloomThreshold.glsl.
    constexpr glm::vec3 kLuma(0.2126f, 0.7152f, 0.0722f);

    // step(edge, x) — GLSL semantics: 1 if x >= edge, else 0.
    f32 Step(f32 edge, f32 x)
    {
        return x >= edge ? 1.0f : 0.0f;
    }

    // C++ mirror of the FINAL bright-extraction expression in
    // PostProcess_BloomThreshold.glsl (the earlier `bright = max(...)` line is
    // overwritten by this one, so this is the effective formula):
    //   brightness = dot(color, luma)
    //   softness   = clamp(brightness - threshold + 0.5, 0, 1)^2
    //   bright     = color * softness * step(threshold, brightness + 0.5)
    glm::vec3 ExtractBright(const glm::vec3& color, f32 threshold)
    {
        const f32 brightness = glm::dot(color, kLuma);
        f32 softness = glm::clamp(brightness - threshold + 0.5f, 0.0f, 1.0f);
        softness = softness * softness;
        return color * softness * Step(threshold, brightness + 0.5f);
    }

    // PostProcess_BloomComposite.glsl: result = sceneColor + bloomColor * intensity.
    glm::vec3 CompositeBloom(const glm::vec3& scene, const glm::vec3& bloom, f32 intensity)
    {
        return scene + bloom * intensity;
    }
} // namespace

// ---- Threshold extraction: black & sub-threshold are suppressed --------------

TEST(BloomMathTest, BlackInputExtractsNothing)
{
    const glm::vec3 bright = ExtractBright(glm::vec3(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(bright.r, 0.0f);
    EXPECT_FLOAT_EQ(bright.g, 0.0f);
    EXPECT_FLOAT_EQ(bright.b, 0.0f);
}

// A dim pixel whose brightness is more than 0.5 below the threshold fails the
// step() gate (brightness + 0.5 < threshold) and is fully suppressed. This is
// the soft-knee gate that keeps ordinary mid-tones out of the bloom buffer.
TEST(BloomMathTest, SubThresholdPixelSuppressed)
{
    constexpr f32 kThreshold = 1.0f;
    // Grey 0.3 → brightness 0.3; 0.3 + 0.5 = 0.8 < 1.0 → gated off.
    const glm::vec3 bright = ExtractBright(glm::vec3(0.3f), kThreshold);
    EXPECT_FLOAT_EQ(bright.r, 0.0f);
    EXPECT_FLOAT_EQ(bright.g, 0.0f);
    EXPECT_FLOAT_EQ(bright.b, 0.0f);

    // Right at the gate edge: brightness + 0.5 == threshold passes the step but
    // softness is (0.5)^2 wait — at brightness = threshold - 0.5, softness =
    // clamp(0,0,1)^2 = 0, so the extracted value is still zero. The knee starts
    // exactly here.
    const f32 b = kThreshold - 0.5f;
    const glm::vec3 atEdge = ExtractBright(glm::vec3(b), kThreshold); // grey, brightness ≈ b
    EXPECT_NEAR(atEdge.r, 0.0f, 1e-6f);
    EXPECT_NEAR(atEdge.g, 0.0f, 1e-6f);
    EXPECT_NEAR(atEdge.b, 0.0f, 1e-6f);
}

// ---- Threshold extraction: HDR pixels are extracted, growing then saturating -

// A bright HDR pixel above the threshold is extracted, and the extracted amount
// rises monotonically with brightness through the soft knee, then — once the
// knee saturates (brightness >= threshold + 0.5) — the whole pixel passes
// through (softness == 1, so bright == color).
TEST(BloomMathTest, AboveThresholdExtractedMonotonicThenFullPassthrough)
{
    constexpr f32 kThreshold = 1.0f;

    // Sweep a grey ramp from just below the knee to well above it. Track the
    // extracted green channel (grey, so all channels equal).
    f32 prev = -1.0f;
    for (u32 i = 0; i <= 200; ++i)
    {
        const f32 v = static_cast<f32>(i) * 0.02f; // 0 .. 4
        const glm::vec3 bright = ExtractBright(glm::vec3(v), kThreshold);
        EXPECT_GE(bright.g + 1e-5f, prev) << "Extraction not monotonic at v=" << v;
        prev = bright.g;
    }

    // Deep HDR pixel (brightness well past threshold + 0.5): softness saturates
    // to 1, so the extracted colour equals the input colour exactly.
    const glm::vec3 hdr(5.0f, 5.0f, 5.0f);
    const glm::vec3 brightHdr = ExtractBright(hdr, kThreshold);
    EXPECT_NEAR(brightHdr.r, hdr.r, 1e-4f);
    EXPECT_NEAR(brightHdr.g, hdr.g, 1e-4f);
    EXPECT_NEAR(brightHdr.b, hdr.b, 1e-4f);

    // A bright HDR pixel extracts a strictly positive amount (it is not gated).
    EXPECT_GT(ExtractBright(glm::vec3(2.0f), kThreshold).g, 0.0f);
}

// The extracted colour keeps the source hue: a saturated-green HDR source
// produces a green-dominant bright pixel (the bloom buffer that becomes the
// coloured halo in the visual test).
TEST(BloomMathTest, ExtractionPreservesSourceHue)
{
    // Green-dominant HDR emitter. brightness = 0.7152*4 ≈ 2.86 > 1.0.
    const glm::vec3 greenSource(0.1f, 4.0f, 0.1f);
    const glm::vec3 bright = ExtractBright(greenSource, 1.0f);
    EXPECT_GT(bright.g, bright.r) << "Extracted bloom lost the source's green hue";
    EXPECT_GT(bright.g, bright.b) << "Extracted bloom lost the source's green hue";
    EXPECT_GT(bright.g, 0.5f) << "Green HDR source should extract a strong green";
}

// ---- Additive composite ------------------------------------------------------

// Intensity 0 → the scene passes through untouched; intensity > 0 → the bloom
// is added in, scaling linearly with intensity. This is the formula behind
// "bloom OFF leaves the frame unchanged; bloom ON adds a halo".
TEST(BloomMathTest, CompositeIsAdditiveAndLinearInIntensity)
{
    const glm::vec3 scene(0.05f, 0.05f, 0.06f); // dark background
    const glm::vec3 bloom(0.0f, 0.8f, 0.0f);    // green halo contribution

    // Intensity 0: exactly the scene.
    const glm::vec3 r0 = CompositeBloom(scene, bloom, 0.0f);
    EXPECT_FLOAT_EQ(r0.r, scene.r);
    EXPECT_FLOAT_EQ(r0.g, scene.g);
    EXPECT_FLOAT_EQ(r0.b, scene.b);

    // Positive intensity: brighter than the scene where bloom is non-zero, and
    // the increment is exactly bloom * intensity (linear).
    for (const f32 intensity : { 0.5f, 1.0f, 2.0f })
    {
        const glm::vec3 r = CompositeBloom(scene, bloom, intensity);
        EXPECT_GT(r.g, scene.g) << "Bloom did not brighten the scene at intensity " << intensity;
        EXPECT_NEAR(r.g - scene.g, bloom.g * intensity, 1e-5f)
            << "Composite increment is not linear in intensity at " << intensity;
        // The green halo stays green-dominant after compositing onto the dark scene.
        EXPECT_GT(r.g, r.r);
        EXPECT_GT(r.g, r.b);
    }

    // Monotonic in intensity.
    f32 prev = -1.0f;
    for (u32 i = 0; i <= 40; ++i)
    {
        const f32 intensity = static_cast<f32>(i) * 0.1f;
        const glm::vec3 r = CompositeBloom(scene, bloom, intensity);
        EXPECT_GE(r.g + 1e-5f, prev);
        prev = r.g;
    }
}
