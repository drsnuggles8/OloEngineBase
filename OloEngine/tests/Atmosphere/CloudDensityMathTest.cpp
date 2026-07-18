// OLO_TEST_LAYER: L1
// =============================================================================
// CloudDensityMathTest.cpp
//
// Pins the volumetric cloudscape's shader math (issue #633) with CPU mirrors
// of OloEditor/assets/shaders/include/CloudscapeCommon.glsl — the same
// literal-transcription discipline as VolumetricFogMathTest: every formula
// and constant below is re-declared from the GLSL on purpose, so a silent
// shader-side change surfaces as a test edit, and headless CI pins the math
// without a GPU.
//
// What gets pinned:
//   - Henyey-Greenstein phase normalizes over the sphere (energy) and the
//     dual-lobe blend keeps forward dominance for g > 0.
//   - Beer-Lambert + powder stays in [0, 1], decays monotonically with
//     optical depth, and the powder term darkens ONLY low optical depths.
//   - The two-layer height gradient is bounded, vanishes at both layer
//     boundaries, and stratus tops out below cumulus.
//   - Ray/layer slab intersection: below / inside / above the layer, and
//     numerical safety for horizontal rays.
//   - The shared extinction scale renders a kilometre of full-density cloud
//     effectively opaque (the "clouds are not translucent fog" contract).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

namespace OloEngine::Tests
{
    namespace
    {
        // ── Literal mirrors of CloudscapeCommon.glsl ──

        constexpr f32 kCloudExtinction = 0.03f; // mirrors kCloudExtinction

        f32 SmoothStep(f32 e0, f32 e1, f32 x)
        {
            const f32 t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // mirrors cloudHeightGradient()
        f32 CloudHeightGradient(f32 heightFrac, f32 cloudType)
        {
            const f32 stratus = SmoothStep(0.0f, 0.08f, heightFrac) *
                                (1.0f - SmoothStep(0.18f, 0.32f, heightFrac));
            const f32 cumulus = SmoothStep(0.0f, 0.14f, heightFrac) *
                                (1.0f - SmoothStep(0.55f, 0.95f, heightFrac));
            return glm::mix(stratus, cumulus, std::clamp(cloudType, 0.0f, 1.0f));
        }

        // mirrors cloudPhaseHG()
        f32 CloudPhaseHG(f32 cosTheta, f32 g)
        {
            const f32 g2 = g * g;
            const f32 denom = 1.0f + g2 - 2.0f * g * cosTheta;
            return (1.0f - g2) / (12.5663706f * denom * std::sqrt(std::max(denom, 1.0e-4f)));
        }

        // mirrors cloudPhase()
        f32 CloudPhase(f32 cosTheta, f32 g)
        {
            return glm::mix(CloudPhaseHG(cosTheta, -0.25f * g), CloudPhaseHG(cosTheta, g), 0.7f);
        }

        // mirrors cloudBeerPowder()
        f32 CloudBeerPowder(f32 opticalDepth, f32 powderStrength)
        {
            const f32 beer = std::exp(-opticalDepth);
            const f32 powder = 1.0f - std::exp(-2.0f * opticalDepth);
            return beer * glm::mix(1.0f, powder, std::clamp(powderStrength, 0.0f, 2.0f) * 0.5f);
        }

        // mirrors cloudLayerIntersect() with u_CloudLayer.xy = (bottom, top)
        glm::vec2 CloudLayerIntersect(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                                      f32 bottom, f32 top)
        {
            const f32 safeY = std::abs(rayDir.y) > 1.0e-5f
                                  ? rayDir.y
                                  : (rayDir.y >= 0.0f ? 1.0e-5f : -1.0e-5f);
            const f32 invY = 1.0f / safeY;
            const f32 t0 = (bottom - rayOrigin.y) * invY;
            const f32 t1 = (top - rayOrigin.y) * invY;
            return { std::max(std::min(t0, t1), 0.0f), std::max(std::max(t0, t1), 0.0f) };
        }
    } // namespace

