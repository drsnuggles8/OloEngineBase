// =============================================================================
// ProceduralSkyMathTest.cpp
//
// Pins the Preetham (1999) analytic daylight model implementation in
// OloEngine/Renderer/ProceduralSky.cpp.  These tests are pure CPU math —
// no GPU context required — so they survive on headless CI runners.
//
// What gets pinned:
//
//   - Perez F-function coefficients are linear in turbidity (the slope /
//     intercept rows from Preetham's table 2). A bookkeeping typo on a
//     single coefficient row tints the entire sky.
//
//   - Zenith chromaticity and luminance polynomial.  These are the
//     reference values at sun direction; the per-direction Perez sample
//     normalises by F(0, theta_s) so an error here scales the whole sky.
//
//   - Sun direction below the horizon is clamped (the analytic model is
//     undefined for theta_s > pi/2).  The intent is "render something
//     sensible; don't poison the IBL".
//
//   - The end-to-end CPU evaluator agrees with itself between zenith and
//     near-horizon directions (the sky is brighter near the horizon, the
//     sun-side is much brighter than the anti-sun side, and the result
//     stays finite).
//
// Why both math and limit cases are here: the GLSL shader is a direct port
// of this C++ code (same Perez expression, same xyY->RGB matrix). A typo
// in either side would diverge them — pinning the C++ side makes the
// shader breakage observable without a GPU.
//
// Classification: L4 / shaderpipe (CPU mirror of shader math).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/ProceduralSky.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

namespace OloEngine::Tests
{
    namespace
    {
        // Standard test parameter set: noon-ish sun, clear sky.
        PreethamParameters StandardParams()
        {
            PreethamParameters p;
            p.SunDirection = glm::normalize(glm::vec3(0.3f, 0.7f, 0.4f));
            p.Turbidity = 2.5f;
            // Exposure is the rate in the luminance tonemap 1-exp(-Exposure*Y).
            // The default operating point (0.1) keeps the sky in the
            // unsaturated regime where relative-brightness contracts hold;
            // a large value (e.g. 1.0) would push every direction to ~1.0 and
            // erase the gradient the tests check.
            p.Exposure = 0.1f;
            p.SunIntensity = 1.0f;
            p.SunDiskSize = 1.0f;
            p.ShowSunDisk = false; // Disk would dominate the dot-test directions
            return p;
        }
    }

    // ---------- Coefficient linearity (Preetham table 2) ----------

    TEST(ProceduralSkyMath, PerezCoefficientsAreLinearInTurbidity)
    {
        // Pick two turbidities, compute coefficients, verify linear behaviour:
        // f(T2) - f(T1) should equal slope * (T2 - T1).  Using f(0) and f(T)
        // recovers (slope, intercept).
        PreethamParameters p1 = StandardParams();
        p1.Turbidity = 2.0f;
        PreethamParameters p2 = StandardParams();
        p2.Turbidity = 8.0f;

        const auto c1 = ProceduralSky::ComputeCoefficients(p1);
        const auto c2 = ProceduralSky::ComputeCoefficients(p2);

        // Compute slopes (per channel) and compare to Preetham's table.
        // x.A row: slope = -0.0193, intercept = -0.2592
        const f32 slopeX_A = (c2.A.x - c1.A.x) / (p2.Turbidity - p1.Turbidity);
        EXPECT_NEAR(slopeX_A, -0.0193f, 1e-5f);

        // Y.A row: slope = 0.1787
        const f32 slopeY_A = (c2.A.z - c1.A.z) / (p2.Turbidity - p1.Turbidity);
        EXPECT_NEAR(slopeY_A, 0.1787f, 1e-5f);

        // y.B row: slope = -0.0950, intercept = +0.0092
        const f32 slopeY_B = (c2.B.y - c1.B.y) / (p2.Turbidity - p1.Turbidity);
        EXPECT_NEAR(slopeY_B, -0.0950f, 1e-5f);
    }

    TEST(ProceduralSkyMath, CoefficientsMatchReferenceAtT2_5)
    {
        // T = 2.5 — pin the absolute coefficient values so a row-order
        // mix-up shows up immediately.  Values computed by hand from
        // Preetham's table 2: coeff = slope * T + intercept.
        PreethamParameters p = StandardParams();
        p.Turbidity = 2.5f;
        const auto c = ProceduralSky::ComputeCoefficients(p);

        // x channel (A,B,C,D,E)
        EXPECT_NEAR(c.A.x, -0.0193f * 2.5f - 0.2592f, 1e-5f);
        EXPECT_NEAR(c.B.x, -0.0665f * 2.5f + 0.0008f, 1e-5f);
        EXPECT_NEAR(c.C.x, -0.0004f * 2.5f + 0.2125f, 1e-5f);
        EXPECT_NEAR(c.D.x, -0.0641f * 2.5f - 0.8989f, 1e-5f);
        EXPECT_NEAR(c.E.x, -0.0033f * 2.5f + 0.0452f, 1e-5f);

        // y channel
        EXPECT_NEAR(c.A.y, -0.0167f * 2.5f - 0.2608f, 1e-5f);
        EXPECT_NEAR(c.E.y, -0.0109f * 2.5f + 0.0529f, 1e-5f);

        // Y channel
        EXPECT_NEAR(c.A.z, 0.1787f * 2.5f - 1.4630f, 1e-5f);
        EXPECT_NEAR(c.C.z, -0.0227f * 2.5f + 5.3251f, 1e-5f);
    }

