#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// GroomFibreDualScatteringTests — the dual-scattering constants (#1533) and the
// coloured coat transmittance they feed, settled in arithmetic.
//
// WHAT IS UNDER TEST. Zinke et al. 2008 splits a dense coat's transport into a
// GLOBAL part — light forwarded through the strands in front, keeping a_f of
// what each crossing intercepts — and a LOCAL part, A_b, the light the strands
// behind scatter back. GroomFibreComputeDualScattering derives a_f, a_b, A_b and
// the back-scatter lobe's shape from the fibre's own attenuations and azimuthal
// lobes; GroomCoatShadow::CoatForwardTransmittance and
// GroomFibreBackScatterProjected are what the strand shader evaluates with them.
//
// Each claim the header comments make is an assertion here:
//
//   1. THE SPLIT partitions the fibre's albedo exactly and agrees with the far
//      field integrated over each half-space — so a_f is the forward energy of
//      the BCSDF on screen, not a second description of it.
//   2. THE CONSTANTS mean what they say: a pale fibre forwards most of what it
//      intercepts, in its own colour; a dark one forwards little; A_b is
//      Zinke's series of those two, summed to every order (#1558), which a
//      coat of clear fibres needs to return all of the light.
//   3. THE LOBE integrates to A_b and vanishes over the forward half-circle.
//   4. THE TRANSMITTANCE reduces to the #1248 term when nothing is forwarded,
//      is never below it, and fails bright on corrupt input like it.
//
// Tolerances follow GroomFibrePropertyTests: tight where a quantity is exact
// (the partition, the series, the reduction), wide where it is a quadrature of
// the far field.
// =============================================================================