    // ---------- Phase function ----------

    TEST(CloudDensityMath, PhaseHGNormalizesOverTheSphere)
    {
        // Integral of phase over all solid angles must be 1 (energy
        // conservation): 2*pi * ∫ p(cosTheta) d(cosTheta) over [-1, 1].
        for (f32 g : { 0.0f, 0.3f, 0.6f, 0.85f })
        {
            const int kSamples = 20000;
            f64 sum = 0.0;
            for (int i = 0; i < kSamples; ++i)
            {
                const f32 cosTheta = -1.0f + 2.0f * (static_cast<f32>(i) + 0.5f) / kSamples;
                sum += static_cast<f64>(CloudPhaseHG(cosTheta, g));
            }
            const f64 integral = 2.0 * glm::pi<f64>() * sum * (2.0 / kSamples);
            EXPECT_NEAR(integral, 1.0, 0.02) << "g = " << g;
        }
    }

    TEST(CloudDensityMath, DualLobePhaseKeepsForwardDominance)
    {
        for (f32 g : { 0.2f, 0.6f, 0.9f })
        {
            EXPECT_GT(CloudPhase(1.0f, g), CloudPhase(-1.0f, g))
                << "forward scattering must dominate for g = " << g;
            EXPECT_GT(CloudPhase(1.0f, g), CloudPhase(0.0f, g));
            EXPECT_TRUE(std::isfinite(CloudPhase(1.0f, g)));
        }
        // Isotropic limit: flat within tolerance.
        EXPECT_NEAR(CloudPhase(1.0f, 0.0f), CloudPhase(-1.0f, 0.0f), 1.0e-5f);
    }

    // ---------- Beer-Lambert + powder ----------

    TEST(CloudDensityMath, BeerPowderIsBoundedAndMonotonicallyDecaying)
    {
        f32 prev = 2.0f;
        for (f32 od = 0.0f; od <= 12.0f; od += 0.25f)
        {
            const f32 plain = CloudBeerPowder(od, 0.0f);
            EXPECT_GE(plain, 0.0f);
            EXPECT_LE(plain, 1.0f);
            EXPECT_LE(plain, prev + 1.0e-6f) << "pure Beer must decay (od " << od << ")";
            prev = plain;
        }
        // Deep cloud is opaque either way.
        EXPECT_LT(CloudBeerPowder(10.0f, 0.0f), 1.0e-3f);
        EXPECT_LT(CloudBeerPowder(10.0f, 1.0f), 1.0e-3f);
    }

    TEST(CloudDensityMath, PowderDarkensOnlyLowOpticalDepths)
    {
        // The powder term models missing back-scatter in thin media: at small
        // optical depth the powdered result must be darker than pure Beer, and
        // the two must converge as depth grows.
        EXPECT_LT(CloudBeerPowder(0.1f, 1.0f), CloudBeerPowder(0.1f, 0.0f));
        EXPECT_LT(CloudBeerPowder(0.5f, 1.0f), CloudBeerPowder(0.5f, 0.0f));
        const f32 deepPlain = CloudBeerPowder(4.0f, 0.0f);
        const f32 deepPowder = CloudBeerPowder(4.0f, 1.0f);
        EXPECT_NEAR(deepPowder / std::max(deepPlain, 1.0e-9f), 1.0f, 0.01f)
            << "powder must converge to plain Beer at depth";
    }

    // ---------- Height gradient ----------

    TEST(CloudDensityMath, HeightGradientVanishesAtLayerBoundariesAndIsBounded)
    {
        for (f32 type : { 0.0f, 0.5f, 1.0f })
        {
            EXPECT_FLOAT_EQ(CloudHeightGradient(0.0f, type), 0.0f);
            EXPECT_FLOAT_EQ(CloudHeightGradient(1.0f, type), 0.0f);
            for (f32 h = 0.0f; h <= 1.0f; h += 0.02f)
            {
                const f32 g = CloudHeightGradient(h, type);
                EXPECT_GE(g, 0.0f);
                EXPECT_LE(g, 1.0f);
            }
        }
    }

