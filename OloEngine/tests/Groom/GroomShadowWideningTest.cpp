#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// GroomShadowWideningTest.cpp — the light-space one-texel width floor (#1323).
//
// THE CLAIM THIS FILE TURNS INTO A NUMBER. "A 70 um hair is far below a cascade
// texel, so rasterised honestly it casts nothing" is the reason the groom
// caster family exists at all, and it is the kind of statement that reads like
// a slogan until somebody computes it. Case 1 computes it: against a 20 m
// cascade at 2048 texels a strand's true half width is under a hundredth of a
// texel, so the floor multiplies it by more than a hundred. Without that
// number, "widen strands to a texel" is a magic constant.
//
// THE SIGN TRAP GETS ITS OWN CASE. Vulkan's clip space has +Y downwards, so the
// engine uploads a projection whose [1][1] is negative there, and a signed read
// of the projection scale returns a NEGATIVE pixels-per-world — which clamps
// the half width to the floor in one axis and is invisible on OpenGL. The same
// trap, in the same shape, that groom-strand-visibility.md gives its own
// heading to; the fix is length(), and this is what says so.
//
// Classification: L1 — pure arithmetic, no device, no context. The GLSL twin
// (include/GroomShadowWidening.glsl) is the other half of the pair and is
// pinned separately.
// =============================================================================

