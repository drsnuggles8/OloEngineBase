// OLO_TEST_LAYER: L1
// =============================================================================
// SkinLayeredSpecularTest.cpp — the energy argument, the filter's monotonicity
// and the history contract of the layered surface response. Issue #1243.
//
// WHAT THIS PINS AND WHAT IT DOES NOT. This file is the CPU maths of
// Renderer/SkinLayeredSpecular.h. That the SHADER computes the same maths is
// SkinLayeredSpecularParityTest's job, and that the result LOOKS right is
// SkinLayeredSpecularEvidenceTest's. All three are needed and none substitutes
// for another: a property can hold while the number is wrong by a factor of a
// thousand, an equality can hold while the frame is black, and a frame can look
// fine while the energy quietly grew by ten percent.
//
// THE CENTRAL CLAIM is the energy bound. A second specular lobe is the classic
// way to make a renderer brighter and call it better, so the convexity is swept
// rather than spot-checked, and it is swept against the ONE function that
// writes the mixture — the same function both shading paths route through.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/SkinLayeredSpecular.h"
#include "OloEngine/Renderer/SkinProfile.h"

#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // A sanitized profile at transport version 3 with the mixture turned up,
        // so the tests below exercise the arm an author actually opts into
        // rather than the neutral default.
        [[nodiscard]] SkinProfileParameters LayeredProfile()
        {
            SkinProfileParameters p;
            p.EvaluationModel = SkinEvaluationModel::LayeredSpecular;
            p.Specular.LobeMix = 0.35f;
            p.Specular.LobeRoughnessScale = 2.0f;
            p.Specular.NormalVarianceStrength = 0.5f;
            p.Specular.DetailStrength = 0.25f;
            p.Specular.ExpressionDetailGain = 1.5f;
            EXPECT_TRUE(p.Sanitize()) << "the fixture profile must already be in range";
            return p;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The energy bound
    // -------------------------------------------------------------------------

    // THE headline claim: the mixture is a convex combination, so it can never
    // exceed the larger of the two lobes it mixes, for any weight in [0, 1].
    //
    // Swept rather than spot-checked because the failure this guards against is
    // a plausible refactor — someone writing `narrow + w * broad` instead of
    // `narrow + w * (broad - narrow)` — and that spelling agrees with the
    // correct one at w = 0 and disagrees everywhere else.
    TEST(SkinLayeredSpecularTest, TheMixtureNeverExceedsEitherLobe)
    {
        for (i32 ni = 0; ni <= 20; ++ni)
        {
            for (i32 bi = 0; bi <= 20; ++bi)
            {
                const f32 narrow = static_cast<f32>(ni) * 0.5f;
                const f32 broad = static_cast<f32>(bi) * 0.5f;
                const f32 upper = std::max(narrow, broad);
                const f32 lower = std::min(narrow, broad);

                for (i32 wi = 0; wi <= 20; ++wi)
                {
                    const f32 w = static_cast<f32>(wi) / 20.0f;
                    const f32 mixed = SkinSpecularMix(narrow, broad, w);

                    // The bound both ways: a convex combination is BETWEEN its
                    // endpoints, not merely below the larger one. The lower half
                    // is what rules out a negative-radiance spelling.
                    EXPECT_LE(mixed, upper + 1.0e-5f)
                        << "narrow=" << narrow << " broad=" << broad << " w=" << w;
                    EXPECT_GE(mixed, lower - 1.0e-5f)
                        << "narrow=" << narrow << " broad=" << broad << " w=" << w;
                }
            }
        }
    }

    // The A/B control the fourth acceptance criterion is demonstrated against
    // has to be an IDENTITY, not an approximation — otherwise "layering off"
    // and "before this feature" are two different frames and a golden image
    // cannot tell a regression from the control.
    TEST(SkinLayeredSpecularTest, AZeroMixIsBitIdenticalToTheNarrowLobe)
    {
        for (i32 i = 0; i <= 40; ++i)
        {
            const f32 narrow = static_cast<f32>(i) * 0.137f;
            const f32 broad = narrow * 3.7f + 0.9f;
            EXPECT_EQ(SkinSpecularMix(narrow, broad, 0.0f), narrow)
                << "a zero mix must be EXACT, not close, at narrow=" << narrow;
        }
    }

    TEST(SkinLayeredSpecularTest, AFullMixIsTheBroadLobe)
    {
        EXPECT_NEAR(SkinSpecularMix(2.0f, 7.0f, 1.0f), 7.0f, 1.0e-5f);
    }

    // -------------------------------------------------------------------------
    // The variance filter
    // -------------------------------------------------------------------------

    // The filter ONLY ever roughens. The other direction is a sharpening filter,
    // and a sharpening filter cannot remove aliasing — it manufactures it.
    TEST(SkinLayeredSpecularTest, TheFilterNeverSharpens)
    {
        for (i32 ai = 1; ai <= 20; ++ai)
        {
            const f32 alpha = static_cast<f32>(ai) / 20.0f;
            for (i32 vi = 0; vi <= 10; ++vi)
            {
                const f32 d = static_cast<f32>(vi) * 0.05f;
                const f32 filtered = SkinFilteredAlpha(alpha, d, d, 0.5f);
                EXPECT_GE(filtered, std::min(alpha, 1.0f) - 1.0e-6f)
                    << "alpha=" << alpha << " d=" << d;
            }
        }
    }

    // Zero variance strength is the A/B control arm for the SPARKLE half of the
    // evidence, so like the zero mix above it has to be exact.
    TEST(SkinLayeredSpecularTest, ZeroVarianceStrengthLeavesAlphaAlone)
    {
        for (i32 ai = 1; ai <= 20; ++ai)
        {
            const f32 alpha = static_cast<f32>(ai) / 20.0f;
            EXPECT_EQ(SkinFilteredAlpha(alpha, 4.0f, 9.0f, 0.0f), alpha)
                << "alpha=" << alpha;
        }
    }

    // A flat surface has no variance to add, whatever the strength.
    TEST(SkinLayeredSpecularTest, ZeroDerivativesLeaveAlphaAlone)
    {
        EXPECT_EQ(SkinFilteredAlpha(0.3f, 0.0f, 0.0f, 4.0f), 0.3f);
    }

    // The clamp is what stops a silhouette pixel — where the shading normal
    // swings most of a hemisphere between neighbours — going fully rough and
    // drawing a bright halo around every outline.
    TEST(SkinLayeredSpecularTest, TheKernelIsClampedSoASilhouetteCannotSaturate)
    {
        // A derivative pair far beyond anything a real surface produces.
        const f32 filtered = SkinFilteredAlpha(0.1f, 1.0e4f, 1.0e4f, kMaxSkinNormalVarianceStrength);
        const f32 ceiling = std::sqrt(0.1f * 0.1f + kSkinVarianceKernelClamp);
        EXPECT_NEAR(filtered, ceiling, 1.0e-5f)
            << "the widening must stop at the clamp, not run to fully rough";
        EXPECT_LT(filtered, 1.0f);
    }

    // Monotone in the measured variance: more normal spread under the pixel can
    // only mean a wider lobe, never a narrower one. A non-monotone filter would
    // make roughness flicker as the camera moves, which is the very thing this
    // is here to stop.
    TEST(SkinLayeredSpecularTest, TheFilterIsMonotoneInTheVariance)
    {
        f32 previous = 0.0f;
        for (i32 i = 0; i <= 40; ++i)
        {
            const f32 d = static_cast<f32>(i) * 0.002f;
            const f32 filtered = SkinFilteredAlpha(0.2f, d, d, 0.5f);
            EXPECT_GE(filtered, previous - 1.0e-6f) << "step " << i;
            previous = filtered;
        }
    }

    TEST(SkinLayeredSpecularTest, TheFilterStaysInTheNdfDomain)
    {
        for (i32 ai = 0; ai <= 20; ++ai)
        {
            for (i32 vi = 0; vi <= 20; ++vi)
            {
                const f32 alpha = static_cast<f32>(ai) / 20.0f;
                const f32 d = static_cast<f32>(vi) * 5.0f;
                const f32 filtered = SkinFilteredAlpha(alpha, d, d, kMaxSkinNormalVarianceStrength);
                EXPECT_GE(filtered, 0.0f);
                EXPECT_LE(filtered, 1.0f);
            }
        }
    }

    // -------------------------------------------------------------------------
    // The lobe pair
    // -------------------------------------------------------------------------

    TEST(SkinLayeredSpecularTest, TheBroadLobeIsNeverNarrowerThanTheSharpOne)
    {
        SkinProfileParameters p = LayeredProfile();
        for (i32 si = 0; si <= 10; ++si)
        {
            p.Specular.LobeRoughnessScale =
                kMinSkinLobeRoughnessScale +
                static_cast<f32>(si) / 10.0f * (kMaxSkinLobeRoughnessScale - kMinSkinLobeRoughnessScale);
            ASSERT_TRUE(p.Sanitize());

            for (i32 ri = 0; ri <= 20; ++ri)
            {
                const f32 roughness = static_cast<f32>(ri) / 20.0f;
                const SkinSpecularLobePair lobes = SkinSpecularLobesFor(roughness, p.Specular);
                EXPECT_GE(lobes.BroadRoughness, lobes.NarrowRoughness - 1.0e-6f)
                    << "scale=" << p.Specular.LobeRoughnessScale << " roughness=" << roughness;
                EXPECT_LE(lobes.BroadRoughness, 1.0f);
            }
        }
    }

    // The narrow lobe is the surface's OWN response, untouched. If it were
    // narrowed as well, a zero mix would stop being the identity and the A/B
    // control would quietly become a third variant.
    TEST(SkinLayeredSpecularTest, TheNarrowLobeIsTheSurfacesOwnRoughness)
    {
        const SkinProfileParameters p = LayeredProfile();
        for (i32 ri = 0; ri <= 20; ++ri)
        {
            const f32 roughness = static_cast<f32>(ri) / 20.0f;
            EXPECT_EQ(SkinSpecularLobesFor(roughness, p.Specular).NarrowRoughness, roughness);
        }
    }

    // -------------------------------------------------------------------------
    // Expression-driven detail
    // -------------------------------------------------------------------------

    // The property that makes the feature opt-in: a face that is not emoting
    // shades exactly as it would with no expression detail authored at all.
    TEST(SkinLayeredSpecularTest, ANeutralFaceHasNoExpressionDetail)
    {
        EXPECT_EQ(SkinExpressionDetailWeight({}), 0.0f);

        const std::array<f32, 4> allZero{ 0.0f, 0.0f, 0.0f, 0.0f };
        EXPECT_EQ(SkinExpressionDetailWeight(allZero), 0.0f);
    }

    // Order-free, so two frames that reached the same expression by different
    // routes shade the same. A running accumulator or a max-of-first-seen would
    // fail this.
    TEST(SkinLayeredSpecularTest, TheExpressionWeightDoesNotDependOnTargetOrder)
    {
        const std::array<f32, 4> a{ 0.1f, 0.25f, 0.05f, 0.3f };
        const std::array<f32, 4> b{ 0.3f, 0.05f, 0.25f, 0.1f };
        EXPECT_EQ(SkinExpressionDetailWeight(a), SkinExpressionDetailWeight(b));
    }

    TEST(SkinLayeredSpecularTest, TheExpressionWeightIsBoundedAndMonotone)
    {
        std::vector<f32> weights;
        f32 previous = 0.0f;
        for (i32 i = 0; i < 12; ++i)
        {
            weights.push_back(0.15f);
            const f32 weight = SkinExpressionDetailWeight(weights);
            EXPECT_GE(weight, previous - 1.0e-6f) << "target " << i;
            EXPECT_GE(weight, 0.0f);
            EXPECT_LE(weight, 1.0f);
            previous = weight;
        }
        EXPECT_EQ(previous, 1.0f) << "twelve targets at 0.15 must saturate the clamp";
    }

    // A NaN weight must not take the material's whole specular term with it.
    // MorphTargetComponent rejects these at the setter, so one arriving here
    // means a path that bypassed it — which is exactly when a guard matters.
    TEST(SkinLayeredSpecularTest, NonFiniteWeightsAreSkippedNotPropagated)
    {
        const std::array<f32, 4> dirty{ 0.25f, std::numeric_limits<f32>::quiet_NaN(), 0.25f,
                                        std::numeric_limits<f32>::infinity() };
        const f32 weight = SkinExpressionDetailWeight(dirty);
        EXPECT_TRUE(std::isfinite(weight));
        EXPECT_NEAR(weight, 0.5f, 1.0e-6f) << "the two clean weights, and only those";
    }

    // Negative weights count toward the expression: a blend shape driven to -0.4
    // is just as far from neutral as one driven to +0.4, and the pores deepen
    // either way.
    TEST(SkinLayeredSpecularTest, TheExpressionWeightUsesMagnitude)
    {
        const std::array<f32, 2> signed_{ -0.3f, 0.2f };
        EXPECT_NEAR(SkinExpressionDetailWeight(signed_), 0.5f, 1.0e-6f);
    }

    TEST(SkinLayeredSpecularTest, DetailStrengthInterpolatesFromBaseToBasePlusGain)
    {
        const SkinProfileParameters p = LayeredProfile();
        const f32 neutral = SkinDetailStrength(p.Specular, 0.0f);
        const f32 full = SkinDetailStrength(p.Specular, 1.0f);

        EXPECT_NEAR(neutral, p.Specular.DetailStrength, 1.0e-6f);
        EXPECT_NEAR(full, p.Specular.DetailStrength + p.Specular.ExpressionDetailGain, 1.0e-6f);
        EXPECT_GT(full, neutral);
    }

    // Two individually legal fields must not be able to add up to a value
    // neither of them could hold — and -1 in particular has to stay a hard
    // floor, because below it the pore band is re-added with the sign flipped
    // and the pores become bumps.
    TEST(SkinLayeredSpecularTest, DetailStrengthClampsTheSumNotTheTerms)
    {
        SkinProfileParameters p = LayeredProfile();
        p.Specular.DetailStrength = kMaxSkinDetailStrength;
        p.Specular.ExpressionDetailGain = kMaxSkinDetailStrength;
        ASSERT_TRUE(p.Sanitize()) << "both are individually in range";
        EXPECT_EQ(SkinDetailStrength(p.Specular, 1.0f), kMaxSkinDetailStrength);

        p.Specular.DetailStrength = kMinSkinDetailStrength;
        p.Specular.ExpressionDetailGain = kMinSkinDetailStrength;
        ASSERT_TRUE(p.Sanitize());
        EXPECT_EQ(SkinDetailStrength(p.Specular, 1.0f), kMinSkinDetailStrength);
    }

    TEST(SkinLayeredSpecularTest, ANonFiniteExpressionWeightFallsBackToTheBase)
    {
        const SkinProfileParameters p = LayeredProfile();
        EXPECT_EQ(SkinDetailStrength(p.Specular, std::numeric_limits<f32>::quiet_NaN()),
                  p.Specular.DetailStrength);
    }

    // -------------------------------------------------------------------------
    // Sanitize, the one validation gate
    // -------------------------------------------------------------------------

    TEST(SkinLayeredSpecularTest, SanitizeRejectsEveryOutOfRangeSpecularField)
    {
        const SkinSpecularParameters defaults{};

        SkinProfileParameters p;
        p.Specular.LobeMix = 5.0f;
        p.Specular.LobeRoughnessScale = 0.1f;
        p.Specular.NormalVarianceStrength = -3.0f;
        p.Specular.DetailStrength = 99.0f;
        p.Specular.ExpressionDetailGain = -99.0f;

        EXPECT_FALSE(p.Sanitize()) << "every field was out of range; Sanitize must say so";
        EXPECT_EQ(p.Specular.LobeMix, kMaxSkinLobeMix);
        EXPECT_EQ(p.Specular.LobeRoughnessScale, kMinSkinLobeRoughnessScale);
        EXPECT_EQ(p.Specular.NormalVarianceStrength, kMinSkinNormalVarianceStrength);
        EXPECT_EQ(p.Specular.DetailStrength, kMaxSkinDetailStrength);
        EXPECT_EQ(p.Specular.ExpressionDetailGain, kMinSkinDetailStrength);
        (void)defaults;
    }

    // std::clamp cannot reject a NaN — every comparison against it is false, so
    // clamp returns it unchanged. The gate has to test finiteness separately,
    // and this is the case that catches it having been simplified away.
    TEST(SkinLayeredSpecularTest, SanitizeReplacesNonFiniteSpecularFieldsWithDefaults)
    {
        const SkinSpecularParameters defaults{};
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();

        SkinProfileParameters p;
        p.Specular.LobeMix = nan;
        p.Specular.LobeRoughnessScale = std::numeric_limits<f32>::infinity();
        p.Specular.NormalVarianceStrength = nan;
        p.Specular.DetailStrength = nan;
        p.Specular.ExpressionDetailGain = -std::numeric_limits<f32>::infinity();

        EXPECT_FALSE(p.Sanitize());
        EXPECT_EQ(p.Specular.LobeMix, defaults.LobeMix);
        EXPECT_EQ(p.Specular.LobeRoughnessScale, defaults.LobeRoughnessScale);
        EXPECT_EQ(p.Specular.NormalVarianceStrength, defaults.NormalVarianceStrength);
        EXPECT_EQ(p.Specular.DetailStrength, defaults.DetailStrength);
        EXPECT_EQ(p.Specular.ExpressionDetailGain, defaults.ExpressionDetailGain);
    }

    // -------------------------------------------------------------------------
    // The version gate
    // -------------------------------------------------------------------------

    // A profile below version 3 must not acquire the layered response, and a
    // version this build does not know must apply NOTHING rather than guess.
    TEST(SkinLayeredSpecularTest, OnlyVersionThreeEvaluatesTheLayeredResponse)
    {
        EXPECT_FALSE(SkinEvaluatesLayeredSpecular(SkinEvaluationModel::DiffuseSpecularSplit));
        EXPECT_FALSE(SkinEvaluatesLayeredSpecular(SkinEvaluationModel::ScreenSpaceDiffusion));
        EXPECT_FALSE(SkinEvaluatesLayeredSpecular(SkinEvaluationModel::ThicknessTransmission));
        EXPECT_TRUE(SkinEvaluatesLayeredSpecular(SkinEvaluationModel::LayeredSpecular));
    }

    // -------------------------------------------------------------------------
    // The GPU lane
    // -------------------------------------------------------------------------

    // The forward path and the deferred path are handed the same four numbers by
    // the same function; this pins WHICH four and in what order, so a reordering
    // shows up here rather than as a head that is subtly wrong on one path.
    TEST(SkinLayeredSpecularTest, TheLaneCarriesTheAuthoredFieldsInOrder)
    {
        const SkinProfileParameters p = LayeredProfile();
        const glm::vec4 lane = SkinSpecularLane(p);

        EXPECT_EQ(lane.x, p.Specular.LobeMix);
        EXPECT_EQ(lane.y, p.Specular.LobeRoughnessScale);
        EXPECT_EQ(lane.z, p.Specular.NormalVarianceStrength);
        EXPECT_EQ(lane.w, 0.0f) << "w is reserved and must stay zero";
    }

    // An unclaimed or stale deferred slot is all-zero, and that has to shade as
    // "no effect" rather than as someone else's lobe.
    TEST(SkinLayeredSpecularTest, AnAllZeroLaneIsNeutral)
    {
        SkinProfileParameters p;
        p.Specular.LobeMix = 0.0f;
        p.Specular.NormalVarianceStrength = 0.0f;
        ASSERT_TRUE(p.Sanitize());

        // A zero mix is the single-lobe identity and a zero strength is the
        // unfiltered roughness; both are asserted exactly above, so what is left
        // to pin here is that the LANE really does carry those zeros.
        const glm::vec4 lane = SkinSpecularLane(p);
        EXPECT_EQ(lane.x, 0.0f);
        EXPECT_EQ(lane.z, 0.0f);
    }

} // namespace OloEngine::Tests