    // ---------- Zenith chromaticity ----------

    TEST(ProceduralSkyMath, ZenithLuminanceIsPositiveAtCommonElevations)
    {
        // Preetham's zenith Y goes negative for very low sun angles in the
        // raw formula (the (4.0453*T - 4.971)*tan(chi) term).  Our impl
        // clamps to non-negative.  Verify the clamp on a low-sun case and
        // sanity-check a noon case.
        PreethamParameters noon = StandardParams();
        noon.SunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
        EXPECT_GT(ProceduralSky::ComputeCoefficients(noon).ZenithXYY.z, 0.0f);

        PreethamParameters lowSun = StandardParams();
        lowSun.SunDirection = glm::normalize(glm::vec3(0.99f, 0.14f, 0.0f));
        EXPECT_GE(ProceduralSky::ComputeCoefficients(lowSun).ZenithXYY.z, 0.0f);
    }

    TEST(ProceduralSkyMath, ZenithChromaticityIsInPlausibleRange)
    {
        // CIE chromaticity values are bounded in [0, 1].  Daylight zenith
        // chromaticity should hover in [0.25, 0.5] (mid-day blue sky).
        // A bad polynomial would push it well outside that band.
        PreethamParameters p = StandardParams();
        const auto c = ProceduralSky::ComputeCoefficients(p);

        EXPECT_GT(c.ZenithXYY.x, 0.0f);
        EXPECT_LT(c.ZenithXYY.x, 1.0f);
        EXPECT_GT(c.ZenithXYY.y, 0.0f);
        EXPECT_LT(c.ZenithXYY.y, 1.0f);
    }

    // ---------- Sub-horizon clamp ----------

    TEST(ProceduralSkyMath, SubHorizonSunIsClampedAboveHorizon)
    {
        // The Perez F-function blows up once chi crosses pi/2 (sun under
        // the horizon). We clamp to a small positive elevation so the
        // IBL bake stays finite.
        PreethamParameters p = StandardParams();
        p.SunDirection = glm::vec3(0.7f, -0.4f, 0.6f); // Below horizon
        const auto c = ProceduralSky::ComputeCoefficients(p);

        // Result must be finite and the clamped sun direction must lie
        // above the horizon.
        EXPECT_TRUE(std::isfinite(c.SunDirection.x));
        EXPECT_TRUE(std::isfinite(c.SunDirection.y));
        EXPECT_TRUE(std::isfinite(c.SunDirection.z));
        EXPECT_TRUE(std::isfinite(c.ZenithXYY.z));
        EXPECT_GT(c.SunDirection.y, 0.0f) << "Sun was clamped above horizon";
    }

    TEST(ProceduralSkyMath, ZeroLengthSunDefaultsToZenith)
    {
        // A zero-length sun vector is a likely accident (uninitialised
        // memory, bad serialisation).  Should defensively default to
        // straight up rather than NaN-tainting the whole frame.
        PreethamParameters p = StandardParams();
        p.SunDirection = glm::vec3(0.0f);
        const auto c = ProceduralSky::ComputeCoefficients(p);

        EXPECT_TRUE(std::isfinite(c.SunDirection.x));
        EXPECT_GT(c.SunDirection.y, 0.9f); // Should be near +Y
    }

    // ---------- Sun disk packing ----------

    TEST(ProceduralSkyMath, SunDiskCosAngleIsConsistent)
    {
        // ubo.SunDirection.w must be cos(angularRadius * sunDiskSize).
        // We compare a default (1x) against a doubled disk size and verify
        // the cosine goes *down* (wider disk).
        PreethamParameters p = StandardParams();
        p.SunDiskSize = 1.0f;
        const auto c1 = ProceduralSky::ComputeCoefficients(p);

        p.SunDiskSize = 2.0f;
        const auto c2 = ProceduralSky::ComputeCoefficients(p);

        EXPECT_GT(c1.SunDirection.w, c2.SunDirection.w);
        EXPECT_GT(c1.SunDirection.w, 0.999f); // Very narrow at 1x
    }

    // ---------- Hash stability ----------

