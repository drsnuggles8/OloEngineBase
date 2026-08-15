// OLO_TEST_LAYER: shaderpipe
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Accessibility/AccessibilitySettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// =============================================================================
// Colour-vision adaptation (issue #458) — CPU contract tests.
//
// These pin the math implemented in PostProcess_ColorBlind.glsl WITHOUT a GL
// context (so they run in headless CI), mirroring CASMathTest /
// ContactShadowMathTest. The rendered frame is checked separately by the GPU
// ColorBlindVisualEvidenceTest. Per the CLAUDE.md rendering rule, math/contract
// tests prove the formula; the visual test proves the frame looks right.
//
// AdaptColorLinear in AccessibilitySettings.h is the CPU twin of the shader's
// ADAPTATION — the LMS simulation and the daltonization error shift, on linear
// light. It deliberately does NOT reproduce the shader's two framing steps: the
// gamma decode/encode around the adaptation (the frame is display-referred at
// this point in the chain) and the [0,1] clamp before re-encoding. Those are
// asserted separately, by reading the .glsl as text at the bottom of this file
// — along with the matrix constants, whose drifting apart is the one failure
// mode a pure-CPU test structurally cannot see.
//
// The clamp matters when reading these tests: correction can push a channel to
// ~3.6 on saturated inputs, and the shader saturates it. Assertions here are
// therefore about the unclamped math; the rendered-pixel behaviour is
// ColorBlindVisualEvidenceTest's job.
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    constexpr f32 kEps = 1e-4f;

    // Perceptual luminance (Rec. 709), used to assert that adaptation preserves
    // brightness ordering rather than just "changes something".
    [[nodiscard]] f32 Luma(const glm::vec3& c) noexcept
    {
        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    }

    // Chromaticity coordinates — the colour with its overall intensity divided
    // out. Colour-vision deficiency is a HUE confusion, not a brightness one, so
    // this is the space the "can the viewer tell these apart?" question lives in.
    //
    // Measuring plain RGB distance instead gives the wrong answer, and not
    // subtly: a protanope perceives pure red as very dark, so red-vs-green stays
    // FAR apart in RGB (0.78 of the trichromat distance) while being almost
    // perfectly confusable — 0.009 of it in chromaticity. An RGB-distance
    // assertion here would have had to be loosened until it proved nothing.
    [[nodiscard]] glm::vec2 Chromaticity(const glm::vec3& c) noexcept
    {
        const glm::vec3 positive = glm::max(c, glm::vec3(0.0f));
        const f32 sum = positive.r + positive.g + positive.b;
        if (sum < 1e-6f)
            return glm::vec2(1.0f / 3.0f);
        return glm::vec2(positive.r / sum, positive.g / sum);
    }

    // How far apart two colours are for a viewer with the given deficiency: the
    // chromatic distance between what they ACTUALLY perceive, not between the
    // source colours. This is the quantity daltonization exists to increase, and
    // asserting on it is what makes these tests about the feature rather than
    // about the matrices.
    [[nodiscard]] f32 PerceivedSeparation(const glm::vec3& a, const glm::vec3& b, ColorBlindMode mode) noexcept
    {
        const glm::vec3 pa = AdaptColorLinear(a, mode, ColorBlindAdaptation::Simulate, 1.0f);
        const glm::vec3 pb = AdaptColorLinear(b, mode, ColorBlindAdaptation::Simulate, 1.0f);
        return glm::length(Chromaticity(pa) - Chromaticity(pb));
    }

    [[nodiscard]] f32 TrichromatSeparation(const glm::vec3& a, const glm::vec3& b) noexcept
    {
        return glm::length(Chromaticity(a) - Chromaticity(b));
    }

    [[nodiscard]] std::string ReadShaderSource()
    {
        // NOT CWD-relative: several suites chdir during a full-suite run, so a
        // relative open here would pass in isolation and fail in the suite.
        // OLO_TEST_EDITOR_ROOT is the compile-time anchor.
        const auto path = std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / "PostProcess_ColorBlind.glsl";
        std::ifstream f(path);
        if (!f)
            return {};
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
} // namespace

// -----------------------------------------------------------------------------
// Identity / passthrough
// -----------------------------------------------------------------------------