#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Groom/GroomFibreScattering.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr f32 kPi = 3.14159265358979323846f;

        [[nodiscard]] GroomFibreParams MakeColoured(const glm::vec3& colour, f32 longitudinal = 0.3f)
        {
            GroomFibreAuthoring authored;
            authored.PigmentMode = GroomFibrePigmentMode::BaseColor;
            authored.BaseColor = colour;
            authored.LongitudinalRoughness = longitudinal;
            return MakeGroomFibreParams(authored);
        }

        [[nodiscard]] GroomFibreParams MakeMelanin(f32 eumelanin, f32 pheomelanin)
        {
            GroomFibreAuthoring authored;
            authored.PigmentMode = GroomFibrePigmentMode::Melanin;
            authored.Eumelanin = eumelanin;
            authored.Pheomelanin = pheomelanin;
            return MakeGroomFibreParams(authored);
        }

        // The pale golden fibre the dog showcase grows, the horses' dark brown,
        // and a fibre that absorbs nothing — the end of the range where the
        // A_b series is at its largest.
        const glm::vec3 kGolden{ 1.0f, 0.82f, 0.56f };
        const glm::vec3 kBrown{ 0.42f, 0.26f, 0.14f };
        const glm::vec3 kClear{ 1.0f, 1.0f, 1.0f };

        // The far field integrated over theta_i and over ONE half of the azimuth
        // circle, with the white furnace's measure (cos(theta_i) dTheta dPhi).
        [[nodiscard]] glm::vec3 IntegrateHalf(const GroomFibreParams& params, f32 sinThetaO, bool forward)
        {
            constexpr u32 kThetaSteps = 256;
            constexpr u32 kPhiSteps = 512;
            const f32 dTheta = kPi / static_cast<f32>(kThetaSteps);
            const f32 dPhi = (2.0f * kPi) / static_cast<f32>(kPhiSteps);
            glm::vec3 total(0.0f);
            for (u32 ti = 0; ti < kThetaSteps; ++ti)
            {
                const f32 theta = (-0.5f * kPi) + (dTheta * (static_cast<f32>(ti) + 0.5f));
                for (u32 pi = 0; pi < kPhiSteps; ++pi)
                {
                    const f32 phi = dPhi * (static_cast<f32>(pi) + 0.5f);
                    const bool isForward = std::cos(phi) < 0.0f;
                    if (isForward != forward)
                    {
                        continue;
                    }
                    const GroomFibreLobeSet lobes = GroomFibreEvaluateFar(params, sinThetaO, std::sin(theta), phi);
                    total += lobes.Sum() * (std::cos(theta) * dTheta * dPhi);
                }
            }
            return total;
        }
    } // namespace

    // ── 1. The split ────────────────────────────────────────────────────────

    TEST(GroomFibreDualScatteringTest, TheSplitPartitionsTheFibresAlbedoExactly)
    {
        // Forward + Backward must be the albedo the ambient response reports
        // (per-lobe attenuations averaged over h, times cos(theta)), because
        // both sum the same attenuations at the same nodes and the split only
        // decides which half each part lands in. EXACT up to f32 rounding.
        for (const glm::vec3& colour : { kGolden, kBrown, kClear })
        {
            const GroomFibreParams params = MakeColoured(colour);
            for (f32 sinTheta : { 0.0f, 0.3f, -0.6f, 0.85f })
            {
                const GroomFibreScatterSplit split = GroomFibreScatterSplitAt(params, sinTheta);
                const f32 cosTheta = std::sqrt(1.0f - (sinTheta * sinTheta));
                const glm::vec3 albedo = GroomFibreAmbientResponse(params, sinTheta).Sum() / cosTheta;
                for (int c = 0; c < 3; ++c)
                {
                    EXPECT_NEAR(split.Forward[c] + split.Backward[c], albedo[c], 2.0e-5f)
                        << "colour " << colour[c] << " sinTheta " << sinTheta << " channel " << c;
                }
            }
        }
    }

    TEST(GroomFibreDualScatteringTest, TheSplitIsTheFarFieldIntegratedOverEachHalf)
    {
        // THE CLAIM THAT MAKES a_f MEAN SOMETHING: the forward half of the
        // split is the energy the rendered BCSDF actually sends forward. Checked
        // by integrating GroomFibreEvaluateFar over |phi| > pi/2 directly. The
        // tolerance is the far field's own quadrature band (its longitudinal M
        // loses a little mass at the sphere's poles), not the split's.
        for (const glm::vec3& colour : { kGolden, kBrown })
        {
            const GroomFibreParams params = MakeColoured(colour);
            const GroomFibreScatterSplit split = GroomFibreScatterSplitAt(params, 0.0f);
            const glm::vec3 forward = IntegrateHalf(params, 0.0f, true);
            const glm::vec3 backward = IntegrateHalf(params, 0.0f, false);
            for (int c = 0; c < 3; ++c)
            {
                EXPECT_NEAR(split.Forward[c], forward[c], 0.03f * std::max(forward[c], 0.05f))
                    << "forward, colour " << colour[c] << " channel " << c;
                EXPECT_NEAR(split.Backward[c], backward[c], 0.03f * std::max(backward[c], 0.05f) + 2.0e-3f)
                    << "backward, colour " << colour[c] << " channel " << c;
            }
        }
    }

    // ── 2. The constants ────────────────────────────────────────────────────

    TEST(GroomFibreDualScatteringTest, APaleFibreForwardsMostOfWhatItInterceptsInItsOwnColour)
    {
        const GroomFibreDualScattering golden = MakeColoured(kGolden).Dual;
        const GroomFibreDualScattering brown = MakeColoured(kBrown).Dual;

        // Pale: TT carries most of the energy forward, and the pigment orders
        // the channels the way the authored colour does.
        EXPECT_GT(golden.ForwardScatter.b, 0.4f);
        EXPECT_GT(golden.ForwardScatter.r, golden.ForwardScatter.g);
        EXPECT_GT(golden.ForwardScatter.g, golden.ForwardScatter.b);
        // Dark: the same fibre geometry forwards a fraction of that.
        for (int c = 0; c < 3; ++c)
        {
            EXPECT_LT(brown.ForwardScatter[c], 0.6f * golden.ForwardScatter[c]) << "channel " << c;
        }
        // Back-scatter is the cuticle's R plus TRT: a few per cent, and never
        // the larger half for a fibre this pale.
        for (int c = 0; c < 3; ++c)
        {
            EXPECT_LT(golden.BackwardScatter[c], golden.ForwardScatter[c]) << "channel " << c;
            EXPECT_GT(golden.BackwardScatter[c], 0.01f) << "channel " << c;
        }
    }

    TEST(GroomFibreDualScatteringTest, TheMultipleBackScatterIsZinkesSeriesSummedToEveryOrder)
    {
        // A_b recomputed from the struct's own a_f and a_b as the reflectance
        // of a stack of layers that each reflect a_b and transmit a_f, less the
        // first layer's (#1558). Exact: the stored value IS that sum, so a later
        // edit to one side cannot leave the other describing a different coat.
        for (const glm::vec3& colour : { kGolden, kBrown, kClear })
        {
            const GroomFibreDualScattering dual = MakeColoured(colour).Dual;
            for (int c = 0; c < 3; ++c)
            {
                const f64 af = dual.ForwardScatter[c];
                const f64 ab = dual.BackwardScatter[c];
                const f64 a = 1.0 + (ab * ab) - (af * af);
                const f64 stack = (a - std::sqrt(std::max((a * a) - (4.0 * ab * ab), 0.0))) / (2.0 * ab);
                const f64 expected = stack - ab;
                EXPECT_NEAR(dual.MultipleBackScatter[c], expected, 1.0e-4 * std::max(1.0, expected))
                    << "colour " << colour[c] << " channel " << c;
                EXPECT_TRUE(std::isfinite(dual.MultipleBackScatter[c]));

                // Zinke's A1 + A3 are its first two terms, and every further
                // term is positive: the sum never falls below the cut series.
                const f64 d = 1.0 - (af * af);
                const f64 cut = ((ab * af * af) / d) + ((ab * ab * ab * af * af) / (d * d * d));
                EXPECT_GE(dual.MultipleBackScatter[c], cut - 1.0e-5) << "colour " << colour[c] << " channel " << c;
            }
        }

        // AN ABSORBING FIBRE LOSES NOTHING TO THE CUT: the terms past A3 carry
        // a_b^5, and a brown fibre's sum is the paper's series to well inside a
        // per cent.
        {
            const GroomFibreDualScattering brown = MakeColoured(kBrown).Dual;
            for (int c = 0; c < 3; ++c)
            {
                const f32 af = brown.ForwardScatter[c];
                const f32 ab = brown.BackwardScatter[c];
                const f32 d = 1.0f - (af * af);
                const f32 cut = ((ab * af * af) / d) + ((ab * ab * ab * af * af) / (d * d * d));
                EXPECT_NEAR(brown.MultipleBackScatter[c], cut, 0.005f * cut + 1.0e-6f) << "channel " << c;
            }
        }

        // THE WHITE FURNACE. A fibre that absorbs nothing forwards or returns
        // everything it intercepts (a_f + a_b = 1), so a deep stack of them
        // reflects everything: a_b off the first layer and A_b = a_f from the
        // rest. The paper's A1 + A3 stops at about 0.6 of that, which is the
        // light a white coat lost in the shade before #1558.
        {
            const GroomFibreDualScattering clear = MakeColoured(kClear).Dual;
            for (int c = 0; c < 3; ++c)
            {
                const f32 af = clear.ForwardScatter[c];
                const f32 ab = clear.BackwardScatter[c];
                ASSERT_NEAR(af + ab, 1.0f, 1.0e-3f) << "a clear fibre absorbs nothing, channel " << c;
                EXPECT_NEAR(clear.MultipleBackScatter[c] + ab, 1.0f, 2.0e-3f)
                    << "a deep coat of clear fibres must return all of the light, channel " << c;
                const f32 d = 1.0f - (af * af);
                const f32 cut = ((ab * af * af) / d) + ((ab * ab * ab * af * af) / (d * d * d));
                EXPECT_LT(cut, 0.7f * clear.MultipleBackScatter[c])
                    << "the cut series no longer falls short for a clear fibre; this assertion's premise moved";
            }
        }
        // And it is what makes a pale coat pale: the local back-scatter of a
        // golden coat dwarfs a brown one's.
        EXPECT_GT(MakeColoured(kGolden).Dual.MultipleBackScatter.r,
                  10.0f * MakeColoured(kBrown).Dual.MultipleBackScatter.r);
    }

    TEST(GroomFibreDualScatteringTest, MakeGroomFibreParamsCarriesTheDerivedConstants)
    {
        const GroomFibreParams params = MakeMelanin(0.1f, 0.05f);
        const GroomFibreDualScattering recomputed = GroomFibreComputeDualScattering(params);
        EXPECT_EQ(params.Dual, recomputed);
        EXPECT_GT(params.Dual.BackWidth, 0.0f);
        EXPECT_LT(params.Dual.BackWidth, 0.5f * kPi + 1.0e-6f);
        EXPECT_TRUE(std::isfinite(params.Dual.BackShift));
    }

    // ── 3. The lobe ─────────────────────────────────────────────────────────

    TEST(GroomFibreDualScatteringTest, TheBackScatterLobeIntegratesToItsAlbedoFromEveryView)
    {
        // Over dTheta_i dPhi — the projected lobe already carries cos(theta_i).
        // At EVERY theta_o, grazing included: the lobe is renormalised over the
        // theta_h range theta_i can reach, and at a grazing view a plain
        // Gaussian would keep only half its mass. The tolerance is the
        // quadrature's, not the lobe's: 1 %.
        for (const glm::vec3& colour : { kGolden, kClear })
        {
            const GroomFibreDualScattering dual = MakeColoured(colour).Dual;
            for (f32 sinThetaO : { 0.0f, 0.5f, -0.8f, 0.97f })
            {
                constexpr u32 kThetaSteps = 1024;
                constexpr u32 kPhiSteps = 256;
                const f32 dTheta = kPi / static_cast<f32>(kThetaSteps);
                const f32 dPhi = (2.0f * kPi) / static_cast<f32>(kPhiSteps);
                glm::vec3 total(0.0f);
                for (u32 ti = 0; ti < kThetaSteps; ++ti)
                {
                    const f32 theta = (-0.5f * kPi) + (dTheta * (static_cast<f32>(ti) + 0.5f));
                    for (u32 pi = 0; pi < kPhiSteps; ++pi)
                    {
                        const f32 phi = dPhi * (static_cast<f32>(pi) + 0.5f);
                        total += GroomFibreBackScatterProjected(dual, sinThetaO, std::sin(theta), std::cos(phi)) *
                                 (dTheta * dPhi);
                    }
                }
                for (int c = 0; c < 3; ++c)
                {
                    EXPECT_NEAR(total[c], dual.MultipleBackScatter[c], 0.01f * dual.MultipleBackScatter[c] + 1.0e-5f)
                        << "colour " << colour[c] << " channel " << c << " sinThetaO " << sinThetaO << " width "
                        << dual.BackWidth;
                }
            }
        }
    }

    TEST(GroomFibreDualScatteringTest, TheErfTheShaderCanSpellIsTheErf)
    {
        // The lobe's normalisation leans on Abramowitz & Stegun 7.1.26 because
        // GLSL has no erf. Its published bound is 1.5e-7; f32 arithmetic
        // allows a little more.
        for (f32 x = -4.0f; x <= 4.0f; x += 0.01f)
        {
            EXPECT_NEAR(GroomFibreErf(x), std::erf(x), 1.0e-6f) << "x " << x;
        }
    }

    TEST(GroomFibreDualScatteringTest, TheBackScatterLobeVanishesOverTheForwardHalfContinuously)
    {
        const GroomFibreDualScattering dual = MakeColoured(kGolden).Dual;
        EXPECT_EQ(GroomFibreBackScatterProjected(dual, 0.1f, -0.1f, -0.5f), glm::vec3(0.0f));
        EXPECT_EQ(GroomFibreBackScatterProjected(dual, 0.1f, -0.1f, 0.0f), glm::vec3(0.0f));
        // Continuous at the seam: just past it the lobe is nearly zero, not a
        // step, so a light sweeping round a strand does not pop.
        const glm::vec3 nearSeam = GroomFibreBackScatterProjected(dual, 0.1f, -0.1f, 0.01f);
        const glm::vec3 straightBack = GroomFibreBackScatterProjected(dual, 0.1f, -0.1f, 1.0f);
        EXPECT_LT(nearSeam.r, 0.02f * straightBack.r);
        EXPECT_GT(straightBack.r, 0.0f);
    }

    // ── 4. The transmittance ────────────────────────────────────────────────

    TEST(GroomFibreDualScatteringTest, TheForwardTransmittanceWithNothingForwardedIsTheColourlessOne)
    {
        for (f64 tau : { 0.05, 0.5, 2.0, 9.0 })
        {
            for (f32 kappa : { 0.25f, 1.25f, 4.0f, 16.0f })
            {
                const f32 colourless = GroomCoatShadow::CoatTransmittance(tau, kappa);
                const glm::vec3 none = GroomCoatShadow::CoatForwardTransmittance(tau, kappa, glm::vec3(0.0f));
                EXPECT_FLOAT_EQ(none.r, colourless) << "tau " << tau << " kappa " << kappa;
                EXPECT_FLOAT_EQ(none.g, colourless);
                EXPECT_FLOAT_EQ(none.b, colourless);
            }
        }
    }

    TEST(GroomFibreDualScatteringTest, TheForwardTransmittanceIsNeverBelowTheColourlessOneAndRisesWithForwarding)
    {
        const glm::vec3 af = MakeColoured(kGolden).Dual.ForwardScatter;
        for (f64 tau : { 0.05, 0.5, 2.0, 9.0 })
        {
            const f32 colourless = GroomCoatShadow::CoatTransmittance(tau, 4.0f);
            const glm::vec3 golden = GroomCoatShadow::CoatForwardTransmittance(tau, 4.0f, af);
            for (int c = 0; c < 3; ++c)
            {
                EXPECT_GE(golden[c], colourless) << "tau " << tau << " channel " << c;
            }
            // Coloured the way the fibre is: red survives the crossings best.
            EXPECT_GE(golden.r, golden.g);
            EXPECT_GE(golden.g, golden.b);
        }
        // A fibre that forwarded everything would be no occluder at all.
        EXPECT_EQ(GroomCoatShadow::CoatForwardTransmittance(5.0, 4.0f, glm::vec3(1.0f)), glm::vec3(1.0f));
        // Nine crossings of a golden coat still pass a visible red where the
        // colourless term had passed nothing: the depth goes warm, not black.
        EXPECT_GT(GroomCoatShadow::CoatForwardTransmittance(9.0, 4.0f, af).r, 0.2f);
        EXPECT_LT(GroomCoatShadow::CoatTransmittance(9.0, 4.0f), 1.0e-3f);
    }

    TEST(GroomFibreDualScatteringTest, TheForwardTransmittanceFailsBrightOnCorruptInput)
    {
        // Rule 10 of groom-coat-self-shadowing.md, inherited: a corrupt optical
        // depth or kappa reads FULLY LIT, in every channel, exactly where
        // CoatTransmittance does. A non-finite a_f is a fibre that forwards
        // nothing, which is the #1248 answer rather than a NaN.
        const f64 inf = std::numeric_limits<f64>::infinity();
        const f64 nan = std::numeric_limits<f64>::quiet_NaN();
        const glm::vec3 af(0.9f, 0.7f, 0.5f);
        EXPECT_EQ(GroomCoatShadow::CoatForwardTransmittance(inf, 4.0f, af), glm::vec3(1.0f));
        EXPECT_EQ(GroomCoatShadow::CoatForwardTransmittance(nan, 4.0f, af), glm::vec3(1.0f));
        EXPECT_EQ(GroomCoatShadow::CoatForwardTransmittance(1.0, std::numeric_limits<f32>::infinity(), af),
                  glm::vec3(1.0f));
        EXPECT_EQ(GroomCoatShadow::CoatForwardTransmittance(-1.0, 4.0f, af), glm::vec3(1.0f));
        const glm::vec3 nanForward = GroomCoatShadow::CoatForwardTransmittance(
            1.0, 4.0f, glm::vec3(std::numeric_limits<f32>::quiet_NaN(), 0.5f, 0.5f));
        EXPECT_FLOAT_EQ(nanForward.r, GroomCoatShadow::CoatTransmittance(1.0, 4.0f));
    }
} // namespace OloEngine::Tests