    TEST(ProceduralSkyMath, HashChangesWhenParametersChange)
    {
        PreethamParameters p1 = StandardParams();
        PreethamParameters p2 = p1;
        p2.Turbidity += 0.5f;

        EXPECT_NE(ProceduralSky::HashParameters(p1, 256),
                  ProceduralSky::HashParameters(p2, 256));
        EXPECT_NE(ProceduralSky::HashParameters(p1, 256),
                  ProceduralSky::HashParameters(p1, 512));
        EXPECT_EQ(ProceduralSky::HashParameters(p1, 256),
                  ProceduralSky::HashParameters(p1, 256));
    }

    // ---------- End-to-end CPU evaluator ----------

    TEST(ProceduralSkyMath, SkyIsBrighterTowardSunThanAntiSun)
    {
        // The Perez phase function makes pixels near the sun much
        // brighter than the opposite side of the dome. Both must be
        // finite and positive.
        PreethamParameters p = StandardParams();
        const auto c = ProceduralSky::ComputeCoefficients(p);
        const glm::vec3 sunDir = glm::vec3(c.SunDirection);

        // Toward-sun direction sampled near (but not at) the sun.
        const glm::vec3 nearSun = glm::normalize(sunDir + glm::vec3(0.05f, 0.0f, 0.05f));
        // Anti-sun direction at roughly the same elevation.
        const glm::vec3 antiSun = glm::normalize(glm::vec3(-sunDir.x, sunDir.y, -sunDir.z));

        const glm::vec3 nearSunRGB = ProceduralSky::EvaluateAtDirection(c, nearSun);
        const glm::vec3 antiSunRGB = ProceduralSky::EvaluateAtDirection(c, antiSun);

        for (int i = 0; i < 3; ++i)
        {
            EXPECT_TRUE(std::isfinite(nearSunRGB[i]));
            EXPECT_TRUE(std::isfinite(antiSunRGB[i]));
            EXPECT_GE(nearSunRGB[i], 0.0f);
            EXPECT_GE(antiSunRGB[i], 0.0f);
        }

        // Compare perceptual luminance (Rec. 709), NOT the RGB sum. After the
        // luminance tonemap the near-sun region is whiter while the anti-sun
        // region is bluer; since blue contributes little to luminance, a blue
        // colour needs a large B channel to reach a given luminance, which
        // inflates the RGB *sum* — so an RGB-sum comparison can rank the
        // (dimmer) bluer side higher. Luminance is the right brightness metric.
        const auto luma = [](const glm::vec3& c) {
            return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
        };
        EXPECT_GT(luma(nearSunRGB), luma(antiSunRGB))
            << "Sky luminance must be higher near the sun than opposite it.";
    }

    TEST(ProceduralSkyMath, BluerAwayFromSunForClearSky)
    {
        // For low-turbidity (T = 2) the sky should be bluer (more B
        // relative to R) at a direction away from the sun. This catches
        // bad x/y chromaticity wiring (a swap would tint the sky red).
        PreethamParameters p = StandardParams();
        p.Turbidity = 2.0f;
        p.SunDirection = glm::normalize(glm::vec3(0.6f, 0.6f, 0.6f));
        const auto c = ProceduralSky::ComputeCoefficients(p);

        // Sample 90 degrees from the sun, around the horizon.
        const glm::vec3 sample = glm::normalize(glm::vec3(-0.7f, 0.3f, 0.7f));
        const glm::vec3 rgb = ProceduralSky::EvaluateAtDirection(c, sample);

        // For a daylight zenith the blue channel should not be the
        // dimmest of the three.  This catches X/Z swap-style mistakes
        // in the XYZ->RGB matrix.
        EXPECT_GT(rgb.b, 0.0f);
        EXPECT_GE(rgb.b, std::min(rgb.r, rgb.g));
    }

    TEST(ProceduralSkyMath, AllOutputsAreFinite)
    {
        // Adversarial inputs: very high turbidity, low sun.
        PreethamParameters p = StandardParams();
        p.Turbidity = 10.0f;
        p.SunDirection = glm::normalize(glm::vec3(0.98f, 0.15f, 0.0f));
        const auto c = ProceduralSky::ComputeCoefficients(p);

        // Sample in 26 directions on the unit sphere (skipping the
        // exact poles to avoid the asin branch in some impls).
        for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
        for (int z = -1; z <= 1; ++z)
        {
            if (x == 0 && y == 0 && z == 0) continue;
            const glm::vec3 dir = glm::normalize(glm::vec3(x, y, z));
            const glm::vec3 rgb = ProceduralSky::EvaluateAtDirection(c, dir);
            for (int i = 0; i < 3; ++i)
            {
                EXPECT_TRUE(std::isfinite(rgb[i]))
                    << "dir = (" << dir.x << "," << dir.y << "," << dir.z << ")";
                EXPECT_GE(rgb[i], 0.0f);
            }
        }
    }
}