    TEST(CloudDensityMath, StratusIsFlatAndLowCumulusIsTall)
    {
        // Stratus (type 0) has no mass above ~1/3 of the layer; cumulus
        // (type 1) still has mass at mid-layer.
        EXPECT_FLOAT_EQ(CloudHeightGradient(0.5f, 0.0f), 0.0f);
        EXPECT_GT(CloudHeightGradient(0.5f, 1.0f), 0.5f);
        // Both profiles have solid mass just above the base.
        EXPECT_GT(CloudHeightGradient(0.15f, 0.0f), 0.5f);
        EXPECT_GT(CloudHeightGradient(0.2f, 1.0f), 0.5f);
    }

    // ---------- Ray/layer intersection ----------

    TEST(CloudDensityMath, LayerIntersectCoversBelowInsideAndAbove)
    {
        const f32 bottom = 1500.0f;
        const f32 top = 4000.0f;

        // Camera below the layer, looking up 45 degrees.
        const glm::vec3 up45 = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));
        const glm::vec2 below = CloudLayerIntersect(glm::vec3(0.0f), up45, bottom, top);
        EXPECT_GT(below.y, below.x);
        EXPECT_NEAR(below.x * up45.y, bottom, 1.0f); // enters at the bottom plane
        EXPECT_NEAR(below.y * up45.y, top, 1.0f);    // exits at the top plane

        // Camera inside the layer: march starts immediately.
        const glm::vec2 inside =
            CloudLayerIntersect(glm::vec3(0.0f, 2000.0f, 0.0f), up45, bottom, top);
        EXPECT_FLOAT_EQ(inside.x, 0.0f);
        EXPECT_GT(inside.y, 0.0f);

        // Camera above, looking further up: misses (enter >= exit at 0).
        const glm::vec2 above =
            CloudLayerIntersect(glm::vec3(0.0f, 5000.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), bottom, top);
        EXPECT_GE(above.x, above.y);

        // Camera above, looking down: hits top first, then bottom.
        const glm::vec2 down =
            CloudLayerIntersect(glm::vec3(0.0f, 5000.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), bottom, top);
        EXPECT_NEAR(down.x, 1000.0f, 1.0f);
        EXPECT_NEAR(down.y, 3500.0f, 1.0f);
    }

    TEST(CloudDensityMath, LayerIntersectSurvivesHorizontalRays)
    {
        // A perfectly horizontal ray hits the epsilon guard, not a divide by
        // zero — results must be finite (a horizontal ray inside the layer
        // conceptually never exits; the guard yields a huge but finite span).
        const glm::vec2 h = CloudLayerIntersect(glm::vec3(0.0f, 2000.0f, 0.0f),
                                                glm::vec3(1.0f, 0.0f, 0.0f), 1500.0f, 4000.0f);
        EXPECT_TRUE(std::isfinite(h.x));
        EXPECT_TRUE(std::isfinite(h.y));
    }

    // ---------- Extinction scale ----------

    TEST(CloudDensityMath, KilometreOfFullDensityCloudIsOpaque)
    {
        // Transmittance through 1 km of density-1 cloud at the shared
        // extinction scale — the raymarch and the ground shadow map both use
        // kCloudExtinction, so this pins the "storm clouds actually block the
        // sun" contract for both.
        const f32 transmittance = std::exp(-kCloudExtinction * 1.0f * 1000.0f);
        EXPECT_LT(transmittance, 1.0e-3f);
        // ...while 50 m of thin cloud stays translucent (wispy edges visible).
        EXPECT_GT(std::exp(-kCloudExtinction * 0.2f * 50.0f), 0.7f); // exp(-0.3) ~ 0.74
    }
} // namespace OloEngine::Tests