TEST(ColorBlindMath, ModeNoneIsExactlyIdentity)
{
    // The stage must be a true no-op when off — not "close enough". A frame
    // that shifts by a quantisation step with accessibility disabled would move
    // every golden image for no reason.
    for (const glm::vec3 c : { glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(0.2f, 0.7f, 0.35f) })
    {
        const glm::vec3 out = AdaptColorLinear(c, ColorBlindMode::None, ColorBlindAdaptation::Correct, 1.0f);
        EXPECT_FLOAT_EQ(out.r, c.r);
        EXPECT_FLOAT_EQ(out.g, c.g);
        EXPECT_FLOAT_EQ(out.b, c.b);
    }
}

TEST(ColorBlindMath, ZeroSeverityIsIdentityForEveryMode)
{
    // Severity is the anomalous-trichromacy slider; at 0 the simulation matrix
    // must collapse to identity, so a player dragging it to the bottom gets the
    // untouched frame rather than a nearly-untouched one.
    const glm::vec3 c(0.6f, 0.3f, 0.15f);
    for (const auto mode : { ColorBlindMode::Protanopia, ColorBlindMode::Deuteranopia, ColorBlindMode::Tritanopia })
    {
        const glm::vec3 sim = AdaptColorLinear(c, mode, ColorBlindAdaptation::Simulate, 0.0f);
        EXPECT_NEAR(sim.r, c.r, kEps);
        EXPECT_NEAR(sim.g, c.g, kEps);
        EXPECT_NEAR(sim.b, c.b, kEps);

        // Correction of a zero-severity deficiency has zero error to redistribute.
        const glm::vec3 corrected = AdaptColorLinear(c, mode, ColorBlindAdaptation::Correct, 0.0f);
        EXPECT_NEAR(corrected.r, c.r, kEps);
        EXPECT_NEAR(corrected.g, c.g, kEps);
        EXPECT_NEAR(corrected.b, c.b, kEps);
    }
}

TEST(ColorBlindMath, GreyIsPreservedByEverySimulation)
{
    // A dichromat sees achromatic colours normally — the LMS projection is
    // constructed to fix the neutral axis. If a matrix constant is mistyped the
    // grey ramp tints, which is the most visible possible regression.
    for (const f32 v : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        const glm::vec3 grey(v);
        for (const auto mode : { ColorBlindMode::Protanopia, ColorBlindMode::Deuteranopia, ColorBlindMode::Tritanopia })
        {
            const glm::vec3 sim = AdaptColorLinear(grey, mode, ColorBlindAdaptation::Simulate, 1.0f);
            EXPECT_NEAR(sim.r, v, 2e-2f) << "mode " << static_cast<i32>(mode);
            EXPECT_NEAR(sim.g, v, 2e-2f) << "mode " << static_cast<i32>(mode);
            EXPECT_NEAR(sim.b, v, 2e-2f) << "mode " << static_cast<i32>(mode);
        }
    }
}

TEST(ColorBlindMath, GreyIsUnchangedByCorrectionToo)
{
    // Follows from the above (zero error ⇒ zero shift), but asserted separately:
    // it is the property that keeps UI chrome and text from tinting when a
    // player enables a mode.
    for (const f32 v : { 0.1f, 0.5f, 0.9f })
    {
        const glm::vec3 grey(v);
        for (const auto mode : { ColorBlindMode::Protanopia, ColorBlindMode::Deuteranopia, ColorBlindMode::Tritanopia })
        {
            const glm::vec3 out = AdaptColorLinear(grey, mode, ColorBlindAdaptation::Correct, 1.0f);
            EXPECT_NEAR(out.r, v, 3e-2f);
            EXPECT_NEAR(out.g, v, 3e-2f);
            EXPECT_NEAR(out.b, v, 3e-2f);
        }
    }
}

// -----------------------------------------------------------------------------
// Simulation — the deficiency actually collapses the confusion axis
// -----------------------------------------------------------------------------

TEST(ColorBlindMath, SimulationCollapsesRedAndGreenForProtanAndDeutan)
{
    // The defining property: pure red and pure green — trivially distinct to a
    // trichromat — must become almost indistinguishable. If this fails the
    // simulation is not simulating anything, and the correction built on it is
    // redistributing noise.
    //
    // Measured margin is enormous (both collapse to under 1% of the trichromat
    // separation), so the 25% threshold is a floor, not a tuned constant.
    const glm::vec3 red(1.0f, 0.0f, 0.0f);
    const glm::vec3 green(0.0f, 1.0f, 0.0f);
    const f32 trichromat = TrichromatSeparation(red, green);
    ASSERT_GT(trichromat, 0.1f);

    for (const auto mode : { ColorBlindMode::Protanopia, ColorBlindMode::Deuteranopia })
    {
        const f32 dichromat = PerceivedSeparation(red, green, mode);
        EXPECT_LT(dichromat, trichromat * 0.25f)
            << "red/green must collapse under mode " << static_cast<i32>(mode)
            << " (trichromat separation " << trichromat << ", simulated " << dichromat << ")";
    }
}