#include "OloEngine/Groom/GroomShadowWidening.h"
#include "OloEngine/Scene/Components.h"

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace OloEngine::Tests
{
    namespace
    {
        // A directional cascade as the engine builds one: an orthographic box
        // around the view frustum's slice, composed with a lookAt down the
        // light. 20 m across is a plausible near cascade in a character scene.
        constexpr f32 kCascadeExtentMetres = 20.0f;
        constexpr f32 kCascadeResolution = 2048.0f;

        [[nodiscard]] glm::mat4 MakeCascadeViewProjection(f32 extentMetres = kCascadeExtentMetres)
        {
            const f32 half = extentMetres * 0.5f;
            const glm::mat4 projection = glm::ortho(-half, half, -half, half, 0.1f, 400.0f);
            const glm::mat4 view =
                glm::lookAt(glm::vec3(30.0f, 40.0f, 30.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            return projection * view;
        }

        // A human hair is 17-180 um in DIAMETER; 70 um is the middle of the
        // range the coverage analysis uses, and the radius is half of it
        // because widths on disk are diameters and are halved exactly once
        // (groom-strand-visibility.md rule 8).
        constexpr f32 kHairRadiusMetres = 0.5f * 70.0e-6f;
    } // namespace

    // ── 1. The measurement the whole caster family rests on ─────────────
    TEST(GroomShadowWidening, AHairIsFarBelowACascadeTexelSoTheFloorIsWhatMakesItCast)
    {
        const glm::mat4 cascade = MakeCascadeViewProjection();
        // An orthographic clip w is 1, so no foreshortening enters here.
        const f32 ndcPerWorld = GroomShadowNdcPerWorld(cascade, 1.0f);

        // One texel spans 2 / resolution of NDC, so a half width expressed in
        // texels is halfNdc * resolution / 2.
        const f32 trueHalfNdc = kHairRadiusMetres * ndcPerWorld;
        const f32 trueHalfTexels = trueHalfNdc * kCascadeResolution * 0.5f;

        // PINNED TO THE VALUE, not merely bounded below 0.01. The agent-rules
        // doc quotes this number, and a one-sided bound cannot catch the doc
        // drifting from it -- which is exactly what happened: the doc said
        // 0.0072, which is the FULL width, because one texel is 2/resolution of
        // NDC and not 1/resolution.
        EXPECT_NEAR(trueHalfTexels, 0.0036f, 0.0001f)
            << "a " << (kHairRadiusMetres * 2.0e6f) << " um hair measured " << trueHalfTexels
            << " texels of half width against a " << kCascadeExtentMetres << " m cascade at " << kCascadeResolution
            << " -- if this moved, groom-into-the-shadow-techniques.md rule 2 quotes the old value";

        const f32 factor = GroomShadowWideningFactor(kHairRadiusMetres, ndcPerWorld, kCascadeResolution, 1.0f);
        EXPECT_NEAR(factor, 139.0f, 1.0f)
            << "the one-texel floor widened the strand by " << factor
            << "x; that ratio IS the opaque-shadow over-occlusion bound, so it has to be a measured number";
    }

    // ── 2. What "one texel" means, exactly ──────────────────────────────
    TEST(GroomShadowWidening, TheFloorIsOneTexelOfFullWidthNotOfHalfWidth)
    {
        // NDC spans [-1, 1] over `resolution` texels, so one texel is
        // 2/resolution and a band of HALF width 1/resolution is one texel
        // across. That is the smallest band that reliably contains a texel
        // centre, which is the entire point of the floor.
        constexpr f32 resolution = 1024.0f;
        const f32 halfNdc = GroomShadowHalfWidthNdc(/*radiusWorld=*/0.0f, /*ndcPerWorld=*/1.0f, resolution, 1.0f);
        EXPECT_FLOAT_EQ(halfNdc, 1.0f / resolution);

        const f32 fullWidthTexels = 2.0f * halfNdc * resolution * 0.5f;
        EXPECT_FLOAT_EQ(fullWidthTexels, 1.0f);
    }

    // ── 3. The sign trap: invisible on OpenGL, silent on Vulkan ─────────
    TEST(GroomShadowWidening, TheScaleTakesTheMagnitudeSoAVulkanYFlipDoesNotInvertIt)
    {
        const glm::mat4 opengl = MakeCascadeViewProjection();
        // The y flip the engine applies for Vulkan's +Y-down clip space, as a
        // whole-row negation of the projection's second row — which is what
        // makes element [1][1] negative there.
        glm::mat4 vulkan = opengl;
        for (int column = 0; column < 4; ++column)
        {
            vulkan[column][1] = -vulkan[column][1];
        }

        const f32 glScale = GroomShadowNdcPerWorld(opengl, 1.0f);
        const f32 vkScale = GroomShadowNdcPerWorld(vulkan, 1.0f);

        EXPECT_GT(glScale, 0.0f);
        EXPECT_FLOAT_EQ(vkScale, glScale)
            << "the two backends widened a strand differently. A signed read of the projection scale is negative "
               "on Vulkan, which clamps every half width to the floor with nothing logged and no validation "
               "message -- the failure that is invisible on OpenGL";
    }

    // ── 4. The A/B control has to actually be a control ─────────────────
    TEST(GroomShadowWidening, AZeroFloorLeavesTheStrandAtItsTrueWidth)
    {
        const glm::mat4 cascade = MakeCascadeViewProjection();
        const f32 ndcPerWorld = GroomShadowNdcPerWorld(cascade, 1.0f);

        const f32 factor = GroomShadowWideningFactor(kHairRadiusMetres, ndcPerWorld, kCascadeResolution, 0.0f);
        EXPECT_FLOAT_EQ(factor, 1.0f)
            << "Width Floor = 0 is the A/B control the evidence is measured against; if it still widens, the "
               "control and the treatment are the same picture";

        EXPECT_FLOAT_EQ(GroomShadowHalfWidthNdc(kHairRadiusMetres, ndcPerWorld, kCascadeResolution, 0.0f),
                        kHairRadiusMetres * ndcPerWorld);
    }

    // ── 5. A strand already wider than a texel is left alone ────────────
    TEST(GroomShadowWidening, AStrandAlreadyWiderThanATexelIsNotWidened)
    {
        const glm::mat4 cascade = MakeCascadeViewProjection();
        const f32 ndcPerWorld = GroomShadowNdcPerWorld(cascade, 1.0f);

        // 20 cm of radius against a 1 cm texel: a rope rather than a hair, but
        // it is the case that says the floor is a FLOOR and not a scale.
        constexpr f32 ropeRadius = 0.2f;
        EXPECT_FLOAT_EQ(GroomShadowWideningFactor(ropeRadius, ndcPerWorld, kCascadeResolution, 1.0f), 1.0f);
        EXPECT_FLOAT_EQ(GroomShadowHalfWidthNdc(ropeRadius, ndcPerWorld, kCascadeResolution, 1.0f),
                        ropeRadius * ndcPerWorld);
    }

    // ── 6. One expression covers both projection kinds ──────────────────
    TEST(GroomShadowWidening, ThePerspectiveArmForeshortensAndTheOrthographicArmDoesNot)
    {
        // A spot light's atlas tile is a PERSPECTIVE projection, and the same
        // expression has to serve it -- which is why it divides by clip w
        // rather than branching on the projection kind. Doubling the distance
        // halves the apparent size.
        const glm::mat4 spot =
            glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f) *
            glm::lookAt(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));

        const f32 near_ = GroomShadowNdcPerWorld(spot, 2.0f);
        const f32 far_ = GroomShadowNdcPerWorld(spot, 4.0f);
        EXPECT_GT(near_, far_);
        EXPECT_NEAR(far_, near_ * 0.5f, near_ * 1.0e-5f);

        // The orthographic arm is the w == 1 case of the same formula, asserted
        // against the EXPECTED scale rather than against itself: an orthographic
        // cascade's row-0 length is 1/halfExtent, so a 20 m extent is 0.1 NDC per
        // metre. Comparing the call to itself is a tautology that passes however
        // wrong the formula is.
        const glm::mat4 cascade = MakeCascadeViewProjection();
        EXPECT_FLOAT_EQ(GroomShadowNdcPerWorld(cascade, 1.0f), 2.0f / kCascadeExtentMetres);
    }

    // ── 7. A degenerate clip w must not produce an infinity ─────────────
    TEST(GroomShadowWidening, AZeroClipWIsGuardedRatherThanDividedBy)
    {
        const glm::mat4 cascade = MakeCascadeViewProjection();
        const f32 scale = GroomShadowNdcPerWorld(cascade, 0.0f);
        EXPECT_TRUE(std::isfinite(scale))
            << "a vertex exactly on the light's near plane produced a non-finite scale; the shader would widen "
               "every ribbon by a NaN and the coat would leave the shadow map entirely";
    }

    // ── 8. The authoring boundary the save file and MCP bypass ──────────
    // ── 8. The caster subset (#1533 E1) ─────────────────────────────────
    //
    // The floor over-occludes by the widening factor, so a uniform share of the
    // coat 1/factor in size blocks what the whole coat really blocks. The share
    // keeps a margin over that and never casts less than the true coverage.

    TEST(GroomShadowWidening, TheCasterShareUndoesTheFloorsOverCoverageAndKeepsItsMargin)
    {
        // The dog's near cascade: about 4 m across at 4096 texels, a texel just
        // under a millimetre, against a coat whose mean strand is 0.1 mm thick.
        const glm::mat4 cascade = MakeCascadeViewProjection(4.0f);
        const f32 ndcPerWorld = GroomShadowNdcPerWorld(cascade, 1.0f);
        const f32 meanRadius = 0.5f * 1.0e-4f;
        const f32 widening = GroomShadowWideningFactor(meanRadius, ndcPerWorld, 4096.0f, 1.0f);
        ASSERT_GT(widening, 2.0f * kGroomCasterCoverageMargin) << "the case must sit where the floor dominates";

        const f32 share = GroomShadowCasterFraction(meanRadius, ndcPerWorld, 4096.0f, 1.0f,
                                                    kGroomCasterCoverageMargin, kGroomCasterMinFraction);
        // PINNED: a texel of 4/4096 m over a 0.1 mm strand is a 9.77x widening,
        // so twice the true coverage is 2/9.77 of the strands.
        EXPECT_NEAR(widening, 9.766f, 0.01f);
        EXPECT_NEAR(share, 0.2048f, 0.0005f);
        // What the subset covers, in units of the whole coat's TRUE coverage: the
        // margin, exactly.
        EXPECT_NEAR(share * widening, kGroomCasterCoverageMargin, 1.0e-4f);
    }

    TEST(GroomShadowWidening, ACoatAlreadyAsThickAsATexelCastsWhole)
    {
        const glm::mat4 cascade = MakeCascadeViewProjection(4.0f);
        const f32 ndcPerWorld = GroomShadowNdcPerWorld(cascade, 1.0f);
        // A 2 mm "strand" is two texels wide: nothing widened, nothing to undo.
        EXPECT_EQ(GroomShadowCasterFraction(1.0e-3f, ndcPerWorld, 4096.0f, 1.0f, kGroomCasterCoverageMargin,
                                            kGroomCasterMinFraction),
                  1.0f);
        // And a coat whose widening is below the margin keeps every strand too.
        const f32 texelHalfMetres = 1.0f / (ndcPerWorld * 4096.0f);
        EXPECT_EQ(GroomShadowCasterFraction(0.7f * texelHalfMetres, ndcPerWorld, 4096.0f, 1.0f,
                                            kGroomCasterCoverageMargin, kGroomCasterMinFraction),
                  1.0f);
    }

    TEST(GroomShadowWidening, NoFloorNoRadiusOrNoProjectionCastsWhole)
    {
        const glm::mat4 cascade = MakeCascadeViewProjection(4.0f);
        const f32 ndcPerWorld = GroomShadowNdcPerWorld(cascade, 1.0f);
        EXPECT_EQ(GroomShadowCasterFraction(5.0e-5f, ndcPerWorld, 4096.0f, 0.0f, 2.0f, 0.0625f), 1.0f)
            << "a zero floor widens nothing, so there is no over-coverage to trade";
        EXPECT_EQ(GroomShadowCasterFraction(0.0f, ndcPerWorld, 4096.0f, 1.0f, 2.0f, 0.0625f), 1.0f)
            << "an unmeasured radius is not an infinitely thin one";
        EXPECT_EQ(GroomShadowCasterFraction(5.0e-5f, 0.0f, 4096.0f, 1.0f, 2.0f, 0.0625f), 1.0f);
        EXPECT_EQ(GroomShadowCasterFraction(std::numeric_limits<f32>::quiet_NaN(), ndcPerWorld, 4096.0f, 1.0f, 2.0f,
                                            0.0625f),
                  1.0f);
    }

    TEST(GroomShadowWidening, TheShareNeverFallsBelowItsMinimumOrUnderTheTrueCoverage)
    {
        // A coarse cascade: 200 m across at 1024 texels, a 0.1 mm strand widened
        // about two thousand-fold.
        const glm::mat4 coarse = MakeCascadeViewProjection(200.0f);
        const f32 ndcPerWorld = GroomShadowNdcPerWorld(coarse, 1.0f);
        EXPECT_EQ(GroomShadowCasterFraction(5.0e-5f, ndcPerWorld, 1024.0f, 1.0f, kGroomCasterCoverageMargin,
                                            kGroomCasterMinFraction),
                  kGroomCasterMinFraction);

        // A margin under 1 -- or not a number -- would cast less than the coat
        // blocks; both are read as 1, which keeps the true coverage exactly.
        const glm::mat4 nearCascade = MakeCascadeViewProjection(4.0f);
        const f32 nearNdc = GroomShadowNdcPerWorld(nearCascade, 1.0f);
        const f32 widening = GroomShadowWideningFactor(5.0e-5f, nearNdc, 4096.0f, 1.0f);
        for (const f32 margin : { 0.25f, std::numeric_limits<f32>::quiet_NaN() })
        {
            const f32 share = GroomShadowCasterFraction(5.0e-5f, nearNdc, 4096.0f, 1.0f, margin, 0.0f);
            EXPECT_NEAR(share * widening, 1.0f, 1.0e-4f) << "margin " << margin;
        }
    }

    TEST(GroomShadowWidening, TheMeanRadiusMakesTheShareALowerBoundForAMixedCoat)
    {
        // A coat of two populations, by length: thin undercoat far below the
        // floor and guard hair ABOVE it. The share computed from the
        // length-weighted mean radius must still keep at least the margin times
        // the true coverage once the thick strands keep their own width.
        const glm::mat4 cascade = MakeCascadeViewProjection(4.0f);
        const f32 ndcPerWorld = GroomShadowNdcPerWorld(cascade, 1.0f);
        const f32 resolution = 4096.0f;
        const f32 floorHalfNdc = 1.0f / resolution;
        struct Population
        {
            f32 Radius;
            f32 Length;
        };
        // 0.06 mm undercoat a sixteenth of a texel wide, and 1.2 mm guard hair
        // a fifth wider than the texel.
        const Population coat[] = { { 3.0e-5f, 90.0f }, { 6.0e-4f, 10.0f } };

        f32 radiusLength = 0.0f;
        f32 length = 0.0f;
        f32 trueArea = 0.0f;
        f32 flooredArea = 0.0f;
        for (const Population& population : coat)
        {
            radiusLength += population.Radius * population.Length;
            length += population.Length;
            const f32 trueHalf = population.Radius * ndcPerWorld;
            trueArea += trueHalf * population.Length;
            flooredArea += std::max(trueHalf, floorHalfNdc) * population.Length;
        }
        const f32 share = GroomShadowCasterFraction(radiusLength / length, ndcPerWorld, resolution, 1.0f,
                                                    kGroomCasterCoverageMargin, 0.0f);
        EXPECT_LT(share, 1.0f) << "the case must actually thin the coat";
        EXPECT_GE(share * flooredArea, kGroomCasterCoverageMargin * trueArea * 0.9999f)
            << "the subset covers less than the margin promises once some strands are wider than the floor";
    }

    TEST(GroomSceneShadowComponentSanitiser, ANonFiniteOrNegativeFloorIsReplacedRatherThanPassedThrough)
    {
        GroomSceneShadowComponent component;
        const f32 authoredDefault = component.m_ShadowWidthTexels;

        // A NaN is the dangerous one: it reaches a DIVISOR in the widening,
        // makes every half width a NaN and removes the whole coat from every
        // shadow map with nothing in any log.
        component.m_ShadowWidthTexels = std::numeric_limits<f32>::quiet_NaN();
        EXPECT_FLOAT_EQ(MakeGroomShadowWidthTexels(component), authoredDefault);

        component.m_ShadowWidthTexels = -1.0f;
        EXPECT_FLOAT_EQ(MakeGroomShadowWidthTexels(component), authoredDefault);

        // Zero is LEGAL: it is the A/B control, not a corrupt value.
        component.m_ShadowWidthTexels = 0.0f;
        EXPECT_FLOAT_EQ(MakeGroomShadowWidthTexels(component), 0.0f);

        component.m_ShadowWidthTexels = 1.0e9f;
        EXPECT_FLOAT_EQ(MakeGroomShadowWidthTexels(component), 16.0f);
    }

    // ── 9. The two directions are separately authorable ─────────────────
    TEST(GroomSceneShadowComponentSanitiser, TheDefaultsRouteBothDirectionsAndTheComponentIsAbsentByDefault)
    {
        const GroomSceneShadowComponent component;
        // Adding the component is the opt-in; once added, both directions are
        // on, because a component you added to get shadows that then does
        // nothing is the worse default.
        EXPECT_TRUE(component.m_CastShadows);
        EXPECT_TRUE(component.m_ReceiveShadows);
        EXPECT_FLOAT_EQ(component.m_ShadowWidthTexels, 1.0f);

        // And they are INDEPENDENT fields, so an A/B of one is a measurement of
        // that one rather than a picture of both.
        GroomSceneShadowComponent casterOnly;
        casterOnly.m_ReceiveShadows = false;
        EXPECT_TRUE(casterOnly.m_CastShadows);
        EXPECT_FALSE(casterOnly == component);
    }
} // namespace OloEngine::Tests