TEST(ColorBlindMath, SimulationCollapsesBlueAndGreenForTritan)
{
    // Tritanopia's confusion axis is blue-vs-GREEN (and yellow-vs-violet), NOT
    // blue-vs-yellow: the S cone is missing, so the blue/yellow opponent channel
    // is exactly the one that survives via the L/M pair. Asserting blue-vs-yellow
    // here would be asserting the opposite of the truth — measured, it slightly
    // INCREASES under the simulation.
    const glm::vec3 blue(0.15f, 0.20f, 0.80f);
    const glm::vec3 green(0.15f, 0.70f, 0.25f);
    const f32 trichromat = TrichromatSeparation(blue, green);
    ASSERT_GT(trichromat, 0.1f);

    const f32 dichromat = PerceivedSeparation(blue, green, ColorBlindMode::Tritanopia);
    EXPECT_LT(dichromat, trichromat * 0.25f)
        << "blue/green must collapse under tritanopia (trichromat " << trichromat
        << ", simulated " << dichromat << ")";
}

TEST(ColorBlindMath, SeverityIsAnExactLerpTowardFullDichromacy)
{
    // The severity slider is a linear blend of the simulation MATRIX toward
    // identity, and because the matrix is applied linearly that is equivalent to
    // blending the RESULT — which is the property worth pinning, because it is
    // exact and it is what makes the control predictable.
    //
    // Deliberately NOT "separation decreases monotonically with severity": that
    // is false. Two colours travel different paths toward the dichromat surface,
    // so their distance can tick back up near the top of the range (measured:
    // deuteranopia, 0.75 -> 1.0 on a saturated red/green pair). A monotonicity
    // assertion would be a threshold tuned until it stopped failing.
    const glm::vec3 c(0.7f, 0.25f, 0.4f);
    const glm::vec3 full = AdaptColorLinear(c, ColorBlindMode::Deuteranopia, ColorBlindAdaptation::Simulate, 1.0f);

    for (const f32 severity : { 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f })
    {
        const glm::vec3 got = AdaptColorLinear(c, ColorBlindMode::Deuteranopia, ColorBlindAdaptation::Simulate, severity);
        const glm::vec3 want = c * (1.0f - severity) + full * severity;
        EXPECT_NEAR(got.r, want.r, kEps) << "severity " << severity;
        EXPECT_NEAR(got.g, want.g, kEps) << "severity " << severity;
        EXPECT_NEAR(got.b, want.b, kEps) << "severity " << severity;
    }
}

TEST(ColorBlindMath, SeverityIsClampedRatherThanExtrapolated)
{
    // Severity above 1 would extrapolate PAST full dichromacy and produce
    // colours no eye models — clamp instead.
    const glm::vec3 c(0.8f, 0.2f, 0.4f);
    const glm::vec3 atOne = AdaptColorLinear(c, ColorBlindMode::Protanopia, ColorBlindAdaptation::Simulate, 1.0f);
    const glm::vec3 beyond = AdaptColorLinear(c, ColorBlindMode::Protanopia, ColorBlindAdaptation::Simulate, 5.0f);
    EXPECT_NEAR(beyond.r, atOne.r, kEps);
    EXPECT_NEAR(beyond.g, atOne.g, kEps);
    EXPECT_NEAR(beyond.b, atOne.b, kEps);

    const glm::vec3 below = AdaptColorLinear(c, ColorBlindMode::Protanopia, ColorBlindAdaptation::Simulate, -2.0f);
    EXPECT_NEAR(below.r, c.r, kEps);
    EXPECT_NEAR(below.g, c.g, kEps);
    EXPECT_NEAR(below.b, c.b, kEps);
}

// -----------------------------------------------------------------------------
// Correction — the accessibility feature itself
// -----------------------------------------------------------------------------

TEST(ColorBlindMath, CorrectionIncreasesPerceivedSeparationOfConfusableColors)
{
    // THE acceptance property. Daltonization is worth shipping only if the two
    // colours a dichromat confuses become more distinguishable AFTER the remap.
    // Measured in the viewer's own perceptual space: simulate the corrected
    // colours and compare against simulating the originals.
    //
    // Each mode is checked against ITS OWN confusion pair — red/green for
    // protan and deutan, blue/green for tritan. Using one pair for all three
    // would let the tritan case pass on a pair it never confused.
    struct Case
    {
        ColorBlindMode Mode;
        glm::vec3 A;
        glm::vec3 B;
    };
    const Case cases[] = {
        { ColorBlindMode::Protanopia, glm::vec3(0.8f, 0.15f, 0.15f), glm::vec3(0.15f, 0.8f, 0.15f) },
        { ColorBlindMode::Deuteranopia, glm::vec3(0.8f, 0.15f, 0.15f), glm::vec3(0.15f, 0.8f, 0.15f) },
        { ColorBlindMode::Tritanopia, glm::vec3(0.15f, 0.20f, 0.80f), glm::vec3(0.15f, 0.70f, 0.25f) },
    };

    for (const auto& [mode, a, b] : cases)
    {
        const f32 before = PerceivedSeparation(a, b, mode);

        const glm::vec3 aFixed = AdaptColorLinear(a, mode, ColorBlindAdaptation::Correct, 1.0f);
        const glm::vec3 bFixed = AdaptColorLinear(b, mode, ColorBlindAdaptation::Correct, 1.0f);
        const f32 after = PerceivedSeparation(aFixed, bFixed, mode);

        // Measured gains are 2.1x / 7.5x / 2.8x, so requiring a clear 1.5x
        // margin keeps the assertion about the feature rather than about noise.
        EXPECT_GT(after, before * 1.5f)
            << "daltonization must make confusable colours MORE separable for mode "
            << static_cast<i32>(mode) << " (before " << before << ", after " << after << ")";
    }
}

TEST(ColorBlindMath, CorrectionAndSimulationDisagreeForAChromaticColor)
{
    // Guards the branch itself: an accidental fallthrough that made Correct
    // behave like Simulate would DARKEN the frame for the exact users the
    // feature is for, and every "did something change?" assertion would pass.
    const glm::vec3 c(0.9f, 0.2f, 0.1f);
    const glm::vec3 simulated = AdaptColorLinear(c, ColorBlindMode::Deuteranopia, ColorBlindAdaptation::Simulate, 1.0f);
    const glm::vec3 corrected = AdaptColorLinear(c, ColorBlindMode::Deuteranopia, ColorBlindAdaptation::Correct, 1.0f);

    EXPECT_GT(glm::length(corrected - simulated), 1e-2f);
}

TEST(ColorBlindMath, CorrectionKeepsBrightnessOrderingIntact)
{
    // Redistributing chroma error must not reorder a light surface behind a dark
    // one — that would break every readability cue that relies on value contrast
    // rather than hue.
    const glm::vec3 dark(0.2f, 0.1f, 0.05f);
    const glm::vec3 light(0.85f, 0.6f, 0.4f);
    ASSERT_LT(Luma(dark), Luma(light));

    for (const auto mode : { ColorBlindMode::Protanopia, ColorBlindMode::Deuteranopia, ColorBlindMode::Tritanopia })
    {
        const glm::vec3 d = AdaptColorLinear(dark, mode, ColorBlindAdaptation::Correct, 1.0f);
        const glm::vec3 l = AdaptColorLinear(light, mode, ColorBlindAdaptation::Correct, 1.0f);
        EXPECT_LT(Luma(d), Luma(l)) << "brightness ordering inverted for mode " << static_cast<i32>(mode);
    }
}

TEST(ColorBlindMath, EveryModeProducesFiniteOutputAcrossTheUnitCube)
{
    // A NaN here reaches pow() in the shader and paints a black or garbage
    // pixel. Sweep the cube rather than spot-checking.
    for (i32 ri = 0; ri <= 4; ++ri)
    {
        for (i32 gi = 0; gi <= 4; ++gi)
        {
            for (i32 bi = 0; bi <= 4; ++bi)
            {
                const glm::vec3 c(static_cast<f32>(ri) * 0.25f, static_cast<f32>(gi) * 0.25f, static_cast<f32>(bi) * 0.25f);
                for (const auto mode : { ColorBlindMode::Protanopia, ColorBlindMode::Deuteranopia, ColorBlindMode::Tritanopia })
                {
                    for (const auto method : { ColorBlindAdaptation::Correct, ColorBlindAdaptation::Simulate })
                    {
                        const glm::vec3 out = AdaptColorLinear(c, mode, method, 1.0f);
                        ASSERT_TRUE(std::isfinite(out.r) && std::isfinite(out.g) && std::isfinite(out.b))
                            << "non-finite output at (" << c.r << "," << c.g << "," << c.b << ") mode "
                            << static_cast<i32>(mode);
                    }
                }
            }
        }
    }
}

TEST(ColorBlindMath, LMSRoundTripIsTheIdentity)
{
    // LinearRGBToLMS and LMSToLinearRGB are hand-transcribed inverses. A single
    // mistyped digit would leave every adapted colour subtly wrong in a way no
    // "did it change?" assertion catches.
    const glm::mat3 roundTrip = LMSToLinearRGB() * LinearRGBToLMS();
    for (i32 col = 0; col < 3; ++col)
    {
        for (i32 row = 0; row < 3; ++row)
        {
            const f32 expected = (col == row) ? 1.0f : 0.0f;
            EXPECT_NEAR(roundTrip[col][row], expected, 1e-3f)
                << "LMS round-trip is not the identity at [" << col << "][" << row << "]";
        }
    }
}

// -----------------------------------------------------------------------------
// Bindings + shader/CPU agreement
// -----------------------------------------------------------------------------

TEST(ColorBlindMath, UBOBindingIsRegisteredAndWithinTheEngineLimit)
{
    EXPECT_EQ(ShaderBindingLayout::UBO_COLORBLIND, 73u);
    EXPECT_LT(ShaderBindingLayout::UBO_COLORBLIND, ShaderBindingLayout::UBO_BINDING_LIMIT);
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_COLORBLIND, "ColorBlindParams"));
}

TEST(ColorBlindMath, ShaderDeclaresTheSameUBOBindingTheCppSideUploadsTo)
{
    // The GLSL spells the binding as a literal (as every other shader here
    // does), so nothing but this test stops it drifting from UBO_COLORBLIND —
    // and a mismatched UBO binding is silent data corruption, not an error.
    const std::string src = ReadShaderSource();
    ASSERT_FALSE(src.empty()) << "PostProcess_ColorBlind.glsl not found under OLO_TEST_EDITOR_ROOT";

    const std::string expected = "layout(std140, binding = " + std::to_string(ShaderBindingLayout::UBO_COLORBLIND) + ") uniform ColorBlindParams";
    EXPECT_NE(src.find(expected), std::string::npos)
        << "shader UBO binding drifted from ShaderBindingLayout::UBO_COLORBLIND";
}

TEST(ColorBlindMath, ShaderCarriesTheSameMatrixConstantsAsTheCppTwin)
{
    // The one failure this file could otherwise not see: the CPU math staying
    // self-consistent while the GLSL drifts. Spot-check one constant from each
    // of the three matrices — enough that a copy-paste edit of any single block
    // trips it.
    const std::string src = ReadShaderSource();
    ASSERT_FALSE(src.empty()) << "PostProcess_ColorBlind.glsl not found under OLO_TEST_EDITOR_ROOT";

    for (const char* needle : {
             "17.8824",     // LinearRGBToLMS column 0
             "0.693511405", // LMSToLinearRGB column 2
             "2.02344",     // protan projection
             "0.494207",    // deutan projection
             "0.801109",    // tritan projection
         })
    {
        EXPECT_NE(src.find(needle), std::string::npos)
            << "shader is missing the constant '" << needle << "' present in AccessibilitySettings.h";
    }
}

TEST(ColorBlindMath, ShaderDecodesAndReEncodesGamma)
{
    // The stage runs post-tonemap on a display-referred image; the cone math is
    // defined on linear light. Losing either pow() shifts every adapted hue and
    // looks like "the matrices are wrong".
    const std::string src = ReadShaderSource();
    ASSERT_FALSE(src.empty()) << "PostProcess_ColorBlind.glsl not found under OLO_TEST_EDITOR_ROOT";

    EXPECT_NE(src.find("pow(max(source.rgb, vec3(0.0)), vec3(gamma))"), std::string::npos)
        << "the gamma DECODE is missing — the cone math would run on display-referred values";
    EXPECT_NE(src.find("vec3(1.0 / gamma)"), std::string::npos)
        << "the gamma RE-ENCODE is missing — the output would be linear where the backbuffer expects encoded";
}
